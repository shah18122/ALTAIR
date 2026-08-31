#!/usr/bin/env python3
"""Ingest TradingView bar exports into the dataset/ partition.

OFFLINE RESEARCH TOOL. This is not runtime code and is never linked into the
engine — CLAUDE.md bans Python from the runtime, not from data prep that runs
once a day on a workstation. The engine reads the NORMALISED OUTPUT through a
C++ loader (a later card), never this script's input.

Two rules govern it:

1.  It never parses a price as a number. `24080.4` round-tripped through a
    float can come back as `24080.400000000001`, and the whole point of P1-04
    was that a price which drifts by one unit is a price that rounds an order
    to the wrong tick. Prices are copied as TEXT, byte for byte.

2.  It refuses to overwrite history. If a timestamp is already on disk with
    DIFFERENT values, that is a disagreement between two exports and this
    script stops and reports it rather than picking one. Same philosophy as
    the P1-06 reconciler: failing loud beats silently rewriting the past.

Usage:
    python research/tools/ingest_bars.py <file-or-dir> [more...]
    python research/tools/ingest_bars.py --dry-run <file>
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import os
import re
import sys
from collections import defaultdict

# ── layout ───────────────────────────────────────────────────────────────
# dataset/<segment>/<symbol>/<timeframe>/<partition>.csv
# Segment names on disk are CLAUDE.md's: spot, fut, opt, cur, com.
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATASET = os.path.join(REPO, "dataset")

KEEP = ("time", "open", "high", "low", "close", "volume")

# TradingView writes the timeframe as a bare number of minutes, or 1D/1W/1M.
TF_MAP = {"1": "1m", "3": "3m", "5": "5m", "15": "15m", "30": "30m",
          "45": "45m", "60": "60m", "120": "120m", "240": "240m",
          "1D": "1d", "1W": "1w", "1M": "1mo"}

# Everything a TradingView export carries beyond OHLCV is dropped. Three
# different reasons, all of them disqualifying:
#
#   * The Monte Carlo overlay ("Expected Value Level", "N Sigma Upper Level",
#     "Developing High Outcome", ...) is a FORWARD PROJECTION. It is populated
#     only on the last ~100 bars of the export because that is the forecast
#     horizon. A forward projection in a training set is look-ahead by
#     definition — CLAUDE.md rule 7.
#   * The divergence labels ("Regular Bullish", "Regular Bearish") need a
#     CONFIRMED PIVOT, which is only known several bars later. TradingView
#     repaints them. A label that repaints is a label that leaks.
#   * RSI and "RSI-based MA" are neither, but they are a black box: unknown
#     period, unknown smoothing, unrecoverable from the file. Rule 10 requires
#     every live decision to be reproducible from a feature_version, and an
#     indicator nobody can recompute cannot carry one. Derived features belong
#     in features/, computed by our code, versioned.
#
# The raw dataset holds measurements. Nothing else.

FNAME_RE = re.compile(r"^(?P<ex>[A-Z]+)_(?P<sym>[A-Z0-9&_-]+),\s*(?P<tf>[0-9]+|1[DWM])_")


def parse_name(path: str):
    """(exchange, symbol, timeframe) from a TradingView export filename."""
    m = FNAME_RE.match(os.path.basename(path))
    if not m:
        return None
    tf = TF_MAP.get(m.group("tf"))
    if tf is None:
        return None
    return m.group("ex"), m.group("sym").lower(), tf


def partition_of(timeframe: str, time_text: str) -> str:
    """Which file a bar belongs in.

    Sized so a partition holds a useful number of bars, because the partition
    is a fold boundary and a read unit, not a filing convention:

      1m        -> one file per TRADING DATE (~375 bars). This is the shape the
                   daily drop arrives in, so an ingest appends one new file
                   instead of rewriting a large one.
      3m..240m  -> one file per MONTH (15m ~500 bars, 60m ~150). Per-date here
                   would mean 7-bar files: 606 of them for two years of 60m,
                   which is clutter rather than a boundary.
      1d and up -> a single file. Small, and always read whole.

    Every choice still leaves a walk-forward fold selectable by filename glob,
    which is the point of the partition.
    """
    if timeframe in ("1d", "1w", "1mo"):
        return "all"
    if timeframe == "1m":
        return time_text[:10]      # YYYY-MM-DD, already the local trading date
    return time_text[:7]           # YYYY-MM


def read_export(path: str):
    """Yield OHLCV dicts with prices left as TEXT. Never float()s a price."""
    with open(path, newline="", encoding="utf-8-sig") as fh:
        rdr = csv.DictReader(fh)
        cols = {c.lower(): c for c in (rdr.fieldnames or [])}
        if "time" not in cols:
            raise ValueError(f"{path}: no 'time' column")
        for need in ("open", "high", "low", "close"):
            if need not in cols:
                raise ValueError(f"{path}: no '{need}' column")
        vol = cols.get("volume")
        for row in rdr:
            t = (row[cols["time"]] or "").strip()
            if not t:
                continue
            yield {
                "time": t,
                "open": (row[cols["open"]] or "").strip(),
                "high": (row[cols["high"]] or "").strip(),
                "low": (row[cols["low"]] or "").strip(),
                "close": (row[cols["close"]] or "").strip(),
                # An index has no traded volume. Empty means "not published",
                # which is not the same as zero and must not become zero.
                "volume": (row[vol] or "").strip() if vol else "",
            }


def load_existing(path: str) -> dict:
    if not os.path.exists(path):
        return {}
    out = {}
    with open(path, newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            out[row["time"]] = row
    return out


# NSE continuous session, IST. Bars are stamped at the START of the minute, so
# the last one is 15:29 and the session yields 375 of them.
SESSION_START = (9, 15)
SESSION_END = (15, 29)


def session_minutes() -> set:
    return {f"{h:02d}:{m:02d}"
            for h in range(SESSION_START[0], SESSION_END[0] + 1)
            for m in range(60)
            if SESSION_START <= (h, m) <= SESSION_END}


def coverage_report(segment: str) -> None:
    """Report 1-minute session coverage.

    This runs on every ingest and not as a one-off audit, because the first
    drop already contained a systematic hole — NIFTY spot was missing 15:16
    through 15:27 on every day while India VIX had all 375 bars. A gap in the
    session close is the worst place for one: it is where a 10-minute-horizon
    label lives, and where NIFTY and VIX stop being joinable bar for bar.
    A finding is worth less than a check that repeats.
    """
    full = session_minutes()
    root = os.path.join(DATASET, segment)
    if not os.path.isdir(root):
        return
    print("\n1-minute session coverage (expected 375 bars, 09:15..15:29 IST):")
    for sym in sorted(os.listdir(root)):
        one = os.path.join(root, sym, "1m")
        if not os.path.isdir(one):
            continue
        holes = defaultdict(int)
        days = 0
        for fn in sorted(os.listdir(one)):
            if not fn.endswith(".csv"):
                continue
            path = os.path.join(one, fn)
            with open(path, newline="", encoding="utf-8") as fh:
                have = {r["time"][11:16] for r in csv.DictReader(fh)}
            if not have:
                continue
            days += 1
            # Only count gaps INSIDE the day's own span — a partial first or
            # last day is a truncated export, not a hole in the data.
            lo, hi = min(have), max(have)
            for t in full:
                if lo <= t <= hi and t not in have:
                    holes[t] += 1
        if days == 0:
            continue
        systematic = sorted(t for t, n in holes.items() if n == days)
        print(f"  {sym:<12}{days:>3} day(s)", end="")
        if not holes:
            print("  complete")
        elif systematic:
            print(f"  MISSING ON EVERY DAY: {systematic[0]}..{systematic[-1]} "
                  f"({len(systematic)} minutes)")
            print(f"  {'':<15}-> a systematic feed gap, not a random dropout. "
                  f"Do not interpolate across it.")
        else:
            print(f"  {len(holes)} intermittent gap(s)")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--segment", default="spot",
                    choices=("spot", "fut", "opt", "cur", "com"))
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    paths = []
    for p in args.inputs:
        if os.path.isdir(p):
            paths += [os.path.join(p, f) for f in sorted(os.listdir(p))
                      if f.lower().endswith(".csv")]
        else:
            paths.append(p)

    # (symbol, timeframe, partition) -> {time: row}
    staged: dict = defaultdict(dict)
    provenance: dict = defaultdict(list)
    conflicts = []
    skipped = []

    for path in paths:
        info = parse_name(path)
        if info is None:
            skipped.append((os.path.basename(path), "filename not a TradingView export"))
            continue
        _ex, sym, tf = info
        rows = list(read_export(path))
        if not rows:
            skipped.append((os.path.basename(path), "no data rows"))
            continue

        digest = hashlib.sha256(open(path, "rb").read()).hexdigest()[:16]
        provenance[(sym, tf)].append({
            "file": os.path.basename(path),
            "sha256_16": digest,
            "rows": len(rows),
            "first": rows[0]["time"],
            "last": rows[-1]["time"],
        })

        for r in rows:
            part = partition_of(tf, r["time"])
            key = (sym, tf, part)
            prev = staged[key].get(r["time"])
            if prev is not None and prev != r:
                conflicts.append((sym, tf, r["time"], prev, r))
            staged[key][r["time"]] = r

    # Merge against what is already on disk, and refuse to rewrite history.
    written = []
    for (sym, tf, part), rows in sorted(staged.items()):
        out_dir = os.path.join(DATASET, args.segment, sym, tf)
        out_path = os.path.join(out_dir, f"{part}.csv")
        existing = load_existing(out_path)
        for t, r in rows.items():
            old = existing.get(t)
            if old is not None and any(old.get(k, "") != r[k] for k in KEEP):
                conflicts.append((sym, tf, t, old, r))
            existing[t] = r
        merged = [existing[t] for t in sorted(existing)]
        written.append((out_dir, out_path, merged, len(rows)))

    if conflicts:
        print(f"REFUSED — {len(conflicts)} bar(s) disagree with data already on disk.")
        print("Nothing was written. Two exports describe the same instant differently;")
        print("silently picking one would rewrite history. First 5:\n")
        for sym, tf, t, a, b in conflicts[:5]:
            print(f"  {sym} {tf} {t}")
            print(f"    on disk : {[a.get(k,'') for k in KEEP]}")
            print(f"    incoming: {[b.get(k,'') for k in KEEP]}")
        return 2

    if args.dry_run:
        print("DRY RUN — nothing written\n")
    total_new = 0
    for out_dir, out_path, merged, n_in in written:
        total_new += n_in
        rel = os.path.relpath(out_path, REPO)
        print(f"  {rel:<52}{len(merged):>7} bars  (+{n_in} from this run)")
        if args.dry_run:
            continue
        os.makedirs(out_dir, exist_ok=True)
        with open(out_path, "w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=list(KEEP), extrasaction="ignore")
            w.writeheader()
            w.writerows(merged)

    for name, why in skipped:
        print(f"  SKIPPED {name}: {why}")

    if not args.dry_run:
        for (sym, tf), prov in sorted(provenance.items()):
            mdir = os.path.join(DATASET, args.segment, sym)
            os.makedirs(mdir, exist_ok=True)
            mpath = os.path.join(mdir, "_manifest.json")
            man = {}
            if os.path.exists(mpath):
                man = json.load(open(mpath, encoding="utf-8"))
            man.setdefault("symbol", sym)
            man.setdefault("segment", args.segment)
            man.setdefault("source", "TradingView export")
            man.setdefault("dropped_columns_reason",
                           "Monte Carlo forecast overlay is forward-looking; "
                           "divergence labels repaint; RSI is a black box that "
                           "cannot carry a feature_version. See ingest_bars.py.")
            man.setdefault("ingests", [])
            man["ingests"].append({
                "at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
                "timeframe": tf,
                "files": prov,
            })
            with open(mpath, "w", encoding="utf-8") as fh:
                json.dump(man, fh, indent=2)

    print(f"\n{total_new} bars ingested across {len(written)} partition(s).")
    if not args.dry_run:
        coverage_report(args.segment)
    return 0


if __name__ == "__main__":
    sys.exit(main())
