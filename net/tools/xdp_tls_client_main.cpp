// altair_xdp_tls_client -- a TLS connection with no kernel TCP under it.
//
// Opens AF_XDP sockets on the interface, steers its own 4-tuple there, runs
// ALTAIR's TCP (net/user_tcp.hpp) and TLS (net/tls_stream.hpp) on top, sends
// one request and reads the answer. This is the whole bypass path the FYERS
// feed will use; ops/linux/xdp_netns_test.sh runs it against Linux's own TCP
// and an OpenSSL server in a private namespace, with packet loss.
//
//   altair_xdp_tls_client --iface vb --local-ip 10.77.0.2 --local-port 50001
//       --remote 10.77.0.1:4443 --host altair.test --insecure
//       --request 'GET / HTTP/1.0\r\n\r\n' --dump /tmp/answer
//
// The next-hop MAC is read from the kernel's neighbour table (/proc/net/arp)
// for the remote address, or for the default gateway when the remote is not
// on the interface's subnet; --next-hop-mac overrides it. Needs root.

#include <net/af_xdp.hpp>
#include <net/neighbour.hpp>
#include <net/packet.hpp>
#include <net/tls_stream.hpp>
#include <net/user_tcp.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifndef ALTAIR_XDP_OBJECT
#define ALTAIR_XDP_OBJECT "altair_flow.bpf.o"
#endif

namespace {

using namespace altair::net;
using Clock = std::chrono::steady_clock;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

std::string unescape(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char c = s[++i];
            o += c == 'r' ? '\r' : c == 'n' ? '\n' : c == 't' ? '\t' : c;
        } else {
            o += s[i];
        }
    }
    return o;
}

int usage() {
    std::fprintf(stderr,
                 "usage: altair_xdp_tls_client --iface IF --local-ip IP --local-port P --remote IP:PORT --host NAME\n"
                 "         [--request TEXT] [--insecure] [--ca FILE] [--next-hop-mac MAC] [--queues N]\n"
                 "         [--native] [--zero-copy] [--seconds S] [--dump FILE] [--bpf FILE]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    XdpOptions o;
    o.bpf_object = ALTAIR_XDP_OBJECT;
    std::string local_ip, remote, host, request, dump, mac_text, ca;
    int local_port = 0, queues = 1;
    bool insecure = false;
    double seconds = 15;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--iface" && (v = next())) o.iface = v;
        else if (a == "--local-ip" && (v = next())) local_ip = v;
        else if (a == "--local-port" && (v = next())) local_port = std::atoi(v);
        else if (a == "--remote" && (v = next())) remote = v;
        else if (a == "--host" && (v = next())) host = v;
        else if (a == "--request" && (v = next())) request = unescape(v);
        else if (a == "--dump" && (v = next())) dump = v;
        else if (a == "--next-hop-mac" && (v = next())) mac_text = v;
        else if (a == "--ca" && (v = next())) ca = v;
        else if (a == "--queues" && (v = next())) queues = std::atoi(v);
        else if (a == "--seconds" && (v = next())) seconds = std::atof(v);
        else if (a == "--bpf" && (v = next())) o.bpf_object = v;
        else if (a == "--insecure") insecure = true;
        else if (a == "--native") o.native = true;
        else if (a == "--zero-copy") o.zero_copy = true;
        else return usage();
    }
    const auto colon = remote.rfind(':');
    TcpEndpoints e;
    if (o.iface.empty() || colon == std::string::npos || local_port <= 0 || local_port > 65535 || queues < 1 ||
        !parse_ipv4(local_ip.c_str(), e.local_ip) || !parse_ipv4(remote.substr(0, colon).c_str(), e.remote_ip))
        return usage();
    const int rport = std::atoi(remote.c_str() + colon + 1);
    if (rport <= 0 || rport > 65535) return usage();
    e.local_port = static_cast<std::uint16_t>(local_port);
    e.remote_port = static_cast<std::uint16_t>(rport);
    if (!interface_mac(o.iface, e.local_mac)) {
        std::fprintf(stderr, "altair_xdp_tls_client: no MAC for %s\n", o.iface.c_str());
        return 1;
    }
    if (mac_text.empty() ? !next_hop_mac(o.iface, remote.substr(0, colon), e.next_hop_mac)
                         : !parse_mac(mac_text, e.next_hop_mac)) {
        std::fprintf(stderr, "altair_xdp_tls_client: no next-hop MAC (ping the remote or gateway once, or pass --next-hop-mac)\n");
        return 1;
    }

    // Hold the port in the kernel so nothing else is given it while ALTAIR owns the flow.
    const int hold = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(e.local_port);
    sa.sin_addr.s_addr = e.local_ip;
    if (hold < 0 || ::bind(hold, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        std::fprintf(stderr, "altair_xdp_tls_client: local port %d is in use\n", local_port);
        return 1;
    }

    std::string err;
    auto filter = XdpFilter::attach(o, err);
    if (!filter) {
        std::fprintf(stderr, "altair_xdp_tls_client: %s\n", err.c_str());
        return 1;
    }
    std::vector<std::unique_ptr<XskQueue>> socks;
    for (int q = 0; q < queues; ++q) {
        auto s = XskQueue::open(o, static_cast<std::uint32_t>(q), *filter, err);
        if (!s) {
            std::fprintf(stderr, "altair_xdp_tls_client: %s\n", err.c_str());
            return 1;
        }
        socks.push_back(std::move(s));
    }
    const std::uint16_t rport_n = htons(e.remote_port), lport_n = htons(e.local_port);
    filter->add_flow(e.remote_ip, rport_n, lport_n);

    TlsClient::Options to;
    to.host = host;
    to.ca_file = ca;
    to.verify = !insecure;
    auto tls = TlsClient::create(to, err);
    if (!tls) {
        std::fprintf(stderr, "altair_xdp_tls_client: %s\n", err.c_str());
        return 1;
    }

    std::vector<std::uint8_t> answer, cipher_out;
    auto send = [&](const std::uint8_t* f, std::size_t n) { return socks[0]->send(f, n); };
    auto deliver = [&](const std::uint8_t* p, std::size_t n) { tls->feed(p, n); };
    UserTcp tcp(e, send, deliver);

    std::vector<std::uint32_t> work_ns;
    work_ns.reserve(1u << 20);
    std::uint64_t foreign = 0, bad = 0;
    auto on_frame = [&](const std::uint8_t* f, std::size_t len) {
        const std::uint64_t t0 = now_ns();
        TcpSegment s{};
        if (parse_frame(f, len, s) != Parse::Ok || s.flow.src_ip != e.remote_ip || s.flow.src_port != rport_n ||
            s.flow.dst_port != lport_n) {
            ++foreign;
            return;
        }
        if (!ipv4_checksum_ok(s) || !tcp_checksum_ok(s)) {
            ++bad;
            return;
        }
        tcp.on_segment(s, t0);
        if (work_ns.size() < work_ns.capacity()) work_ns.push_back(static_cast<std::uint32_t>(now_ns() - t0));
    };

    std::mt19937 rng(std::random_device{}());
    const std::uint64_t start = now_ns();
    tcp.connect(static_cast<std::uint32_t>(rng()), start);
    const std::uint64_t until = start + static_cast<std::uint64_t>(seconds * 1e9);
    std::uint64_t established_at = 0, handshake_at = 0;
    bool requested = false;
    while (now_ns() < until) {
        for (auto& s : socks) s->poll(on_frame);
        const std::uint64_t t = now_ns();
        tcp.tick(t);
        if (tcp.state() == TcpState::Failed || tcp.state() == TcpState::Closed) break;
        if (!tcp.established() && tcp.state() != TcpState::CloseWait) continue;
        if (!established_at) established_at = t;
        if (!tls->ready()) {
            if (tls->handshake() && !handshake_at) handshake_at = now_ns();
            if (tls->failed()) break;
        }
        if (tls->ready()) {
            if (!requested && !request.empty()) {
                tls->write(reinterpret_cast<const std::uint8_t*>(request.data()), request.size());
                requested = true;
            }
            tls->read(answer);
            if (tls->failed()) break;
        }
        cipher_out.clear();
        if (tls->drain_ciphertext(cipher_out)) tcp.send(cipher_out.data(), cipher_out.size(), t);
        if (tcp.state() == TcpState::CloseWait && tcp.unacked() == 0) tcp.close(t);
    }
    filter->remove_flow(e.remote_ip, rport_n, lport_n);
    ::close(hold);

    const auto& st = tcp.stats();
    const auto& rs = tcp.rx_stats();
    auto ms = [&](std::uint64_t t) { return t ? static_cast<double>(t - start) / 1e6 : -1.0; };
    std::printf("TCP: %s; connected at %.3f ms, TLS ready at %.3f ms (%s)\n", state_name(tcp.state()), ms(established_at),
                ms(handshake_at), tls->ready() ? tls->cipher().c_str() : tls->error().c_str());
    std::printf("TCP: %llu segments out, %llu in, %llu retransmits (%llu fast), %llu bytes sent, %llu acked\n",
                static_cast<unsigned long long>(st.segments_out), static_cast<unsigned long long>(st.segments_in),
                static_cast<unsigned long long>(st.retransmits), static_cast<unsigned long long>(st.fast_retransmits),
                static_cast<unsigned long long>(st.bytes_sent), static_cast<unsigned long long>(st.bytes_acked));
    std::printf("TCP in: %llu bytes in order, %llu duplicate, %llu early; refused %llu bad checksum, %llu other\n",
                static_cast<unsigned long long>(rs.delivered_bytes), static_cast<unsigned long long>(rs.duplicate_bytes),
                static_cast<unsigned long long>(rs.queued_segments), static_cast<unsigned long long>(bad),
                static_cast<unsigned long long>(foreign));
    std::printf("TLS: %zu bytes of answer\n", answer.size());
    if (!work_ns.empty()) {
        std::sort(work_ns.begin(), work_ns.end());
        auto pct = [&](double p) {
            return work_ns[std::min(work_ns.size() - 1, static_cast<std::size_t>(p * static_cast<double>(work_ns.size())))];
        };
        std::printf("ALTAIR TCP work per frame in (parse + checksums + TCP + ACK out): p50 %u / p99 %u ns (%zu)\n",
                    pct(0.50), pct(0.99), work_ns.size());
    }
    if (!dump.empty())
        std::ofstream(dump, std::ios::binary).write(reinterpret_cast<const char*>(answer.data()),
                                                    static_cast<std::streamsize>(answer.size()));
    return tls->ready() && !answer.empty() ? 0 : 1;
}
