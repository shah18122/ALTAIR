#!/usr/bin/env python3
"""ops/linux/push_data.py -- send the day's compact data to the PRIVATE data repo.

nightly.py runs this last, and only when config/data_push.local.toml
(git-ignored) names the local clone of the data repo:

    repo = '/srv/altair-data'

The clone must have an empty .altair-data-repo file at its root. This script
refuses any other folder, and refuses a clone whose origin is the public ALTAIR repo.

WHAT GOES, for the one day: bars/<yyyy>/<day>.csv.gz, candles/<seg>/<inst>/<tf>/<day>.csv,
iv/<INDEX>/<day>.csv, vol/<yyyy>/<day>.csv.gz, chain_iv/<yyyy>/<day>.csv.gz,
chain_snap/<day>/<HHMM>.csv.gz, reports/<day>/ (decode, snapshot manifest, conflicts,
run.json, forecast_curriculum.txt, leak_check.csv) and logs/altair-<day>.log.
WHAT NEVER GOES: tapes, anything under data/live/ other than the IV, and session
or credential files of any kind. Every file is checked again before the commit,
by name and by content.

It pushes with the server's own git login (a deploy key for the data repo only);
ALTAIR stores no token.

    ops/linux/push_data.py --day 2026-11-03 [--go]
"""

from __future__ import annotations

import argparse
import gzip
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

from altair_ops import ROOT, log

NAME_FENCE = re.compile(r"(?i)session|secret|token|credential|\.env|api_key|password")
CONTENT_FENCE = re.compile(r'(?i)"?(access_token|refresh_token|client_secret|secret_key)"?\s*[:=]\s*"?[A-Za-z0-9._-]{12,}')


def repo_from_config(cfg: Path) -> str | None:
    for line in cfg.read_text().splitlines():
        m = re.match(r"""^\s*repo\s*=\s*['"](.+)['"]\s*$""", line)
        if m:
            return m.group(1)
    return None


def public_origin(origin: str) -> bool:
    return not origin or re.search(r"(?i)shah18122/altair(\.git)?$", origin) is not None


def git(repo: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--day", required=True)
    ap.add_argument("--go", action="store_true")
    a = ap.parse_args()
    day = a.day

    cfg = ROOT / "config" / "data_push.local.toml"
    if not cfg.is_file():
        log("push", "WARN", f"no {cfg}: nothing pushed")
        return 0
    r = repo_from_config(cfg)
    if not r:
        log("push", "ERROR", f"{cfg} has no repo = '...' line")
        return 2
    repo = Path(r)
    if not (repo / ".altair-data-repo").is_file():
        log("push", "ERROR", f"{repo} is not a data repo (no .altair-data-repo marker): nothing pushed")
        return 2
    origin = git(repo, "remote", "get-url", "origin").stdout.strip()
    if public_origin(origin):
        log("push", "ERROR", f"refusing to push: {repo}'s origin is '{origin}' (must be the private data repo)")
        return 2

    copied: list[str] = []

    def gz(src: Path, rel: str) -> None:
        dst = repo / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        with open(src, "rb") as i, gzip.open(dst, "wb", compresslevel=9) as o:
            shutil.copyfileobj(i, o)
        copied.append(rel)

    def plain(src: Path, rel: str) -> None:
        dst = repo / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
        copied.append(rel)

    yyyy = day[:4]
    s = ROOT / "data" / "store"
    ticks = s / "ticks" / day
    for name, kind in (("bars_1m.csv", "bars"), ("vol_1m.csv", "vol"), ("chain_iv_1m.csv", "chain_iv")):
        if (ticks / name).is_file():
            gz(ticks / name, f"{kind}/{yyyy}/{day}.csv.gz")
    snapdir = s / "chain_snap" / day
    if snapdir.is_dir():
        for f in sorted(snapdir.glob("*.csv")):
            gz(f, f"chain_snap/{day}/{f.name}.gz")
    if (ticks / "_decode.json").is_file():
        plain(ticks / "_decode.json", f"reports/{day}/decode.json")
    fy = s / "fyers"
    if fy.is_dir():
        for f in sorted(fy.rglob(f"{day}.csv")):
            plain(f, f"candles/{f.relative_to(fy).as_posix()}")
    iv = ROOT / "data" / "live" / "iv"
    if iv.is_dir():
        for f in sorted(iv.rglob(f"{day}.csv")):
            plain(f, f"iv/{f.parent.name}/{day}.csv")
    snap = s / "snapshots" / day
    if (snap / "_snapshot.json").is_file():
        plain(snap / "_snapshot.json", f"reports/{day}/snapshot.json")
    if (snap / "_conflicts.csv").is_file():
        gz(snap / "_conflicts.csv", f"reports/{day}/conflicts.csv.gz")
    for f in ("run.json", "forecast_curriculum.txt", "leak_check.csv"):
        if (s / "runs" / day / f).is_file():
            plain(s / "runs" / day / f, f"reports/{day}/{f}")
    lg = ROOT / "data" / "logs" / f"altair-{day}.log"
    if lg.is_file():
        plain(lg, f"logs/altair-{day}.log")

    if not copied:
        log("push", "WARN", f"nothing to push for {day}")
        return 0

    # The second fence: no credential by name or by content, whatever was copied.
    for rel in copied:
        if NAME_FENCE.search(rel):
            log("push", "ERROR", f"refusing to push: {rel} has a credential-like name")
            return 3
        p = repo / rel
        text = gzip.open(p, "rt", errors="replace").read() if rel.endswith(".gz") else p.read_text(errors="replace")
        if CONTENT_FENCE.search(text):
            p.unlink()
            log("push", "ERROR", f"refusing to push: {rel} contains a credential-like value (file removed from the data repo)")
            return 3

    if not a.go:
        print(f"DRY RUN: {len(copied)} file(s) copied into {repo}, nothing committed")
        return 0
    git(repo, "add", "--", *copied)
    if git(repo, "diff", "--cached", "--quiet").returncode == 0:
        log("push", "INFO", f"data {day} already pushed: nothing new")
        return 0
    git(repo, "commit", "-q", "-m", f"data {day}")
    for wait in (0, 2, 4, 8, 16):
        time.sleep(wait)
        if git(repo, "push", "-q", "origin", "HEAD").returncode == 0:
            log("push", "INFO", f"pushed {len(copied)} file(s) for {day} to {origin}")
            return 0
    log("push", "ERROR", f"push for {day} failed after 5 tries (committed locally; tomorrow's push carries it)")
    return 4


if __name__ == "__main__":
    sys.exit(main())
