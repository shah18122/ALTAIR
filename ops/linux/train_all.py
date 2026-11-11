#!/usr/bin/env python3
"""ops/linux/train_all.py -- train every model after the nightly retrain. No AI, no tokens.

nightly.py runs this at the end of its retrain step (altair-nightly.timer, 20:00
IST, Monday to Friday), after altair_forecast_curriculum and altair_vol_forecast
have written data/store/runs/<day>/. It runs every other trainer and
walk-forward study, each on FYERS data only (docs/DATA_POLICY.md), each with a
time limit, each to its own folder under data/store/runs/<day>/train_all/:

  trader        altair_trader on the curriculum's forecast logs (leak-checked)
  combos        altair_trader combos (the meta-trader) on data/fyers_meta/
  magnitude     altair_magnitude on the curriculum's forecast logs
  regimes_*     altair_regimes: HMM stress probability, NIFTY and BANKNIFTY daily
  as_mm_*       altair_as_mm: Avellaneda-Stoikov on 1m futures, with the regimes
  band_demo     altair_band_option_demo: the 80 % band sellers
  vol_premium   altair_vol_premium on the saved FYERS chains
  tune          altair_tune: every event-driven strategy on the session tapes;
                a setting passes its guard -> config/model_params/<key>.toml,
                which the engine loads at its next start (--no-tune-write: dry run)
  obi_select    altair_obi_select: today's OBI ranking (no --write)
  paper_report  altair_paper_report: what the paper book earned, per model
  tick_forecast altair_tick_forecast on data/drive-ticks/ (source drive-ticks,
                NOT FYERS; only when that folder exists)

A job whose input is missing is skipped and says why; a job that fails is
logged and the rest go on. summary.json and summary.md say what ran, how long,
and how it ended; push_data.py sends them (and each job's last lines) to the
private data repo. It refuses to start in market hours (09:00-15:45 IST on a
weekday) unless --force, and every job ends by --stop-at (05:30 IST), leaving the
nightly's push time before altair-nightly.service's 10-hour limit.

    ops/linux/train_all.py                 # dry run: what would run, and what is missing
    ops/linux/train_all.py --go [--day 2026-11-11] [--only trader,regimes] [--force]
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import subprocess
import sys
import time
from pathlib import Path

from altair_ops import ROOT, ist_now, log, tail, tool

STORE = ROOT / "data" / "store"


def newest_snapshot(day: str) -> Path | None:
    snaps = STORE / "snapshots"
    if (snaps / day / "_snapshot.json").is_file():
        return snaps / day
    found = sorted(p for p in snaps.glob("????-??-??") if (p / "_snapshot.json").is_file() and p.name <= day) \
        if snaps.is_dir() else []
    return found[-1] if found else None


def jobs(day: str, run: Path, snap: Path | None, tune_write: bool, ticks: Path) -> list[dict]:
    """Every job: name, tool, args, the inputs it needs, minutes it may take."""
    out = run / "train_all"
    logs = run / "forecast_log"
    fy = STORE / "fyers"
    s = str(snap) if snap else ""
    j: list[dict] = [
        dict(name="trader", tool="altair_trader", minutes=120, needs=[logs, snap],
             args=["--logs", str(logs), "--dataset", s, "--out", str(out / "trader"), "--leak-check"]
             + (["--recorded-iv", str(snap / "_inputs" / "iv")] if snap and (snap / "_inputs" / "iv").is_dir() else [])),
        dict(name="combos", tool="altair_trader", minutes=120, needs=[ROOT / "data" / "fyers_meta" / "SOURCE.txt", logs],
             args=["combos", "--fyers", str(ROOT / "data" / "fyers_meta"), "--logs", str(logs),
                   "--out", str(out / "combos"), "--leak-check"]),
        dict(name="magnitude", tool="altair_magnitude", minutes=30, needs=[logs, snap],
             args=["--logs", str(logs), "--dataset", s, "--out", str(out / "magnitude")]),
    ]
    for under, lot in (("nifty", 65), ("banknifty", 30)):
        reg = out / "regimes" / f"{under}.csv"
        j.append(dict(name=f"regimes_{under}", tool="altair_regimes", minutes=20, needs=[fy / "spot" / under / "1d"],
                      args=["--fyers", str(fy / "spot" / under / "1d"), "--tf", "1d", "--out", str(reg), "--leak-check"]))
        j.append(dict(name=f"as_mm_{under}", tool="altair_as_mm", minutes=60, needs=[fy / "fut" / under / "1m"],
                      args=["--fyers", str(fy / "fut" / under / "1m"), "--out", str(out / "as_mm" / under),
                            "--lot", str(lot), "--leak-check"], regimes=reg))
    j += [
        dict(name="band_demo", tool="altair_band_option_demo", minutes=60, needs=[snap],
             args=["--dataset", s, "--out", str(out)]),
        dict(name="vol_premium", tool="altair_vol_premium", minutes=30, needs=[STORE / "fyers_chain", snap],
             args=["--dataset", s, "--out", str(out / "vol_premium")]),
        dict(name="tune", tool="altair_tune", minutes=180, needs=[STORE / "tapes"],
             args=["--strategy", "all"] + ([] if tune_write else ["--dry-run"])),
        dict(name="obi_select", tool="altair_obi_select", minutes=20, needs=[STORE / "tapes" / day],
             args=["--day", day]),
        dict(name="paper_report", tool="altair_paper_report", minutes=20, needs=[ROOT / "data" / "live" / "paper"],
             args=["--out", str(out / "paper_report")]),
        dict(name="tick_forecast", tool="altair_tick_forecast", minutes=240, needs=[ticks],
             args=["--ticks", str(ticks), "--out", str(out / "tick_forecast")]),
    ]
    return j


def market_hours(now: dt.datetime) -> bool:
    return now.weekday() < 5 and dt.time(9, 0) <= now.time() < dt.time(15, 45)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--go", action="store_true")
    ap.add_argument("--day", default="")
    ap.add_argument("--only", default="", help="comma-separated job names (prefix match)")
    ap.add_argument("--force", action="store_true", help="run even in market hours")
    ap.add_argument("--no-tune-write", action="store_true", help="altair_tune --dry-run: write no model_params")
    ap.add_argument("--ticks-dir", default=str(ROOT / "data" / "drive-ticks"), help="Drive tick CSVs (NOT FYERS)")
    ap.add_argument("--stop-at", default="05:30", help="IST, next morning: every job ends by this")
    a = ap.parse_args()

    now = ist_now()
    day = a.day or now.date().isoformat()
    run = STORE / "runs" / day
    snap = newest_snapshot(day)
    todo = jobs(day, run, snap, not a.no_tune_write, Path(a.ticks_dir))
    if a.only:
        keep = [k.strip() for k in a.only.split(",") if k.strip()]
        todo = [t for t in todo if any(t["name"].startswith(k) for k in keep)]

    def missing(t: dict) -> str:
        if not tool(t["tool"]).is_file():
            return f"{t['tool']} not built (ops/build.sh app)"
        for n in t["needs"]:
            if n is None:
                return "no FYERS snapshot yet"
            if not Path(n).exists():
                return f"no {Path(n).relative_to(ROOT) if Path(n).is_relative_to(ROOT) else n}"
        return ""

    if not a.go:
        print(f"DRY RUN for {day} (snapshot {snap.name if snap else 'none'}). With --go these run:")
        for t in todo:
            why = missing(t)
            print(f"  {t['name']:<15} {'SKIP: ' + why if why else ' '.join([t['tool']] + t['args'])}")
        return 0
    if market_hours(now) and not a.force:
        log("train_all", "WARN", "refused: market hours (the recorder and feed come first); --force to override")
        return 2

    out = run / "train_all"
    out.mkdir(parents=True, exist_ok=True)
    stop_h, stop_m = (int(x) for x in a.stop_at.split(":"))
    deadline = dt.datetime.combine(dt.date.fromisoformat(day) + dt.timedelta(days=1), dt.time(stop_h, stop_m), now.tzinfo)
    if now.date().isoformat() > day:   # a by-hand rerun of an older day: no overnight limit
        deadline = now + dt.timedelta(hours=10)
    log("train_all", "INFO", f"train_all {day} started: {len(todo)} job(s), snapshot {snap.name if snap else 'none'}")
    results = []
    for t in todo:
        r = {"job": t["name"], "tool": t["tool"], "args": t["args"]}
        why = missing(t)
        left = (deadline - ist_now()).total_seconds()
        if not why and left < 60:
            why = f"past --stop-at {a.stop_at} IST"
        if why:
            r.update(status="skipped", why=why)
            log("train_all", "INFO", f"{t['name']}: skipped ({why})")
            results.append(r)
            continue
        args = list(t["args"])
        if t.get("regimes") and Path(t["regimes"]).is_file():
            args += ["--regimes", str(t["regimes"])]
        txt = out / f"{t['name']}.txt"
        t0 = time.monotonic()
        try:
            with open(txt, "wb") as o, open(str(txt) + ".err", "wb") as e:
                rc = subprocess.run([str(tool(t["tool"])), *args], stdout=o, stderr=e, cwd=ROOT,
                                    timeout=min(t["minutes"] * 60, left)).returncode
        except subprocess.TimeoutExpired:
            rc = -9
        secs = round(time.monotonic() - t0)
        status = "ok" if rc == 0 else ("timeout" if rc == -9 else ("LEAK" if rc == 3 else "failed"))
        r.update(status=status, exit=rc, seconds=secs, last=tail(txt, 3))
        log("train_all", "INFO" if rc == 0 else "ERROR",
            f"{t['name']}: {status} (exit {rc}) in {secs // 60} min {secs % 60} s. {r['last'][:300]}")
        results.append(r)

    ok = sum(r["status"] == "ok" for r in results)
    (out / "summary.json").write_text(json.dumps({"day": day, "snapshot": str(snap) if snap else None,
                                                  "finished": ist_now().isoformat(timespec="seconds"),
                                                  "ok": ok, "jobs": results}, indent=2))
    lines = [f"# Training {day}", "", f"{ok} of {len(results)} jobs ok. Snapshot: {snap.name if snap else 'none'}.", "",
             "| job | status | time | last lines |", "|---|---|---|---|"]
    for r in results:
        t = f"{r['seconds'] // 60}m{r['seconds'] % 60:02d}s" if "seconds" in r else ""
        note = r.get("why") or r.get("last", "")
        lines.append(f"| {r['job']} | {r['status']} | {t} | {note.replace('|', '/')[:200]} |")
    (out / "summary.md").write_text("\n".join(lines) + "\n")
    log("train_all", "INFO", f"train_all {day} finished: {ok}/{len(results)} ok (runs/{day}/train_all/summary.md)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
