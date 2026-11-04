#!/usr/bin/env python3
"""Send a TCP stream as raw Ethernet frames, the way a network would deliver it.

Used by ops/linux/xdp_netns_test.sh inside a private network namespace: the
stream from SRC:SPORT to DST:DPORT is cut into segments, shuffled, partly
duplicated and overlapped, with a SYN-ACK first and a FIN last, plus decoy
frames of another flow that the XDP filter must pass to the kernel. Standard
library only. Needs root (AF_PACKET).

  xdp_netns_send.py IFACE SRC_MAC DST_MAC SRC_IP SPORT DST_IP DPORT PAYLOAD_FILE [SEED]
"""

import random
import socket
import struct
import sys
import time


def csum(data: bytes) -> int:
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def frame(smac, dmac, sip, dip, sport, dport, seq, flags, payload=b""):
    tcp_wo = struct.pack("!HHIIBBHHH", sport, dport, seq, 4242, 5 << 4, flags, 65535, 0, 0)
    pseudo = sip + dip + struct.pack("!BBH", 0, 6, len(tcp_wo) + len(payload))
    tcp = tcp_wo[:16] + struct.pack("!H", csum(pseudo + tcp_wo + payload)) + tcp_wo[18:]
    total = 20 + len(tcp) + len(payload)
    ip_wo = struct.pack("!BBHHHBBH4s4s", 0x45, 0, total, 1, 0x4000, 64, 6, 0, sip, dip)
    ip = ip_wo[:10] + struct.pack("!H", csum(ip_wo)) + ip_wo[12:]
    return dmac + smac + b"\x08\x00" + ip + tcp + payload


def main() -> int:
    if len(sys.argv) < 9:
        print(__doc__, file=sys.stderr)
        return 2
    iface, smac, dmac, sip, sport, dip, dport, path = sys.argv[1:9]
    rng = random.Random(int(sys.argv[9]) if len(sys.argv) > 9 else 20261104)
    smac = bytes.fromhex(smac.replace(":", ""))
    dmac = bytes.fromhex(dmac.replace(":", ""))
    sip, dip = socket.inet_aton(sip), socket.inet_aton(dip)
    sport, dport = int(sport), int(dport)
    data = open(path, "rb").read()
    isn = rng.randrange(1 << 32)

    segs = []
    at = 0
    while at < len(data):
        n = min(rng.randint(200, 1400), len(data) - at)
        segs.append((at, n))
        if rng.random() < 0.15:
            segs.append((at, n))                                   # retransmit
        if rng.random() < 0.10 and at >= 100:
            segs.append((at - 100, min(n + 100, 1400)))   # overlapping re-send
        at += n
    # Reorder within a bounded window, like a real network (no segment moves
    # more than ~8 places), so a receiver with a normal set of slots keeps up.
    keyed = sorted(range(len(segs)), key=lambda i: i + rng.uniform(0, 8))
    segs = [segs[i] for i in keyed]

    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
    s.bind((iface, 0))
    send = lambda f: s.send(f)
    send(frame(smac, dmac, sip, dip, sport, dport, isn, 0x12))     # SYN-ACK
    decoys = 0
    for off, n in segs:
        send(frame(smac, dmac, sip, dip, sport, dport, (isn + 1 + off) & 0xFFFFFFFF, 0x18, data[off:off + n]))
        if rng.random() < 0.2:
            send(frame(smac, dmac, sip, dip, sport, dport + 1, 1, 0x18, b"decoy"))   # another flow: kernel's
            decoys += 1
    send(frame(smac, dmac, sip, dip, sport, dport, (isn + 1 + len(data)) & 0xFFFFFFFF, 0x11))   # FIN
    time.sleep(0.2)
    print(f"sent {len(data)} bytes as {len(segs)} segments (+SYN-ACK, FIN, {decoys} decoys)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
