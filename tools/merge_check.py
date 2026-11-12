#!/usr/bin/env python3
"""tools/merge_check.py -- check the bar sources against each other BEFORE any merge.

Smit (2026-11-12): "we must check data first, what if rows and columns conflict
with different values?". Nothing is merged here. For every series (segment,
symbol, timeframe) found in two or more sources, it lines the bars up on their
IST start time and reports:

  * per source: rows, first/last bar, duplicate times, impossible bars
    (high < max(open, close), low > min(open, close), price <= 0), bars
    outside 09:15-15:30 IST (intraday frames only);
  * per pair of sources: overlapping bars, bars only in one, and how far apart
    open/high/low/close are (exact, <=1 bp, 1-10 bp, 10-100 bp, >100 bp),
    volume agreement where both have volume;
  * a time-alignment test: the close gap with the second source shifted by
    -1/0/+1 bar, so a source stamping bars at their END instead of START shows
    up as "best shift != 0" instead of as thousands of fake conflicts.

Each source is a tree <root>/<seg>/<symbol>/<tf>/*.csv with columns
time,open,high,low,close[,volume[,oi]]; folders starting with "_" are skipped.

    tools/merge_check.py --source dataset=dataset \\
        --source drive-ticks=data/drive_ticks \\
        --source fyers=../altair-data/candles --out reports/merge_check
"""
from __future__ import annotations

import argparse
import glob
import itertools
import os
import sys

import numpy as np
import pandas as pd

TFS = ("1m", "5m", "15m", "60m", "1d")
PX = ("open", "high", "low", "close")
BUCKETS = [(0.0, "exact"), (1.0, "<=1bp"), (10.0, "1-10bp"), (100.0, "10-100bp"), (np.inf, ">100bp")]


def load_source(root: str) -> dict[tuple[str, str, str], pd.DataFrame]:
    out = {}
    for seg in sorted(os.listdir(root)):
        sp = os.path.join(root, seg)
        if seg.startswith("_") or not os.path.isdir(sp):
            continue
        for sym in sorted(os.listdir(sp)):
            yp = os.path.join(sp, sym)
            if sym.startswith("_") or not os.path.isdir(yp):
                continue
            for tf in TFS:
                files = sorted(glob.glob(os.path.join(yp, tf, "*.csv")))
                if not files:
                    continue
                df = pd.concat([pd.read_csv(f) for f in files], ignore_index=True)
                df["time"] = pd.to_datetime(df["time"], utc=True).dt.tz_convert("Asia/Kolkata")
                if tf == "1d":  # daily bars: compare by date whatever the stamp hour
                    df["time"] = df["time"].dt.normalize()
                for c in PX + ("volume",):
                    if c in df:
                        df[c] = pd.to_numeric(df[c], errors="coerce")
                out[(seg, sym.lower(), tf)] = df.sort_values("time", kind="stable").reset_index(drop=True)
    return out


def self_check(df: pd.DataFrame, tf: str) -> dict:
    dup = int(df["time"].duplicated().sum())
    o, h, l, c = (df[k] for k in PX)
    bad = int(((h < np.maximum(o, c) - 1e-9) | (l > np.minimum(o, c) + 1e-9) | (df[list(PX)] <= 0).any(axis=1)).sum())
    nan = int(df[list(PX)].isna().any(axis=1).sum())
    outside = 0
    if tf != "1d":
        mins = df["time"].dt.hour * 60 + df["time"].dt.minute
        outside = int(((mins < 9 * 60 + 15) | (mins >= 15 * 60 + 30)).sum())
    return dict(rows=len(df), first=str(df["time"].iloc[0]), last=str(df["time"].iloc[-1]),
                dup_times=dup, impossible=bad, missing_px=nan, outside_session=outside)


def bp(a, b):
    return (a - b).abs() / b.abs().where(b.abs() > 0) * 1e4


def compare(a: pd.DataFrame, b: pd.DataFrame, tf: str):
    a1 = a.drop_duplicates("time", keep="last").set_index("time")
    b1 = b.drop_duplicates("time", keep="last").set_index("time")
    j = a1.join(b1, how="inner", lsuffix="_a", rsuffix="_b")
    res = dict(overlap=len(j), only_a=int((~a1.index.isin(b1.index)).sum()),
               only_b=int((~b1.index.isin(a1.index)).sum()))
    if not len(j):
        return res, None
    # alignment: median close gap (bp) with b shifted by -1/0/+1 bars, on the overlap window
    lo, hi = j.index.min(), j.index.max()
    bw = b1.loc[lo - pd.Timedelta(days=3): hi + pd.Timedelta(days=3), "close"]
    aw = a1.loc[lo:hi, "close"]
    shifts = {}
    for s in (-1, 0, 1):
        bs = bw.shift(-s)  # b's bar s positions later, put at this time
        m = aw.to_frame("a").join(bs.rename("b"), how="inner").dropna()
        if len(m):
            shifts[s] = float(np.median(bp(m["a"], m["b"])))
    res["close_gap_bp_by_shift"] = {k: round(v, 2) for k, v in shifts.items()}
    res["best_shift"] = min(shifts, key=shifts.get) if shifts else 0
    worst = pd.concat([bp(j[f"{k}_a"], j[f"{k}_b"]) for k in PX], axis=1).max(axis=1)
    j["worst_bp"] = worst
    edges = [x[0] for x in BUCKETS]
    counts = {}
    for i, (_, name) in enumerate(BUCKETS):
        if i == 0:
            m = (j[[f"{k}_a" for k in PX]].values == j[[f"{k}_b" for k in PX]].values).all(axis=1)
            m |= worst.values < 0.05  # floating noise / paise rounding
        else:
            m = (worst > (edges[i - 1] if i > 1 else 0.05)) & (worst <= edges[i])
            m = m.values
        counts[name] = int(m.sum())
    counts["nan"] = int(worst.isna().sum())
    res["ohlc_agreement"] = counts
    res["median_worst_bp"] = round(float(worst.median()), 3)
    res["p99_worst_bp"] = round(float(worst.quantile(0.99)), 2)
    if "volume_a" in j and "volume_b" in j:
        v = j[["volume_a", "volume_b"]].dropna()
        v = v[(v > 0).all(axis=1)]
        if len(v):
            r = (v["volume_a"] / v["volume_b"])
            res["volume_rows_both"] = len(v)
            res["volume_ratio_median"] = round(float(r.median()), 4)
            res["volume_equal_pct"] = round(100 * float((v["volume_a"] == v["volume_b"]).mean()), 2)
    return res, j


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--source", action="append", required=True, help="name=root (repeat; order = priority low->high)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--conflict-bp", type=float, default=10.0, help="rows worse than this go to conflicts.csv")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    srcs = {}
    for s in args.source:
        name, root = s.split("=", 1)
        srcs[name] = load_source(root)
        print(f"{name}: {len(srcs[name])} series from {root}", file=sys.stderr)

    keys = sorted(set().union(*[set(v) for v in srcs.values()]))
    lines = ["# Source check before merge", "", "Nothing merged. Bars aligned on IST bar start; daily bars by date.", ""]
    lines += ["## Each source on its own", "", "| series | source | rows | first | last | dup times | impossible | missing px | outside 09:15-15:30 |", "|---|---|---|---|---|---|---|---|---|"]
    for k in keys:
        for n, d in srcs.items():
            if k in d:
                r = self_check(d[k], k[2])
                lines.append(f"| {'/'.join(k)} | {n} | {r['rows']} | {r['first'][:16]} | {r['last'][:16]} | {r['dup_times']} | {r['impossible']} | {r['missing_px']} | {r['outside_session']} |")
    lines += ["", "## Where two sources overlap", "",
              "Agreement = worst of the four OHLC gaps on each bar. Best shift != 0 means one source stamps bars a bar off.", "",
              "| series | A vs B | overlap | only A | only B | exact | <=1bp | 1-10bp | 10-100bp | >100bp | median bp | p99 bp | close gap by shift (-1/0/+1) | volume A/B median |",
              "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    conflicts = []
    for k in keys:
        have = [n for n in srcs if k in srcs[n]]
        for na, nb in itertools.combinations(have, 2):
            r, j = compare(srcs[na][k], srcs[nb][k], k[2])
            if not r["overlap"]:
                continue
            c = r["ohlc_agreement"]
            sh = r["close_gap_bp_by_shift"]
            lines.append(f"| {'/'.join(k)} | {na} vs {nb} | {r['overlap']} | {r['only_a']} | {r['only_b']} | {c['exact']} | {c['<=1bp']} | {c['1-10bp']} | {c['10-100bp']} | {c['>100bp']} | {r['median_worst_bp']} | {r['p99_worst_bp']} | {sh.get(-1)} / {sh.get(0)} / {sh.get(1)} | {r.get('volume_ratio_median', '-')} |")
            bad = j[j["worst_bp"] > args.conflict_bp].copy()
            if len(bad):
                bad.insert(0, "series", "/".join(k))
                bad.insert(1, "a", na)
                bad.insert(2, "b", nb)
                conflicts.append(bad.reset_index())
    if conflicts:
        allc = pd.concat(conflicts, ignore_index=True)
        cols = ["series", "a", "b", "time", "worst_bp"] + [f"{k}_{s}" for k in PX for s in ("a", "b")]
        cols += [c for c in ("volume_a", "volume_b") if c in allc]
        allc[cols].sort_values(["series", "a", "b", "time"]).to_csv(os.path.join(args.out, "conflicts.csv"), index=False)
        lines += ["", f"{len(allc)} bars differ by more than {args.conflict_bp:g} bp: conflicts.csv"]
    open(os.path.join(args.out, "report.md"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
