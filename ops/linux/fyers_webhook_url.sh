#!/usr/bin/env bash
# Prints the webhook URL to paste into the FYERS API dashboard (your app ->
# Webhook): https://altair.thesmitshah.com/fyers/callback?key=<secret>.
# The key is the secret: paste it only there. Statuses: tick all four.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
sec="$root/data/fyers_webhook_secret.txt"
[ -s "$sec" ] || { echo "no $sec yet: sudo ops/install.sh --go" >&2; exit 2; }
echo "https://altair.thesmitshah.com/fyers/callback?key=$(head -n1 "$sec" | tr -d '\r\n')"
