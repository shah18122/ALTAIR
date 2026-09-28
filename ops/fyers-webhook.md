# FYERS webhook deployment

The Altair receiver intentionally listens only on `127.0.0.1`; it is not a
public TLS server. Put nginx, Caddy, IIS or an equivalent TLS reverse proxy in
front of it.

## FYERS dashboard values

- Redirect URL: `https://altair.thesmitshah.com/fyers/callback`
- Webhook URL: `https://altair.thesmitshah.com/fyers/callback`
- Webhook secret: generate a new secret locally; do **not** use the example
  `69hsd`, and do not commit the real value.
- Enable only the required order statuses: Pending, Rejected, Cancelled and
  Traded.

The supplied `146.75.205.0` must be checked with the hosting provider before
activation. An address ending in `.0` is commonly a subnet/network address and
may not be assignable to a host. Enter the actual public static egress/host IP
that FYERS will observe, not a private LAN address.

## Local receiver

```powershell
$env:ALTAIR_FYERS_WEBHOOK_SECRET = '<generate-a-long-random-secret>'
New-Item -ItemType Directory -Force data | Out-Null
& .\build\net\app\altair_fyers_webhook.exe 8787
```

Keep this process running under a service manager on the public host. Starting
the executable on a developer workstation does not make the Cloudflare-hosted
domain reachable. A 502 from FYERS means the public reverse proxy is reachable
but its configured upstream is unavailable; check that the process is running,
that localhost:8787 is listening on the same machine as the proxy, and that the
proxy target is not the developer workstation.

The receiver accepts only `POST /fyers/callback`, requires the
`X-Fyers-Webhook-Secret` header, caps the body at 64 KiB, accepts only the four
selected statuses, and writes only redacted status/order-presence records to
`data/fyers_webhook_events.jsonl`. **Verify the exact FYERS webhook delivery
contract before activation:** this receiver currently expects the configured
secret in `X-Fyers-Webhook-Secret`; if FYERS documents a different header or
signature scheme, adapt the boundary before accepting production events. Do not
assume that the dashboard's Secret field automatically maps to this header.

## nginx example

```nginx
server {
    listen 443 ssl;
    server_name altair.thesmitshah.com;

    # Configure certificate/key and TLS policy here.
    location = /fyers/callback {
        proxy_pass http://127.0.0.1:8787/fyers/callback;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        # The upstream provider must supply this header, or the boundary must be
        # adapted to the provider's documented signature scheme.
        proxy_set_header X-Fyers-Webhook-Secret $http_x_fyers_webhook_secret;
        proxy_request_buffering on;
        client_max_body_size 64k;
    }
}
```

Do not expose port 8787 to the internet. Configure a service manager to restart
the receiver, restrict `data/fyers_webhook_events.jsonl`, rotate the secret if
it is exposed, and test the proxy with a synthetic payload before activating
the FYERS dashboard entry. No order placement is implemented by this receiver.

## Local smoke test

```powershell
Invoke-WebRequest http://127.0.0.1:8787/fyers/callback `
  -Method Post `
  -Headers @{ 'X-Fyers-Webhook-Secret' = $env:ALTAIR_FYERS_WEBHOOK_SECRET } `
  -ContentType 'application/json' `
  -Body '{"status":"Traded","order_id":"local-test"}'
```

Expected response: HTTP 200 and `ok`. This proves only the local receiver; it
does not prove DNS, Cloudflare, TLS or remote reverse-proxy reachability.

## Cloudflare Tunnel origin for this PC

Because Apache/XAMPP is the public-facing local web server on this machine, set
the Cloudflare Published Application origin to:

```text
http://127.0.0.1:80
```

Do **not** point the tunnel directly at `127.0.0.1:8787`. Apache owns port 80
and proxies `/fyers/callback` to the private receiver on port 8787. The local
Apache → receiver chain was tested and returned HTTP 200 / `ok`.