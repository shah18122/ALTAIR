#!/usr/bin/env python3
"""Exchange a Kite request_token for an access_token.

STOPGAP for P2-10c. The C++ half computes the checksum and is NIST-verified,
but Altair has no HTTP client yet (LEDGER blocker 7, vcpkg). This script does
the one POST, using only the standard library, until that lands. Delete it
when P2-10c ships.

It lives in broker/ because broker/ is the only directory that touches a
credential -- the same rule that applies to the C++ next to it.

The secret is read from the environment and is NEVER printed, never written to
a file, and never passed on a command line where it would land in shell
history or a process listing.

Usage
-----
    setx ALTAIR_KITE_API_KEY    "your-api-key"
    setx ALTAIR_KITE_API_SECRET "your-api-secret"
    # open a NEW terminal so setx takes effect, then:

    python broker/tools/kite_login.py "<the whole redirect URL>"

The argument may be the full redirect Zerodha sent you, or just the bare
request_token -- the URL is parsed for you, because copying the whole address
bar is what actually happens.

The access_token it writes is a DAILY artifact: it expires the next morning.
That is why it goes to data/kite_session.json (gitignored) rather than into
config or an environment variable.
"""

from __future__ import annotations

import datetime
import hashlib
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

SESSION_URL = "https://api.kite.trade/session/token"
OUT_PATH = os.path.join("data", "kite_session.json")


def request_token_from(arg: str) -> str | None:
    """Accept a bare token or a full redirect URL."""
    if "request_token=" not in arg:
        return arg.strip() or None
    q = urllib.parse.urlparse(arg).query
    vals = urllib.parse.parse_qs(q).get("request_token")
    return vals[0] if vals else None


def main() -> int:
    api_key = os.environ.get("ALTAIR_KITE_API_KEY", "").strip()
    secret = os.environ.get("ALTAIR_KITE_API_SECRET", "").strip()

    if not api_key or not secret:
        print("ALTAIR_KITE_API_KEY and ALTAIR_KITE_API_SECRET must be set.")
        print("Set them with setx, then open a NEW terminal.")
        return 2
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    req_tok = request_token_from(sys.argv[1])
    if not req_tok:
        print("No request_token found in that argument.")
        return 2

    # SHA256(api_key + request_token + api_secret), lowercase hex, no separator.
    # The order is load-bearing and Kite's error does not say which part is
    # wrong -- see prompts/P2-10_kite_auth.md.
    checksum = hashlib.sha256(
        (api_key + req_tok + secret).encode()).hexdigest()

    body = urllib.parse.urlencode({
        "api_key": api_key,
        "request_token": req_tok,
        "checksum": checksum,
    }).encode()

    req = urllib.request.Request(
        SESSION_URL,
        data=body,
        headers={
            "X-Kite-Version": "3",
            "Content-Type": "application/x-www-form-urlencoded",
        },
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            payload = json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        detail = e.read().decode()
        print(f"HTTP {e.code}")
        print(detail)
        if e.code in (400, 403):
            print("\nA request_token is SINGLE USE and expires within minutes.")
            print("Open the login URL again and paste the fresh redirect.")
        return 1
    except Exception as e:                      # noqa: BLE001 - report and stop
        print("network error:", type(e).__name__, e)
        return 2

    if payload.get("status") != "success":
        print("non-success response:", json.dumps(payload)[:400])
        return 3

    d = payload["data"]
    out = {
        "user_id": d.get("user_id"),
        "user_name": d.get("user_name"),
        "broker": d.get("broker"),
        "access_token": d.get("access_token"),
        "public_token": d.get("public_token"),
        "login_time": d.get("login_time"),
        "issued_at_utc": datetime.datetime.now(datetime.timezone.utc)
                         .isoformat(timespec="seconds"),
        "exchanges": d.get("exchanges"),
        "products": d.get("products"),
        "order_types": d.get("order_types"),
    }

    os.makedirs("data", exist_ok=True)
    with open(OUT_PATH, "w", encoding="utf-8") as fh:
        json.dump(out, fh, indent=2)

    at = out["access_token"] or ""
    print("SESSION ESTABLISHED")
    print("  user_id      :", out["user_id"])
    print("  user_name    :", out["user_name"])
    print("  broker       :", out["broker"])
    print("  login_time   :", out["login_time"])
    print("  exchanges    :", ",".join(out["exchanges"] or []))
    print("  access_token :", (at[:6] + "..." + at[-4:]) if at else "(none)",
          f"({len(at)} chars)")
    print("  written to   :", OUT_PATH, " (gitignored)")
    print("\nExpires tomorrow morning. Re-run after each login.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
