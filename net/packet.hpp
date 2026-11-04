#pragma once

// net/packet.hpp -- Ethernet / IPv4 / TCP parsing for the kernel-bypass feed.
//
// AF_XDP and DPDK hand ALTAIR raw Ethernet frames; this turns one frame into a
// TcpSegment that points INTO the frame (no copy, no allocation). Everything
// the bypass path does not handle is reported, never guessed at:
//   - IPv4 only (FYERS's feed endpoints are IPv4)
//   - fragments are refused (a TCP flow with DF set never fragments)
//   - one 802.1Q VLAN tag is skipped; anything deeper is refused
//
// Byte order: FlowKey keeps addresses and ports in NETWORK order exactly as
// they sit in the packet, so matching a flow is four integer compares and no
// byte swapping. seq/ack/window are converted to host order.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace altair::net {

struct FlowKey {
    std::uint32_t src_ip = 0;    // network order
    std::uint32_t dst_ip = 0;    // network order
    std::uint16_t src_port = 0;  // network order
    std::uint16_t dst_port = 0;  // network order
    friend bool operator==(const FlowKey&, const FlowKey&) = default;
};

namespace tcp_flag {
inline constexpr std::uint8_t fin = 0x01;
inline constexpr std::uint8_t syn = 0x02;
inline constexpr std::uint8_t rst = 0x04;
inline constexpr std::uint8_t psh = 0x08;
inline constexpr std::uint8_t ack = 0x10;
} // namespace tcp_flag

struct TcpSegment {
    FlowKey flow;
    std::uint32_t seq = 0;   // host order
    std::uint32_t ack = 0;   // host order
    std::uint16_t window = 0;
    std::uint8_t flags = 0;
    const std::uint8_t* ip = nullptr;   // start of the IPv4 header
    std::size_t ip_len = 0;             // IPv4 total length
    const std::uint8_t* tcp = nullptr;  // start of the TCP header
    std::size_t tcp_len = 0;            // TCP header + payload
    const std::uint8_t* payload = nullptr;
    std::size_t payload_len = 0;
};

enum class Parse : std::uint8_t {
    Ok,
    Truncated,     // shorter than its headers say
    NotIpv4,
    NotTcp,
    Fragment,
    BadHeader,     // IHL < 5, data offset < 5, total length < headers
};

inline std::uint16_t load_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
inline std::uint32_t load_be32(const std::uint8_t* p) noexcept {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
inline std::uint32_t load_raw32(const std::uint8_t* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
inline std::uint16_t load_raw16(const std::uint8_t* p) noexcept {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

inline constexpr std::size_t kEthHeader = 14;
inline constexpr std::uint16_t kEtherIpv4 = 0x0800;
inline constexpr std::uint16_t kEtherVlan = 0x8100;
inline constexpr std::uint8_t kIpProtoTcp = 6;

/// Parse one Ethernet frame. On Ok every field of `out` is set; on anything
/// else `out` is left in an unspecified state.
inline Parse parse_frame(const std::uint8_t* frame, std::size_t len, TcpSegment& out) noexcept {
    if (len < kEthHeader) return Parse::Truncated;
    std::size_t off = 12;
    std::uint16_t ether = load_be16(frame + off);
    off += 2;
    if (ether == kEtherVlan) {
        if (len < off + 4) return Parse::Truncated;
        ether = load_be16(frame + off + 2);
        off += 4;
    }
    if (ether != kEtherIpv4) return Parse::NotIpv4;
    if (len < off + 20) return Parse::Truncated;
    const std::uint8_t* ip = frame + off;
    if ((ip[0] >> 4) != 4) return Parse::NotIpv4;
    const std::size_t ihl = std::size_t{ip[0] & 0x0Fu} * 4;
    if (ihl < 20) return Parse::BadHeader;
    const std::size_t total = load_be16(ip + 2);
    if (total < ihl + 20) return Parse::BadHeader;
    if (len < off + total) return Parse::Truncated;
    const std::uint16_t frag = load_be16(ip + 6);
    if ((frag & 0x2000u) || (frag & 0x1FFFu)) return Parse::Fragment;   // MF or offset
    if (ip[9] != kIpProtoTcp) return Parse::NotTcp;
    const std::uint8_t* tcp = ip + ihl;
    const std::size_t doff = static_cast<std::size_t>(tcp[12] >> 4) * 4;
    if (doff < 20 || ihl + doff > total) return Parse::BadHeader;
    out.flow.src_ip = load_raw32(ip + 12);
    out.flow.dst_ip = load_raw32(ip + 16);
    out.flow.src_port = load_raw16(tcp + 0);
    out.flow.dst_port = load_raw16(tcp + 2);
    out.seq = load_be32(tcp + 4);
    out.ack = load_be32(tcp + 8);
    out.flags = tcp[13];
    out.window = load_be16(tcp + 14);
    out.ip = ip;
    out.ip_len = total;
    out.tcp = tcp;
    out.tcp_len = total - ihl;
    out.payload = tcp + doff;
    out.payload_len = total - ihl - doff;
    return Parse::Ok;
}

/// One's-complement sum of `len` bytes, folded later; `acc` chains calls.
inline std::uint32_t csum_add(std::uint32_t acc, const std::uint8_t* p, std::size_t len) noexcept {
    while (len >= 2) {
        acc += load_be16(p);
        p += 2;
        len -= 2;
    }
    if (len) acc += std::uint32_t{p[0]} << 8;
    return acc;
}
inline std::uint16_t csum_fold(std::uint32_t acc) noexcept {
    while (acc >> 16) acc = (acc & 0xFFFFu) + (acc >> 16);
    return static_cast<std::uint16_t>(~acc & 0xFFFFu);
}

/// True when the IPv4 header checksum is right.
inline bool ipv4_checksum_ok(const TcpSegment& s) noexcept {
    const std::size_t ihl = static_cast<std::size_t>(s.tcp - s.ip);
    return csum_fold(csum_add(0, s.ip, ihl)) == 0;
}

/// The TCP checksum over the pseudo-header, header and payload as they stand
/// (a correct segment folds to 0). The bypass path cannot rely on the NIC's
/// receive checksum offload, which XDP does not expose.
inline std::uint16_t tcp_checksum(const TcpSegment& s) noexcept {
    std::uint32_t acc = 0;
    acc = csum_add(acc, s.ip + 12, 8);   // source + destination address
    acc += kIpProtoTcp;
    acc += static_cast<std::uint32_t>(s.tcp_len);
    acc = csum_add(acc, s.tcp, s.tcp_len);
    return csum_fold(acc);
}
inline bool tcp_checksum_ok(const TcpSegment& s) noexcept { return tcp_checksum(s) == 0; }

/// "a.b.c.d" -> network-order address; false on anything else.
inline bool parse_ipv4(const char* text, std::uint32_t& out) noexcept {
    std::array<std::uint8_t, 4> b{};
    int part = 0;
    unsigned v = 0;
    bool digit = false;
    for (const char* c = text;; ++c) {
        if (*c >= '0' && *c <= '9') {
            v = v * 10 + static_cast<unsigned>(*c - '0');
            if (v > 255) return false;
            digit = true;
        } else if (*c == '.' || *c == '\0') {
            if (!digit || part > 3) return false;
            b[static_cast<std::size_t>(part++)] = static_cast<std::uint8_t>(v);
            v = 0;
            digit = false;
            if (*c == '\0') break;
        } else {
            return false;
        }
    }
    if (part != 4) return false;
    std::memcpy(&out, b.data(), 4);
    return true;
}

} // namespace altair::net
