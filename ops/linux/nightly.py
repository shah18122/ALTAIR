#!/usr/bin/env python3
"""ops/linux/nightly.py -- store the day, snapshot it, retrain, and push the compact data.

altair-nightly.timer runs this at 20:00 IST, Monday to Friday. Every step logs to
the activity log (data/logs/altair-<day>.log); a step that fails is logged and
the rest go on:

  1. DECODE   data/store/tapes/<day>/ -> data/store/ticks/<day>/ (1-minute bars, vol,
              chain IV) and data/store/tape_bars/ (indices, near futures).
  2. IV       altair_iv_record over the day's tapes -> data/live/iv/ (ATM IV a minute).
  3. FYERS    the day's FYERS candles (1m/5m/15m/60m/1d; spot, VIX, near futures) into
              data/store/fyers/ (laid out like dataset/), backfilling --backfill-days.
              Needs today's FYERS login; without one the step is skipped.
  4. SNAPSHOT altair_store snapshot at one IST cutoff. FYERS data only by default:
              --with-dataset opts in. Snapshots are never rebuilt over.
  5. RETRAIN  altair_forecast_curriculum on the snapshot with --leak-check, then
              altair_vol_forecast (next-move size, every timeframe), into
              data/store/runs/<day>/. Promoted to data/store/runs/LATEST.txt only if
              the leak check is clean.
  6. TRADES   the day's paper and live round trips, with their reasons, into the log.
  7. TIDY     old snapshots (runs are kept), logs past 30 days, a disk-space line.
  8. PUSH     only when config/data_push.local.toml names a data repo (push_data.py).

    ops/linux/nightly.py --go [--day 2026-10-26] [--only daily] [--ticks]
    ops/linux/nightly.py --go --quick     # 15:47 (altair-push.timer): wait for the recorder, decode, IV, push
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

from altair_ops import ROOT, ist_now, last_tuesday, log, remove_old_logs, require_tools, run_to_file, tool

TIMEFRAMES = (("1", "1m"), ("5", "5m"), ("15", "15m"), ("60", "60m"), ("D", "1d"))


def near_future(under: str, day: dt.date) -> str:
    """The near-month future's FYERS symbol: from data/live/universe.csv if it is there."""
    ds = day.isoformat()
    u = ROOT / "data" / "live" / "universe.csv"
    if u.is_file():
        with open(u, newline="") as f:
            fut = sorted((r for r in csv.DictReader(f)
                          if r.get("kind") == "future" and r.get("underlying") == under
                          and r.get("fyers", "").startswith("NSE:") and r.get("expiry", "") >= ds),
                         key=lambda r: r["expiry"])
        if fut:
            return fut[0]["fyers"]
    # The monthly expiry is the last Tuesday; the day after it, the next month is near.
    y, m = day.year, day.month
    if day > last_tuesday(y, m):
        y, m = (y + 1, 1) if m == 12 else (y, m + 1)
    mon = ("JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC")[m - 1]
    return f"NSE:{under}{y % 100:02d}{mon}FUT"


def series(day: dt.date) -> list[tuple[str, str, bool]]:
    """(key, FYERS symbol, continuous) for every price series ALTAIR keeps."""
    return [("spot/nifty", "NSE:NIFTY50-INDEX", False),
            ("spot/banknifty", "NSE:NIFTYBANK-INDEX", False),
            ("spot/indiavix", "NSE:INDIAVIX-INDEX", False),
            ("fut/nifty", near_future("NIFTY", day), True),
            ("fut/banknifty", near_future("BANKNIFTY", day), True)]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--go", action="store_true")
    ap.add_argument("--day", default="")
    ap.add_argument("--only", default="")
    ap.add_argument("--ticks", action="store_true")
    ap.add_argument("--no-retrain", action="store_true")
    # The 15:47 push (altair-push.timer): decode, IV, trades, tidy, push -- no
    # FYERS calls (altair-fyers-chain is fetching then), no snapshot (it must
    # wait for the day's FYERS candles: snapshots are never rebuilt over), no
    # retrain. The 20:00 nightly does the rest and pushes again.
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--backfill-days", type=int, default=7)
    ap.add_argument("--keep-snapshots", type=int, default=10)
    ap.add_argument("--with-dataset", action="store_true")
    a = ap.parse_args()

    day_s = a.day or ist_now().date().isoformat()
    day = dt.date.fromisoformat(day_s)
    store = ROOT / "data" / "store"
    names = ("altair_store", "altair_iv_record", "altair_fyers_history", "altair_forecast_curriculum")
    if not a.go:
        print(f"DRY RUN for {day_s}. With --go: decode, IV, FYERS candles, snapshot, retrain (leak-checked), trades, tidy, push.")
        for n in names:
            p = tool(n)
            print(f"  {n:<27} {p} {'' if p.is_file() else '(NOT BUILT)'}")
        return 0
    exe = require_tools("nightly", *names)
    log("nightly", "INFO", f"nightly {day_s} started")
    status: dict[str, int] = {}
    logs = ROOT / "data" / "logs"

    def step(name: str, prog: str, args: list[str]) -> int:
        return run_to_file(exe[prog], args, logs / f"nightly-{day_s}-{name}.txt")

    if a.quick and a.go:
        # The recorder writes its last tape until --until (15:45) and then
        # exits: wait for it (at most 10 minutes) so the push decodes a finished day.
        for _ in range(120):
            if subprocess.run(["systemctl", "is-active", "-q", "altair-recorder.service"]).returncode != 0:
                break
            time.sleep(5)
        else:
            log("nightly", "WARN", "the recorder is still running after 10 minutes; pushing what is on disk")

    # ---- 1-2. decode and IV ------------------------------------------------------
    tapes = store / "tapes" / day_s
    if tapes.is_dir():
        args = ["decode", "--day", day_s] + (["--ticks"] if a.ticks else [])
        status["decode"] = step("decode", "altair_store", args)
        status["iv"] = step("iv", "altair_iv_record", ["--tapes", str(tapes), "--out", "data/live/iv"])
        log("nightly", "INFO" if status["iv"] == 0 else "WARN", f"ATM IV from the day's tapes: exit {status['iv']}")
    else:
        log("nightly", "WARN", f"no tapes for {day_s} (holiday, or the recorder did not run): decode and IV skipped")

    # ---- 3. FYERS candles --------------------------------------------------------
    fetched = failed = 0
    refused = False
    if a.quick:
        log("nightly", "INFO", "quick run: FYERS candles, snapshot and retrain are left to the 20:00 nightly")
    for k in (range(a.backfill_days, -1, -1) if not a.quick else ()):
        if refused:
            break
        d = day - dt.timedelta(days=k)
        if d.weekday() >= 5:
            continue
        ds = d.isoformat()
        for key, sym, cont in series(day):
            for res, tf in TIMEFRAMES:
                out = store / "fyers" / key / tf / f"{ds}.csv"
                if out.is_file() and k > 0:
                    continue  # an older day already fetched stays
                out.parent.mkdir(parents=True, exist_ok=True)
                args = ["--symbol", sym, "--resolution", res, "--from", ds, "--to", ds, "--out", str(out), "--force", "--go"]
                if cont:
                    args.append("--continuous")
                if step("fyers", "altair_fyers_history", args) == 0:
                    fetched += 1
                else:
                    failed += 1
                    why = (logs / f"nightly-{day_s}-fyers.txt").read_text(errors="replace").splitlines()[-1:] or [""]
                    if any(s in why[0] for s in ("session", "authenticate", "-16", "HTTP 401", "HTTP 403")):
                        refused = True
                        break
                time.sleep(0.35)
            if refused:
                break
    if a.quick:
        pass
    elif refused:
        log("nightly", "WARN", "FYERS candles skipped: no valid FYERS session today (log in on Brokers). "
            "The snapshot uses the FYERS candles already on disk and the tapes.")
    else:
        log("nightly", "INFO" if failed == 0 else "WARN",
            f"FYERS candles: {fetched} file(s) fetched, {failed} failed or empty (holidays answer empty)")

    if not a.quick:
        # ---- 4. snapshot -------------------------------------------------------------
        snap = store / "snapshots" / day_s
        if snap.is_dir():
            log("nightly", "INFO", f"snapshot {day_s} already exists: kept (snapshots are never rebuilt over)")
            status["snapshot"] = 0
        else:
            args = ["snapshot", "--cutoff", day_s]
            if a.with_dataset:
                args += ["--source", "dataset=dataset"]
            args += ["--source", f"fyers={store / 'fyers'}", "--fill", f"tape={store / 'tape_bars'}", "--iv", "data/live/iv"]
            status["snapshot"] = step("snapshot", "altair_store", args)

        # ---- 5. retrain --------------------------------------------------------------
        manifest = snap / "_snapshot.json"
        if a.no_retrain:
            log("nightly", "INFO", "retrain skipped (--no-retrain)")
        elif status["snapshot"] != 0 or not manifest.is_file():
            log("nightly", "ERROR", "retrain skipped: no snapshot to train on")
        else:
            snap_id = json.loads(manifest.read_text()).get("id", "")
            run = store / "runs" / day_s
            run.mkdir(parents=True, exist_ok=True)
            args = ["--dataset", str(snap), "--out", str(run), "--recorded-iv", str(snap / "_inputs" / "iv"), "--leak-check"]
            if a.only:
                args += ["--only", a.only]
            log("nightly", "INFO", f"retrain on snapshot {snap_id} (cutoff {day_s}): altair_forecast_curriculum {' '.join(args)}")
            t0 = time.monotonic()
            status["retrain"] = step("retrain", "altair_forecast_curriculum", args)
            mins = int((time.monotonic() - t0) / 60)
            rev = subprocess.run(["git", "rev-parse", "--short=12", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
            (run / "run.json").write_text(json.dumps({"day": day_s, "snapshot_id": snap_id, "snapshot": str(snap),
                                                      "exit": status["retrain"], "minutes": mins, "code": rev, "args": args}, indent=2))
            if status["retrain"] == 0:
                (store / "runs" / "LATEST.txt").write_text(f"{day_s} {snap_id} {run}\n")
                log("nightly", "INFO", f"retrain done in {mins} min, leak check clean: promoted (runs/LATEST.txt)")
            elif status["retrain"] == 3:
                log("nightly", "ERROR", f"retrain: THE LEAK CHECK FAILED (see {run}/leak_check.csv). Not promoted.")
            else:
                log("nightly", "ERROR", f"retrain exited {status['retrain']} after {mins} min. Not promoted.")
            # How big the next move is, every timeframe (models/vol_forecast.hpp). Optional
            # until the binary is built on this machine.
            vf = tool("altair_vol_forecast")
            if vf.is_file():
                rc = run_to_file(vf, ["--dataset", str(snap), "--out", str(run)], run / "vol_forecast.txt")
                log("nightly", "INFO" if rc == 0 else "ERROR", f"vol forecast on snapshot {snap_id}: exit {rc} (runs/{day_s}/vol_forecast.txt)")
            else:
                log("nightly", "WARN", "vol forecast skipped: altair_vol_forecast not built (ops/build.sh app)")

    # ---- 6. the day's trades, with their reasons ---------------------------------
    for path, kind in ((ROOT / "data/live/paper/trades.csv", "paper"), (ROOT / "data/live_orders/strategy_trades.csv", "LIVE")):
        if not path.is_file():
            continue
        with open(path, newline="") as f:
            rows = [r for r in csv.DictReader(f) if r.get("date") == day_s]
        # The paper ledger takes simulator (--sim) fills too; its "source"
        # column says which. SIM rows are listed and totalled apart, so a
        # simulator run never reads as the market's result.
        net: dict[str, float] = {}
        count: dict[str, int] = {}
        for r in rows:
            label = f"{kind} SIM" if r.get("source") == "SIM" else kind
            count[label] = count.get(label, 0) + 1
            try:
                net[label] = net.get(label, 0.0) + float(r.get("net") or 0)
            except ValueError:
                net.setdefault(label, 0.0)
            log("trades", "INFO", f"{label} {r.get('model')} {r.get('side')} {r.get('symbol')} x{r.get('qty')} "
                f"{r.get('entry')}->{r.get('exit')} net {r.get('net')}; in: {r.get('why_in')}; out: {r.get('why_out')}")
        for label in sorted(count) or [kind]:
            log("trades", "INFO", f"{label}: {count.get(label, 0)} round trip(s) on {day_s}, net {net.get(label, 0.0):,.0f}")

    # ---- 7. tidy -----------------------------------------------------------------
    snaps = sorted(p for p in (store / "snapshots").glob("????-??-??") if p.is_dir()) if (store / "snapshots").is_dir() else []
    for old in snaps[:max(0, len(snaps) - a.keep_snapshots)]:
        shutil.rmtree(old, ignore_errors=True)
        log("nightly", "INFO", f"old snapshot removed: {old.name} (its run in runs/ is kept)")
    remove_old_logs(30)
    free_gb = round(shutil.disk_usage(ROOT).free / 2**30, 1)
    tape_root = store / "tapes"
    tape_gb = round(sum(p.stat().st_size for p in tape_root.rglob("*") if p.is_file()) / 2**30, 1) if tape_root.is_dir() else 0.0
    log("nightly", "WARN" if free_gb < 20 else "INFO",
        f"disk: {free_gb} GB free; tapes hold {tape_gb} GB (kept: they are the only tick history)")

    # ---- 8. push -----------------------------------------------------------------
    if (ROOT / "config" / "data_push.local.toml").is_file():
        subprocess.run([sys.executable, str(Path(__file__).with_name("push_data.py")), "--day", day_s, "--go"], cwd=ROOT)
    else:
        log("nightly", "INFO", "push skipped: no config/data_push.local.toml (see ops/recorder.md)")

    log("nightly", "INFO", f"nightly {day_s} finished: " + ", ".join(f"{k} {v}" for k, v in status.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
