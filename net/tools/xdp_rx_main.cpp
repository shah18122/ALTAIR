// altair_xdp_rx -- receive one TCP flow through AF_XDP and say what arrived.
//
// Attaches ALTAIR's XDP filter, steers exactly one flow (remote ip:port ->
// local port) to AF_XDP sockets on the first N queues, busy-polls them, and
// reassembles the stream. Every other frame passes to the kernel.
//
//   altair_xdp_rx --iface veth-b --remote 10.77.0.1:4443 --local-port 50000
//                 --seconds 5 --dump /tmp/stream.bin
//
// Prints frames seen, the filter's redirect/pass counters, the in-order
// bytes, and how long ALTAIR's own work took per frame (parse + checksum +
// reassembly), in nanoseconds. Needs root (or CAP_NET_ADMIN + CAP_BPF).

#include <net/af_xdp.hpp>
#include <net/packet.hpp>
#include <net/tcp_reassembly.hpp>

#include <arpa/inet.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifndef ALTAIR_XDP_OBJECT
#define ALTAIR_XDP_OBJECT "altair_flow.bpf.o"
#endif

namespace {

using namespace altair::net;
using Clock = std::chrono::steady_clock;

int usage() {
    std::fprintf(stderr,
                 "usage: altair_xdp_rx --iface IF --remote IP:PORT --local-port P [--queues N] [--native]\n"
                 "                     [--zero-copy] [--seconds S] [--dump FILE] [--bpf FILE]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    XdpOptions o;
    o.bpf_object = ALTAIR_XDP_OBJECT;
    std::string remote, dump;
    int local_port = 0, queues = 1;
    double seconds = 10;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--iface" && (v = next())) o.iface = v;
        else if (a == "--remote" && (v = next())) remote = v;
        else if (a == "--local-port" && (v = next())) local_port = std::atoi(v);
        else if (a == "--queues" && (v = next())) queues = std::atoi(v);
        else if (a == "--seconds" && (v = next())) seconds = std::atof(v);
        else if (a == "--dump" && (v = next())) dump = v;
        else if (a == "--bpf" && (v = next())) o.bpf_object = v;
        else if (a == "--native") o.native = true;
        else if (a == "--zero-copy") o.zero_copy = true;
        else return usage();
    }
    const auto colon = remote.rfind(':');
    std::uint32_t rip = 0;
    if (o.iface.empty() || colon == std::string::npos || local_port <= 0 || local_port > 65535 || queues < 1 ||
        !parse_ipv4(remote.substr(0, colon).c_str(), rip))
        return usage();
    const int rport_host = std::atoi(remote.c_str() + colon + 1);
    if (rport_host <= 0 || rport_host > 65535) return usage();
    const std::uint16_t rport = htons(static_cast<std::uint16_t>(rport_host));
    const std::uint16_t lport = htons(static_cast<std::uint16_t>(local_port));

    std::string err;
    auto filter = XdpFilter::attach(o, err);
    if (!filter) {
        std::fprintf(stderr, "altair_xdp_rx: %s\n", err.c_str());
        return 1;
    }
    std::vector<std::unique_ptr<XskQueue>> socks;
    for (int q = 0; q < queues; ++q) {
        auto s = XskQueue::open(o, static_cast<std::uint32_t>(q), *filter, err);
        if (!s) {
            std::fprintf(stderr, "altair_xdp_rx: %s\n", err.c_str());
            return 1;
        }
        socks.push_back(std::move(s));
    }
    if (!filter->add_flow(rip, rport, lport)) {
        std::fprintf(stderr, "altair_xdp_rx: cannot add the flow to the filter\n");
        return 1;
    }
    std::printf("steering %s -> local port %d to AF_XDP on %s, %d queue(s), %s mode%s\n", remote.c_str(), local_port,
                o.iface.c_str(), queues, o.native ? "native" : "generic", o.zero_copy ? ", zero-copy" : "");
    std::fflush(stdout);

    TcpReassembler<> stream;
    std::string bytes;
    std::uint64_t frames = 0, bad_checksum = 0, foreign = 0, not_tcp = 0;
    std::vector<std::uint32_t> work_ns;
    work_ns.reserve(1u << 20);

    auto on_frame = [&](const std::uint8_t* f, std::size_t len) {
        const auto t0 = Clock::now();
        ++frames;
        TcpSegment s{};
        if (parse_frame(f, len, s) != Parse::Ok) {
            ++not_tcp;
            return;
        }
        if (s.flow.src_ip != rip || s.flow.src_port != rport || s.flow.dst_port != lport) {
            ++foreign;
            return;
        }
        if (!ipv4_checksum_ok(s) || !tcp_checksum_ok(s)) {
            ++bad_checksum;
            return;
        }
        if ((s.flags & tcp_flag::syn) && !stream.started()) stream.start(s.seq + 1);
        else if (!stream.started() && s.payload_len) stream.start(s.seq);   // joined mid-stream
        stream.on_segment(s.seq + ((s.flags & tcp_flag::syn) ? 1u : 0u), s.payload, s.payload_len,
                          (s.flags & tcp_flag::fin) != 0,
                          [&](const std::uint8_t* p, std::size_t n) { bytes.append(reinterpret_cast<const char*>(p), n); });
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
        if (work_ns.size() < work_ns.capacity()) work_ns.push_back(static_cast<std::uint32_t>(ns));
    };

    const auto until = Clock::now() + std::chrono::duration<double>(seconds);
    while (Clock::now() < until && !stream.closed())
        for (auto& s : socks) s->poll(on_frame);

    filter->remove_flow(rip, rport, lport);
    std::uint64_t redirected = 0, passed = 0;
    filter->counters(redirected, passed);
    std::printf("frames %llu (filter: %llu redirected, %llu passed to the kernel)\n",
                static_cast<unsigned long long>(frames), static_cast<unsigned long long>(redirected),
                static_cast<unsigned long long>(passed));
    std::printf("stream: %zu bytes in order, %llu duplicate, %llu early, %llu dropped; %s\n", bytes.size(),
                static_cast<unsigned long long>(stream.stats().duplicate_bytes),
                static_cast<unsigned long long>(stream.stats().queued_segments),
                static_cast<unsigned long long>(stream.stats().dropped_segments), stream.closed() ? "closed (FIN)" : "open");
    std::printf("refused: %llu bad checksum, %llu other flow, %llu not TCP/IPv4\n",
                static_cast<unsigned long long>(bad_checksum), static_cast<unsigned long long>(foreign),
                static_cast<unsigned long long>(not_tcp));
    if (!work_ns.empty()) {
        std::sort(work_ns.begin(), work_ns.end());
        auto pct = [&](double p) { return work_ns[std::min(work_ns.size() - 1, static_cast<std::size_t>(p * static_cast<double>(work_ns.size())))]; };
        std::printf("ALTAIR work per frame (parse + checksums + reassembly): p50 %u / p99 %u / max %u ns (%zu)\n",
                    pct(0.50), pct(0.99), work_ns.back(), work_ns.size());
    }
    if (!dump.empty()) std::ofstream(dump, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return frames ? 0 : 3;
}
