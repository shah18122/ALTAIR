#pragma once

// net/neighbour.hpp -- the MACs a bypass frame needs: this interface's own,
// and the next hop's (the peer on the same subnet, else the default gateway),
// read from the kernel's tables. Run once at connect time, never per frame.

#include <arpa/inet.h>
#include <netinet/in.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace altair::net {

inline bool parse_mac(const std::string& s, std::array<std::uint8_t, 6>& out) {
    unsigned b[6];
    char tail = 0;
    if (std::sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &tail) != 6) return false;
    for (int i = 0; i < 6; ++i) {
        if (b[i] > 255) return false;
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(b[i]);
    }
    return true;
}

inline bool interface_mac(const std::string& iface, std::array<std::uint8_t, 6>& mac) {
    std::ifstream f("/sys/class/net/" + iface + "/address");
    std::string s;
    return std::getline(f, s) && parse_mac(s, mac);
}

/// The MAC for `ip` in /proc/net/arp on `iface`, else for its default gateway.
inline bool next_hop_mac(const std::string& iface, const std::string& ip, std::array<std::uint8_t, 6>& mac) {
    auto lookup = [&](const std::string& want) {
        std::ifstream f("/proc/net/arp");
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            std::istringstream l(line);
            std::string a, hw, flags, m, mask, dev;
            l >> a >> hw >> flags >> m >> mask >> dev;
            if (a == want && dev == iface && m != "00:00:00:00:00:00") return parse_mac(m, mac);
        }
        return false;
    };
    if (lookup(ip)) return true;
    std::ifstream r("/proc/net/route");
    std::string line;
    std::getline(r, line);
    while (std::getline(r, line)) {
        std::istringstream l(line);
        std::string dev, dest, gw;
        l >> dev >> dest >> gw;
        if (dev == iface && dest == "00000000") {
            in_addr a{};
            a.s_addr = static_cast<std::uint32_t>(std::strtoul(gw.c_str(), nullptr, 16));
            return lookup(inet_ntoa(a));
        }
    }
    return false;
}

} // namespace altair::net
