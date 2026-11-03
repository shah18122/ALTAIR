#!/usr/bin/env python3
"""ops/linux/fyers_chain.py -- save the day's NIFTY / BANKNIFTY option chains from FYERS.

FYERS history refuses expired options and futures, so a chain not saved before
its expiry is gone for good. altair_fyers_chain saves every listed future and
the calls and puts of the nearest expiries, ATM +/- --strikes, as one-minute
bars and a close per day (app/fyers_chain.hpp), into
    data/store/fyers_chain/<day>/<UNDERLYING>.csv and <UNDERLYING>_1m.csv
It fetches the last --days days, so a missed day is filled while its contracts
still trade; a day already saved is kept.

    altair-fyers-chain.timer   Mon-Fri 15:45 IST

Needs today's FYERS login. Without one it logs and exits 0, so the timer does
not show as failed for a missed login.

    ops/linux/fyers_chain.py          # dry run: the contracts it would save
    ops/linux/fyers_chain.py --go
"""

from __future__ import annotations

import argparse
import datetime as dt
import subprocess
import sys

from altair_ops import ROOT, ist_now, log, require_tools


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--go", action="store_true")
    ap.add_argument("--days", type=int, default=7)
    ap.add_argument("--strikes", type=int, default=40)
    a = ap.parse_args()

    exe = require_tools("chain", "altair_fyers_chain")["altair_fyers_chain"]
    today = ist_now().date()
    to = today.isoformat()
    frm = (today - dt.timedelta(days=a.days - 1)).isoformat()
    args = ["--from", frm, "--to", to, "--root", str(ROOT), "--strikes", str(a.strikes)]
    if a.go:
        args.append("--go")
    r = subprocess.run([str(exe), *args], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(r.stdout)
    code = r.returncode
    if code == 0:
        log("chain", "INFO", f"FYERS chains {frm} to {to} saved")
    elif code == 3:
        log("chain", "WARN", f"FYERS chains {frm} to {to}: skipped, the FYERS login has expired (log in, then rerun)")
        return 0
    elif code == 1:
        log("chain", "WARN", f"FYERS chains {frm} to {to}: nothing saved (no bars from FYERS, or every day already saved)")
    else:
        log("chain", "ERROR", f"FYERS chains {frm} to {to}: altair_fyers_chain exit {code}")
    return code


if __name__ == "__main__":
    sys.exit(main())
