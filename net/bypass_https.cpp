// net/bypass_https.cpp -- see net/bypass_https.hpp.

#include <net/bypass_https.hpp>

#include <live/latency.hpp>
#include <net/packet.hpp>
#include <net/tls_stream.hpp>
#include <net/user_tcp.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <random>

namespace altair::net {

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

bool resolve4(const std::string& host, std::uint32_t& ip) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), "443", &hints, &res) != 0 || res == nullptr) return false;
    ip = reinterpret_cast<const sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
    ::freeaddrinfo(res);
    return true;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

} // namespace

/// One HTTP/1.1 response from the front of `buf`: Content-Length or chunked.
/// Returns 1 complete (consumed set), 0 more needed, -1 malformed.
int parse_http_response(const std::uint8_t* data, std::size_t size, BypassHttpsResponse& out, bool& keep_alive,
                        std::size_t& consumed) {
    const std::string_view all(reinterpret_cast<const char*>(data), size);
    const std::size_t head_end = all.find("\r\n\r\n");
    if (head_end == std::string_view::npos) return size > (64u << 10) ? -1 : 0;
    const std::string_view head = all.substr(0, head_end);
    const std::size_t eol = head.find("\r\n");
    const std::string_view status_line = head.substr(0, eol);
    if (status_line.rfind("HTTP/1.", 0) != 0 || status_line.size() < 12) return -1;
    const bool http10 = status_line[7] == '0';
    out.status = static_cast<unsigned>(std::atoi(std::string(status_line.substr(9, 3)).c_str()));
    if (out.status < 100 || out.status > 999) return -1;
    long long length = -1;
    bool chunked = false;
    keep_alive = !http10;
    for (std::size_t at = eol == std::string_view::npos ? head.size() : eol + 2; at < head.size();) {
        std::size_t next = head.find("\r\n", at);
        if (next == std::string_view::npos) next = head.size();
        const std::string_view line = head.substr(at, next - at);
        at = next + 2;
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string_view name = trim(line.substr(0, colon)), value = trim(line.substr(colon + 1));
        if (iequals(name, "content-length")) length = std::atoll(std::string(value).c_str());
        else if (iequals(name, "transfer-encoding") && value.find("chunked") != std::string_view::npos) chunked = true;
        else if (iequals(name, "connection")) {
            if (iequals(value, "close")) keep_alive = false;
            else if (iequals(value, "keep-alive")) keep_alive = true;
        }
    }
    std::size_t at = head_end + 4;
    out.body.clear();
    if (out.status == 204 || out.status == 304 || (out.status >= 100 && out.status < 200)) { consumed = at; return 1; }
    if (chunked) {
        for (;;) {
            const std::size_t le = all.find("\r\n", at);
            if (le == std::string_view::npos) return 0;
            const std::string size_text(all.substr(at, le - at));
            char* end = nullptr;
            const unsigned long long n = std::strtoull(size_text.c_str(), &end, 16);
            if (end == size_text.c_str()) return -1;
            if (n > (8ull << 20)) return -1;
            at = le + 2;
            if (n == 0) {   // the trailer ends with an empty line
                const std::size_t te = all.find("\r\n", at);
                if (te == std::string_view::npos) return 0;
                if (te == at) { consumed = at + 2; return 1; }
                const std::size_t tend = all.find("\r\n\r\n", at);
                if (tend == std::string_view::npos) return 0;
                consumed = tend + 4;
                return 1;
            }
            if (size - at < n + 2) return 0;
            out.body.append(all.substr(at, n));
            at += n + 2;
        }
    }
    if (length < 0) return -1;   // a reply read to the close is not one an order can wait for
    if (length > (8ll << 20)) return -1;
    if (size - at < static_cast<std::size_t>(length)) return 0;
    out.body.assign(all.substr(at, static_cast<std::size_t>(length)));
    consumed = at + static_cast<std::size_t>(length);
    return 1;
}

struct BypassHttps::Impl {
    using Send = std::function<bool(const std::uint8_t*, std::size_t)>;
    using Deliver = std::function<void(const std::uint8_t*, std::size_t)>;
    using Tcp = UserTcp<Send, Deliver>;

    struct Conn {
        TcpEndpoints e;
        std::unique_ptr<TlsClient> tls;
        std::unique_ptr<Tcp> tcp;
        std::vector<std::uint8_t> plain;   ///< decrypted, not yet consumed
        std::uint64_t replies = 0;
    };

    Impl(NetLink& l, BypassHttpsConfig c) : link(l), cfg(std::move(c)), rng(std::random_device{}()) {
        my_mac = link.mac();
        ok_ip = parse_ipv4(cfg.local_ip.c_str(), my_ip) && parse_ipv4(cfg.next_hop_ip.c_str(), hop_ip);
    }

    NetLink& link;
    BypassHttpsConfig cfg;
    std::mt19937 rng;
    Mac my_mac{}, hop_mac{};
    std::uint32_t my_ip = 0, hop_ip = 0;
    bool ok_ip = false, have_hop = false;
    std::uint8_t arp_buf[64]{};
    std::unique_ptr<Conn> c;
    std::vector<std::uint8_t> cipher, tmp;
    Clock::time_point last_use{};
    std::uint64_t requests = 0, connects = 0, replaced_stale = 0, replaced_idle = 0, failures = 0, foreign = 0;
    live::LatencyHistogram rtt, connect_h;

    void pump() {
        if (!c) return;
        TlsClient& tls = *c->tls;
        if (!tls.ready()) tls.handshake();
        if (tls.ready()) {
            tmp.clear();
            tls.read(tmp);
            if (!tmp.empty()) c->plain.insert(c->plain.end(), tmp.begin(), tmp.end());
        }
        cipher.clear();
        if (tls.drain_ciphertext(cipher) && (c->tcp->established() || c->tcp->state() == TcpState::CloseWait))
            c->tcp->send(cipher.data(), cipher.size(), now_ns());
    }

    void on_frame(const std::uint8_t* f, std::size_t len) {
        ArpPacket a{};
        if (parse_arp(f, len, a)) {
            if (const std::size_t n = arp_reply_to(arp_buf, a, my_mac, my_ip)) link.send(arp_buf, n);
            if (a.sender_ip == hop_ip) { hop_mac = a.sender_mac; have_hop = true; }
            return;
        }
        if (!c) return;
        TcpSegment s{};
        if (parse_frame(f, len, s) != Parse::Ok || s.flow.src_ip != c->e.remote_ip || s.flow.src_port != htons(c->e.remote_port)
            || s.flow.dst_port != htons(c->e.local_port) || !ipv4_checksum_ok(s) || !tcp_checksum_ok(s)) {
            ++foreign;
            return;
        }
        c->tcp->on_segment(s, now_ns());
        if (c->tcp->established() || c->tcp->state() == TcpState::CloseWait) pump();
    }

    void service() {
        link.poll([this](const std::uint8_t* f, std::size_t n) { on_frame(f, n); });
        if (c) {
            c->tcp->tick(now_ns());
            if (c->tcp->established()) pump();
        }
    }

    bool resolve_hop(Clock::time_point until, std::string& err) {
        if (have_hop) return true;
        for (auto next_ask = Clock::now(); !have_hop && Clock::now() < until;) {
            if (Clock::now() >= next_ask) {
                link.send(arp_buf, arp_request(arp_buf, my_mac, my_ip, hop_ip));
                next_ask = Clock::now() + std::chrono::milliseconds(200);
            }
            service();
        }
        if (!have_hop) err = "no ARP answer from the next hop " + cfg.next_hop_ip;
        return have_hop;
    }

    void drop() {
        if (!c) return;
        if (c->tcp->established() || c->tcp->state() == TcpState::CloseWait) {
            c->tcp->close(now_ns());   // a FIN, not waited for: the next connection uses another port
            link.poll([this](const std::uint8_t* f, std::size_t n) { on_frame(f, n); });
        }
        c.reset();
    }

    bool open(std::chrono::milliseconds timeout, std::string& err) {
        drop();
        if (!ok_ip) { err = "local or next-hop address is not IPv4"; return false; }
        const auto t0 = Clock::now();
        const auto until = t0 + timeout;
        if (!resolve_hop(until, err)) return false;
        auto n = std::make_unique<Conn>();
        n->e.local_mac = my_mac;
        n->e.next_hop_mac = hop_mac;
        n->e.local_ip = my_ip;
        n->e.mss = cfg.mss;
        n->e.remote_port = cfg.remote_port;
        n->e.local_port = static_cast<std::uint16_t>(32768 + rng() % 28000);
        if (cfg.remote_ip.empty() ? !resolve4(cfg.host, n->e.remote_ip) : !parse_ipv4(cfg.remote_ip.c_str(), n->e.remote_ip)) {
            err = "cannot resolve " + (cfg.remote_ip.empty() ? cfg.host : cfg.remote_ip);
            return false;
        }
        TlsClient::Options to;
        to.host = cfg.host;
        to.ca_file = cfg.ca_file;
        to.verify = cfg.verify;
        n->tls = TlsClient::create(to, err);
        if (!n->tls) return false;
        Conn* raw = n.get();
        n->tcp = std::make_unique<Tcp>(
            n->e, Send([this](const std::uint8_t* f, std::size_t len) { return link.send(f, len); }),
            Deliver([raw](const std::uint8_t* p, std::size_t len) { raw->tls->feed(p, len); }));
        c = std::move(n);
        c->tcp->connect(static_cast<std::uint32_t>(rng()), now_ns());
        while (!c->tcp->established()) {
            service();
            if (c->tcp->state() == TcpState::Failed || c->tcp->state() == TcpState::Closed) { err = "TCP connect failed"; break; }
            if (Clock::now() >= until) { err = "TCP connect timed out (no SYN-ACK: is there a public IP on this card?)"; break; }
        }
        if (!err.empty()) { c.reset(); return false; }
        pump();   // the ClientHello
        while (!c->tls->ready()) {
            service();
            if (c->tls->failed()) { err = "TLS: " + c->tls->error(); break; }
            if (!c->tcp->established()) { err = "TCP closed during the TLS handshake"; break; }
            if (Clock::now() >= until) { err = "TLS handshake timed out"; break; }
        }
        if (!err.empty()) { c.reset(); return false; }
        ++connects;
        last_use = Clock::now();
        connect_h.record(std::chrono::duration_cast<std::chrono::nanoseconds>(last_use - t0).count());
        return true;
    }

    bool usable() {
        if (!c) return false;
        service();
        if (!c->tcp->established() || c->tls->failed() || (c->replies > 0 && !c->plain.empty())) {
            ++replaced_stale;
            drop();
            return false;
        }
        if (Clock::now() - last_use > cfg.max_idle) {
            ++replaced_idle;
            drop();
            return false;
        }
        return true;
    }
};

BypassHttps::BypassHttps(NetLink& link, BypassHttpsConfig cfg) : d_(std::make_unique<Impl>(link, std::move(cfg))) {}
BypassHttps::~BypassHttps() { if (d_) d_->drop(); }

void BypassHttps::service() { d_->service(); }
bool BypassHttps::connected() const noexcept { return d_->c != nullptr && d_->c->tcp->established() && d_->c->tls->ready(); }
std::chrono::steady_clock::duration BypassHttps::idle() const noexcept { return Clock::now() - d_->last_use; }
void BypassHttps::close() { d_->drop(); }

bool BypassHttps::warm(std::chrono::milliseconds timeout) {
    if (d_->usable()) return true;
    err_.clear();
    return d_->open(timeout, err_);
}

BypassHttps::Result BypassHttps::request(std::string_view method, std::string_view target, std::string_view body,
                                         std::string_view authorization, std::chrono::milliseconds timeout) {
    Result r;
    Impl& d = *d_;
    if (!d.usable()) {
        err_.clear();
        if (!d.open(timeout, err_)) { ++d.failures; r.why = err_; return r; }   // NotSent: nothing written
    }
    std::string req;
    req.reserve(256 + body.size() + authorization.size());
    req.append(method).append(" ").append(target).append(" HTTP/1.1\r\nHost: ").append(d.cfg.host);
    req.append("\r\nUser-Agent: altair/0.1\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nversion: 3\r\n");
    if (!authorization.empty()) req.append("Authorization: ").append(authorization).append("\r\n");
    if (method != "GET") req.append("Content-Length: ").append(std::to_string(body.size())).append("\r\n");
    req.append("\r\n");
    if (method != "GET") req.append(body);

    r.error = BypassHttpsError::Transport;   // from here on, something may have gone out
    d.c->plain.clear();
    const auto t0 = Clock::now();
    const auto until = t0 + timeout;
    if (!d.c->tls->write(reinterpret_cast<const std::uint8_t*>(req.data()), req.size())) {
        ++d.failures;
        r.why = "TLS write failed";
        d.drop();
        return r;
    }
    d.pump();
    bool keep_alive = true;
    for (;;) {
        d.service();
        if (!d.c) { r.why = "connection lost"; break; }
        std::size_t used = 0;
        const int p = parse_http_response(d.c->plain.data(), d.c->plain.size(), r.response, keep_alive, used);
        if (p == 1) {
            d.c->plain.erase(d.c->plain.begin(), d.c->plain.begin() + static_cast<std::ptrdiff_t>(used));
            r.ok = true;
            break;
        }
        if (p < 0) { r.why = "malformed HTTP reply"; break; }
        if (d.c->tls->failed()) { r.why = "TLS: " + d.c->tls->error(); break; }
        const TcpState s = d.c->tcp->state();
        if (s == TcpState::Failed || s == TcpState::Closed || s == TcpState::CloseWait) { r.why = "the server closed the connection"; break; }
        if (Clock::now() >= until) { r.why = "no reply in time"; break; }
    }
    if (!r.ok) {
        ++d.failures;
        d.drop();
        return r;
    }
    const auto t1 = Clock::now();
    d.rtt.record(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    ++d.requests;
    ++d.c->replies;
    d.last_use = t1;
    if (!keep_alive) d.drop();
    return r;
}

std::string BypassHttps::json() const {
    const Impl& d = *d_;
    return "{\"host\": \"" + d.cfg.host + "\", \"link\": \"bypass\", \"requests\": " + std::to_string(d.requests)
         + ", \"connects\": " + std::to_string(d.connects) + ", \"replaced_stale\": " + std::to_string(d.replaced_stale)
         + ", \"replaced_idle\": " + std::to_string(d.replaced_idle) + ", \"failures\": " + std::to_string(d.failures)
         + ", \"rtt_us\": " + d.rtt.json(1e3) + ", \"connect_us\": " + d.connect_h.json(1e3) + "}";
}

} // namespace altair::net
