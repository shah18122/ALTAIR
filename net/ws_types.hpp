#pragma once

// net/ws_types.hpp -- what both bypass WebSocket paths report (AF_XDP:
// net/bypass_ws.hpp; any NetLink: net/bypass_ws_link.hpp). Apart so the DPDK
// path builds without libxdp.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace altair::net {

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

} // namespace altair::net
