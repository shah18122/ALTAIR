#!/usr/bin/env bash
# ops/linux/dpdk_netns_test.sh -- test the DPDK path end to end, safely.
#
# Two private network namespaces joined by a veth pair; the real NIC is never
# touched. Side B's veth has NO kernel address, as a DPDK-owned NIC has none:
# altair_dpdk_tls_client drives it through DPDK's net_af_packet driver,
# answers ARP for 10.77.0.2 itself, resolves the server's MAC by ARP, and runs
# ALTAIR's TCP and TLS 1.3 to an OpenSSL server on side A over a link that
# loses 2% of packets and reorders 10%. The 512 KiB answer must match.
#
#   sudo ops/linux/dpdk_netns_test.sh [build/xdp]
#
# (Build with -DALTAIR_DPDK=ON. Uses a few MB of the 2 MiB hugepages.)

set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
build="${1:-$here/../../build/xdp}"
client="$build/net/altair_dpdk_tls_client"
[ "$(id -u)" = 0 ] || { echo "run it with sudo" >&2; exit 2; }
[ -x "$client" ] || { echo "build it first: cmake -B build/xdp -DALTAIR_DPDK=ON && ninja -C build/xdp altair_dpdk_tls_client" >&2; exit 2; }

a=altair-da; b=altair-db
work="$(mktemp -d)"
cleanup() { ip netns del "$a" 2>/dev/null || true; ip netns del "$b" 2>/dev/null || true; rm -rf "$work"; }
trap cleanup EXIT
ip netns del "$a" 2>/dev/null || true; ip netns del "$b" 2>/dev/null || true

ip netns add "$a"; ip netns add "$b"
ip link add va netns "$a" type veth peer name vb netns "$b"
ip -n "$a" addr add 10.77.0.1/24 dev va
ip -n "$a" link set va up; ip -n "$b" link set vb up
ip -n "$a" link set lo up; ip -n "$b" link set lo up
# Real NICs hand over complete checksums and frames no larger than the MTU.
ip netns exec "$a" ethtool -K va tx off >/dev/null
ip netns exec "$b" ethtool -K vb gro off rx off >/dev/null 2>&1 || true
ip netns exec "$a" tc qdisc add dev va root netem loss 2% reorder 10% 50% delay 1ms

openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=altair.test \
    -keyout "$work/key.pem" -out "$work/cert.pem" >/dev/null 2>&1
head -c $((512 << 10)) /dev/urandom > "$work/answer"
ip netns exec "$a" python3 -I "$here/xdp_tls_server.py" 10.77.0.1 4443 "$work/cert.pem" "$work/key.pem" "$work/answer" \
    > "$work/server.log" 2>&1 &
srvpid=$!
sleep 0.5
ip netns exec "$b" "$client" --eal "--no-pci --in-memory --log-level=lib.*:error --vdev=net_af_packet0,iface=vb" \
    --port net_af_packet0 --local-ip 10.77.0.2 --remote 10.77.0.1:4443 --host altair.test --ca "$work/cert.pem" \
    --request 'GET / HTTP/1.0\r\n\r\n' --seconds 20 --dump "$work/got" || true
wait "$srvpid" || true
cat "$work/server.log"
if cmp -s "$work/answer" "$work/got"; then
    echo "PASS: TLS over ALTAIR's own TCP on a DPDK port, ARP its own, 512 KiB answer byte for byte, lossy link"
else
    echo "FAIL: the answer differs ($(stat -c %s "$work/got" 2>/dev/null || echo 0) of $((512 << 10)) bytes)" >&2
    exit 1
fi
