#pragma once

// net/links.hpp -- the Ethernet links BypassHttps (net/bypass_https.hpp) runs on.
//
//   DpdkLink    a DPDK port (net/dpdk_port.hpp): the spare ENI on vfio-pci, the
//               production order line. Built with ALTAIR_DPDK.
//   PacketLink  a raw AF_PACKET socket on an interface: the tests' veth in a
//               private namespace. NOT a bypass (the kernel still carries the
//               frames); it exists so the whole TCP/TLS/HTTP path runs in CI.

#include <net/bypass_https.hpp>

#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>

namespace altair::net {

class PacketLink final : public NetLink {
public:
    explicit PacketLink(const std::string& iface) {
        fd_ = ::socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, htons(ETH_P_ALL));
        if (fd_ < 0) return;
        ifreq ifr{};
        std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
        if (::ioctl(fd_, SIOCGIFINDEX, &ifr) < 0) { ::close(fd_); fd_ = -1; return; }
        ifindex_ = ifr.ifr_ifindex;
        if (::ioctl(fd_, SIOCGIFHWADDR, &ifr) < 0) { ::close(fd_); fd_ = -1; return; }
        std::memcpy(mac_.data(), ifr.ifr_hwaddr.sa_data, 6);
        sockaddr_ll sll{};
        sll.sll_family = AF_PACKET;
        sll.sll_protocol = htons(ETH_P_ALL);
        sll.sll_ifindex = ifindex_;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&sll), sizeof sll) < 0) { ::close(fd_); fd_ = -1; }
    }
    ~PacketLink() override { if (fd_ >= 0) ::close(fd_); }
    PacketLink(const PacketLink&) = delete;
    PacketLink& operator=(const PacketLink&) = delete;
    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }

    unsigned poll(const OnFrame& on_frame) override {
        unsigned n = 0;
        for (; n < 64; ++n) {
            sockaddr_ll from{};
            socklen_t fl = sizeof from;
            const ssize_t r = ::recvfrom(fd_, buf_, sizeof buf_, 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (r <= 0) break;
            if (from.sll_pkttype == PACKET_OUTGOING) continue;   // our own frames, looped back to the socket
            on_frame(buf_, static_cast<std::size_t>(r));
        }
        return n;
    }
    bool send(const std::uint8_t* frame, std::size_t len) override {
        return ::send(fd_, frame, len, 0) == static_cast<ssize_t>(len);
    }
    [[nodiscard]] Mac mac() const override { return mac_; }

private:
    int fd_ = -1, ifindex_ = 0;
    Mac mac_{};
    std::uint8_t buf_[2048]{};
};

} // namespace altair::net

#if defined(ALTAIR_DPDK)
#include <net/dpdk_port.hpp>

namespace altair::net {

class DpdkLink final : public NetLink {
public:
    explicit DpdkLink(DpdkPort& p) : p_(p) { std::memcpy(mac_.data(), p.mac(), 6); }
    unsigned poll(const OnFrame& on_frame) override {
        return p_.poll([&](const std::uint8_t* f, std::size_t n) { on_frame(f, n); });
    }
    bool send(const std::uint8_t* frame, std::size_t len) override { return p_.send(frame, len); }
    [[nodiscard]] Mac mac() const override { return mac_; }

private:
    DpdkPort& p_;
    Mac mac_{};
};

} // namespace altair::net
#endif
