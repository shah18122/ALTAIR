#!/usr/bin/env python3
"""ops/linux/record_session.py -- record the whole FYERS session, whether or not the terminal is open.

altair-recorder.timer runs this at 08:50 IST, Monday to Friday, and 90 s after
every boot, so a restart during the session resumes the recording. It:
  1. starts altair_feed_recorder, which records everything the price service
     publishes to data/store/tapes/<day>/ until --until (15:45; it waits for the feed);
  2. makes sure a FYERS feed is running on 127.0.0.1:7421. If the terminal
     already runs one, that one is recorded. Otherwise it starts
     altair_price_service --live with the RECORDER'S universe (NIFTY and
     BANKNIFTY spot, INDIA VIX, near futures, the nearest --expiries option
     expiries ATM +/- --strikes plus the nearest monthly, the NIFTY 50 and their
     futures, the 5-level book on --depth-strikes each side, the 50-level book
     on the slots the plan allows);
  3. if the feed stops (no FYERS login yet, a drop), tries again every
     5 minutes until 15:30, and says so in the activity log.

THE FYERS LOGIN IS AUTOMATIC. The altair-fyers-login timer (08:30) gets the
day's token with TOTP + PIN (broker/tools/fyers_auto_login.py), and this job
runs the same login again whenever it is waiting for a session (it does nothing
when today's session already works). Until a session exists the log says
"waiting for today's FYERS login".

    ops/linux/record_session.py          # dry run
    ops/linux/record_session.py --go
"""

from __future__ import annotations

import argparse
import os
import datetime as dt
import socket
import subprocess
import sys
import time

from altair_ops import ROOT, ist_now, log, require_tools, tail


def feed_up(port: int = 7421) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.5):
            return True
    except OSError:
        return False


def auto_login() -> None:
    """Today's FYERS login by TOTP + PIN; a no-op when the session already works."""
    script = ROOT / "broker" / "tools" / "fyers_auto_login.py"
    if not script.is_file():
        return
    try:
        subprocess.run([sys.executable, str(script), "--root", str(ROOT)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300, check=False)
    except (OSError, subprocess.TimeoutExpired) as e:
        log("record", "WARN", f"auto-login did not finish: {type(e).__name__}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--go", action="store_true")
    ap.add_argument("--until", default="15:45")
    ap.add_argument("--strikes", type=int, default=40)
    ap.add_argument("--depth-strikes", type=int, default=10)
    ap.add_argument("--expiries", type=int, default=3)
    ap.add_argument("--depth50", default="1")
    a = ap.parse_args()

    feed_args = ["--live", "--go", "--until", a.until, "--strikes", str(a.strikes),
                 "--depth-strikes", str(a.depth_strikes), "--expiries", str(a.expiries),
                 "--monthly", "--depth50", a.depth50]
    # The feed over kernel bypass (AF_XDP, ALTAIR's own TCP/TLS), set by
    # ops/linux/feed_bypass.sh in /etc/altair/altair.env; the price service
    # falls back to the kernel socket by itself if the bypass cannot start.
    if os.environ.get("ALTAIR_FEED_BYPASS") and os.environ.get("ALTAIR_FEED_BYPASS_IP"):
        feed_args += ["--bypass", os.environ["ALTAIR_FEED_BYPASS"], "--bypass-ip", os.environ["ALTAIR_FEED_BYPASS_IP"]]
    rec_args = ["--until", a.until, "--go"]
    t = require_tools("record", "altair_feed_recorder", "altair_price_service")
    recorder, service = t["altair_feed_recorder"], t["altair_price_service"]
    if not a.go:
        print("DRY RUN. With --go this runs:")
        print(f"  {recorder} {' '.join(rec_args)}")
        print(f"  {service} {' '.join(feed_args)}   (only when nothing streams on 7421)")
        return 0

    now = ist_now()
    day = now.date().isoformat()
    hh, mm = (int(x) for x in a.until.split(":"))
    end = now.replace(hour=hh, minute=mm, second=0, microsecond=0)
    last_try = now.replace(hour=15, minute=30, second=0, microsecond=0)
    if now >= end:
        log("record", "INFO", f"started after {a.until} IST; nothing to record today")
        return 0
    # The timer also starts this at boot: outside a weekday's session there is
    # nothing to do (before 08:45 the 08:50 timer starts it).
    if now.weekday() >= 5:
        log("record", "INFO", "started on a weekend; nothing to record")
        return 0
    if now < now.replace(hour=8, minute=45, second=0, microsecond=0):
        log("record", "INFO", "started before 08:45 IST; the 08:50 timer starts the session")
        return 0
    logs = ROOT / "data" / "logs"
    logs.mkdir(parents=True, exist_ok=True)

    log("record", "INFO", f"session {day}: starting the recorder (until {a.until} IST)")
    rec = subprocess.Popen([str(recorder), *rec_args], cwd=ROOT,
                           stdout=open(logs / f"recorder-{day}.out.txt", "ab"), stderr=subprocess.STDOUT)
    feed = None
    started = 0
    waiting_since: dt.datetime | None = None
    feed_out = logs / f"price_service-{day}.out.txt"
    while ist_now() < end and rec.poll() is None:
        if feed_up():
            if waiting_since is not None:
                log("record", "INFO", "the feed is streaming")
                waiting_since = None
            time.sleep(30)
            continue
        if feed is not None and feed.poll() is None:
            time.sleep(10)  # still starting
            continue
        if feed is not None:
            why = tail(feed_out)
            if any(k in why for k in ("no live source today", "no valid FYERS session", "session")):
                if waiting_since is None or (ist_now() - waiting_since).total_seconds() >= 1800:
                    log("record", "WARN", f"waiting for today's FYERS login (fyers_auto_login.py, Brokers page, or altair_fyers_login): {why}")
                    waiting_since = ist_now()
                auto_login()
            else:
                log("record", "WARN", f"the feed exited ({feed.returncode}): {why}")
            feed = None
            if ist_now() >= last_try:
                break
            time.sleep(300)
            continue
        started += 1
        log("record", "INFO", f"starting altair_price_service (attempt {started}): {' '.join(feed_args)}")
        feed = subprocess.Popen([str(service), *feed_args], cwd=ROOT,
                                stdout=open(feed_out, "ab"),
                                stderr=open(logs / f"price_service-{day}.err.txt", "ab"))
        time.sleep(15)

    code = rec.wait()
    log("record", "INFO", f"session {day} done: recorder exit {code}, feed started {started} time(s) by this job")
    return code


if __name__ == "__main__":
    sys.exit(main())
