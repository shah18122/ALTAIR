#!/usr/bin/env bash
# ops/linux/fyers_tunnel.sh -- carry https://altair.thesmitshah.com/fyers/* to this server.
#
# The domain sits behind Cloudflare, whose tunnel (Zero Trust -> Networks ->
# Tunnels) already sends it to http://127.0.0.1:80, where
# altair-fyers-callback.service answers. cloudflared makes an OUTBOUND
# connection to Cloudflare, so no inbound port is opened: SSH on 22/443 and DCV
# on 8443 are untouched.
#
#   sudo ops/linux/fyers_tunnel.sh     install cloudflared, ask for the tunnel's
#                                      token (hidden input), run it at every boot
#
# The token is the long string in the tunnel's "Install and run a connector"
# command on the Cloudflare dashboard. Stop cloudflared on any other machine
# that used the same tunnel (the old Windows PC), or Cloudflare splits requests.

set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run it with sudo" >&2; exit 2; }
if ss -ltnpH 'sport = :80' | grep -qv python3; then
    echo "port 80 is taken by something other than the callback:" >&2
    ss -ltnpH 'sport = :80' >&2
    exit 3
fi
systemctl enable --now altair-fyers-callback.service
sleep 1
curl -fsS http://127.0.0.1:80/healthz >/dev/null && echo "callback: up on 127.0.0.1:80"

if ! command -v cloudflared >/dev/null; then
    mkdir -p --mode=0755 /usr/share/keyrings
    curl -fsSL https://pkg.cloudflare.com/cloudflare-main.gpg -o /usr/share/keyrings/cloudflare-main.gpg
    echo 'deb [signed-by=/usr/share/keyrings/cloudflare-main.gpg] https://pkg.cloudflare.com/cloudflared any main' \
        > /etc/apt/sources.list.d/cloudflared.list
    apt-get update -qq
    apt-get install -y -qq cloudflared
fi
if systemctl is-active --quiet cloudflared; then
    echo "cloudflared: already running (sudo systemctl status cloudflared)"
else
    read -rsp "Cloudflare tunnel token (hidden): " token; echo
    [ -n "$token" ] || { echo "no token given" >&2; exit 2; }
    cloudflared service install "$token"
    unset token
fi
sleep 3
code="$(curl -s -o /dev/null -w '%{http_code}' https://altair.thesmitshah.com/healthz || true)"
echo "https://altair.thesmitshah.com/healthz -> HTTP ${code:-none} (200 = the tunnel reaches this server)"
echo "browser login: https://altair.thesmitshah.com/fyers/login"
