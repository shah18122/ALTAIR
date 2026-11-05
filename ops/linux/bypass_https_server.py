"""A kept-alive HTTPS/1.1 echo server for ops/linux/bypass_https_netns_test.sh.

GET answers {"s":"ok"}; POST echoes the body; GET /close answers and closes.
Content-Length and chunked replies alternate, as Cloudflare sends either.
"""
import http.server
import ssl
import sys


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    n = 0

    def log_message(self, *a):
        pass

    def reply(self, body: bytes, close=False):
        H.n += 1
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        if close:
            self.send_header("Connection", "close")
        if H.n % 2:
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            half = len(body) // 2
            for part in (body[:half], body[half:]):
                if part:
                    self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
        if close:
            self.close_connection = True

    def do_GET(self):
        self.reply(b'{"s":"ok"}', close=self.path == "/close")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        self.reply(b'{"echo":' + self.rfile.read(n) + b"}")


host, port, cert, key = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
srv = http.server.ThreadingHTTPServer((host, port), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
srv.timeout = 30
srv.serve_forever()
