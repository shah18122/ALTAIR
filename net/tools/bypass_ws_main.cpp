// altair_bypass_ws -- open a TLS WebSocket over the kernel-bypass path and
// record every binary message (length-prefixed) to a file.
//
//   altair_bypass_ws --iface vb --local-ip 10.77.0.2 --remote 10.77.0.1:4443
//       --host altair.test --path /feed --ca cert.pem --send SUB --ping PING
//       --seconds 10 --dump /tmp/messages.bin
//
// The FYERS feed runs on the same function (net/bypass_ws.hpp) with FYERS's
// own opening frames. Needs root (or CAP_NET_ADMIN + CAP_BPF + CAP_NET_RAW).

#include <net/bypass_ws.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#ifndef ALTAIR_XDP_OBJECT
#define ALTAIR_XDP_OBJECT "altair_flow.bpf.o"
#endif

int main(int argc, char** argv) {
    using namespace altair::net;
    BypassWsConfig c;
    c.xdp.bpf_object = ALTAIR_XDP_OBJECT;
    std::string remote, dump, send, ping;
    int seconds = 10;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--iface" && (v = next())) c.xdp.iface = v;
        else if (a == "--local-ip" && (v = next())) c.local_ip = v;
        else if (a == "--local-port" && (v = next())) c.local_port = static_cast<std::uint16_t>(std::atoi(v));
        else if (a == "--remote" && (v = next())) remote = v;
        else if (a == "--host" && (v = next())) c.host = v;
        else if (a == "--path" && (v = next())) c.path = v;
        else if (a == "--ca" && (v = next())) c.ca_file = v;
        else if (a == "--next-hop-mac" && (v = next())) c.next_hop_mac = v;
        else if (a == "--queues" && (v = next())) c.queues = std::atoi(v);
        else if (a == "--send" && (v = next())) send = v;
        else if (a == "--ping" && (v = next())) ping = v;
        else if (a == "--seconds" && (v = next())) seconds = std::atoi(v);
        else if (a == "--dump" && (v = next())) dump = v;
        else if (a == "--bpf" && (v = next())) c.xdp.bpf_object = v;
        else if (a == "--native") c.xdp.native = true;
        else if (a == "--zero-copy") c.xdp.zero_copy = true;
        else if (a == "--insecure") c.verify = false;
        else {
            std::fprintf(stderr, "usage: altair_bypass_ws --iface IF --local-ip IP --remote IP:PORT --host NAME [--path P]\n"
                                 "         [--ca FILE|--insecure] [--send TEXT] [--ping TEXT] [--seconds S] [--dump FILE]\n"
                                 "         [--queues N] [--native] [--zero-copy] [--local-port P] [--next-hop-mac MAC]\n");
            return 2;
        }
    }
    const auto colon = remote.rfind(':');
    if (colon != std::string::npos) {
        c.remote_ip = remote.substr(0, colon);
        c.remote_port = static_cast<std::uint16_t>(std::atoi(remote.c_str() + colon + 1));
    }
    std::ofstream out;
    if (!dump.empty()) out.open(dump, std::ios::binary);
    std::vector<BypassFrame> opening;
    if (!send.empty()) opening.emplace_back(send.begin(), send.end());
    const BypassFrame ping_frame(ping.begin(), ping.end());
    BypassWsStats st;
    std::string err;
    const bool ok = bypass_ws_run(
        c, opening,
        [&](const std::uint8_t* p, std::size_t n, std::vector<BypassFrame>&) {
            if (out) {
                const std::uint32_t len = static_cast<std::uint32_t>(n);
                out.write(reinterpret_cast<const char*>(&len), 4);
                out.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
            }
            return true;
        },
        ping_frame, std::chrono::seconds{1}, std::chrono::seconds{seconds}, std::chrono::seconds{10}, st, err);
    std::printf("bypass WebSocket: TCP up %.3f ms, TLS %.3f ms (%s), upgrade %.3f ms\n", st.connect_ms, st.tls_ms,
                st.tls_cipher.c_str(), st.upgrade_ms);
    std::printf("  %zu messages, %zu bytes; %zu frames sent, %zu pings; %llu TCP retransmits; %s\n", st.messages, st.bytes,
                st.frames_sent, st.pings, static_cast<unsigned long long>(st.tcp_retransmits),
                st.closed_by_server ? "closed by the server" : "closed by us");
    std::printf("  ALTAIR work per frame in (parse, checksums, TCP, TLS, WebSocket, callback): p50 %u / p99 %u ns\n",
                st.work_p50_ns, st.work_p99_ns);
    if (!ok) std::fprintf(stderr, "altair_bypass_ws: %s\n", err.c_str());
    return ok && st.messages ? 0 : 1;
}
