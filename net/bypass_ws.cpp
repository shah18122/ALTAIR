// net/bypass_ws.cpp -- see bypass_ws.hpp.

#include <net/bypass_ws.hpp>

#include <net/neighbour.hpp>
#include <net/packet.hpp>
#include <net/tls_stream.hpp>
#include <net/user_tcp.hpp>
#include <net/websocket.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <random>

namespace altair::net {

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

bool resolve4(const std::string& host, std::uint32_t& ip) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    ip = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return true;
}

// Bind a kernel socket to the local address: it reserves the port (an
// ephemeral one when `port` is 0) for as long as ALTAIR owns the flow.
int hold_port(std::uint32_t ip, std::uint16_t& port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = ip;
    socklen_t len = sizeof sa;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len) != 0) {
        ::close(fd);
        return -1;
    }
    port = ntohs(sa.sin_port);
    return fd;
}

struct PortHold {
    int fd = -1;
    ~PortHold() {
        if (fd >= 0) ::close(fd);
    }
};

} // namespace

bool bypass_ws_run(const BypassWsConfig& cfg, const std::vector<BypassFrame>& opening,
                   const std::function<bool(const std::uint8_t*, std::size_t, std::vector<BypassFrame>&)>& on_message,
                   const BypassFrame& ping, std::chrono::seconds ping_every, std::chrono::seconds run_for,
                   std::chrono::seconds idle_limit, BypassWsStats& st, std::string& err,
                   const std::function<bool()>& stop) {
    st = {};
    TcpEndpoints e;
    e.mss = cfg.mss;
    e.remote_port = cfg.remote_port;
    if (!parse_ipv4(cfg.local_ip.c_str(), e.local_ip)) {
        err = "local address '" + cfg.local_ip + "' is not IPv4";
        return false;
    }
    if (cfg.remote_ip.empty() ? !resolve4(cfg.host, e.remote_ip) : !parse_ipv4(cfg.remote_ip.c_str(), e.remote_ip)) {
        err = "cannot resolve " + (cfg.remote_ip.empty() ? cfg.host : cfg.remote_ip);
        return false;
    }
    in_addr ra{};
    ra.s_addr = e.remote_ip;
    if (!interface_mac(cfg.xdp.iface, e.local_mac)) {
        err = "no MAC for " + cfg.xdp.iface;
        return false;
    }
    if (cfg.next_hop_mac.empty() ? !next_hop_mac(cfg.xdp.iface, inet_ntoa(ra), e.next_hop_mac)
                                 : !parse_mac(cfg.next_hop_mac, e.next_hop_mac)) {
        err = "no next-hop MAC on " + cfg.xdp.iface + " (no ARP entry for the peer or the gateway)";
        return false;
    }
    PortHold hold;
    std::uint16_t lport = cfg.local_port;
    hold.fd = hold_port(e.local_ip, lport);
    if (hold.fd < 0) {
        err = "cannot hold local port " + std::to_string(cfg.local_port);
        return false;
    }
    e.local_port = lport;

    auto filter = XdpFilter::attach(cfg.xdp, err);
    if (!filter) return false;
    std::vector<std::unique_ptr<XskQueue>> socks;
    for (int q = 0; q < std::max(1, cfg.queues); ++q) {
        auto s = XskQueue::open(cfg.xdp, static_cast<std::uint32_t>(q), *filter, err);
        if (!s) return false;
        socks.push_back(std::move(s));
    }
    const std::uint16_t rport_n = htons(e.remote_port), lport_n = htons(e.local_port);
    if (!filter->add_flow(e.remote_ip, rport_n, lport_n)) {
        err = "cannot steer the flow";
        return false;
    }

    TlsClient::Options to;
    to.host = cfg.host;
    to.ca_file = cfg.ca_file;
    to.verify = cfg.verify;
    auto tls = TlsClient::create(to, err);
    if (!tls) return false;
    WebSocketClient ws({cfg.host, cfg.path, cfg.authorization, 16u << 20});

    std::vector<std::uint8_t> plain, cipher;
    std::vector<BypassFrame> replies;
    std::vector<std::uint32_t> work;
    work.reserve(1u << 20);
    bool keep = true;
    auto on_ws_message = [&](std::uint8_t op, const std::uint8_t* p, std::size_t n) {
        ++st.messages;
        st.bytes += n;
        if (op != ws_op::binary || !keep) return;
        replies.clear();
        keep = on_message(p, n, replies);
        for (const auto& r : replies) {
            cfg.text ? ws.send_text({reinterpret_cast<const char*>(r.data()), r.size()}) : ws.send_binary(r.data(), r.size());
            ++st.frames_sent;
        }
    };
    auto send = [&](const std::uint8_t* f, std::size_t n) { return socks[0]->send(f, n); };
    auto deliver = [&](const std::uint8_t* p, std::size_t n) { tls->feed(p, n); };
    UserTcp tcp(e, send, deliver);

    // Decrypt and frame whatever TLS has; queue what TLS and WebSocket want sent.
    auto pump = [&](std::uint64_t t) {
        if (!tls->ready()) {
            if (tls->handshake()) {
                if (!ws.out().empty()) {
                    tls->write(ws.out().data(), ws.out().size());   // the upgrade request
                    ws.out().clear();
                }
            }
        }
        if (tls->ready()) {
            plain.clear();
            tls->read(plain);
            if (!plain.empty()) ws.feed(plain.data(), plain.size(), on_ws_message);
            if (!ws.out().empty()) {
                tls->write(ws.out().data(), ws.out().size());
                ws.out().clear();
            }
        }
        cipher.clear();
        if (tls->drain_ciphertext(cipher)) tcp.send(cipher.data(), cipher.size(), t);
    };

    std::uint64_t foreign = 0;
    auto on_frame = [&](const std::uint8_t* f, std::size_t len) {
        const std::uint64_t t0 = now_ns();
        TcpSegment s{};
        if (parse_frame(f, len, s) != Parse::Ok || s.flow.src_ip != e.remote_ip || s.flow.src_port != rport_n ||
            s.flow.dst_port != lport_n || !ipv4_checksum_ok(s) || !tcp_checksum_ok(s)) {
            ++foreign;
            return;
        }
        tcp.on_segment(s, t0);
        if (tcp.established() || tcp.state() == TcpState::CloseWait) pump(t0);
        if (work.size() < work.capacity()) work.push_back(static_cast<std::uint32_t>(now_ns() - t0));
    };

    const std::uint64_t start = now_ns();
    const std::uint64_t deadline = start + static_cast<std::uint64_t>(run_for.count()) * 1'000'000'000ull;
    const std::uint64_t idle_ns = static_cast<std::uint64_t>(idle_limit.count()) * 1'000'000'000ull;
    const std::uint64_t ping_ns = static_cast<std::uint64_t>(ping_every.count()) * 1'000'000'000ull;
    std::uint64_t next_ping = 0, next_check = start;
    bool opened = false, sent_opening = false, closing = false;
    auto ms = [&](std::uint64_t t) { return static_cast<double>(t - start) / 1e6; };
    std::mt19937 rng(std::random_device{}());
    tcp.connect(static_cast<std::uint32_t>(rng()), start);

    while (true) {
        for (auto& s : socks) s->poll(on_frame);
        const std::uint64_t t = now_ns();
        tcp.tick(t);
        if (tcp.state() == TcpState::Failed) {
            err = st.connect_ms < 0 ? "TCP connect failed (no SYN-ACK)" : "TCP connection reset or timed out";
            break;
        }
        if (tcp.state() == TcpState::Closed) break;
        if (tcp.established() && st.connect_ms < 0) {
            st.connect_ms = ms(t);
            pump(t);   // the ClientHello
        }
        if (tls->failed()) {
            err = tls->error();
            break;
        }
        if (tls->ready() && st.tls_ms <= 0) {
            st.tls_ms = ms(t);
            st.tls_cipher = tls->cipher();
            pump(t);
        }
        if (ws.state() == WebSocketClient::State::Failed) {
            err = ws.error();
            break;
        }
        if (ws.open() && !opened) {
            opened = true;
            st.upgrade_ms = ms(t);
            next_ping = t + ping_ns;
        }
        if (opened && !sent_opening) {
            sent_opening = true;
            for (const auto& f : opening) {
                cfg.text ? ws.send_text({reinterpret_cast<const char*>(f.data()), f.size()}) : ws.send_binary(f.data(), f.size());
                ++st.frames_sent;
            }
            pump(t);
        }
        if (ws.state() == WebSocketClient::State::Closed && !closing) {
            st.closed_by_server = !(stop && stop()) && keep;
            closing = true;
            pump(t);
            tcp.close(t);
        }
        if (tcp.state() == TcpState::CloseWait && tcp.unacked() == 0) tcp.close(t);
        if (t >= next_check) {   // the slow checks, once a second
            next_check = t + 1'000'000'000ull;
            if (!closing && (t >= deadline || !keep || (stop && stop()))) {
                ws.close();
                pump(t);
                closing = true;
                tcp.close(t);
            }
            if (!closing && st.messages == 0 && t - start >= idle_ns) {
                err = "connected, but the server sent nothing";
                break;
            }
            if (closing && t >= deadline + 5'000'000'000ull) break;   // a peer that never finishes the close
        }
        if (opened && !closing && !ping.empty() && t >= next_ping) {
            cfg.text ? ws.send_text({reinterpret_cast<const char*>(ping.data()), ping.size()}) : ws.send_binary(ping.data(), ping.size());
            ++st.pings;
            next_ping = t + ping_ns;
            pump(t);
        }
    }
    filter->remove_flow(e.remote_ip, rport_n, lport_n);
    st.tcp_retransmits = tcp.stats().retransmits;
    if (!work.empty()) {
        std::sort(work.begin(), work.end());
        st.work_p50_ns = work[work.size() / 2];
        st.work_p99_ns = work[std::min(work.size() - 1, work.size() * 99 / 100)];
    }
    return err.empty();
}

} // namespace altair::net
