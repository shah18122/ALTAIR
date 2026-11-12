#!/usr/bin/env python3
"""The FYERS app's redirect URL on the server: a browser login ends here and is saved.

The daily login needs no browser (fyers_auto_login.py: TOTP + PIN -> auth_code ->
access token, all server-side). This is for the other way in: a person logs in to
FYERS in a browser, FYERS redirects to

    https://altair.thesmitshah.com/fyers/callback?s=ok&auth_code=...&state=...

and this server exchanges the auth_code for today's access token, checks it
against the FYERS profile, writes data/fyers_session.json (the recorder and the
desktop pick it up by themselves) and answers with a page that says "Connected
to FYERS", or a page that says exactly what failed.

    GET  /fyers/login      sends the browser to FYERS's login for this app
    GET  /fyers/callback   the redirect: exchange, save, show the result
    POST /fyers/callback   order webhooks: passed to altair_fyers_webhook on
                           127.0.0.1:8787 when it runs (ops/fyers-webhook.md)
    GET  /healthz          "ok"

It listens on 127.0.0.1 only (default port 80: the origin the Cloudflare tunnel
for altair.thesmitshah.com already points at). Nothing is exposed on the
server's public ports, so SSH on 22/443 and DCV are untouched: the tunnel
(ops/linux/fyers_tunnel.sh) carries HTTPS in from Cloudflare.

Only a token for the configured FYERS user id is saved: an auth_code from any
other account is refused. Nothing secret is printed, logged or put in a page.

    python3 broker/tools/fyers_callback_server.py [--port 80] [--root DIR]
"""

from __future__ import annotations

import argparse
import contextlib
import html
import secrets
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fyers_auto_login as fal  # noqa: E402

URL_AUTHCODE = "https://api-t1.fyers.in/api/v3/generate-authcode"
WEBHOOK_UPSTREAM = "http://127.0.0.1:8787/fyers/callback"
STATE_TTL = 900          # a login started on /fyers/login must finish within 15 minutes
MAX_BODY = 64 * 1024

_states: dict[str, float] = {}
_used_codes: dict[str, float] = {}


def page(title: str, ok: bool, lines: list[str]) -> bytes:
    colour = "#2EA043" if ok else "#DA3633"
    body = "".join(f"<p>{html.escape(x)}</p>" for x in lines)
    return f"""<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>ALTAIR - {html.escape(title)}</title>
<style>body{{font-family:system-ui,sans-serif;background:#0D1117;color:#E6EDF3;display:flex;
min-height:100vh;align-items:center;justify-content:center;margin:0;padding:16px}}
.card{{max-width:520px;width:100%;background:#161B22;border:1px solid #30363D;border-radius:12px;padding:28px}}
h1{{margin:0 0 12px;font-size:22px;color:{colour}}}p{{margin:6px 0;color:#C9D1D9;line-height:1.5}}
.mark{{font-size:40px;color:{colour}}}</style></head><body><div class="card">
<div class="mark">{"&#10003;" if ok else "&#10007;"}</div><h1>{html.escape(title)}</h1>{body}
</div></body></html>""".encode("utf-8")


def exchange(root: Path, auth_code: str) -> tuple[bool, str, list[str]]:
    """auth_code -> access token -> profile check -> data/fyers_session.json."""
    values, _src = fal.load_credentials(root)
    missing = [m for m in fal.missing_fields(values) if "PIN" not in m and "TOTP" not in m]
    if missing:
        return False, "FYERS app not configured", ["Missing on the server: " + ", ".join(missing) + "."]
    client = values["client_id"]
    try:
        st, body = fal._request(fal.URL_VALIDATE, {"grant_type": "authorization_code",
                                                   "appIdHash": fal.app_id_hash(client, values["secret"]),
                                                   "code": auth_code})
        body = fal._step("validate-authcode", st, body)
    except (fal.Transient, fal.LoginRefused) as e:
        fal.log(root, "ERROR", f"browser login: {e}")
        return False, "FYERS did not accept the login", [
            "The auth code could not be exchanged for a token (it may be used already or expired).",
            "Log in again from the start.", fal.mask(str(e))[:300]]
    access = body.get("access_token") or ""
    if not access:
        return False, "FYERS sent no token", ["validate-authcode answered without an access token."]
    profile_id = fal.verify_profile(client, access)
    if not profile_id:
        return False, "Token not usable", ["FYERS issued a token but its profile check failed. Try again."]
    expected = values.get("fy_id", "")
    if expected and profile_id.upper() != expected.upper():
        fal.log(root, "ERROR", "browser login: a token for another FYERS account was refused")
        return False, "Wrong FYERS account", ["This login belongs to a different FYERS user id than the one "
                                              "ALTAIR is set up for. Nothing was saved."]
    try:
        path = fal.write_session(root, client, profile_id, access, body.get("refresh_token") or "")
    except OSError as e:
        return False, "Could not save the session", [f"Writing data/fyers_session.json failed: {e.strerror}."]
    with contextlib.suppress(OSError):
        fal.refused_marker(root).unlink()
    fal.log(root, "INFO", f"browser login: FYERS session established for {profile_id} "
                          f"(token {len(access)} chars, redacted) -> {path}")
    return True, "Connected to FYERS", [
        f"Auth received and exchanged. Today's session for {profile_id} is saved on the server.",
        "The recorder and the ALTAIR desktop pick the new token up by themselves; nothing needs a restart.",
        "You can close this tab."]


def make_handler(root: Path):
    class Handler(BaseHTTPRequestHandler):
        server_version = "altair-fyers-callback"
        sys_version = ""

        def log_message(self, fmt, *args):  # the query holds the auth_code: never log request lines
            return

        def send(self, code: int, body: bytes, ctype: str = "text/html; charset=utf-8",
                 extra: dict[str, str] | None = None) -> None:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Referrer-Policy", "no-referrer")
            self.send_header("X-Content-Type-Options", "nosniff")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def do_HEAD(self):
            self.do_GET()

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            q = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
            now = time.time()
            for d in (_states, _used_codes):
                for k in [k for k, t in d.items() if now - t > STATE_TTL]:
                    del d[k]
            if url.path == "/healthz":
                return self.send(200, b"ok\n", "text/plain")
            if url.path == "/fyers/login":
                values, _ = fal.load_credentials(root)
                if not values.get("client_id"):
                    return self.send(500, page("FYERS app not configured", False,
                                               ["No app id on the server (fyers_auto_login.py --store)."]))
                state = secrets.token_urlsafe(16)
                _states[state] = now
                target = URL_AUTHCODE + "?" + urllib.parse.urlencode({
                    "client_id": values["client_id"], "redirect_uri": values["redirect"],
                    "response_type": "code", "state": state})
                return self.send(302, b"", extra={"Location": target})
            if url.path != "/fyers/callback":
                return self.send(404, page("Not found", False, ["Nothing here."]))
            code = q.get("auth_code", "")
            if not code:
                if q.get("s") == "error" or q.get("message"):
                    msg = q.get("message", "FYERS reported an error")[:200]
                    return self.send(400, page("FYERS login failed", False, [msg, "Log in again from the start."]))
                return self.send(200, page("ALTAIR FYERS callback", True,
                                           ["This is the FYERS redirect URL. It is working.",
                                            "Open /fyers/login to log in to FYERS from a browser."]))
            if code in _used_codes:
                return self.send(200, page("Already received", True,
                                           ["This login was already exchanged; the session is saved.",
                                            "You can close this tab."]))
            _states.pop(q.get("state", ""), None)
            _used_codes[code] = now
            ok, title, lines = exchange(root, code)
            return self.send(200 if ok else 400, page(title, ok, lines))

        def do_POST(self):
            if urllib.parse.urlsplit(self.path).path != "/fyers/callback":
                return self.send(404, b"not found\n", "text/plain")
            try:
                n = int(self.headers.get("Content-Length") or 0)
            except ValueError:
                n = -1
            if n < 0 or n > MAX_BODY:
                return self.send(413, b"too large\n", "text/plain")
            data = self.rfile.read(n)
            # The query carries the webhook key (?key=...), so it goes along.
            query = urllib.parse.urlsplit(self.path).query
            upstream = WEBHOOK_UPSTREAM + ("?" + query if query else "")
            req = urllib.request.Request(upstream, data=data, method="POST")
            for h in ("Content-Type", "X-Fyers-Webhook-Secret"):
                if self.headers.get(h):
                    req.add_header(h, self.headers[h])
            try:
                with urllib.request.urlopen(req, timeout=10) as r:
                    status, body = r.status, r.read()
            except urllib.error.HTTPError as e:
                status, body = e.code, e.read()
            except (urllib.error.URLError, OSError):
                status, body = 503, b"webhook receiver not running\n"
            return self.send(status, body, "text/plain")

    return Handler


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="FYERS redirect URL: exchange a browser login and save it.")
    p.add_argument("--root", type=Path, default=None)
    p.add_argument("--port", type=int, default=80)
    p.add_argument("--bind", default="127.0.0.1")
    a = p.parse_args(argv)
    root = (a.root or fal.default_root()).resolve()
    srv = HTTPServer((a.bind, a.port), make_handler(root))
    fal.log(root, "INFO", f"browser-login callback listening on {a.bind}:{a.port} (/fyers/login, /fyers/callback)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
