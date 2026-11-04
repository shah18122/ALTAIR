#pragma once

// net/arp.hpp -- ARP for a port the kernel does not own (DPDK).
//
// Under DPDK the NIC is ALTAIR's alone: nobody else answers "who has
// 172.31.x.y?" for it, and nobody resolves the gateway's MAC. These build
// and read the two ARP frames that needs (RFC 826, Ethernet + IPv4 only).

#include <net/packet.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace altair::net {

inline constexpr std::uint16_t kEtherArp = 0x0806;
inline constexpr std::size_t kArpFrame = 42;

using Mac = std::array<std::uint8_t, 6>;

namespace detail {
inline std::size_t arp_frame(std::uint8_t* out, const Mac& dst_eth, const Mac& src_mac, std::uint16_t op,
                             const Mac& sha, std::uint32_t spa, const Mac& tha, std::uint32_t tpa) noexcept {
    std::memcpy(out, dst_eth.data(), 6);
    std::memcpy(out + 6, src_mac.data(), 6);
    out[12] = 0x08;
    out[13] = 0x06;
    std::uint8_t* a = out + 14;
    a[0] = 0; a[1] = 1;          // Ethernet
    a[2] = 0x08; a[3] = 0x00;    // IPv4
    a[4] = 6; a[5] = 4;
    a[6] = 0; a[7] = static_cast<std::uint8_t>(op);
    std::memcpy(a + 8, sha.data(), 6);
    std::memcpy(a + 14, &spa, 4);
    std::memcpy(a + 18, tha.data(), 6);
    std::memcpy(a + 24, &tpa, 4);
    return kArpFrame;
}
} // namespace detail

/// "Who has `target_ip`? Tell `my_ip`" -- broadcast. Addresses in network order.
inline std::size_t arp_request(std::uint8_t* out, const Mac& my_mac, std::uint32_t my_ip, std::uint32_t target_ip) noexcept {
    const Mac bcast{0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    return detail::arp_frame(out, bcast, my_mac, 1, my_mac, my_ip, Mac{}, target_ip);
}

struct ArpPacket {
    std::uint16_t op = 0;
    Mac sender_mac{};
    std::uint32_t sender_ip = 0;   // network order
    std::uint32_t target_ip = 0;   // network order
};

/// Parse an ARP frame (Ethernet/IPv4, no VLAN). False when it is not one.
inline bool parse_arp(const std::uint8_t* f, std::size_t len, ArpPacket& out) noexcept {
    if (len < kArpFrame || load_be16(f + 12) != kEtherArp) return false;
    const std::uint8_t* a = f + 14;
    if (load_be16(a) != 1 || load_be16(a + 2) != kEtherIpv4 || a[4] != 6 || a[5] != 4) return false;
    out.op = load_be16(a + 6);
    std::memcpy(out.sender_mac.data(), a + 8, 6);
    std::memcpy(&out.sender_ip, a + 14, 4);
    std::memcpy(&out.target_ip, a + 24, 4);
    return true;
}

/// The answer to a request for `my_ip`; 0 when `req` is not one.
inline std::size_t arp_reply_to(std::uint8_t* out, const ArpPacket& req, const Mac& my_mac, std::uint32_t my_ip) noexcept {
    if (req.op != 1 || req.target_ip != my_ip) return 0;
    return detail::arp_frame(out, req.sender_mac, my_mac, 2, my_mac, my_ip, req.sender_mac, req.sender_ip);
}

} // namespace altair::net
