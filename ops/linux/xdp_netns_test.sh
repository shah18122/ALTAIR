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
# Part 2 (when altair_xdp_tls_client is built): the full bypass path. ALTAIR's
# own TCP and TLS connect through AF_XDP to Linux's TCP and an OpenSSL server
# (Python's ssl), over a link that loses 2% of packets and reorders 10%
# (tc netem) and drops every 5th segment ALTAIR sends, and must receive a 512 KiB answer byte for byte.
#
# Part 3 (when altair_bypass_ws is built): the FYERS transport. A TLS
# WebSocket over the same lossy link: ALTAIR upgrades, subscribes, pings every
# second, and must record 3000 binary messages (up to 70 KB) exactly as sent.
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

tls="$build/net/altair_xdp_tls_client"
[ -x "$tls" ] || exit 0
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=altair.test \
    -keyout "$work/key.pem" -out "$work/cert.pem" >/dev/null 2>&1
head -c $((512 << 10)) /dev/urandom > "$work/answer"
# A veth leaves TCP checksums for "the NIC" to fill in (CHECKSUM_PARTIAL); a
# real NIC delivers them complete. Make the server side compute them in software.
ip netns exec "$a" ethtool -K va tx off >/dev/null
ip netns exec "$a" tc qdisc add dev va root netem loss 2% reorder 10% 50% delay 1ms
# AF_XDP sends bypass the qdisc, so ALTAIR's own segments are dropped on the
# server's way in instead: every 5th one, which exercises ALTAIR's retransmits.
ip netns exec "$a" tc qdisc add dev va ingress
ip netns exec "$a" tc filter add dev va ingress protocol ip prio 1 u32 match ip protocol 6 0xff \
    action gact pass random determ drop 5
ip netns exec "$a" python3 -I "$here/xdp_tls_server.py" 10.77.0.1 4443 "$work/cert.pem" "$work/key.pem" "$work/answer" \
    > "$work/server.log" 2>&1 &
srvpid=$!
sleep 0.5
ip netns exec "$b" ping -c1 -W1 10.77.0.1 >/dev/null   # the neighbour entry for the next-hop MAC
ip netns exec "$b" "$tls" --iface vb --local-ip 10.77.0.2 --local-port 50001 --remote 10.77.0.1:4443 \
    --host altair.test --ca "$work/cert.pem" --request 'GET / HTTP/1.0\r\n\r\n' --seconds 20 \
    --bpf "$obj" --dump "$work/got_tls" $flag || true
wait "$srvpid" || true
cat "$work/server.log"
if cmp -s "$work/answer" "$work/got_tls"; then
    echo "PASS: TLS over ALTAIR's own TCP through AF_XDP, 512 KiB answer byte for byte, 2% loss + reordering in, every 5th segment out dropped ($mode mode)"
else
    echo "FAIL: the TLS answer differs ($(stat -c %s "$work/got_tls" 2>/dev/null || echo 0) of $((512 << 10)) bytes)" >&2
    exit 1
fi

ws="$build/net/altair_bypass_ws"
[ -x "$ws" ] || exit 0
ip netns exec "$a" python3 -I "$here/xdp_ws_server.py" 10.77.0.1 4444 "$work/cert.pem" "$work/key.pem" "$work/ws_sent" \
    > "$work/ws_server.log" 2>&1 &
wspid=$!
sleep 0.5
ip netns exec "$b" "$ws" --iface vb --local-ip 10.77.0.2 --remote 10.77.0.1:4444 --host altair.test --path /feed \
    --ca "$work/cert.pem" --send SUB --ping PING --seconds 30 --bpf "$obj" --dump "$work/ws_got" $flag || true
wait "$wspid" || true
cat "$work/ws_server.log"
if cmp -s "$work/ws_sent" "$work/ws_got"; then
    echo "PASS: TLS WebSocket over the bypass path, 3000 messages exactly as sent, lossy link ($mode mode)"
else
    echo "FAIL: the WebSocket messages differ" >&2
    exit 1
fi
