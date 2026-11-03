"""ops/linux/altair_ops.py -- what every unattended ALTAIR job on Linux shares.

The Linux counterparts of the old PowerShell jobs (record_session, chain_snapshot,
fyers_chain, nightly, push_data) import this. Python 3 standard library only, so
a stock Ubuntu 24.04 server needs nothing installed for them.

THE ACTIVITY LOG. Same file and the same rules as app/activity_log.hpp:
data/logs/altair-YYYY-MM-DD.log (IST day), one line an event,
"2026-10-26T20:00:01.123+05:30 [nightly] INFO ...". Each line is ONE write()
on an O_APPEND descriptor, which POSIX makes atomic against the C++ writers,
so lines never interleave. Credential-looking values are masked first.

IST is UTC+05:30 all year (no daylight saving), so no time-zone database is used.
"""

from __future__ import annotations

import datetime as _dt
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
IST = _dt.timezone(_dt.timedelta(hours=5, minutes=30))


def bin_dir() -> Path:
    """Where the built tools live: $ALTAIR_BIN, else build/linux/app (ops/build.sh)."""
    env = os.environ.get("ALTAIR_BIN")
    return Path(env) if env else ROOT / "build" / "linux" / "app"


def tool(name: str) -> Path:
    return bin_dir() / name


def ist_now() -> _dt.datetime:
    return _dt.datetime.now(IST)


_KEYS = (r"bearer", r"access_token|refresh_token|secret|password|pin|authorization|auth_code|api_key")


def hide_secrets(text: str) -> str:
    """The key, any of ' \":=' after it, then the value up to a space, comma, quote or brace.

    "Bearer" goes first, so "Authorization: Bearer X" masks X and not only the word.
    """
    t = re.sub(r"[\r\n]+", " ", text)
    for keys in _KEYS:
        t = re.sub(r"(?i)\b(" + keys + r")\b([\s\"':=]+)(?!<redacted>)([^\s,\"}]+)", r"\1\2<redacted>", t)
    return t


def log(component: str, level: str, message: str) -> None:
    if level not in ("INFO", "WARN", "ERROR"):
        raise ValueError(level)
    now = ist_now()
    d = ROOT / "data" / "logs"
    d.mkdir(parents=True, exist_ok=True)
    f = d / f"altair-{now:%Y-%m-%d}.log"
    stamp = now.strftime("%Y-%m-%dT%H:%M:%S.") + f"{now.microsecond // 1000:03d}+05:30"
    line = f"{stamp} [{component}] {level} {hide_secrets(message)}\n"
    fd = os.open(f, os.O_CREAT | os.O_WRONLY | os.O_APPEND, 0o644)
    try:
        os.write(fd, line.encode("utf-8"))
    finally:
        os.close(fd)
    print(line.rstrip(), flush=True)


def remove_old_logs(keep_days: int = 30) -> None:
    d = ROOT / "data" / "logs"
    if not d.is_dir():
        return
    oldest = (ist_now().date() - _dt.timedelta(days=keep_days)).isoformat()
    for p in d.glob("altair-????-??-??.log"):
        if p.name[7:17] < oldest:
            try:
                p.unlink()
            except OSError:
                pass


def require_tools(component: str, *names: str) -> dict[str, Path]:
    """The tools' paths; logs and exits 1 if one is not built."""
    out = {}
    for n in names:
        p = tool(n)
        if not p.is_file():
            log(component, "ERROR", f"no {p} -- build it first: ops/build.sh")
            sys.exit(1)
        out[n] = p
    return out


def run_to_file(exe: Path, args: list[str], out: Path) -> int:
    """Run a tool to completion, stdout to `out` and stderr to `out`.err; its exit code."""
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "wb") as o, open(str(out) + ".err", "wb") as e:
        return subprocess.run([str(exe), *args], stdout=o, stderr=e, cwd=ROOT).returncode


def tail(path: Path, n: int = 3) -> str:
    try:
        lines = path.read_text(errors="replace").splitlines()
    except OSError:
        return ""
    return " | ".join(lines[-n:])


def last_tuesday(year: int, month: int) -> _dt.date:
    nxt = _dt.date(year + (month == 12), month % 12 + 1, 1)
    d = nxt - _dt.timedelta(days=1)
    while d.weekday() != 1:
        d -= _dt.timedelta(days=1)
    return d
