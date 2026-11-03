#!/usr/bin/env python3
"""ops/linux/fetch.py -- one-off FYERS fetches and recordings, run by hand.

These were separate PowerShell scripts. Every subcommand is a DRY RUN unless
--go is given, keeps files already fetched unless --force, and needs today's
FYERS login (log in on the terminal's Brokers page first). None of them can
place an order: they call only FYERS's read-only history and data endpoints.

  history   the whole FYERS price history ALTAIR trains on, into
            data/store/fyers/<seg>/<inst>/<tf>/history.csv (the nightly job adds each new day)
  universe  daily closes for every stock in config/universe_nifty50.csv -> data/pairs/
  pairs     daily closes for every stock leg in config/pairs.csv -> data/pairs/
  meta      the FYERS history `altair_trader combos` reads, one year a file, into
            data/fyers_meta/ (--run then runs the meta-trader on it)
  depth     record 5-level depth until 15:30 IST to data/ticks/depth-<yyyymmdd>.jsonl
            for altair_depth_study
  audit     cross-check dataset/ against FYERS, then
            write data/verified/data_audit.xlsx with altair_data_audit

    ops/linux/fetch.py history --go
    ops/linux/fetch.py universe --go
    ops/linux/fetch.py meta --go --run
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import subprocess
import sys
import time
from pathlib import Path

from altair_ops import ROOT, ist_now, last_tuesday, log, require_tools
from nightly import TIMEFRAMES, series

HISTORY_FROM = {  # (intraday, daily): as far back as FYERS serves, checked 2026-10-26
    "spot/nifty": ("2018-01-01", "2005-01-01"),
    "spot/banknifty": ("2018-01-01", "2005-01-01"),
    "spot/indiavix": ("2018-01-01", "2010-01-01"),
    "fut/nifty": ("2019-01-01", "2018-01-01"),
    "fut/banknifty": ("2019-01-01", "2018-01-01"),
}


def run(exe: Path, args: list[str], quiet: bool = False) -> tuple[int, str]:
    r = subprocess.run([str(exe), *args], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if not quiet:
        print(r.stdout, end="")
    last = r.stdout.strip().splitlines()[-1:] or [""]
    return r.returncode, last[0]


def years(frm: dt.date, to: dt.date):
    for y in range(frm.year, to.year + 1):
        yield y, max(frm, dt.date(y, 1, 1)), min(to, dt.date(y, 12, 31))


def near_month_tag(today: dt.date) -> str:
    y, m = today.year, today.month
    if today > last_tuesday(y, m):
        y, m = (y + 1, 1) if m == 12 else (y, m + 1)
    return f"{y % 100:02d}" + ("JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC")[m - 1]


def instruments_future(name: str) -> dict | None:
    """The near future of `name` from data/instruments.csv (built from the FYERS masters; expiry on or after today)."""
    p = ROOT / "data" / "instruments.csv"
    if not p.is_file():
        return None
    today = dt.date.today().isoformat()
    with open(p, newline="") as f:
        rows = [r for r in csv.DictReader(f) if r.get("exchange") == "NFO" and r.get("name") == name
                and r.get("instrument_type") == "FUT" and r.get("expiry", "") >= today]
    return min(rows, key=lambda r: r["expiry"]) if rows else None


def cmd_history(a) -> int:
    exe = require_tools("history", "altair_fyers_history")["altair_fyers_history"]
    to = a.to or (ist_now().date() - dt.timedelta(days=1)).isoformat()
    ok = bad = 0
    for key, sym, cont in series(dt.date.fromisoformat(to)):
        for res, tf in TIMEFRAMES:
            start = HISTORY_FROM[key][1 if res == "D" else 0]
            out = ROOT / "data/store/fyers" / key / tf / "history.csv"
            if out.is_file() and not a.force:
                print(f"kept {out}")
                continue
            args = ["--symbol", sym, "--resolution", res, "--from", start, "--to", to, "--out", str(out), "--force"]
            if cont:
                args.append("--continuous")
            if not a.go:
                print(f"would fetch: {exe} {' '.join(args)}")
                continue
            out.parent.mkdir(parents=True, exist_ok=True)
            code, _ = run(exe, args + ["--go"])
            if code == 0 and out.is_file():
                ok += 1
                n = sum(1 for _ in open(out))
                log("history", "INFO", f"{sym} {tf} {start}..{to} -> {out} ({n} lines)")
            else:
                bad += 1
                log("history", "WARN", f"{sym} {tf} {start}..{to} failed (exit {code})")
    if not a.go:
        print("\nDry run. Add --go to fetch.")
        return 0
    log("history", "INFO" if bad == 0 else "WARN", f"FYERS history: {ok} series fetched, {bad} failed")
    return 0 if ok > 0 else 1


def daily_closes(a, legs: dict[str, str]) -> int:
    exe = require_tools("history", "altair_fyers_history")["altair_fyers_history"]
    failed = 0
    for symbol in sorted(legs):
        d = ROOT / legs[symbol]
        d.mkdir(parents=True, exist_ok=True)
        out = d / "fyers.csv"
        args = ["--symbol", symbol, "--resolution", "D", "--from", a.frm, "--to", a.to or dt.date.today().isoformat(), "--out", str(out)]
        if a.force:
            args.append("--force")
        if a.go:
            args.append("--go")
        print(f"{symbol} -> {out}")
        code, _ = run(exe, args)
        if code != 0:
            print(f"  failed ({code}): {symbol}")
            failed += 1
        if a.go:
            time.sleep(0.4)
    if not a.go:
        print("\nDry run. Pass --go to fetch.")
    if failed:
        print(f"{failed} symbol(s) failed")
        return 1
    return 0


def cmd_universe(a) -> int:
    legs = {}
    for line in (ROOT / "config/universe_nifty50.csv").read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#") or line.startswith("symbol,"):
            continue
        f = line.split(",")
        if len(f) >= 4:
            legs[f[1]] = f[3]
    return daily_closes(a, legs)


def cmd_pairs(a) -> int:
    # pair,leg_a,a_fyers,a_dir,leg_b,b_fyers,b_dir,ratio_cap
    legs = {}
    for line in (ROOT / "config/pairs.csv").read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        f = line.split(",")
        if len(f) < 7:
            continue
        for sym, d in ((f[2], f[3]), (f[5], f[6])):
            if not d.startswith("dataset/"):  # tracked already
                legs[sym] = d
    return daily_closes(a, legs)


def cmd_meta(a) -> int:
    exe = require_tools("meta", "altair_fyers_history")["altair_fyers_history"]
    out = ROOT / "data/fyers_meta"
    out.mkdir(parents=True, exist_ok=True)
    logf = out / "fetch_log.txt"

    def note(m: str) -> None:
        line = f"{dt.datetime.now():%Y-%m-%d %H:%M:%S}  {m}"
        print(line)
        with open(logf, "a") as f:
            f.write(line + "\n")

    todo = [("NIFTY/1m", "NSE:NIFTY50-INDEX", "1", False), ("BANKNIFTY/1m", "NSE:NIFTYBANK-INDEX", "1", False),
            ("INDIAVIX/1d", "NSE:INDIAVIX-INDEX", "D", False), ("INDIAVIX/1m", "NSE:INDIAVIX-INDEX", "1", False)]
    for n in ("NIFTY", "BANKNIFTY"):
        fut = instruments_future(n)
        if fut:
            todo.append((f"{n}_FUT/1m", "NSE:" + fut["tradingsymbol"], "1", True))
        else:
            note(f"no current {n} future in data/instruments.csv: {n}_FUT skipped (the index price is used)")
    frm = dt.date.fromisoformat(a.frm)
    to = dt.date.fromisoformat(a.to) if a.to else dt.date.today()
    failed = 0
    for sub, sym, res, cont in todo:
        d = out / sub
        d.mkdir(parents=True, exist_ok=True)
        for y, s, e in years(frm, to):
            f = d / f"{y}.csv"
            if f.is_file() and not a.force and y < to.year:
                continue
            args = ["--symbol", sym, "--resolution", res, "--from", s.isoformat(), "--to", e.isoformat(), "--out", str(f), "--force"]
            if cont:
                args.append("--continuous")
            if a.go:
                args.append("--go")
            code, last = run(exe, args, quiet=True)
            if code == 0:
                note(f"ok      {sym} {res} {y}")
            else:
                note(f"skipped {sym} {res} {y} -- {last}")
                failed += 1
    if not a.go:
        print("DRY RUN: nothing fetched. Add --go.")
        return 0
    (out / "SOURCE.txt").write_text(f"FYERS history API, fetched {dt.datetime.now():%Y-%m-%d %H:%M} by ops/linux/fetch.py meta\n")
    note(f"done, {failed} request files skipped")
    if a.run:
        trader = require_tools("meta", "altair_trader")["altair_trader"]
        args = ["combos", "--fyers", "data/fyers_meta", "--leak-check"]
        if a.with_curriculum:
            args += ["--logs", "data/verified/forecast_log"]
        if a.unverified_costs:
            args.append("--unverified-costs")
        if subprocess.run([str(trader), *args], cwd=ROOT).returncode != 0:
            print("altair_trader combos failed")
            return 1
        print("Report: data/verified/altair_trader/combos/report.html")
    return 0


def cmd_depth(a) -> int:
    exe = require_tools("depth", "altair_fyers_ticker")["altair_fyers_ticker"]
    now = ist_now()
    tag = near_month_tag(now.date())
    symbols = a.symbols or ",".join([f"NSE:NIFTY{tag}FUT", f"NSE:BANKNIFTY{tag}FUT", "NSE:RELIANCE-EQ",
                                     "NSE:HDFCBANK-EQ", "NSE:ICICIBANK-EQ", "NSE:INFY-EQ", "NSE:SBIN-EQ"])
    close = now.replace(hour=15, minute=30, second=0, microsecond=0)
    seconds = max(60, int((close - now).total_seconds()))
    if now > close:
        seconds = 60
        print("after 15:30 IST: recording 60 s only (a smoke test)")
    out = ROOT / "data/ticks"
    out.mkdir(parents=True, exist_ok=True)
    f = out / f"depth-{now:%Y%m%d}.jsonl"
    args = ["--symbols", symbols, "--depth", "--jsonl", str(f), "--stamp", "--seconds", str(seconds),
            "--out", "data/fyers_ticks.json"]
    if a.go:
        args.append("--go")
    print(f"symbols : {symbols}\nuntil   : 15:30 IST ({seconds} s)\nfile    : {f}")
    return subprocess.run([str(exe), *args], cwd=ROOT).returncode


def cmd_audit(a) -> int:
    base = ROOT / "data/broker_audit"
    base.mkdir(parents=True, exist_ok=True)
    logf = base / "fetch_log.txt"

    def note(m: str) -> None:
        line = f"{dt.datetime.now():%Y-%m-%d %H:%M:%S}  {m}"
        print(line)
        with open(logf, "a") as f:
            f.write(line + "\n")

    frm = dt.date.fromisoformat(a.frm).replace(day=1)  # windows start on the 1st of a month
    to = dt.date.fromisoformat(a.to) if a.to else dt.date.today()
    indices = [("spot", "nifty", "NSE:NIFTY50-INDEX"), ("spot", "banknifty", "NSE:NIFTYBANK-INDEX"),
               ("spot", "indiavix", "NSE:INDIAVIX-INDEX")]
    tfs = [("1m", "1"), ("5m", "5"), ("15m", "15"), ("60m", "60"), ("1d", "D")]
    fut = instruments_future("NIFTY")
    if not fut:
        note("no current NIFTY future in data/instruments.csv; futures skipped")
    if not a.audit_only:
        fy = require_tools("audit", "altair_fyers_history")["altair_fyers_history"]

        def get_fyers(seg, inst, sym, tf, fres, cont):
            d = base / "fyers" / seg / inst / tf
            d.mkdir(parents=True, exist_ok=True)
            for y, s, e in years(frm, to):
                out = d / f"{y}.csv"
                if out.is_file() and not a.force:
                    continue
                args = ["--symbol", sym, "--resolution", fres, "--from", s.isoformat(), "--to", e.isoformat(),
                        "--out", str(out), "--force", "--go"] + (["--continuous"] if cont else [])
                code, last = run(fy, args, quiet=True)
                note(f"ok      FYERS {sym} {tf} {y}" if code == 0 else f"skipped FYERS {sym} {tf} {y} -- {last}")

        for seg, inst, sym in indices:
            for tf, fres in tfs:
                get_fyers(seg, inst, sym, tf, fres, False)
        if fut:
            for tf, fres in tfs:
                get_fyers("fut", "nifty", "NSE:" + fut["tradingsymbol"], tf, fres, True)
    audit = require_tools("audit", "altair_data_audit")["altair_data_audit"]
    args = ["--dataset", "dataset", "--out", "data/verified"]
    if (base / "fyers").is_dir():
        args += ["--broker", "FYERS=data/broker_audit/fyers"]
    if subprocess.run([str(audit), *args], cwd=ROOT).returncode != 0:
        print("altair_data_audit failed")
        return 1
    print("\nReport: data/verified/data_audit.xlsx   Merged series: data/verified/merged/")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p, frm="2015-01-01"):
        p.add_argument("--go", action="store_true")
        p.add_argument("--force", action="store_true")
        p.add_argument("--from", dest="frm", default=frm)
        p.add_argument("--to", default="")
        return p

    common(sub.add_parser("history"))
    common(sub.add_parser("universe"))
    common(sub.add_parser("pairs"))
    m = common(sub.add_parser("meta"), "2018-01-01")
    m.add_argument("--run", action="store_true")
    m.add_argument("--with-curriculum", action="store_true")
    m.add_argument("--unverified-costs", action="store_true")
    d = sub.add_parser("depth")
    d.add_argument("--go", action="store_true")
    d.add_argument("--symbols", default="")
    au = common(sub.add_parser("audit"))
    au.add_argument("--audit-only", action="store_true")
    a = ap.parse_args()
    return {"history": cmd_history, "universe": cmd_universe, "pairs": cmd_pairs, "meta": cmd_meta,
            "depth": cmd_depth, "audit": cmd_audit}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
