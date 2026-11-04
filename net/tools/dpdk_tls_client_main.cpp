// altair_dpdk_tls_client -- a TLS connection over a DPDK port.
//
// The DPDK twin of altair_xdp_tls_client: ALTAIR's TCP (net/user_tcp.hpp) and
// TLS (net/tls_stream.hpp) on a NIC the kernel does not own, so this also
// answers ARP for its address and resolves the next hop itself.
//
//   On the server (second ENI bound with ops/linux/dpdk_bind.sh):
//     sudo altair_dpdk_tls_client --eal "-a 0000:28:00.0 --in-memory" --port 0000:28:00.0
//         --local-ip 172.31.40.10 --next-hop-ip 172.31.32.1 --remote 104.18.4.135:443
//         --host api-t1.fyers.in --request 'GET / HTTP/1.1\r\nHost: api-t1.fyers.in\r\n\r\n'
//   In the namespace test: --eal "--no-pci --in-memory --vdev=net_af_packet0,iface=vb"
//         --port net_af_packet0 ...
//
// Needs root (hugepages and the NIC).

#include <net/arp.hpp>
#include <net/dpdk_port.hpp>
#include <net/neighbour.hpp>
#include <net/packet.hpp>
#include <net/tls_stream.hpp>
#include <net/user_tcp.hpp>

#include <arpa/inet.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <vector>

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
                 "usage: altair_dpdk_tls_client --eal ARGS --port NAME --local-ip IP --remote IP:PORT --host NAME\n"
                 "         [--next-hop-ip IP | --next-hop-mac MAC] [--local-port P] [--request TEXT]\n"
                 "         [--ca FILE | --insecure] [--seconds S] [--dump FILE]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string eal, port, local_ip, remote, host, request, dump, ca, hop_ip, hop_mac;
    int local_port = 50002;
    bool insecure = false;
    double seconds = 15;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--eal" && (v = next())) eal = v;
        else if (a == "--port" && (v = next())) port = v;
        else if (a == "--local-ip" && (v = next())) local_ip = v;
        else if (a == "--local-port" && (v = next())) local_port = std::atoi(v);
        else if (a == "--remote" && (v = next())) remote = v;
        else if (a == "--host" && (v = next())) host = v;
        else if (a == "--request" && (v = next())) request = unescape(v);
        else if (a == "--dump" && (v = next())) dump = v;
        else if (a == "--ca" && (v = next())) ca = v;
        else if (a == "--next-hop-ip" && (v = next())) hop_ip = v;
        else if (a == "--next-hop-mac" && (v = next())) hop_mac = v;
        else if (a == "--seconds" && (v = next())) seconds = std::atof(v);
        else if (a == "--insecure") insecure = true;
        else return usage();
    }
    const auto colon = remote.rfind(':');
    TcpEndpoints e;
    std::uint32_t hop = 0;
    if (port.empty() || colon == std::string::npos || local_port <= 0 || local_port > 65535 ||
        !parse_ipv4(local_ip.c_str(), e.local_ip) || !parse_ipv4(remote.substr(0, colon).c_str(), e.remote_ip))
        return usage();
    if (hop_mac.empty() && !parse_ipv4((hop_ip.empty() ? remote.substr(0, colon) : hop_ip).c_str(), hop)) return usage();
    e.local_port = static_cast<std::uint16_t>(local_port);
    e.remote_port = static_cast<std::uint16_t>(std::atoi(remote.c_str() + colon + 1));

    std::string err;
    auto nic = DpdkPort::open(eal, port, err);
    if (!nic) {
        std::fprintf(stderr, "altair_dpdk_tls_client: %s\n", err.c_str());
        return 1;
    }
    std::copy(nic->mac(), nic->mac() + 6, e.local_mac.begin());

    std::uint8_t arp_buf[64];
    bool have_hop = !hop_mac.empty() && parse_mac(hop_mac, e.next_hop_mac);
    std::uint64_t arp_replies_sent = 0;
    auto handle_arp = [&](const std::uint8_t* f, std::size_t len) {
        ArpPacket a{};
        if (!parse_arp(f, len, a)) return false;
        if (const std::size_t n = arp_reply_to(arp_buf, a, e.local_mac, e.local_ip)) {
            nic->send(arp_buf, n);
            ++arp_replies_sent;
        }
        if (!have_hop && a.sender_ip == hop) {
            e.next_hop_mac = a.sender_mac;
            have_hop = true;
        }
        return true;
    };

    // Resolve the next hop: ARP, every 200 ms, for up to 3 s.
    const std::uint64_t t_arp = now_ns();
    for (std::uint64_t next_ask = 0; !have_hop && now_ns() - t_arp < 3'000'000'000ull;) {
        if (now_ns() >= next_ask) {
            nic->send(arp_buf, arp_request(arp_buf, e.local_mac, e.local_ip, hop));
            next_ask = now_ns() + 200'000'000ull;
        }
        nic->poll([&](const std::uint8_t* f, std::size_t n) { handle_arp(f, n); });
    }
    if (!have_hop) {
        std::fprintf(stderr, "altair_dpdk_tls_client: no ARP answer from the next hop\n");
        return 1;
    }

    TlsClient::Options to;
    to.host = host;
    to.ca_file = ca;
    to.verify = !insecure;
    auto tls = TlsClient::create(to, err);
    if (!tls) {
        std::fprintf(stderr, "altair_dpdk_tls_client: %s\n", err.c_str());
        return 1;
    }
    std::vector<std::uint8_t> answer, cipher_out;
    auto send = [&](const std::uint8_t* f, std::size_t n) { return nic->send(f, n); };
    auto deliver = [&](const std::uint8_t* p, std::size_t n) { tls->feed(p, n); };
    UserTcp tcp(e, send, deliver);

    const std::uint16_t rport_n = htons(e.remote_port), lport_n = htons(e.local_port);
    std::vector<std::uint32_t> work;
    work.reserve(1u << 20);
    auto on_frame = [&](const std::uint8_t* f, std::size_t len) {
        const std::uint64_t t0 = now_ns();
        if (handle_arp(f, len)) return;
        TcpSegment s{};
        if (parse_frame(f, len, s) != Parse::Ok || s.flow.src_ip != e.remote_ip || s.flow.src_port != rport_n ||
            s.flow.dst_port != lport_n || !ipv4_checksum_ok(s) || !tcp_checksum_ok(s))
            return;
        tcp.on_segment(s, t0);
        if (work.size() < work.capacity()) work.push_back(static_cast<std::uint32_t>(now_ns() - t0));
    };

    std::mt19937 rng(std::random_device{}());
    const std::uint64_t start = now_ns();
    tcp.connect(static_cast<std::uint32_t>(rng()), start);
    const std::uint64_t until = start + static_cast<std::uint64_t>(seconds * 1e9);
    std::uint64_t up_at = 0, tls_at = 0;
    bool requested = false;
    while (now_ns() < until) {
        nic->poll(on_frame);
        const std::uint64_t t = now_ns();
        tcp.tick(t);
        if (tcp.state() == TcpState::Failed || tcp.state() == TcpState::Closed) break;
        if (!tcp.established() && tcp.state() != TcpState::CloseWait) continue;
        if (!up_at) up_at = t;
        if (!tls->ready()) {
            if (tls->handshake()) tls_at = now_ns();
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
    auto ms = [&](std::uint64_t t) { return t ? static_cast<double>(t - start) / 1e6 : -1.0; };
    std::printf("DPDK port %s: %llu frames in, %llu out, %llu ARP replies\n", port.c_str(),
                static_cast<unsigned long long>(nic->rx_frames()), static_cast<unsigned long long>(nic->tx_frames()),
                static_cast<unsigned long long>(arp_replies_sent));
    std::printf("TCP: %s; connected at %.3f ms, TLS ready at %.3f ms (%s); %llu retransmits\n", state_name(tcp.state()),
                ms(up_at), ms(tls_at), tls->ready() ? tls->cipher().c_str() : tls->error().c_str(),
                static_cast<unsigned long long>(tcp.stats().retransmits));
    std::printf("TLS: %zu bytes of answer\n", answer.size());
    if (!work.empty()) {
        std::sort(work.begin(), work.end());
        std::printf("ALTAIR TCP work per frame in (parse + checksums + TCP + ACK out): p50 %u / p99 %u ns (%zu)\n",
                    work[work.size() / 2], work[std::min(work.size() - 1, work.size() * 99 / 100)], work.size());
    }
    if (!dump.empty())
        std::ofstream(dump, std::ios::binary).write(reinterpret_cast<const char*>(answer.data()),
                                                    static_cast<std::streamsize>(answer.size()));
    std::fflush(stdout);
    const int rc = tls->ready() && !answer.empty() ? 0 : 1;
    // The port before the EAL, and the EAL before the process's static
    // destructors: DPDK torn down in any other order can crash on exit.
    tls.reset();
    nic.reset();
    DpdkPort::shutdown();
    return rc;
}
