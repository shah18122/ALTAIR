# FYERS auto-login (TOTP + PIN, no browser)

A FYERS access token lasts one trading day. `broker/tools/fyers_auto_login.py`
gets it every morning without the browser and writes `data/fyers_session.json`
in exactly the format `altair_fyers_login` writes, so the price service,
recorder, desktop and every data CLI use it unchanged.

**Status:** tested offline against a simulated FYERS (`fyers_auto_login_py`
ctest). It has not yet been run against the real FYERS login: the first real
run is the one-time check below.

## One-time setup (Windows PC)

1. In FYERS (web or app): **My Account > Security > External 2FA TOTP**, enable
   it and keep the base32 key it shows (the same key your authenticator app
   scans). Your FYERS app on myapi.fyers.in must have the redirect URI you use.
2. Python 3 from python.org (tick "Add to PATH"). No packages are needed.
3. Type the values once into Windows Credential Manager (hidden input, nothing
   is echoed or written to disk):

   ```powershell
   py -3 broker\tools\fyers_auto_login.py --store
   py -3 broker\tools\fyers_auto_login.py --check      # which values are found, never the values
   py -3 broker\tools\fyers_auto_login.py --force      # first real login; prints only "session established"
   ```

   App id, secret and redirect URI already saved by the Brokers page are reused
   (same vault entries, `Altair/altair.fyers/<name>`), so `--store` only needs
   the FYERS user id, PIN and TOTP key; leave the others blank.
4. Register the task: `powershell -ExecutionPolicy Bypass -File ops\install_tasks.ps1 -Go`
   adds **ALTAIR FYERS Login**, Mon-Fri 08:30, before the recorder (08:50).

## Where credentials can live

Each value is looked up in this order: environment variable, then the OS vault,
then `config/fyers_login.local.env` (gitignored; the template is
`config/fyers_login.example.env`).

| value | environment variable | vault name |
|---|---|---|
| app id (`ABCD1234-100`) | `ALTAIR_FYERS_CLIENT_ID` | `client-id` |
| app secret | `ALTAIR_FYERS_SECRET` | `app-secret` |
| redirect URI (optional) | `ALTAIR_FYERS_REDIRECT_URI` | `redirect-uri` |
| FYERS user id | `ALTAIR_FYERS_FY_ID` | `fy-id` |
| 4-digit PIN | `ALTAIR_FYERS_PIN` | `pin` |
| TOTP key (base32) | `ALTAIR_FYERS_TOTP_KEY` | `totp-key` |

Never paste these into a chat, an issue or a commit.

## What it does, and its safety rules

* Skips the login when today's saved session still passes FYERS's profile
  check (`--force` overrides).
* Network failures and 5xx answers: up to 5 attempts, waiting 5, 10, 20, 40 s.
* A refused TOTP is retried once, on the next 30 s window (clock skew, or a code
  used a moment ago). Any other refusal (wrong PIN, secret, app id) stops at
  once and writes `data/fyers_login_refused.txt`: no scheduled run tries again
  that day, because FYERS locks the account after a few wrong PINs. Fix the
  value, then run with `--force`.
* Nothing secret is printed or logged: one line per event goes to stdout and
  to `data/logs/altair-<IST day>.log` (component `fyers-login`), with the same
  masking as `ops/altair_log.ps1`.
* The session file is written to a temp file and renamed, so a reader never
  sees half a token.

Exit codes: 0 ready, 2 credentials missing, 3 refused, 4 network, 5 write failed.

## Linux / the AWS server

Put the values in the environment of the service user (for example a
root-only `EnvironmentFile=` in a systemd unit), or install `keyring`. A timer:

```ini
# /etc/systemd/system/altair-fyers-login.service
[Service]
Type=oneshot
User=altair
EnvironmentFile=/etc/altair/fyers.env          # chmod 600, never in git
ExecStart=/usr/bin/python3 /opt/altair/broker/tools/fyers_auto_login.py --root /opt/altair

# /etc/systemd/system/altair-fyers-login.timer
[Timer]
OnCalendar=Mon..Fri 03:00 UTC                  # 08:30 IST
Persistent=true
[Install]
WantedBy=timers.target
```
