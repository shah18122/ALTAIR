#!/usr/bin/env python3
"""FYERS API v3 daily login without a browser (TOTP + PIN).

Writes data/fyers_session.json in exactly the format altair_fyers_login writes,
so every ALTAIR program (price service, recorder, desktop, data CLIs) picks the
day's token up unchanged. Standard library only: no pip install needed.

The flow is FYERS's own web-login sequence:
  1. send_login_otp_v2   FYERS user id            -> request_key
  2. verify_otp          TOTP from the authenticator secret -> request_key
  3. verify_pin_v2       4-digit PIN              -> short-lived login token
  4. /api/v3/token       app id + redirect uri    -> auth_code
  5. validate-authcode   appIdHash = sha256(client_id:app_secret) -> access token
  6. /api/v3/profile     the token is checked before the session is saved

CREDENTIALS. Each value is looked up in this order, the first found wins:
  environment variable  ->  OS vault  ->  config/fyers_login.local.env (gitignored)

  value          environment variable          OS vault account (service altair.fyers)
  app id         ALTAIR_FYERS_CLIENT_ID        client-id     e.g. ABCD1234-100
  app secret     ALTAIR_FYERS_SECRET           app-secret
  redirect uri   ALTAIR_FYERS_REDIRECT_URI     redirect-uri  (the one registered on the app)
  FYERS user id  ALTAIR_FYERS_FY_ID            fy-id         e.g. XS12345
  4-digit PIN    ALTAIR_FYERS_PIN              pin
  TOTP secret    ALTAIR_FYERS_TOTP_KEY         totp-key      (base32 key shown when TOTP was enabled)

The vault is ALTAIR's owner-only file vault, the one the C++ credential store
uses: ~/.config/altair/credentials/altair.fyers__<account> (0700 folder, 0600
files; $ALTAIR_CREDENTIAL_DIR or $XDG_CONFIG_HOME move it).

NOTHING SECRET IS EVER PRINTED OR LOGGED: not the PIN, TOTP, secret, auth code
or token. Only lengths and "set/missing" appear. Never commit the local file.

Usage:
  python broker/tools/fyers_auto_login.py              # log in unless today's session already works
  python broker/tools/fyers_auto_login.py --force      # log in even if today's session works
  python broker/tools/fyers_auto_login.py --check      # which credentials are found, and where (no values)
  python broker/tools/fyers_auto_login.py --store      # type the values once into the OS vault (hidden input)

Exit codes: 0 session ready, 2 credentials missing, 3 FYERS refused the login
(wrong PIN/TOTP/secret: retrying will not help), 4 network/server failure after
all retries, 5 the session could not be written.

WHAT LOCKS THE DAY. Only a definite "wrong PIN" or "wrong TOTP" answer (twice
for the TOTP) writes data/fyers_login_refused.txt, because FYERS locks the
account after a few of those. Everything else -- a 403 or 429 from FYERS's
firewall, a 5xx, an HTML page, a timeout, a refused send_login_otp -- is retried
with backoff for --retry-minutes (default 10), and the 08:40 backstop and the
recorder try again after that. Every refusal logs FYERS's whole answer
(secrets masked) so the cause is in the activity log, not a guess.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import contextlib
import datetime as _dt
import getpass
import hashlib
import hmac
import http.cookiejar
import json
import os
import re
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

SERVICE = "altair.fyers"
DEFAULT_REDIRECT = "https://altair.thesmitshah.com/fyers/callback"

# (key, environment variable, vault account, label, required)
FIELDS = [
    ("client_id", "ALTAIR_FYERS_CLIENT_ID", "client-id", "app id", True),
    ("secret", "ALTAIR_FYERS_SECRET", "app-secret", "app secret", True),
    ("redirect", "ALTAIR_FYERS_REDIRECT_URI", "redirect-uri", "redirect uri", False),
    ("fy_id", "ALTAIR_FYERS_FY_ID", "fy-id", "FYERS user id", True),
    ("pin", "ALTAIR_FYERS_PIN", "pin", "4-digit PIN", True),
    ("totp_key", "ALTAIR_FYERS_TOTP_KEY", "totp-key", "TOTP secret", True),
]

URL_SEND_OTP = "https://api-t2.fyers.in/vagator/v2/send_login_otp_v2"
URL_VERIFY_OTP = "https://api-t2.fyers.in/vagator/v2/verify_otp"
URL_VERIFY_PIN = "https://api-t2.fyers.in/vagator/v2/verify_pin_v2"
URL_TOKEN = "https://api-t1.fyers.in/api/v3/token"
URL_VALIDATE = "https://api-t1.fyers.in/api/v3/validate-authcode"
URL_PROFILE = "https://api-t1.fyers.in/api/v3/profile"

IST = _dt.timezone(_dt.timedelta(hours=5, minutes=30))
HTTP_TIMEOUT = 20


class LoginRefused(Exception):
    """FYERS answered and said no. `lock` is True only for a wrong PIN or TOTP:
    those lock the account after a few tries, so no scheduled run repeats them today."""

    def __init__(self, message: str, lock: bool = False):
        super().__init__(message)
        self.lock = lock


class Transient(Exception):
    """Network failure or a 5xx/garbled answer: worth another attempt."""


# --------------------------------------------------------------------------
# Root, activity log
# --------------------------------------------------------------------------

def default_root() -> Path:
    env = os.environ.get("ALTAIR_ROOT")
    if env:
        return Path(env)
    return Path(__file__).resolve().parents[2]


_BEARER_RE = re.compile(r"(?i)\b(bearer)(\s+)([^\s,\"}]+)")
_SECRET_RE = re.compile(
    r"(?i)\b(access_token|refresh_token|secret|password|pin|authorization|auth_code|api_key|otp|totp|request_key)\b"
    r"(\s*[\"']?\s*[:=][\s\"']*)([^\s,\"}]+)")


def mask(text: str) -> str:
    """The same masking rule as app/activity_log.hpp, plus a catch-all for long token-like runs."""
    text = re.sub(r"[\r\n]+", " ", text)
    text = _BEARER_RE.sub(r"\1\2<redacted>", text)
    text = _SECRET_RE.sub(r"\1\2<redacted>", text)
    return re.sub(r"[A-Za-z0-9_\-\.]{40,}", "<redacted>", text)


def log(root: Path, level: str, message: str) -> None:
    """One line to stdout and to data/logs/altair-<IST day>.log (the shared activity log)."""
    now = _dt.datetime.now(IST)
    line = f"{now.strftime('%Y-%m-%dT%H:%M:%S')}.{now.microsecond // 1000:03d}+05:30 [fyers-login] {level} {mask(message)}"
    print(line, flush=True)
    path = root / "data" / "logs" / f"altair-{now.strftime('%Y-%m-%d')}.log"
    for _ in range(80):  # the C++ and PowerShell writers deny other writers while they append
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            with open(path, "a", encoding="utf-8", newline="\n") as f:
                f.write(line + "\n")
            return
        except PermissionError:
            time.sleep(0.025)
        except OSError:
            return


# --------------------------------------------------------------------------
# Credential sources
# --------------------------------------------------------------------------

def vault_dir() -> Path | None:
    """The owner-only file vault broker/credential_store.cpp uses."""
    d = os.environ.get("ALTAIR_CREDENTIAL_DIR", "")
    if d.startswith("/"):
        return Path(d)
    x = os.environ.get("XDG_CONFIG_HOME", "")
    if x.startswith("/"):
        return Path(x) / "altair" / "credentials"
    h = os.environ.get("HOME", "")
    return Path(h) / ".config" / "altair" / "credentials" if h.startswith("/") else None


def _vault_file(account: str) -> Path | None:
    d = vault_dir()
    if d is None or not re.fullmatch(r"[A-Za-z0-9._-]+", account):
        return None
    return d / f"{SERVICE}__{account}"


def vault_read(account: str) -> str | None:
    """None when absent, or when the file or folder is not owner-only."""
    f = _vault_file(account)
    if f is None:
        return None
    try:
        st = os.stat(f.parent)
        if st.st_uid != os.geteuid() or st.st_mode & 0o077:
            return None
        fd = os.open(f, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            st = os.fstat(fd)
            if st.st_uid != os.geteuid() or st.st_mode & 0o077:
                return None
            data = b""
            while chunk := os.read(fd, 4096):
                data += chunk
        finally:
            os.close(fd)
        return data.decode("utf-8") or None
    except OSError:
        return None


def vault_write(account: str, value: str) -> bool:
    """Write a 0600 temp file, fsync it and rename it over the old one."""
    f = _vault_file(account)
    if f is None:
        return False
    tmp = f.with_name(f".{f.name}.{os.getpid()}.tmp")
    try:
        f.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chmod(f.parent, 0o700)
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        try:
            os.write(fd, value.encode("utf-8"))
            os.fsync(fd)
        finally:
            os.close(fd)
        os.replace(tmp, f)
        return True
    except OSError:
        with contextlib.suppress(OSError):
            os.unlink(tmp)
        return False


def read_local_file(path: Path) -> dict[str, str]:
    """KEY=VALUE lines; '#' comments; optional quotes. Unknown keys are ignored."""
    out: dict[str, str] = {}
    if not path.is_file():
        return out
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        value = value.strip()
        if value.startswith("#"):
            value = ""
        elif value[:1] not in ("\"", "'"):
            value = value.split(" #", 1)[0].split("\t#", 1)[0].strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        if value:
            out[key.strip()] = value
    return out


def load_credentials(root: Path) -> tuple[dict[str, str], dict[str, str]]:
    """(values, where each came from). Values never leave this process except to FYERS."""
    local = read_local_file(root / "config" / "fyers_login.local.env")
    values: dict[str, str] = {}
    source: dict[str, str] = {}
    for key, env, account, _label, _req in FIELDS:
        v = os.environ.get(env, "").strip()
        if v:
            values[key], source[key] = v, "environment"
            continue
        v = (vault_read(account) or "").strip()
        if v:
            values[key], source[key] = v, "OS vault"
            continue
        v = local.get(env, "").strip()
        if v:
            values[key], source[key] = v, "config/fyers_login.local.env"
    if "redirect" not in values:
        values["redirect"], source["redirect"] = DEFAULT_REDIRECT, "default"
    return values, source


def missing_fields(values: dict[str, str]) -> list[str]:
    return [f"{label} ({env})" for key, env, _a, label, req in FIELDS if req and not values.get(key)]


# --------------------------------------------------------------------------
# TOTP (RFC 6238, SHA-1, 6 digits, 30 s) -- what FYERS's authenticator setup uses
# --------------------------------------------------------------------------

def totp(secret_b32: str, at: float | None = None, step: int = 30, digits: int = 6) -> str:
    key_text = re.sub(r"[\s-]", "", secret_b32).upper()
    key = base64.b32decode(key_text + "=" * (-len(key_text) % 8))
    counter = int((time.time() if at is None else at) // step)
    digest = hmac.new(key, struct.pack(">Q", counter), hashlib.sha1).digest()
    offset = digest[-1] & 0x0F
    code = (struct.unpack(">I", digest[offset:offset + 4])[0] & 0x7FFFFFFF) % (10 ** digits)
    return str(code).zfill(digits)


def fresh_totp(secret_b32: str, used_window: int | None) -> tuple[str, int]:
    """A code with at least 5 s left, from a window not used before (FYERS refuses a reused code)."""
    while True:
        now = time.time()
        window = int(now // 30)
        if 30 - (now % 30) >= 5 and window != used_window:
            return totp(secret_b32, now), window
        time.sleep(30 - (now % 30) + 0.5)


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------

class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: D401
        return None  # /api/v3/token answers 308 with the auth_code in the JSON body


# Cookies the login hosts set (their firewall's among them) are kept for the run.
_opener = urllib.request.build_opener(_NoRedirect, urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

# What FYERS's own web login (login.fyers.in) sends. urllib's default
# "Python-urllib/3.x" user agent and a bare Accept are what bot filters in front
# of api-t1/api-t2 answer with 403, so the requests look like that page's.
BROWSER_HEADERS = {
    "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
                  "Chrome/130.0.0.0 Safari/537.36",
    "Accept": "application/json, text/plain, */*",
    "Accept-Language": "en-US,en;q=0.9",
    "Origin": "https://login.fyers.in",
    "Referer": "https://login.fyers.in/",
}

# Answers that say nothing about the credentials: a firewall, a timeout, a busy server.
TRANSIENT_STATUS = {403, 408, 409, 425, 429}


def _describe(status: int, raw: bytes, headers) -> str:
    """HTTP status, the firewall/server headers and the whole body (masked by log())."""
    text = raw.decode("utf-8", "replace").strip()
    if "<html" in text[:400].lower():
        title = re.search(r"(?is)<title>(.*?)</title>", text)
        text = "HTML page" + (f" '{title.group(1).strip()[:80]}'" if title else "")
    hdr = ""
    if headers is not None:
        keep = [f"{k}={headers.get(k)}" for k in ("server", "content-type", "cf-ray", "x-amzn-requestid",
                                                     "x-cache", "retry-after") if headers.get(k)]
        hdr = f" [{', '.join(keep)}]" if keep else ""
    return f"HTTP {status}{hdr}: {text[:600] or 'empty body'}"


def _request(url: str, payload: dict | None, headers: dict[str, str] | None = None) -> tuple[int, dict]:
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, method="GET" if payload is None else "POST")
    for k, v in BROWSER_HEADERS.items():
        req.add_header(k, v)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with _opener.open(req, timeout=HTTP_TIMEOUT) as resp:
            status, body, rh = resp.status, resp.read(), resp.headers
    except urllib.error.HTTPError as e:
        status, rh = e.code, e.headers
        try:
            body = e.read()
        except OSError:
            body = b""
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise Transient(f"{urllib.parse.urlsplit(url).netloc}: {getattr(e, 'reason', e)}") from None
    try:
        parsed = json.loads(body.decode("utf-8") or "{}")
    except (UnicodeDecodeError, json.JSONDecodeError):
        parsed = None
    path = urllib.parse.urlsplit(url).path
    if status >= 500 or status in TRANSIENT_STATUS or not isinstance(parsed, dict) \
            or (not parsed and status not in (200, 308)):
        raise Transient(f"{path}: {_describe(status, body, rh)}")
    return status, parsed


def _message(body: dict) -> str:
    """FYERS's whole answer (code, message and anything else), for the log."""
    try:
        return json.dumps(body, separators=(",", ":"))[:600]
    except (TypeError, ValueError):
        return str(body)[:600]


def _step(name: str, status: int, body: dict, lock: bool = False) -> dict:
    """`lock`: a refusal at this step means a wrong PIN/TOTP (see LoginRefused)."""
    if status >= 400 or body.get("s", "ok") != "ok":
        if status in TRANSIENT_STATUS or status >= 500:
            raise Transient(f"{name}: HTTP {status}: {_message(body)}")
        raise LoginRefused(f"{name} refused (HTTP {status}): {_message(body)}", lock=lock)
    return body


def b64(text: str) -> str:
    return base64.b64encode(text.encode("utf-8")).decode("ascii")


def app_id_hash(client_id: str, secret: str) -> str:
    return hashlib.sha256(f"{client_id}:{secret}".encode("utf-8")).hexdigest()


def login_once(c: dict[str, str], used_window: int | None) -> tuple[str, str, str, int]:
    """One pass of the flow. Returns (access_token, refresh_token, profile fy_id, TOTP window used)."""
    st, body = _request(URL_SEND_OTP, {"fy_id": b64(c["fy_id"]), "app_id": "2"})
    # No PIN or TOTP is spent here, so a refusal cannot lock the account: retry it.
    try:
        key = _step("send_login_otp", st, body).get("request_key")
    except LoginRefused as e:
        raise Transient(str(e)) from None
    if not key:
        raise Transient("send_login_otp: no request_key in the answer")

    code, window = fresh_totp(c["totp_key"], used_window)
    st, body = _request(URL_VERIFY_OTP, {"request_key": key, "otp": code})
    key = _step("verify_totp", st, body, lock=True).get("request_key")
    if not key:
        raise Transient("verify_totp: no request_key in the answer")

    st, body = _request(URL_VERIFY_PIN, {"request_key": key, "identity_type": "pin", "identifier": b64(c["pin"])})
    login_token = (_step("verify_pin", st, body, lock=True).get("data") or {}).get("access_token")
    if not login_token:
        raise Transient("verify_pin: no login token in the answer")

    client = c["client_id"]
    app_id, _, app_type = client.partition("-")
    st, body = _request(URL_TOKEN, {
        "fyers_id": c["fy_id"], "app_id": app_id, "redirect_uri": c["redirect"],
        "appType": app_type or "100", "code_challenge": "", "state": "altair", "scope": "",
        "nonce": "", "response_type": "code", "create_cookie": True,
    }, {"Authorization": f"Bearer {login_token}"})
    body = _step("authorize app", st, body)
    url = body.get("Url") or (body.get("data") or {}).get("url") or ""
    auth_code = (urllib.parse.parse_qs(urllib.parse.urlsplit(url).query).get("auth_code") or [None])[0]
    if not auth_code:
        raise LoginRefused("authorize app: FYERS returned no auth_code (is the redirect uri the one registered on the app?)")

    st, body = _request(URL_VALIDATE, {"grant_type": "authorization_code",
                                       "appIdHash": app_id_hash(client, c["secret"]), "code": auth_code})
    body = _step("validate-authcode", st, body)
    access = body.get("access_token")
    if not access:
        raise LoginRefused("validate-authcode: no access token in the answer")

    profile_id = verify_profile(client, access)
    if not profile_id:
        raise Transient("token issued but the FYERS profile check failed")
    return access, body.get("refresh_token") or "", profile_id, window


def verify_profile(client_id: str, access: str) -> str | None:
    try:
        st, body = _request(URL_PROFILE, None, {"Authorization": f"{client_id}:{access}"})
    except Transient:
        return None
    if st != 200 or body.get("s") != "ok":
        return None
    return (body.get("data") or {}).get("fy_id") or None


# --------------------------------------------------------------------------
# Session file (same layout as app/fyers_login_main.cpp)
# --------------------------------------------------------------------------

def session_path(root: Path) -> Path:
    return root / "data" / "fyers_session.json"


def write_session(root: Path, client: str, profile_id: str, access: str, refresh: str) -> Path:
    path = session_path(root)
    path.parent.mkdir(parents=True, exist_ok=True)
    doc = {
        "status": "success",
        "data": {"broker": "FYERS", "client_id": client, "profile_id": profile_id,
                 "logged_at": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                 "source": "auto-login"},
        "access_token": access,
        "refresh_token": refresh,
    }
    tmp = path.with_name(path.name + ".tmp")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(doc, separators=(",", ":")) + "\n")
    os.replace(tmp, path)  # readers never see a half-written session
    return path


def todays_session_works(root: Path, client: str) -> bool:
    """True when the saved session was issued today (IST) for this app and FYERS still accepts it."""
    try:
        doc = json.loads(session_path(root).read_text(encoding="utf-8"))
        data = doc.get("data") or {}
        logged = _dt.datetime.strptime(data.get("logged_at", ""), "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=_dt.timezone.utc)
        if data.get("client_id") != client or logged.astimezone(IST).date() != _dt.datetime.now(IST).date():
            return False
        return verify_profile(client, doc.get("access_token") or "") is not None
    except (OSError, ValueError, AttributeError):
        return False


# --------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------

def cmd_check(root: Path) -> int:
    values, source = load_credentials(root)
    for key, env, account, label, req in FIELDS:
        where = source.get(key)
        state = f"set ({where})" if where else ("MISSING" if req else "not set")
        print(f"  {label:14} {state:40} env {env} / vault {SERVICE}:{account}")
    missing = missing_fields(values)
    if missing:
        print("\nmissing: " + ", ".join(missing))
        return 2
    try:
        totp(values["totp_key"])
    except (ValueError, binascii.Error):
        print("\nthe TOTP secret is not valid base32 (copy the key FYERS showed when TOTP was enabled)")
        return 2
    print("\nall credentials found.")
    return 0


def cmd_store() -> int:
    print("Values are typed hidden and saved to the OS vault only. Leave blank to keep what is there.")
    for _key, env, account, label, _req in FIELDS:
        value = getpass.getpass(f"  {label} [{account}]: ").strip()
        if not value:
            continue
        if account == "totp-key":
            try:
                totp(value)
            except (ValueError, binascii.Error):
                print("    not valid base32; skipped")
                continue
        print(f"    {'saved' if vault_write(account, value) else 'could NOT be saved (no OS vault: use ' + env + ')'}")
    return 0


def refused_marker(root: Path) -> Path:
    return root / "data" / "fyers_login_refused.txt"


def refused_today(root: Path) -> bool:
    """The marker is "<IST day> wrong-credential <reason>". A bare date is the old
    format, written for ANY refusal (a firewall 403 included): it is ignored."""
    try:
        parts = refused_marker(root).read_text(encoding="utf-8").split(maxsplit=2)
    except OSError:
        return False
    return len(parts) >= 2 and parts[0] == _dt.datetime.now(IST).strftime("%Y-%m-%d") and parts[1] == "wrong-credential"


def cmd_login(root: Path, force: bool, attempts: int, retry_minutes: float = 10.0) -> int:
    values, _src = load_credentials(root)
    missing = missing_fields(values)
    if missing:
        log(root, "ERROR", "credentials missing: " + ", ".join(missing)
            + ". Run: python broker/tools/fyers_auto_login.py --store (or set the environment variables).")
        return 2
    client = values["client_id"]
    if not force and todays_session_works(root, client):
        log(root, "INFO", "today's FYERS session is already valid; nothing to do")
        return 0
    # FYERS locks the account after a few wrong PINs/TOTPs. After one such refusal
    # no scheduled run tries again that day; a person fixes the value and uses --force.
    if not force and refused_today(root):
        log(root, "ERROR", "FYERS said the PIN or TOTP was wrong earlier today; not trying again "
                           "(fix the value, then run with --force)")
        return 3

    used_window: int | None = None
    totp_retried = False
    last = ""
    deadline = time.monotonic() + max(0.0, retry_minutes) * 60
    attempt = 0
    while attempt < attempts:
        attempt += 1
        try:
            access, refresh, profile_id, used_window = login_once(values, used_window)
        except LoginRefused as e:
            last = str(e)
            # A refused TOTP can be clock skew or a code used a moment ago: ONE more
            # try on the next window. Anything else is a wrong credential.
            if "verify_totp" in last and not totp_retried and attempt < attempts:
                totp_retried = True
                log(root, "WARN", f"attempt {attempt}: {last}; retrying once on the next TOTP window")
                used_window = int(time.time() // 30)
                continue
            if not e.lock:
                # The app's settings (redirect uri, secret), not the PIN/TOTP: this
                # run stops, but the backstop and the recorder may try again.
                log(root, "ERROR", f"FYERS refused the login: {last}. Check the app id, secret and redirect uri (--check).")
                return 3
            log(root, "ERROR", f"FYERS refused the login: {last}. The PIN or TOTP is wrong (--check); not retrying today.")
            try:
                refused_marker(root).parent.mkdir(parents=True, exist_ok=True)
                refused_marker(root).write_text(
                    f"{_dt.datetime.now(IST).strftime('%Y-%m-%d')} wrong-credential {mask(last)[:300]}\n", encoding="utf-8")
            except OSError:
                pass
            return 3
        except Transient as e:
            last = str(e)
            wait = min(120, 10 * 2 ** (attempt - 1))
            if attempt < attempts and time.monotonic() + wait < deadline:
                log(root, "WARN", f"attempt {attempt}/{attempts} failed: {last}; retrying in {wait}s")
                time.sleep(wait)
                continue
            break
        try:
            path = write_session(root, client, profile_id, access, refresh)
        except OSError as e:
            log(root, "ERROR", f"token obtained but the session could not be written: {e.strerror}")
            return 5
        with contextlib.suppress(OSError):
            refused_marker(root).unlink()
        log(root, "INFO", f"FYERS session established for {profile_id} (token {len(access)} chars, redacted) -> {path}")
        return 0
    log(root, "ERROR", f"FYERS login failed after {attempt} attempt(s): {last}")
    return 4


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="FYERS v3 daily login with TOTP + PIN (no browser).")
    p.add_argument("--root", type=Path, default=None, help="the ALTAIR folder (default: this checkout, or ALTAIR_ROOT)")
    p.add_argument("--force", action="store_true", help="log in even if today's session still works")
    p.add_argument("--check", action="store_true", help="show which credentials are found (never their values)")
    p.add_argument("--store", action="store_true", help="type the credentials once into the OS vault")
    p.add_argument("--attempts", type=int, default=12, help="most tries in one run (default 12)")
    p.add_argument("--retry-minutes", type=float, default=10.0,
                   help="keep retrying firewall/network/server failures this long (default 10)")
    a = p.parse_args(argv)
    root = (a.root or default_root()).resolve()
    if a.store:
        return cmd_store()
    if a.check:
        return cmd_check(root)
    return cmd_login(root, a.force, max(1, a.attempts), a.retry_minutes)


if __name__ == "__main__":
    sys.exit(main())
