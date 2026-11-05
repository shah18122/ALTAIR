#!/usr/bin/env bash
# ops/linux/bypass_https_netns_test.sh -- the bypass order line end to end, safely.
#
# Two private namespaces joined by a veth pair; no real NIC is touched. Side B's
# veth has NO kernel address, as the DPDK spare has none: the test drives it
# through a raw packet socket, answers ARP for 10.77.0.2 itself, resolves the
# server by ARP, and runs ALTAIR's TCP + TLS + kept-alive HTTP/1.1 against a
# Python HTTPS server on side A over a link that loses 2 % and reorders 10 %.
#
# Then the feed: the WebSocket path (net/bypass_ws_link.hpp) on the same link.
#
#   sudo ops/linux/bypass_https_netns_test.sh [build/linux]

set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
build="${1:-$here/../../build/linux}"
t="$build/net/altair_bypass_https_test"
[ "$(id -u)" = 0 ] || { echo "run it with sudo" >&2; exit 2; }
[ -x "$t" ] || { echo "build it first: ninja -C $build altair_bypass_https_test" >&2; exit 2; }

a=altair-ha; b=altair-hb
work="$(mktemp -d)"
cleanup() { [ -n "${srvpid:-}" ] && kill "$srvpid" 2>/dev/null; ip netns del "$a" 2>/dev/null || true; ip netns del "$b" 2>/dev/null || true; rm -rf "$work"; }
trap cleanup EXIT
ip netns del "$a" 2>/dev/null || true; ip netns del "$b" 2>/dev/null || true
ip netns add "$a"; ip netns add "$b"
ip link add va netns "$a" type veth peer name vb netns "$b"
ip -n "$a" addr add 10.77.0.1/24 dev va
ip -n "$a" link set va up; ip -n "$b" link set vb up
ip -n "$a" link set lo up; ip -n "$b" link set lo up
ip netns exec "$a" ethtool -K va tx off >/dev/null 2>&1 || true
ip netns exec "$b" ethtool -K vb gro off rx off >/dev/null 2>&1 || true
ip netns exec "$a" tc qdisc add dev va root netem loss 2% reorder 10% 50% delay 1ms 2>/dev/null \
    || echo "(no netem in this kernel: the link is lossless)"

openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=altair.test -addext subjectAltName=DNS:altair.test \
    -keyout "$work/key.pem" -out "$work/cert.pem" >/dev/null 2>&1
ip netns exec "$a" python3 -I "$here/bypass_https_server.py" 10.77.0.1 4443 "$work/cert.pem" "$work/key.pem" &
srvpid=$!
sleep 0.7
ip netns exec "$b" "$t" --live vb 10.77.0.2 10.77.0.1 10.77.0.1 4443 altair.test "$work/cert.pem" 20

# The feed path on the same kind of link (net/bypass_ws_link.hpp): 3000 WebSocket messages, byte for byte.
ip netns exec "$a" python3 -I "$here/xdp_ws_server.py" 10.77.0.1 4444 "$work/cert.pem" "$work/key.pem" "$work/ws_sent" \
    > "$work/ws_server.log" 2>&1 &
wspid=$!
sleep 0.5
ip netns exec "$b" "$t" --ws vb 10.77.0.2 10.77.0.1 10.77.0.1 4444 altair.test "$work/cert.pem" "$work/ws_got" || true
wait "$wspid" || true
cat "$work/ws_server.log"
if cmp -s "$work/ws_sent" "$work/ws_got"; then
    echo "PASS: the FYERS WebSocket path on a link ALTAIR owns, ARP its own, every message exactly as sent"
else
    echo "FAIL: the WebSocket messages differ" >&2
    exit 1
fi
