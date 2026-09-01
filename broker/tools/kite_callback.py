#!/usr/bin/env python3
"""A localhost callback server that captures a Kite request_token.

Run it, open the login URL it prints, log in, and it captures the redirect,
exchanges the token, and writes data/kite_session.json. Then it stops.

    python broker/tools/kite_callback.py

TWO ADDRESSES, AND THEY ARE NOT THE SAME ONE.

Kite redirects to the URL registered on the app -- never to whatever we ask
for -- and that is the PUBLIC one:

    https://altair.thesmitshah.com/zerodha/callback

A tunnel forwards it to where this server actually listens:

    http://127.0.0.1:53123/zerodha/callback

So the registered redirect stays as it is; nothing on the Kite app needs
changing. What has to be true is that the tunnel is UP and the paths match on
both sides. Override the public URL with ALTAIR_KITE_REDIRECT if the tunnel
address ever changes.

THE TUNNEL IS CHECKED BEFORE YOU LOG IN, not after. A request_token is SINGLE
USE and expires in minutes, so a tunnel that is down costs a whole login round
trip to discover. On startup this fetches its own public URL with a probe
parameter and confirms the request arrives here. If it does not, it says so and
stops, before anything is burned.

SECURITY
--------
  * Binds 127.0.0.1 only. Never 0.0.0.0. That still matters WITH a tunnel: the
    tunnel client connects from localhost, so binding wider would additionally
    expose the port on the LAN and buy nothing.
  * The tunnel does make this endpoint reachable from the internet while it is
    up. Bring it up for the login and take it down afterwards. The server also
    stops itself after one successful capture, so the window is short by
    construction.
  * A probe request carries no token and never triggers an exchange.
  * The secret comes from the environment. Never printed, never written to a
    file, never on a command line.
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
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

HOST = "127.0.0.1"
PORT = 53123
CALLBACK_PATH = "/zerodha/callback"
# Where this process listens. The tunnel forwards to here.
LISTEN_URL = f"http://{HOST}:{PORT}{CALLBACK_PATH}"
# Where KITE redirects. Must equal the redirect registered on the Kite app,
# byte for byte -- Kite compares it as a string, so a trailing slash or http
# for https is a rejected login rather than a warning.
PUBLIC_REDIRECT = os.environ.get(
    "ALTAIR_KITE_REDIRECT",
    "https://altair.thesmitshah.com/zerodha/callback").strip()
SESSION_URL = "https://api.kite.trade/session/token"
OUT_PATH = os.path.join("data", "kite_session.json")
# Query parameter that marks a liveness probe rather than a real callback.
PROBE_PARAM = "altair_probe"

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

        # A liveness probe. Answered before anything else looks for a token,
        # so a probe can never be mistaken for a login or trigger an exchange.
        probe = (q.get(PROBE_PARAM) or [""])[0]
        if probe:
            _result["probe_seen"] = probe
            self._reply(200, "Tunnel is up",
                        "This probe reached the local callback server.")
            return

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


def probe_tunnel(srv: http.server.HTTPServer) -> tuple[bool, str]:
    """Fetch our own PUBLIC url and confirm the request lands here.

    Runs the fetch on a background thread and services exactly one request on
    the main thread, because a single-threaded server cannot answer a request
    it is itself blocked on making.
    """
    nonce = hashlib.sha256(os.urandom(16)).hexdigest()[:16]
    sep = "&" if "?" in PUBLIC_REDIRECT else "?"
    url = f"{PUBLIC_REDIRECT}{sep}{PROBE_PARAM}={nonce}"
    outcome: dict = {}

    def fetch() -> None:
        try:
            # A BROWSER user agent, and this is not cosmetic. The tunnel here
            # sits behind Cloudflare, whose bot rules answer the default
            # "Python-urllib/3.x" with 403 while letting a browser through --
            # measured, on this exact hostname: curl 200, browser 200,
            # Python-urllib 403. A probe that lies about being blocked is
            # worse than no probe, because it reports a working tunnel as
            # broken. The real callback IS a browser navigation, so looking
            # like one is what makes this a faithful test.
            req = urllib.request.Request(url, method="GET", headers={
                "User-Agent": ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                               "AppleWebKit/537.36 (KHTML, like Gecko) "
                               "Chrome/131.0.0.0 Safari/537.36"),
                "Accept": "text/html,application/xhtml+xml,*/*;q=0.8",
            })
            with urllib.request.urlopen(req, timeout=8) as r:
                outcome["code"] = r.status
        except urllib.error.HTTPError as e:
            outcome["code"] = e.code
        except Exception as e:                       # noqa: BLE001
            outcome["error"] = f"{type(e).__name__}: {e}"

    t = threading.Thread(target=fetch, daemon=True)
    t.start()
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline and not _result.get("probe_seen"):
        try:
            srv.handle_request()
        except socket.timeout:
            continue
    t.join(timeout=2.0)

    if _result.get("probe_seen") == nonce:
        return True, "the probe arrived here with the nonce it was sent with"
    if "error" in outcome:
        return False, outcome["error"]
    if _result.get("probe_seen"):
        return False, "a probe arrived, but not the one just sent"
    return False, f"no probe arrived (remote replied {outcome.get('code')})"


def main() -> int:
    api_key = os.environ.get("ALTAIR_KITE_API_KEY", "").strip()
    secret = os.environ.get("ALTAIR_KITE_API_SECRET", "").strip()
    if not api_key or not secret:
        print("ALTAIR_KITE_API_KEY and ALTAIR_KITE_API_SECRET must be set.")
        print("Set them with setx, then open a NEW terminal.")
        return 2

    skip_probe = "--no-probe" in sys.argv

    login = (f"https://kite.zerodha.com/connect/login"
             f"?api_key={urllib.parse.quote(api_key)}&v=3")

    print()
    print("  Kite login callback server")
    print("  " + "-" * 62)
    print(f"  listening on   {LISTEN_URL}")
    print(f"  bound to       {HOST} only (never 0.0.0.0)")
    print(f"  Kite redirects to  {PUBLIC_REDIRECT}")
    print("                 (must match the redirect on the Kite app exactly)")
    print()

    try:
        srv = http.server.HTTPServer((HOST, PORT), Handler)
    except OSError as e:
        print(f"  cannot bind {HOST}:{PORT} -- {e}")
        print("  another process may already hold it.")
        return 2
    srv.socket.settimeout(1.0)

    # Check the tunnel BEFORE the login, because a request_token is single use
    # and expires in minutes: discovering a dead tunnel afterwards costs a
    # whole round trip through Zerodha's 2FA.
    if not skip_probe:
        print("  checking the tunnel before you log in...")
        ok, why = probe_tunnel(srv)
        if not ok:
            print(f"  TUNNEL DID NOT ANSWER: {why}")
            print()
            print(f"  {PUBLIC_REDIRECT}")
            print(f"  must reach {LISTEN_URL}")
            print()
            print("  Start the tunnel and run this again. Nothing has been")
            print("  used up -- no login was attempted.")
            print()
            print("  If you believe the tunnel IS up, the probe may be the")
            print("  thing being blocked rather than the tunnel being down --")
            print("  a gateway that rejects non-browser clients would do that.")
            print("  Check by hand, and use --no-probe to go ahead regardless:")
            print(f"      curl -i \"{PUBLIC_REDIRECT}?{PROBE_PARAM}=1\"")
            srv.server_close()
            return 2
        print(f"  tunnel is up: {why}")
        _result.pop("probe_seen", None)
        print()

    print("  Now open this and log in:")
    print(f"      {login}")
    print()
    print("  Waiting for one callback, then stopping. Ctrl-C to give up.")
    print()

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
