#!/usr/bin/env bash
# ops/linux/feed_bypass.sh -- put the recorder's FYERS feed on kernel bypass, or take it off.
#
# "on" writes ALTAIR_FEED_BYPASS / ALTAIR_FEED_BYPASS_IP to /etc/altair/altair.env;
# the next recorder run (08:50 IST, or 90 s after a boot) starts
# altair_price_service with --bypass: the HSM feed over AF_XDP and ALTAIR's own
# TCP/TLS on that interface, in generic XDP mode (no link reset), steering only
# its own connection. If the bypass cannot start, the price service carries the
# feed on the kernel socket instead and says so in its log. A running recorder
# is never touched.
#
#   sudo ops/linux/feed_bypass.sh               show the setting
#   sudo ops/linux/feed_bypass.sh on enp39s0    the interface carrying the default route
#   sudo ops/linux/feed_bypass.sh off

set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
env_file=/etc/altair/altair.env
[ "$(id -u)" = 0 ] || { echo "run it with sudo" >&2; exit 2; }
mkdir -p /etc/altair
touch "$env_file"

case "${1:-}" in
    "")
        grep -E '^ALTAIR_FEED_BYPASS' "$env_file" || echo "the feed uses the kernel socket (bypass off)"
        ;;
    on)
        ifc="${2:-}"
        [ -n "$ifc" ] && [ -e "/sys/class/net/$ifc" ] || { echo "usage: feed_bypass.sh on IFACE (an interface that exists)" >&2; exit 2; }
        ip="$(ip -4 -br addr show dev "$ifc" | awk '{print $3}' | cut -d/ -f1)"
        [ -n "$ip" ] || { echo "$ifc has no IPv4 address" >&2; exit 2; }
        cache="$root/build/linux/CMakeCache.txt"
        if ! grep -q '^ALTAIR_KERNEL_BYPASS:BOOL=ON' "$cache" 2>/dev/null || [ ! -f "$root/build/linux/net/altair_flow.bpf.o" ]; then
            echo "refused: build/linux has no kernel bypass yet; run sudo ops/setup.sh first (outside market hours)" >&2
            exit 3
        fi
        sed -i '/^ALTAIR_FEED_BYPASS/d' "$env_file"
        printf 'ALTAIR_FEED_BYPASS=%s\nALTAIR_FEED_BYPASS_IP=%s\n' "$ifc" "$ip" >> "$env_file"
        echo "on: the next recorder run streams FYERS over $ifc ($ip) by kernel bypass, kernel socket as fallback"
        ;;
    off)
        sed -i '/^ALTAIR_FEED_BYPASS/d' "$env_file"
        echo "off: the next recorder run uses the kernel socket"
        ;;
    *)
        echo "usage: feed_bypass.sh [on IFACE | off]" >&2
        exit 2
        ;;
esac
