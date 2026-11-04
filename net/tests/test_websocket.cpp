// net/websocket.hpp against hand-built server bytes. Plain main().

#include <net/websocket.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

using namespace altair::net;
using Bytes = std::vector<std::uint8_t>;

// A server frame: never masked.
Bytes server_frame(std::uint8_t op, const std::string& payload, bool fin = true) {
    Bytes f;
    f.push_back(static_cast<std::uint8_t>((fin ? 0x80 : 0) | op));
    const std::size_t n = payload.size();
    if (n < 126) {
        f.push_back(static_cast<std::uint8_t>(n));
    } else if (n <= 0xFFFF) {
        f.push_back(126);
        f.push_back(static_cast<std::uint8_t>(n >> 8));
        f.push_back(static_cast<std::uint8_t>(n));
    } else {
        f.push_back(127);
        for (int i = 7; i >= 0; --i) f.push_back(static_cast<std::uint8_t>(std::uint64_t{n} >> (8 * i)));
    }
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

// Unmask the first client frame in `b` (after the upgrade request).
bool client_frame(const Bytes& b, std::size_t at, std::uint8_t& op, std::string& payload) {
    if (b.size() < at + 6) return false;
    op = b[at] & 0x0F;
    if (!(b[at + 1] & 0x80)) return false;   // must be masked
    std::size_t len = b[at + 1] & 0x7F, hdr = 2;
    if (len == 126) { len = (std::size_t{b[at + 2]} << 8) | b[at + 3]; hdr = 4; }
    const std::uint8_t* mask = b.data() + at + hdr;
    payload.clear();
    for (std::size_t i = 0; i < len; ++i) payload += static_cast<char>(b[at + hdr + 4 + i] ^ mask[i & 3]);
    return true;
}

Bytes accept_answer(const std::string& key, const std::string& accept_override = {}) {
    const std::string a = accept_override.empty() ? ws_accept_for(key) : accept_override;
    const std::string s = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: " + a + "\r\n\r\n";
    return Bytes(s.begin(), s.end());
}

struct Got {
    std::vector<std::pair<std::uint8_t, std::string>> msgs;
    void operator()(std::uint8_t op, const std::uint8_t* p, std::size_t n) {
        msgs.emplace_back(op, std::string(reinterpret_cast<const char*>(p), n));
    }
};

} // namespace

int main() {
    std::puts("WebSocket client");
    check(ws_accept_for("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "RFC 6455 accept example");

    WebSocketClient ws({"socket.fyers.in", "/hsm/v1-5/prod", "APPID:TOKEN"}, 42);
    const std::string req(ws.out().begin(), ws.out().end());
    check(req.rfind("GET /hsm/v1-5/prod HTTP/1.1\r\n", 0) == 0 && req.find("Host: socket.fyers.in\r\n") != std::string::npos &&
              req.find("Sec-WebSocket-Key: " + ws.key() + "\r\n") != std::string::npos &&
              req.find("Authorization: APPID:TOKEN\r\n") != std::string::npos && req.ends_with("\r\n\r\n"),
          "the upgrade request carries host, path, key and authorization");
    ws.out().clear();

    Got got;
    Bytes stream = accept_answer(ws.key());
    const std::string big(70000, 'x'), mid(300, 'm');
    for (const auto& f : {server_frame(ws_op::binary, "\x01\x02\x03"), server_frame(ws_op::text, mid),
                          server_frame(ws_op::binary, big), server_frame(ws_op::text, "frag-", false),
                          server_frame(ws_op::ping, "hb"), server_frame(ws_op::cont, "ment", true)})
        stream.insert(stream.end(), f.begin(), f.end());
    for (std::uint8_t b : stream) ws.feed(&b, 1, got);   // the worst split: one byte at a time
    check(ws.open(), "the 101 answer opens the socket");
    check(got.msgs.size() == 4, "four messages out of six frames");
    check(got.msgs.size() == 4 && got.msgs[0] == std::make_pair(ws_op::binary, std::string("\x01\x02\x03")) &&
              got.msgs[1].second == mid && got.msgs[2].second == big && got.msgs[3] == std::make_pair(ws_op::text, std::string("frag-ment")),
          "7-bit, 16-bit and 64-bit lengths and a fragmented message, a ping between the fragments");
    std::uint8_t op = 0;
    std::string pl;
    check(client_frame(ws.out(), 0, op, pl) && op == ws_op::pong && pl == "hb", "the ping is answered with a masked pong");
    ws.out().clear();

    ws.send_text("{\"T\":\"SUB_L2\"}");
    check(client_frame(ws.out(), 0, op, pl) && op == ws_op::text && pl == "{\"T\":\"SUB_L2\"}", "a text frame goes out masked");
    ws.out().clear();
    const std::string long_out(1000, 'q');
    ws.send_binary(reinterpret_cast<const std::uint8_t*>(long_out.data()), long_out.size());
    check(client_frame(ws.out(), 0, op, pl) && op == ws_op::binary && pl == long_out, "a 16-bit-length binary frame goes out masked");
    ws.out().clear();

    const Bytes close = server_frame(ws_op::close, std::string("\x03\xe8", 2));
    ws.feed(close.data(), close.size(), got);
    check(ws.state() == WebSocketClient::State::Closed && client_frame(ws.out(), 0, op, pl) && op == ws_op::close,
          "a server close is answered and ends the session");

    {
        WebSocketClient bad({"h", "/", "", 16u << 20}, 1);
        Got g;
        const Bytes a = accept_answer(bad.key(), "AAAAAAAAAAAAAAAAAAAAAAAAAAA=");
        bad.feed(a.data(), a.size(), g);
        check(bad.state() == WebSocketClient::State::Failed, "a wrong Sec-WebSocket-Accept is refused");
    }
    {
        WebSocketClient bad({"h", "/", "", 16u << 20}, 1);
        Got g;
        const std::string s = "HTTP/1.1 401 Unauthorized\r\n\r\n";
        bad.feed(reinterpret_cast<const std::uint8_t*>(s.data()), s.size(), g);
        check(bad.state() == WebSocketClient::State::Failed && bad.error().find("401") != std::string::npos,
              "a refused upgrade fails with the status");
    }
    {
        WebSocketClient bad({"h", "/", "", 16u << 20}, 1);
        Got g;
        Bytes a = accept_answer(bad.key());
        a.insert(a.end(), {0x82, 0x81, 1, 2, 3, 4, 5});   // masked by the server
        bad.feed(a.data(), a.size(), g);
        check(bad.state() == WebSocketClient::State::Failed, "a masked server frame is a protocol error");
    }

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
