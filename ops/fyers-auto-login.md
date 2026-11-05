# FYERS auto-login (TOTP + PIN, no browser)

A FYERS access token lasts one trading day. `broker/tools/fyers_auto_login.py`
gets it every morning without the browser and writes `data/fyers_session.json`
in exactly the format `altair_fyers_login` writes, so the price service,
recorder, desktop and every data CLI use it unchanged.

**Status:** tested offline against a simulated FYERS (`fyers_auto_login_py`
ctest). It has not yet been run against the real FYERS login: the first real
run is the one-time check below.

## One-time setup (the Linux server, through the DCV screen)

1. In FYERS (web or app): **My Account > Security > External 2FA TOTP**, enable
   it and keep the base32 key it shows (the same key your authenticator app
   scans). Your FYERS app on myapi.fyers.in must have the redirect URI you use.
2. Type the values once into ALTAIR's file vault (hidden input, nothing is
   echoed; each value goes to an owner-only file under
   `~/.config/altair/credentials/`):

   ```bash
   python3 broker/tools/fyers_auto_login.py --store
   python3 broker/tools/fyers_auto_login.py --check      # which values are found, never the values
   python3 broker/tools/fyers_auto_login.py --force      # first real login; prints only "session established"
   ```

3. `sudo ops/setup.sh` (or `sudo ops/install.sh --go`) registers the
   **altair-fyers-login** timer, Mon-Fri 08:30 IST and again at 08:40 as a
   backstop (a no-op once today's token works), before the recorder (08:50).
   `journalctl -u altair-fyers-login` shows its last run.

## Where credentials can live

Each value is looked up in this order: environment variable, then the file vault,
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
* Requests carry the headers FYERS's own web login sends (a browser
  User-Agent, Origin/Referer `login.fyers.in`) and keep cookies for the run:
  urllib's default `Python-urllib` agent is what a bot filter answers with 403.
* Network failures, timeouts, 5xx, 403/429 (a firewall), HTML pages and a
  refused `send_login_otp` (no PIN or TOTP spent) are retried with backoff
  (10, 20, 40, 80, 120 s...) for `--retry-minutes` (default 10). The 08:40
  backstop and the recorder (every 5 minutes while it has no session) try again.
* A refused TOTP is retried once, on the next 30 s window (clock skew, or a code
  used a moment ago). A second TOTP refusal or a wrong PIN stops at once and
  writes `data/fyers_login_refused.txt` (`<day> wrong-credential <reason>`): no
  scheduled run tries again that day, because FYERS locks the account after a
  few wrong PINs. Fix the value, then run with `--force`. A refusal of the app
  settings (secret, redirect uri) stops that run but does not lock the day.
* Every refusal logs FYERS's whole answer, the HTTP status and the
  server/firewall headers (secrets masked), so the cause is in the log.
* Programs pick the new token up by themselves: the recorder restarts its feed
  (kernel socket or AF_XDP bypass) when `data/fyers_session.json` changes, and
  the desktop's Terminal starts or restarts its live feed within 30 s.
* Nothing secret is printed or logged: one line per event goes to stdout and
  to `data/logs/altair-<IST day>.log` (component `fyers-login`), with the same
  masking as `app/activity_log.hpp`.
* The session file is written to a temp file and renamed, so a reader never
  sees half a token.

Exit codes: 0 ready, 2 credentials missing, 3 refused, 4 network, 5 write failed.

## Browser login (the redirect URL)

The daily login above is fully server-side: TOTP, PIN, `auth_code`, access
token, profile check, saved. No redirect is ever visited.

For a login from a browser, `altair-fyers-callback.service`
(`broker/tools/fyers_callback_server.py`) is the FYERS app's redirect URL. It
listens on `127.0.0.1:80` only; `sudo ops/linux/fyers_tunnel.sh` installs
cloudflared with the Cloudflare tunnel's token, so
`https://altair.thesmitshah.com/fyers/*` reaches it over an outbound connection
(SSH on 22/443 and DCV are untouched).

* Open `https://altair.thesmitshah.com/fyers/login`, log in to FYERS, and the
  redirect lands on a page that says **Connected to FYERS** (or exactly what
  failed). The token is exchanged, checked and saved to `data/fyers_session.json`;
  the recorder and the desktop pick it up by themselves.
* Only a token for the configured FYERS user id is saved.
* `POST /fyers/callback` (order webhooks) is passed to `altair_fyers_webhook`
  on 127.0.0.1:8787 when it runs (`ops/fyers-webhook.md`).
