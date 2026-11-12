#!/usr/bin/env python3
"""tools/merge_bars.py -- one merged bar dataset from several sources, nothing lost.

Smit's choice (2026-11-12): keep every source's row, plus one clean series.
Run tools/merge_check.py first; it reports where the sources disagree.

Sources are given lowest priority first, e.g. drive-ticks, dataset, fyers
(FYERS wins). Each is a tree <root>/<seg>/<symbol>/<tf>/*.csv with columns
time,open,high,low,close[,volume[,oi]].

Output, per series:

  <out>/<seg>/<sym>/<tf>/all_sources.csv.gz
      every row of every source: time,source,open,high,low,close,volume,oi,flag
      flag: ok | impossible (open/close outside high/low, price <= 0)
                | spike (a wick far outside its neighbours, see below)
                | outside_day (spot intraday high/low outside that day's
                  official daily range from an official source)
  <out>/<seg>/<sym>/<tf>/clean.csv.gz
      one row per time: time,open,high,low,close,volume,oi,source,flag
      - each DAY comes from one source (never spliced inside a day): the
        highest-priority source holding at least 90% of that day's best bar
        count;
      - a flagged bar in that source is replaced by the same bar from the next
        source that has it unflagged (flag "substituted:<from>"); if none
        has, the bar is dropped from clean (it stays in all_sources);
      - daily bars: tick-built sources (--tick-built) are used only on days
        no official source has, since their open/close are first/last trade,
        not the exchange's official open/close.
  <out>/MANIFEST.json  sources, priority, rules, row counts per series.

Spike rule (intraday): wick = how far high (low) sits beyond the larger
(smaller) of the previous close and this close, in bp. A bar is a spike when
the wick is > max(--spike-min-bp, --spike-k x rolling median |1-bar return|
over the last 375 bars). Crash days raise the threshold with the volatility.

    tools/merge_bars.py --source drive-ticks=data/drive_ticks \\
        --source dataset=dataset --source fyers=data/store/fyers_tree \\
        --tick-built drive-ticks --out data/merged
"""
from __future__ import annotations

import argparse
import json
import os
import sys

import numpy as np
import pandas as pd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from merge_check import PX, load_source  # noqa: E402

COLS = ["open", "high", "low", "close", "volume", "oi"]


def flag_rows(df: pd.DataFrame, tf: str, day_range: pd.DataFrame | None, a) -> pd.Series:
    o, h, l, c = (df[k] for k in PX)
    flag = pd.Series("ok", index=df.index, dtype=object)
    imp = (h < np.maximum(o, c) - 1e-9) | (l > np.minimum(o, c) + 1e-9) | (df[list(PX)] <= 0).any(axis=1) | df[list(PX)].isna().any(axis=1)
    flag[imp] = "impossible"
    if tf == "1d":
        return flag
    day = df["time"].dt.normalize()
    pc = c.shift(1).where(day == day.shift(1), o)  # first bar of the day: compare with its own open
    up = (h - np.maximum(pc, c)) / c * 1e4
    dn = (np.minimum(pc, c) - l) / c * 1e4
    wick = np.maximum(up, dn)
    ret = (c / c.shift(1) - 1).abs() * 1e4
    thr = np.maximum(a.spike_min_bp, a.spike_k * ret.rolling(375, min_periods=30).median().fillna(ret.median()))
    flag[(wick > thr) & (flag == "ok")] = "spike"
    if day_range is not None:
        r = day_range.reindex(day.values)
        tol = 1 + a.day_tol_bp / 1e4
        out = (h.values > r["high"].values * tol) | (l.values < r["low"].values / tol)
        out &= ~np.isnan(r["high"].values)
        flag[out & (flag == "ok").values] = "outside_day"
    return flag


def build_clean(rows: pd.DataFrame, prio: list[str], tf: str, tick_built: set[str]) -> pd.DataFrame:
    rank = {s: i for i, s in enumerate(prio)}  # higher = wins
    rows = rows.assign(_r=rows["source"].map(rank), day=rows["time"].dt.normalize())
    if tf == "1d":
        rows = rows.assign(_r=rows["_r"] - np.where(rows["source"].isin(tick_built), 1000, 0))
        ok = rows[rows["flag"] == "ok"].sort_values(["time", "_r"]).drop_duplicates("time", keep="last")
        out = ok.copy()
        out["flag"] = "ok"
        return out
    # choose one source per day
    cnt = rows.groupby(["day", "source"]).size().rename("n").reset_index()
    cnt["best"] = cnt.groupby("day")["n"].transform("max")
    cnt = cnt[cnt["n"] >= 0.9 * cnt["best"]]
    cnt["_r"] = cnt["source"].map(rank)
    pick = cnt.sort_values(["day", "_r"]).drop_duplicates("day", keep="last")[["day", "source"]]
    base = rows.merge(pick, on=["day", "source"])
    good = base[base["flag"] == "ok"].copy()
    bad = base[base["flag"] != "ok"]
    subs = []
    if len(bad):
        alt = rows[(rows["flag"] == "ok") & rows["time"].isin(bad["time"])]
        alt = alt.merge(bad[["time", "source"]].rename(columns={"source": "_from"}), on="time")
        alt = alt[alt["source"] != alt["_from"]].sort_values(["time", "_r"]).drop_duplicates("time", keep="last")
        alt["flag"] = "substituted:" + alt["_from"]
        subs.append(alt.drop(columns="_from"))
    good["flag"] = "ok"
    return pd.concat([good] + subs, ignore_index=True).sort_values("time")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--source", action="append", required=True, help="name=root, lowest priority first")
    ap.add_argument("--tick-built", action="append", default=[], help="source whose 1d bars are built from ticks")
    ap.add_argument("--out", required=True)
    ap.add_argument("--spike-min-bp", type=float, default=50.0)
    ap.add_argument("--spike-k", type=float, default=15.0)
    ap.add_argument("--day-tol-bp", type=float, default=5.0)
    a = ap.parse_args()
    prio, srcs = [], {}
    for s in a.source:
        n, root = s.split("=", 1)
        prio.append(n)
        srcs[n] = load_source(root)
    tick_built = set(a.tick_built)
    keys = sorted(set().union(*[set(v) for v in srcs.values()]))
    manifest = dict(created=pd.Timestamp.now(tz="Asia/Kolkata").isoformat(), priority_low_to_high=prio,
                    tick_built=sorted(tick_built), sources={n: s.split("=", 1)[1] for n, s in zip(prio, a.source)},
                    rules=dict(spike_min_bp=a.spike_min_bp, spike_k=a.spike_k, day_tol_bp=a.day_tol_bp,
                               day_pick="highest priority with >=90% of the day's best bar count",
                               daily="official sources first; tick-built only where no official bar"),
                    series={})
    for k in keys:
        seg, sym, tf = k
        # official daily range for spot intraday checks (futures: rolls differ by source, so skipped)
        rng = None
        if tf != "1d" and seg == "spot":
            d1 = [srcs[n][(seg, sym, "1d")].assign(_r=i) for i, n in enumerate(prio)
                  if (seg, sym, "1d") in srcs[n] and n not in tick_built]
            if d1:
                d1 = pd.concat(d1).sort_values(["time", "_r"]).drop_duplicates("time", keep="last")
                rng = d1.set_index("time")[["high", "low"]]
        parts = []
        for n in prio:
            if k not in srcs[n]:
                continue
            df = srcs[n][k].drop_duplicates("time", keep="last").reset_index(drop=True)
            for col in COLS:
                if col not in df:
                    df[col] = np.nan
            df["flag"] = flag_rows(df, tf, rng, a).values
            df["source"] = n
            parts.append(df[["time", "source"] + COLS + ["flag"]])
        rows = pd.concat(parts, ignore_index=True).sort_values(["time", "source"], kind="stable")
        clean = build_clean(rows, prio, tf, tick_built)
        d = os.path.join(a.out, seg, sym, tf)
        os.makedirs(d, exist_ok=True)
        fmt = lambda x: x.assign(time=x["time"].dt.strftime("%Y-%m-%dT%H:%M:%S+05:30"))  # noqa: E731
        fmt(rows).to_csv(os.path.join(d, "all_sources.csv.gz"), index=False)
        fmt(clean)[["time"] + COLS + ["source", "flag"]].to_csv(os.path.join(d, "clean.csv.gz"), index=False)
        manifest["series"]["/".join(k)] = dict(
            rows_by_source=rows.groupby("source").size().to_dict(),
            flags_by_source={s: g["flag"].value_counts().to_dict() for s, g in rows.groupby("source")},
            clean_rows=len(clean), clean_by_source=clean["source"].value_counts().to_dict(),
            substituted=int(clean["flag"].str.startswith("substituted").sum()),
            dropped_from_clean=int(rows["time"].nunique() - len(clean)),
            first=str(clean["time"].min()), last=str(clean["time"].max()))
        print("/".join(k), manifest["series"]["/".join(k)], file=sys.stderr)
    json.dump(manifest, open(os.path.join(a.out, "MANIFEST.json"), "w"), indent=1, default=str)
    return 0


if __name__ == "__main__":
    sys.exit(main())
