#!/usr/bin/env python3
"""ops/linux/chain_snapshot.py -- a REST snapshot of the whole NIFTY and BANKNIFTY option chains.

FYERS history refuses expired options and futures, so the only option data
ALTAIR will ever have is what it records itself. This is the cheap second line
next to the tape recorder: two scheduled snapshots a day, independent of the
recorder, the terminal and the socket:

    altair-chain@open.timer    Mon-Fri 09:20 IST
    altair-chain@close.timer   Mon-Fri 15:27 IST

Each quotes, through FYERS /data/quotes (read-only): NIFTY 50, NIFTY BANK,
INDIA VIX, the NIFTY and BANKNIFTY futures, and the nearest --expiries option
expiries of each index plus the nearest monthly, ATM +/- --strikes, calls and puts.
The chain comes from altair_price_service's own universe builder (dry run, no
socket), centred on the index levels just quoted.

WRITES (data/store/ is git-ignored):
    data/store/chain_snap/<day>/<HHMM>.csv
        snap_time,fyers,underlying,kind,expiry,strike,ltp,bid,ask,volume,
        open,high,low,prev_close,exch_time
    data/store/chain_snap/<day>/<HHMM>-<n>.json   the FYERS responses as received

Needs today's FYERS login. Without one it logs and exits 1.

    ops/linux/chain_snapshot.py          # dry run: the symbols it would quote
    ops/linux/chain_snapshot.py --go
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import subprocess
import sys
from pathlib import Path

from altair_ops import IST, ROOT, ist_now, log, require_tools

HEADER = "snap_time,fyers,underlying,kind,expiry,strike,ltp,bid,ask,volume,open,high,low,prev_close,exch_time"


def read_quotes(path: Path) -> dict[str, dict]:
    """The `v` block of every ok quote in a FYERS quotes response file, by symbol."""
    out: dict[str, dict] = {}
    if not path.is_file():
        return out
    j = json.loads(path.read_text())
    for r in j.get("responses", []) or []:
        body = r.get("body")
        if not body:
            continue
        for d in body.get("d", []) or []:
            if d.get("s") == "ok" and d.get("v"):
                out[d["n"]] = d["v"]
    return out


def chain_rows(universe: Path) -> list[dict]:
    with open(universe, newline="") as f:
        return [r for r in csv.DictReader(f)
                if (r["underlying"] in ("NIFTY", "BANKNIFTY") and r["kind"] in ("index", "future", "call", "put")
                    and r["fyers"].startswith("NSE:")) or r["fyers"] == "NSE:INDIAVIX-INDEX"]


def csv_cell(v) -> str:
    return "" if v is None else str(v)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--go", action="store_true")
    ap.add_argument("--tag", default="")
    ap.add_argument("--strikes", type=int, default=40)
    ap.add_argument("--expiries", type=int, default=3)
    a = ap.parse_args()

    now = ist_now()
    day = now.date().isoformat()
    tag = a.tag or now.strftime("%H%M")
    t = require_tools("chain", "altair_fyers_quotes", "altair_price_service")
    quotes, service = t["altair_fyers_quotes"], t["altair_price_service"]
    root = ROOT / "data" / "store" / "chain_snap"
    uni, out_dir = root / "_universe", root / day
    uni.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)

    # 1. The index levels, to centre the chains on.
    atm: list[str] = []
    if a.go:
        r = subprocess.run([str(quotes), "--symbols", "NSE:NIFTY50-INDEX,NSE:NIFTYBANK-INDEX",
                            "--out", str(uni / "spots.json"), "--go"], cwd=ROOT, capture_output=True)
        if r.returncode != 0:
            log("chain", "WARN", f"snapshot {tag} skipped: FYERS quotes did not answer (no login today?)")
            return 1
        spot = read_quotes(uni / "spots.json")
        if "NSE:NIFTY50-INDEX" in spot:
            atm += ["--atm-nifty", str(spot["NSE:NIFTY50-INDEX"].get("lp"))]
        if "NSE:NIFTYBANK-INDEX" in spot:
            atm += ["--atm-banknifty", str(spot["NSE:NIFTYBANK-INDEX"].get("lp"))]

    # 2. The chain: the price service's universe builder, dry (no socket, no --go).
    subprocess.run([str(service), "--fyers", "--live-dir", str(uni), "--strikes", str(a.strikes),
                    "--depth-strikes", "0", "--expiries", str(a.expiries), "--monthly", "--no-stocks", *atm],
                   cwd=ROOT, capture_output=True)
    if not (uni / "universe.csv").is_file():
        log("chain", "ERROR", f"snapshot {tag}: no universe built")
        return 2
    rows = chain_rows(uni / "universe.csv")
    meta = {r["fyers"]: r for r in rows}
    symbols = sorted(meta)
    expiries = ", ".join(sorted({f"{r['underlying']} {r['expiry']}" for r in rows if r["kind"] in ("call", "put")}))
    if not a.go:
        print(f"DRY RUN: would quote {len(symbols)} symbols ({expiries}) into {out_dir}/{tag}.csv")
        return 0

    # 3. Quote them, 500 a call (the quotes tool's own limit; 50 a request inside it).
    n = 0
    for at in range(0, len(symbols), 500):
        n += 1
        lst = uni / f"symbols-{n}.txt"
        lst.write_text("\n".join(symbols[at:at + 500]) + "\n")
        subprocess.run([str(quotes), "--symbols-file", str(lst), "--out", str(out_dir / f"{tag}-{n}.json"), "--go"],
                       cwd=ROOT, capture_output=True)

    # 4. One CSV row a symbol.
    stamp = now.strftime("%Y-%m-%dT%H:%M:%S") + "+05:30"
    lines = [HEADER]
    got = 0
    for k in range(1, n + 1):
        for sym, v in read_quotes(out_dir / f"{tag}-{k}.json").items():
            m = meta.get(sym, {})
            tt = ""
            if v.get("tt"):
                tt = dt.datetime.fromtimestamp(int(v["tt"]), IST).strftime("%Y-%m-%dT%H:%M:%S") + "+05:30"
            lines.append(",".join(csv_cell(x) for x in (
                stamp, sym, m.get("underlying"), m.get("kind"), m.get("expiry"), m.get("strike"), v.get("lp"),
                v.get("bid"), v.get("ask"), v.get("volume"), v.get("open_price"), v.get("high_price"),
                v.get("low_price"), v.get("prev_close_price"), tt)))
            got += 1
    (out_dir / f"{tag}.csv").write_text("\n".join(lines) + "\n")
    level = "ERROR" if got == 0 else ("WARN" if got < len(symbols) else "INFO")
    log("chain", level, f"snapshot {tag}: {got} of {len(symbols)} quoted ({expiries}) -> {out_dir}/{tag}.csv")
    return 1 if got == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
