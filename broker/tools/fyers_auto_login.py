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

The Windows vault entries are the same ones altair_fyers_login and the desktop
Brokers page use (Credential Manager target "Altair/altair.fyers/<account>"),
so the app id/secret saved there already are reused. On Linux the vault is the
`keyring` package when installed; otherwise use the environment or the file.

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
    """FYERS answered and said no (bad PIN, TOTP, secret...): do not retry blindly."""


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
    """The same masking rule as ops/altair_log.ps1, plus a catch-all for long token-like runs."""
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

def _win_target(account: str) -> str:
    return f"Altair/{SERVICE}/{account}"


def _win_vault_read(account: str) -> str | None:
    import ctypes
    from ctypes import wintypes

    class CREDENTIAL(ctypes.Structure):
        _fields_ = [("Flags", wintypes.DWORD), ("Type", wintypes.DWORD),
                    ("TargetName", wintypes.LPWSTR), ("Comment", wintypes.LPWSTR),
                    ("LastWritten", wintypes.FILETIME), ("CredentialBlobSize", wintypes.DWORD),
                    ("CredentialBlob", ctypes.POINTER(ctypes.c_ubyte)), ("Persist", wintypes.DWORD),
                    ("AttributeCount", wintypes.DWORD), ("Attributes", ctypes.c_void_p),
                    ("TargetAlias", wintypes.LPWSTR), ("UserName", wintypes.LPWSTR)]

    advapi = ctypes.WinDLL("advapi32", use_last_error=True)
    advapi.CredReadW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                 ctypes.POINTER(ctypes.POINTER(CREDENTIAL))]
    advapi.CredReadW.restype = wintypes.BOOL
    advapi.CredFree.argtypes = [ctypes.c_void_p]
    raw = ctypes.POINTER(CREDENTIAL)()
    if not advapi.CredReadW(_win_target(account), 1, 0, ctypes.byref(raw)):  # 1 = CRED_TYPE_GENERIC
        return None
    try:
        blob = ctypes.string_at(raw.contents.CredentialBlob, raw.contents.CredentialBlobSize)
        return blob.decode("utf-8") if blob else None
    finally:
        ctypes.memset(raw.contents.CredentialBlob, 0, raw.contents.CredentialBlobSize)
        advapi.CredFree(raw)


def _win_vault_write(account: str, value: str) -> bool:
    import ctypes
    from ctypes import wintypes

    class CREDENTIAL(ctypes.Structure):
        _fields_ = [("Flags", wintypes.DWORD), ("Type", wintypes.DWORD),
                    ("TargetName", wintypes.LPWSTR), ("Comment", wintypes.LPWSTR),
                    ("LastWritten", wintypes.FILETIME), ("CredentialBlobSize", wintypes.DWORD),
                    ("CredentialBlob", ctypes.POINTER(ctypes.c_ubyte)), ("Persist", wintypes.DWORD),
                    ("AttributeCount", wintypes.DWORD), ("Attributes", ctypes.c_void_p),
                    ("TargetAlias", wintypes.LPWSTR), ("UserName", wintypes.LPWSTR)]

    advapi = ctypes.WinDLL("advapi32", use_last_error=True)
    advapi.CredWriteW.argtypes = [ctypes.POINTER(CREDENTIAL), wintypes.DWORD]
    advapi.CredWriteW.restype = wintypes.BOOL
    data = value.encode("utf-8")
    buf = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
    cred = CREDENTIAL()
    cred.Type = 1                 # CRED_TYPE_GENERIC, as broker/credential_store.cpp
    cred.TargetName = _win_target(account)
    cred.CredentialBlobSize = len(data)
    cred.CredentialBlob = ctypes.cast(buf, ctypes.POINTER(ctypes.c_ubyte))
    cred.Persist = 2              # CRED_PERSIST_LOCAL_MACHINE
    cred.UserName = "Altair"
    ok = bool(advapi.CredWriteW(ctypes.byref(cred), 0))
    ctypes.memset(buf, 0, len(data))
    return ok


def vault_read(account: str) -> str | None:
    try:
        if os.name == "nt":
            return _win_vault_read(account)
        import keyring  # optional on Linux/macOS
        return keyring.get_password(SERVICE, account)
    except Exception:
        return None


def vault_write(account: str, value: str) -> bool:
    try:
        if os.name == "nt":
            return _win_vault_write(account, value)
        import keyring
        keyring.set_password(SERVICE, account, value)
        return True
    except Exception:
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


_opener = urllib.request.build_opener(_NoRedirect)


def _request(url: str, payload: dict | None, headers: dict[str, str] | None = None) -> tuple[int, dict]:
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, method="GET" if payload is None else "POST")
    req.add_header("Accept", "application/json")
    if data is not None:
        req.add_header("Content-Type", "application/json")
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with _opener.open(req, timeout=HTTP_TIMEOUT) as resp:
            status, body = resp.status, resp.read()
    except urllib.error.HTTPError as e:
        status, body = e.code, e.read()
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise Transient(f"{urllib.parse.urlsplit(url).netloc}: {getattr(e, 'reason', e)}") from None
    try:
        parsed = json.loads(body.decode("utf-8") or "{}")
    except (UnicodeDecodeError, json.JSONDecodeError):
        parsed = {}
    if status >= 500 or (not parsed and status not in (200, 308)):
        raise Transient(f"{urllib.parse.urlsplit(url).path}: HTTP {status}")
    return status, parsed


def _message(body: dict) -> str:
    m = body.get("message") or body.get("msg") or body.get("s") or "no message"
    return str(m)[:160]


def _step(name: str, status: int, body: dict) -> dict:
    if status >= 400 or body.get("s", "ok") != "ok":
        if status == 429:
            raise Transient(f"{name}: rate limited (HTTP 429)")
        raise LoginRefused(f"{name} refused (HTTP {status}): {_message(body)}")
    return body


def b64(text: str) -> str:
    return base64.b64encode(text.encode("utf-8")).decode("ascii")


def app_id_hash(client_id: str, secret: str) -> str:
    return hashlib.sha256(f"{client_id}:{secret}".encode("utf-8")).hexdigest()


def login_once(c: dict[str, str], used_window: int | None) -> tuple[str, str, str, int]:
    """One pass of the flow. Returns (access_token, refresh_token, profile fy_id, TOTP window used)."""
    st, body = _request(URL_SEND_OTP, {"fy_id": b64(c["fy_id"]), "app_id": "2"})
    key = _step("send_login_otp", st, body).get("request_key")
    if not key:
        raise Transient("send_login_otp: no request_key in the answer")

    code, window = fresh_totp(c["totp_key"], used_window)
    st, body = _request(URL_VERIFY_OTP, {"request_key": key, "otp": code})
    key = _step("verify_totp", st, body).get("request_key")
    if not key:
        raise Transient("verify_totp: no request_key in the answer")

    st, body = _request(URL_VERIFY_PIN, {"request_key": key, "identity_type": "pin", "identifier": b64(c["pin"])})
    login_token = (_step("verify_pin", st, body).get("data") or {}).get("access_token")
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
    try:
        return refused_marker(root).read_text(encoding="utf-8").strip() == _dt.datetime.now(IST).strftime("%Y-%m-%d")
    except OSError:
        return False


def cmd_login(root: Path, force: bool, attempts: int) -> int:
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
    # FYERS locks the account after a few wrong PINs/TOTPs. After one refusal no
    # scheduled run tries again that day; a person fixes the value and uses --force.
    if not force and refused_today(root):
        log(root, "ERROR", "FYERS refused this login earlier today; not trying again (fix the value, then run with --force)")
        return 3

    used_window: int | None = None
    totp_retried = False
    last = ""
    for attempt in range(1, attempts + 1):
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
            log(root, "ERROR", f"FYERS refused the login: {last}. Check the credentials (--check); not retrying today.")
            try:
                refused_marker(root).parent.mkdir(parents=True, exist_ok=True)
                refused_marker(root).write_text(_dt.datetime.now(IST).strftime("%Y-%m-%d") + "\n", encoding="utf-8")
            except OSError:
                pass
            return 3
        except Transient as e:
            last = str(e)
            if attempt < attempts:
                wait = min(60, 5 * 2 ** (attempt - 1))
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
    log(root, "ERROR", f"FYERS login failed after {attempts} attempts: {last}")
    return 4


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="FYERS v3 daily login with TOTP + PIN (no browser).")
    p.add_argument("--root", type=Path, default=None, help="the ALTAIR folder (default: this checkout, or ALTAIR_ROOT)")
    p.add_argument("--force", action="store_true", help="log in even if today's session still works")
    p.add_argument("--check", action="store_true", help="show which credentials are found (never their values)")
    p.add_argument("--store", action="store_true", help="type the credentials once into the OS vault")
    p.add_argument("--attempts", type=int, default=5, help="network retries (default 5)")
    a = p.parse_args(argv)
    root = (a.root or default_root()).resolve()
    if a.store:
        return cmd_store()
    if a.check:
        return cmd_check(root)
    return cmd_login(root, a.force, max(1, a.attempts))


if __name__ == "__main__":
    sys.exit(main())
