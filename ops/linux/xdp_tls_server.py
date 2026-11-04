#!/usr/bin/env python3
"""A one-shot TLS server for ops/linux/xdp_netns_test.sh: accept one client,
read its request line, send PAYLOAD_FILE, close. Standard library only.

  xdp_tls_server.py ADDR PORT CERT KEY PAYLOAD_FILE
"""
import socket
import ssl
import sys

addr, port, cert, key, path = sys.argv[1:6]
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)
payload = open(path, "rb").read()
with socket.create_server((addr, int(port))) as srv:
    srv.settimeout(20)
    conn, peer = srv.accept()
    with ctx.wrap_socket(conn, server_side=True) as tls:
        req = b""
        while b"\r\n\r\n" not in req:
            chunk = tls.recv(4096)
            if not chunk:
                break
            req += chunk
        tls.sendall(payload)
        print(f"served {len(payload)} bytes to {peer[0]}:{peer[1]} over {tls.version()} after {req.split(b' ')[0].decode()}")
