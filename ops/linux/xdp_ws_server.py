#!/usr/bin/env python3
"""A TLS WebSocket server for ops/linux/xdp_netns_test.sh (part 3), standard
library only: accept one client, check its upgrade and its masked "SUB"
frame, then stream COUNT binary messages of mixed sizes (7-bit, 16-bit and
64-bit lengths, like a feed's ticks and snapshots), counting the client's
pings meanwhile, and close. What it sent is written to RECORD in the format
altair_bypass_ws --dump writes (4-byte little-endian length, then bytes).

  xdp_ws_server.py ADDR PORT CERT KEY RECORD [COUNT]
"""
import base64
import hashlib
import random
import socket
import ssl
import struct
import sys
import threading
import time

addr, port, cert, key, record = sys.argv[1:6]
count = int(sys.argv[6]) if len(sys.argv) > 6 else 3000
GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def read_exact(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError
        b += c
    return b


def read_frame(s):
    h = read_exact(s, 2)
    op, masked, n = h[0] & 0x0F, h[1] & 0x80, h[1] & 0x7F
    if n == 126:
        n = struct.unpack("!H", read_exact(s, 2))[0]
    elif n == 127:
        n = struct.unpack("!Q", read_exact(s, 8))[0]
    if not masked:
        raise ValueError("client frame not masked")
    m = read_exact(s, 4)
    p = bytes(b ^ m[i & 3] for i, b in enumerate(read_exact(s, n)))
    return op, p


def frame(op, p):
    n = len(p)
    if n < 126:
        h = struct.pack("!BB", 0x80 | op, n)
    elif n <= 0xFFFF:
        h = struct.pack("!BBH", 0x80 | op, 126, n)
    else:
        h = struct.pack("!BBQ", 0x80 | op, 127, n)
    return h + p


ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)
rng = random.Random(7)
with socket.create_server((addr, int(port))) as srv:
    srv.settimeout(20)
    raw, peer = srv.accept()
    s = ctx.wrap_socket(raw, server_side=True)
    req = b""
    while b"\r\n\r\n" not in req:
        req += s.recv(4096)
    head = req.decode()
    k = [l.split(":", 1)[1].strip() for l in head.split("\r\n") if l.lower().startswith("sec-websocket-key:")][0]
    acc = base64.b64encode(hashlib.sha1(k.encode() + GUID).digest()).decode()
    s.sendall(f"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
              f"Sec-WebSocket-Accept: {acc}\r\n\r\n".encode())
    op, p = read_frame(s)
    assert op == 2 and p == b"SUB", (op, p)
    pings = 0
    closed = threading.Event()

    def reader():
        global pings
        try:
            while True:
                o, q = read_frame(s)
                if o == 2 and q == b"PING":
                    pings += 1
                if o == 8:
                    closed.set()
                    return
        except Exception:
            closed.set()

    threading.Thread(target=reader, daemon=True).start()
    with open(record, "wb") as rec:
        for i in range(count):
            r = rng.random()
            size = 70000 if r < 0.002 else rng.randint(126, 2000) if r < 0.3 else rng.randint(8, 125)
            msg = struct.pack("<I", i) + rng.randbytes(size - 4)
            s.sendall(frame(2, msg))
            rec.write(struct.pack("<I", len(msg)) + msg)
            if i % 300 == 0:
                time.sleep(0.4)   # long enough for the client's 1-second pings to arrive
    s.sendall(frame(8, struct.pack("!H", 1000)))
    closed.wait(5)
    print(f"sent {count} messages over {s.version()}; the client pinged {pings} times")
