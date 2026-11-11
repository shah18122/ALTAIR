#!/usr/bin/env python3
"""tools/drive_ticks_to_bars.py -- Smit's Google Drive tick files -> ALTAIR bars.

The files Smit shared on 2026-11-11 (docs/DATA_POLICY.md, source "drive-ticks";
not FYERS, never merged into a FYERS series) are trade ticks, one contract a
file, no header:

    YYYYMMDD,HH:MM:SS,price,qty,oi        e.g. NIFTY20JANFUT.csv
    (spot files: YYYYMMDD,HH:MM:SS,price[,...])

This builds the dataset-shaped tree every ALTAIR tool reads:

    <out>/fut/<nifty|banknifty>/<1m|5m|15m|60m|1d>/all.csv
    <out>/spot/nifty/<tf>/all.csv
    <out>/SOURCE.txt                       "drive-ticks (NOT FYERS)"

Futures are stitched into one continuous front-month series: each day uses
the contract with the most traded quantity that day (the near month until
the roll). Ticks outside 09:15:00-15:29:59 IST are dropped. Bars are stamped
at their START in IST, columns time,open,high,low,close,volume,oi.

    tools/drive_ticks_to_bars.py --in S:/altair/drive-ticks --out data/drive_ticks
"""
from __future__ import annotations

import argparse
import collections
import os
import re
import sys
from pathlib import Path

TFS = (("1m", 1), ("5m", 5), ("15m", 15), ("60m", 60))
OPEN_S, CLOSE_S = 9 * 3600 + 15 * 60, 15 * 3600 + 30 * 60
FUT_RE = re.compile(r"^(BANKNIFTY|NIFTY)(\d\d)([A-Z]{3})FUT\.csv$", re.I)


def read_ticks(path: Path):
    """Yield (day 'YYYYMMDD', seconds of day, price, qty, oi) in file order."""
    with open(path, "r", newline="") as f:
        for line in f:
            p = line.rstrip("\r\n").split(",")
            if len(p) < 3 or len(p[0]) != 8 or not p[0].isdigit():
                continue
            try:
                hh, mm, ss = p[1].split(":")
                sec = int(hh) * 3600 + int(mm) * 60 + int(ss)
                px = float(p[2])
                qty = float(p[3]) if len(p) > 3 and p[3] else 0.0
                oi = float(p[4]) if len(p) > 4 and p[4] else float("nan")
            except ValueError:
                continue
            if not (OPEN_S <= sec < CLOSE_S) or not px > 0:
                continue
            yield p[0], sec, px, qty, oi


def stamp(day: str, sec: int) -> str:
    return f"{day[:4]}-{day[4:6]}-{day[6:]}T{sec // 3600:02d}:{sec % 3600 // 60:02d}:00+05:30"


def write_bars(ticks_by_day: dict[str, list[tuple[int, float, float, float]]], out: Path) -> dict[str, int]:
    """ticks_by_day: day -> [(sec, price, qty, oi)] in time order. Writes every timeframe."""
    counts = {}
    for name, minutes in TFS + (("1d", 0),):
        d = out / name
        d.mkdir(parents=True, exist_ok=True)
        n = 0
        with open(d / "all.csv", "w", newline="") as f:
            f.write("time,open,high,low,close,volume,oi\n")
            for day in sorted(ticks_by_day):
                ticks = ticks_by_day[day]
                if not ticks:
                    continue
                if minutes == 0:
                    groups = [(OPEN_S, ticks)]
                else:
                    g: dict[int, list] = collections.OrderedDict()
                    w = minutes * 60
                    for t in ticks:
                        g.setdefault(OPEN_S + (t[0] - OPEN_S) // w * w, []).append(t)
                    groups = list(g.items())
                for start, ts in groups:
                    o, c = ts[0][1], ts[-1][1]
                    h = max(t[1] for t in ts)
                    lo = min(t[1] for t in ts)
                    v = sum(t[2] for t in ts)
                    oi = ts[-1][3]
                    tt = f"{day[:4]}-{day[4:6]}-{day[6:]}T00:00:00+05:30" if minutes == 0 else stamp(day, start)
                    f.write(f"{tt},{o},{h},{lo},{c},{v:g},{'' if oi != oi else f'{oi:g}'}\n")
                    n += 1
        counts[name] = n
    return counts


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in", dest="src", required=True, help="folder holding the Drive files (searched recursively)")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    src, out = Path(a.src), Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "SOURCE.txt").write_text("drive-ticks (NOT FYERS): Smit's Google Drive tick files, 2020/2022/2023. docs/DATA_POLICY.md\n")

    # Futures: per underlying, per day, the contract with the most quantity.
    per: dict[str, dict[str, dict[str, list]]] = collections.defaultdict(lambda: collections.defaultdict(dict))
    for path in sorted(src.rglob("*.csv")):
        m = FUT_RE.match(path.name)
        if not m:
            continue
        under = m.group(1).upper()
        by_day: dict[str, list] = collections.defaultdict(list)
        for day, sec, px, qty, oi in read_ticks(path):
            by_day[day].append((sec, px, qty, oi))
        for day, ts in by_day.items():
            ts.sort(key=lambda t: t[0])
            per[under][day][path.name] = ts
        print(f"  {path.name}: {sum(len(v) for v in by_day.values())} ticks on {len(by_day)} days", flush=True)
    for under, days in per.items():
        chosen = {}
        for day, contracts in days.items():
            best = max(contracts, key=lambda k: sum(t[2] for t in contracts[k]))
            chosen[day] = contracts[best]
        counts = write_bars(chosen, out / "fut" / under.lower())
        print(f"{under} FUT: {len(chosen)} days -> {counts}")

    # Spot: any NIFTY*.csv that is not a futures file (NIFTY Spot Index folders).
    spot: dict[str, list] = collections.defaultdict(list)
    for path in sorted(src.rglob("*.csv")):
        if FUT_RE.match(path.name) or not path.name.upper().startswith("NIFTY") or "spot" not in str(path.parent).lower():
            continue
        for day, sec, px, qty, oi in read_ticks(path):
            spot[day].append((sec, px, qty, oi))
        print(f"  spot {path}: {len(spot)} days so far", flush=True)
    if spot:
        for ts in spot.values():
            ts.sort(key=lambda t: t[0])
        counts = write_bars(spot, out / "spot" / "nifty")
        print(f"NIFTY spot: {len(spot)} days -> {counts}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
