#pragma once

// net/bypass_ws_link.hpp -- the FYERS WebSocket on a link ALTAIR owns outright
// (a DPDK port: net/links.hpp DpdkLink). Same callback shape as bypass_ws_run
// (net/bypass_ws.hpp), so altair_price_service runs the feed on either.

#include <net/bypass_https.hpp>   // NetLink
#include <net/ws_types.hpp>       // BypassFrame, BypassWsStats

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace altair::net {

struct BypassWsLinkConfig {
    std::string local_ip;            // the port's private address
    std::string next_hop_ip;         // the subnet's gateway (ARP resolves its MAC)
    std::uint16_t local_port = 0;    // 0: a random one (the link is ours alone)
    std::string remote_ip;           // empty: resolve `host`
    std::uint16_t remote_port = 443;
    std::string host;
    std::string path = "/";
    std::string authorization;
    bool text = false;
    std::string ca_file;
    bool verify = true;
    std::uint16_t mss = 1460;
};

bool bypass_ws_run_link(NetLink& link, const BypassWsLinkConfig& cfg, const std::vector<BypassFrame>& opening,
                        const std::function<bool(const std::uint8_t*, std::size_t, std::vector<BypassFrame>&)>& on_message,
                        const BypassFrame& ping, std::chrono::seconds ping_every, std::chrono::seconds run_for,
                        std::chrono::seconds idle_limit, BypassWsStats& st, std::string& err,
                        const std::function<bool()>& stop = {});

} // namespace altair::net
