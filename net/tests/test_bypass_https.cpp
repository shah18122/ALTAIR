// net/bypass_https: the HTTP reply parser on hand-built bytes; with --live, the
// whole order line (ARP, ALTAIR's TCP, TLS, kept-alive HTTP) over a raw packet
// socket against a real HTTPS server (ops/linux/bypass_https_netns_test.sh).

#include <net/bypass_https.hpp>
#include <net/bypass_ws_link.hpp>
#include <net/links.hpp>

#include <fstream>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace altair::net {
int parse_http_response(const std::uint8_t* data, std::size_t size, BypassHttpsResponse& out, bool& keep_alive,
                        std::size_t& consumed);
}

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

using namespace altair::net;

int parse(const std::string& s, BypassHttpsResponse& r, bool& ka, std::size_t& used) {
    return parse_http_response(reinterpret_cast<const std::uint8_t*>(s.data()), s.size(), r, ka, used);
}

void parser_tests() {
    BypassHttpsResponse r;
    bool ka = false;
    std::size_t used = 0;
    const std::string cl = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\n\r\n{\"s\":\"ok\"}X";
    check(parse(cl.substr(0, 30), r, ka, used) == 0, "a partial head asks for more");
    check(parse(cl.substr(0, cl.size() - 3), r, ka, used) == 0, "a partial body asks for more");
    check(parse(cl, r, ka, used) == 1 && r.status == 200 && r.body == "{\"s\":\"ok\"}X" && ka && used == cl.size(),
          "Content-Length: status, body, keep-alive by default on HTTP/1.1");
    const std::string two = cl + "HTTP/1.1 201";
    check(parse(two, r, ka, used) == 1 && used == cl.size(), "a reply's end is found with more bytes behind it");
    const std::string ch = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    check(parse(ch, r, ka, used) == 1 && r.body == "Wikipedia" && !ka && used == ch.size(), "chunked, and Connection: close");
    check(parse(ch.substr(0, ch.size() - 2), r, ka, used) == 0, "chunked without its last line asks for more");
    const std::string ce = "HTTP/1.1 401 Unauthorized\r\ncontent-length: 0\r\n\r\n";
    check(parse(ce, r, ka, used) == 1 && r.status == 401 && r.body.empty(), "an empty body, header names in any case");
    check(parse("HTTX/1.1 200 OK\r\n\r\n", r, ka, used) == -1, "not HTTP is refused");
    check(parse("HTTP/1.1 200 OK\r\n\r\nabc", r, ka, used) == -1, "no length and not chunked is refused (no read-to-close)");
    const std::string h10 = "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nhi";
    check(parse(h10, r, ka, used) == 1 && !ka, "HTTP/1.0 closes unless it says keep-alive");
}

int live(int argc, char** argv) {
    // --live IFACE LOCAL_IP HOP_IP REMOTE_IP PORT HOST CA [N]
    if (argc < 9) { std::fprintf(stderr, "usage: --live IFACE LOCAL_IP HOP_IP REMOTE_IP PORT HOST CA [N]\n"); return 2; }
    PacketLink link(argv[2]);
    if (!link.ok()) { std::fprintf(stderr, "cannot open a packet socket on %s\n", argv[2]); return 1; }
    BypassHttpsConfig c;
    c.local_ip = argv[3];
    c.next_hop_ip = argv[4];
    c.remote_ip = argv[5];
    c.remote_port = static_cast<std::uint16_t>(std::atoi(argv[6]));
    c.host = argv[7];
    c.ca_file = argv[8];
    const int n = argc > 9 ? std::atoi(argv[9]) : 20;
    BypassHttps h(link, c);
    check(h.warm(std::chrono::milliseconds(5000)), "warm: ARP, TCP and TLS come up");
    if (!h.connected()) std::printf("  why: %s\n", h.last_error().c_str());
    int good = 0;
    for (int i = 0; i < n; ++i) {
        const std::string body = "{\"i\":" + std::to_string(i) + "}";
        const auto r = h.request(i % 2 ? "POST" : "GET", "/api/v3/echo", i % 2 ? body : "", "app:token");
        if (r.ok && r.response.status == 200 && (i % 2 == 0 || r.response.body.find(body) != std::string::npos)) ++good;
        else std::printf("  request %d: %s\n", i, r.why.c_str());
    }
    check(good == n, "every request answered on the kept connection");
    const std::string j = h.json();
    std::printf("  %s\n", j.c_str());
    check(j.find("\"connects\": 1,") != std::string::npos, "one connection for all of them");
    // The server closes after "/close": the next request must reconnect BEFORE it writes.
    const auto c1 = h.request("GET", "/close", "", "app:token");
    check(c1.ok, "a reply with Connection: close is read");
    const auto r2 = h.request("GET", "/api/v3/echo", "", "app:token");
    check(r2.ok, "the next request reconnects first and is answered");
    check(h.json().find("\"connects\": 2,") != std::string::npos, "that was the second connection");
    return 0;
}

int ws(int argc, char** argv) {
    // --ws IFACE LOCAL_IP HOP_IP REMOTE_IP PORT HOST CA DUMP: the feed path on a link
    if (argc < 10) { std::fprintf(stderr, "usage: --ws IFACE LOCAL_IP HOP_IP REMOTE_IP PORT HOST CA DUMP\n"); return 2; }
    PacketLink link(argv[2]);
    if (!link.ok()) { std::fprintf(stderr, "cannot open a packet socket on %s\n", argv[2]); return 1; }
    BypassWsLinkConfig c;
    c.local_ip = argv[3];
    c.next_hop_ip = argv[4];
    c.remote_ip = argv[5];
    c.remote_port = static_cast<std::uint16_t>(std::atoi(argv[6]));
    c.host = argv[7];
    c.ca_file = argv[8];
    c.path = "/feed";
    std::ofstream dump(argv[9], std::ios::binary);
    const BypassFrame sub{'S', 'U', 'B'}, ping{'P', 'I', 'N', 'G'};
    BypassWsStats st;
    std::string err;
    std::size_t got = 0;
    const bool ok = bypass_ws_run_link(
        link, c, {sub},
        [&](const std::uint8_t* p, std::size_t n, std::vector<BypassFrame>&) {
            const std::uint32_t len = static_cast<std::uint32_t>(n);
            dump.write(reinterpret_cast<const char*>(&len), 4);
            dump.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
            ++got;
            return true;
        },
        ping, std::chrono::seconds{1}, std::chrono::seconds{30}, std::chrono::seconds{10}, st, err);
    std::printf("  feed on a link: %zu messages, TCP %.3f ms, TLS %.3f ms, work p50 %u ns p99 %u ns, %llu retransmits%s%s\n",
                st.messages, st.connect_ms, st.tls_ms, st.work_p50_ns, st.work_p99_ns,
                static_cast<unsigned long long>(st.tcp_retransmits), err.empty() ? "" : ", ", err.c_str());
    check(got > 0 && (ok || st.closed_by_server), "the WebSocket feed runs on the link to the server's close");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--ws") == 0) {
        ws(argc, argv);
    } else if (argc > 1 && std::strcmp(argv[1], "--live") == 0) {
        live(argc, argv);
    } else {
        parser_tests();
    }
    std::printf(failures ? "FAILED: %d\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
