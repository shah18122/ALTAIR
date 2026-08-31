#!/usr/bin/env python3
"""A localhost callback server that captures a Kite request_token.

Run it, open the login URL it prints, log in, and it captures the redirect,
exchanges the token, and writes data/kite_session.json. Then it stops.

    python broker/tools/kite_callback.py

BEFORE THIS WORKS you must change the redirect URL registered on your Kite app
to exactly:

    http://127.0.0.1:53123/zerodha/callback

Kite redirects to the URL registered on the app, not to whatever we ask for.
While it points at https://altair.thesmitshah.com/zerodha/callback, the browser
goes there and this server never sees anything. The script says so on startup
rather than sitting silent.

SECURITY
--------
  * Binds 127.0.0.1 only. Never 0.0.0.0 -- a request_token arriving on a
    LAN-visible port is a request_token anyone on the LAN can take.
  * The secret comes from the environment. Never printed, never written to a
    file, never on a command line.
  * Serves exactly one path and shuts down after one successful capture. A
    callback listener that outlives the login is a listener nobody is watching.
  * The access_token is written to data/kite_session.json (gitignored) and is
    a LIVE TRADING CREDENTIAL until tomorrow morning. Treat the file the way
    you treat the secret.
"""

from __future__ import annotations

import datetime
import hashlib
import http.server
import json
import os
import socket
import sys
import urllib.error
import urllib.parse
import urllib.request

HOST = "127.0.0.1"
PORT = 53123
CALLBACK_PATH = "/zerodha/callback"
REDIRECT_URL = f"http://{HOST}:{PORT}{CALLBACK_PATH}"
SESSION_URL = "https://api.kite.trade/session/token"
OUT_PATH = os.path.join("data", "kite_session.json")

_result: dict = {}


def exchange(api_key: str, secret: str, req_tok: str) -> tuple[bool, str]:
    """Swap a request_token for an access_token. Returns (ok, message)."""
    checksum = hashlib.sha256((api_key + req_tok + secret).encode()).hexdigest()
    body = urllib.parse.urlencode({
        "api_key": api_key,
        "request_token": req_tok,
        "checksum": checksum,
    }).encode()
    req = urllib.request.Request(
        SESSION_URL, data=body,
        headers={"X-Kite-Version": "3",
                 "Content-Type": "application/x-www-form-urlencoded"},
        method="POST")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            payload = json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        detail = e.read().decode()[:300]
        hint = ""
        if e.code in (400, 403):
            hint = ("\nA request_token is SINGLE USE and expires within "
                    "minutes. Log in again.")
        return False, f"HTTP {e.code}: {detail}{hint}"
    except Exception as e:                      # noqa: BLE001
        return False, f"network error: {type(e).__name__}: {e}"

    if payload.get("status") != "success":
        return False, f"non-success: {json.dumps(payload)[:300]}"

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
    _result.update(out)
    at = out["access_token"] or ""
    return True, (f"user {out['user_id']} ({out['user_name']}), "
                  f"token {at[:6]}...{at[-4:]} ({len(at)} chars)")


class Handler(http.server.BaseHTTPRequestHandler):
    # Silence the default per-request logging; we print what matters ourselves.
    def log_message(self, fmt, *args):  # noqa: A003
        pass

    def _reply(self, code: int, title: str, body: str) -> None:
        html = (f"<!doctype html><meta charset=utf-8>"
                f"<title>{title}</title>"
                f"<body style='font:15px system-ui;padding:3rem;max-width:40rem'>"
                f"<h2>{title}</h2><p>{body}</p></body>")
        raw = html.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self) -> None:                    # noqa: N802
        parts = urllib.parse.urlparse(self.path)
        if parts.path != CALLBACK_PATH:
            self._reply(404, "Not here",
                        f"This server answers only <code>{CALLBACK_PATH}</code>.")
            return

        q = urllib.parse.parse_qs(parts.query)
        status = (q.get("status") or [""])[0]
        req_tok = (q.get("request_token") or [""])[0]

        if status and status != "success":
            self._reply(400, "Login did not succeed",
                        f"Kite reported status <code>{status}</code>.")
            print(f"  login reported status={status}")
            return
        if not req_tok:
            self._reply(400, "No request_token",
                        "The redirect carried no <code>request_token</code>.")
            print("  redirect carried no request_token")
            return

        print(f"  captured request_token ({len(req_tok)} chars); exchanging...")
        ok, msg = exchange(os.environ["ALTAIR_KITE_API_KEY"],
                           os.environ["ALTAIR_KITE_API_SECRET"], req_tok)
        if ok:
            self._reply(200, "Session established",
                        f"Written to <code>{OUT_PATH}</code>. "
                        f"You can close this tab.")
            print(f"  SESSION ESTABLISHED: {msg}")
            print(f"  written to {OUT_PATH} (gitignored)")
        else:
            self._reply(502, "Token exchange failed",
                        f"<pre>{msg}</pre>")
            print(f"  exchange failed: {msg}")
        _result["done"] = True


def main() -> int:
    api_key = os.environ.get("ALTAIR_KITE_API_KEY", "").strip()
    secret = os.environ.get("ALTAIR_KITE_API_SECRET", "").strip()
    if not api_key or not secret:
        print("ALTAIR_KITE_API_KEY and ALTAIR_KITE_API_SECRET must be set.")
        print("Set them with setx, then open a NEW terminal.")
        return 2

    login = (f"https://kite.zerodha.com/connect/login"
             f"?api_key={urllib.parse.quote(api_key)}&v=3")

    print()
    print("  Kite login callback server")
    print("  " + "-" * 60)
    print(f"  listening on   {REDIRECT_URL}")
    print(f"  bound to       {HOST} only (never 0.0.0.0)")
    print()
    print("  FIRST: set the redirect URL on your Kite app to EXACTLY")
    print(f"      {REDIRECT_URL}")
    print("  Kite redirects to the URL registered on the app. While it still")
    print("  points elsewhere, the browser goes there and this server sees")
    print("  nothing at all.")
    print()
    print("  THEN open this and log in:")
    print(f"      {login}")
    print()
    print("  Waiting for one callback, then stopping. Ctrl-C to give up.")
    print()

    try:
        srv = http.server.HTTPServer((HOST, PORT), Handler)
    except OSError as e:
        print(f"  cannot bind {HOST}:{PORT} -- {e}")
        print("  another process may already hold it.")
        return 2

    srv.socket.settimeout(1.0)
    try:
        while not _result.get("done"):
            srv.handle_request()
    except KeyboardInterrupt:
        print("\n  stopped without a session.")
        return 1
    except socket.timeout:
        pass
    finally:
        srv.server_close()

    return 0 if _result.get("access_token") else 1


if __name__ == "__main__":
    sys.exit(main())
