#!/usr/bin/env bash
# ops/linux/xdp_netns_test.sh -- test the AF_XDP receive path end to end, safely.
#
# Builds two private network namespaces joined by a veth pair, so nothing
# touches the server's real network card (enp39s0), SSH, DCV or the recorder.
# A sender cuts a 1 MiB stream into shuffled, duplicated, overlapping TCP
# segments plus decoy frames of another flow; altair_xdp_rx receives the flow
# through XDP + AF_XDP and must rebuild the stream byte for byte while the
# filter passes every decoy to the kernel.
#
#   sudo ops/linux/xdp_netns_test.sh [build/xdp] [generic|native]
#
# Exit 0 when the stream matches. The namespaces are removed on exit.

set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
build="${1:-$here/../../build/xdp}"
mode="${2:-generic}"
rx="$build/net/altair_xdp_rx"
obj="$build/net/altair_flow.bpf.o"
[ "$(id -u)" = 0 ] || { echo "run it with sudo" >&2; exit 2; }
[ -x "$rx" ] && [ -f "$obj" ] || { echo "build it first: cmake -B build/xdp -DALTAIR_KERNEL_BYPASS=ON && ninja -C build/xdp altair_xdp_rx" >&2; exit 2; }

a=altair-xa; b=altair-xb
work="$(mktemp -d)"
cleanup() { ip netns del "$a" 2>/dev/null || true; ip netns del "$b" 2>/dev/null || true; rm -rf "$work"; }
trap cleanup EXIT
cleanup; work="$(mktemp -d)"

ip netns add "$a"; ip netns add "$b"
ip link add va netns "$a" type veth peer name vb netns "$b"
ip -n "$a" addr add 10.77.0.1/24 dev va; ip -n "$a" link set va up
ip -n "$b" addr add 10.77.0.2/24 dev vb; ip -n "$b" link set vb up
ip -n "$a" link set lo up; ip -n "$b" link set lo up
if [ "$mode" = native ]; then
    # veth runs native XDP on the receiver only when the peer side has XDP too.
    ip -n "$a" link set va xdpgeneric off 2>/dev/null || true
    flag=--native
else
    flag=
fi
mac_a="$(ip -n "$a" -br link show va | awk '{print $3}')"
mac_b="$(ip -n "$b" -br link show vb | awk '{print $3}')"

head -c $((1 << 20)) /dev/urandom > "$work/payload"
ip netns exec "$b" "$rx" --iface vb --remote 10.77.0.1:4443 --local-port 50000 --seconds 10 \
    --bpf "$obj" --dump "$work/got" $flag > "$work/rx.log" 2>&1 &
rxpid=$!
for _ in $(seq 50); do grep -q steering "$work/rx.log" 2>/dev/null && break; sleep 0.1; done
grep -q steering "$work/rx.log" || { cat "$work/rx.log"; echo "FAIL: receiver did not start" >&2; exit 1; }

ip netns exec "$a" python3 -I "$here/xdp_netns_send.py" va "$mac_a" "$mac_b" 10.77.0.1 4443 10.77.0.2 50000 "$work/payload"
wait "$rxpid" || true
cat "$work/rx.log"
if cmp -s "$work/payload" "$work/got"; then
    echo "PASS: 1 MiB stream rebuilt byte for byte through AF_XDP ($mode mode)"
else
    echo "FAIL: the received stream differs ($(stat -c %s "$work/got" 2>/dev/null || echo 0) of $((1 << 20)) bytes)" >&2
    exit 1
fi
grep -qE 'passed to the kernel\)' "$work/rx.log"
