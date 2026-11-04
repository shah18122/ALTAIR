#pragma once

// net/bypass_ws.hpp -- a TLS WebSocket with no kernel TCP under it.
//
// AF_XDP (net/af_xdp.hpp) -> ALTAIR's TCP (net/user_tcp.hpp) -> TLS on memory
// BIOs (net/tls_stream.hpp) -> WebSocket framing (net/websocket.hpp), driven by
// one busy-polling thread. bypass_ws_run() has the shape of the kernel-socket
// fyers_data_socket_run() (broker/fyers_data_socket.hpp): the same opening
// frames, per-frame callback with replies, keep-alive ping and limits, so the
// FYERS feed can run on either transport.
//
// It owns the flow only while it runs: the XDP filter steers this one 4-tuple
// and is detached on return, and the local port is held in the kernel so no
// kernel socket can be given it meanwhile.

#include <net/af_xdp.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace altair::net {

struct BypassWsConfig {
    XdpOptions xdp;                  // interface, XDP object, native/zero-copy, UMEM
    int queues = 1;                  // AF_XDP sockets on queues 0..queues-1 (RSS spreads flows)
    std::string local_ip;            // the interface's address
    std::uint16_t local_port = 0;    // 0: one from the kernel's ephemeral range, held for the session
    std::string remote_ip;           // empty: resolve `host`
    std::uint16_t remote_port = 443;
    std::string next_hop_mac;        // empty: from /proc/net/arp (remote, or the default gateway)
    std::string host;                // SNI, certificate name, Host header
    std::string path = "/";
    std::string authorization;       // Authorization header on the upgrade; never logged
    bool text = false;               // frames sent are text, not binary
    std::string ca_file;             // empty: the system store
    bool verify = true;
    std::uint16_t mss = 1460;
};

struct BypassWsStats {
    std::size_t messages = 0;
    std::size_t bytes = 0;
    std::size_t frames_sent = 0;
    std::size_t pings = 0;
    std::uint64_t tcp_retransmits = 0;
    double connect_ms = -1, tls_ms = -1, upgrade_ms = -1;   // from the SYN
    std::string tls_cipher;
    bool closed_by_server = false;
    /// ALTAIR's own work per received frame (parse, checksums, TCP, TLS
    /// decrypt, WebSocket, your callback), nanoseconds, p50 / p99.
    std::uint32_t work_p50_ns = 0, work_p99_ns = 0;
};

using BypassFrame = std::vector<std::uint8_t>;

/// Returns false with `err` set on failure; `st` is filled either way.
bool bypass_ws_run(const BypassWsConfig& cfg, const std::vector<BypassFrame>& opening,
                   const std::function<bool(const std::uint8_t*, std::size_t, std::vector<BypassFrame>&)>& on_message,
                   const BypassFrame& ping, std::chrono::seconds ping_every, std::chrono::seconds run_for,
                   std::chrono::seconds idle_limit, BypassWsStats& st, std::string& err,
                   const std::function<bool()>& stop = {});

} // namespace altair::net
