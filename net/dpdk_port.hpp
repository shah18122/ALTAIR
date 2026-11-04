#pragma once

// net/dpdk_port.hpp -- one DPDK Ethernet port: the NIC's rings in ALTAIR's
// own memory, polled from ALTAIR's thread, no kernel anywhere in the path.
//
// On the c7i server the port is a SECOND ENI bound to vfio-pci (the first,
// enp39s0, stays with the kernel for SSH, DCV and the recorder):
//   ops/linux/dpdk_bind.sh <pci address>       then
//   --eal "-a <pci address> --in-memory"  --port <pci address>
// In tests it is a net_af_packet vdev on a veth in a private namespace:
//   --eal "--no-pci --in-memory --vdev=net_af_packet0,iface=vb"  --port net_af_packet0
//
// The same receive callback / send(frame) shape as XskQueue (net/af_xdp.hpp),
// so UserTcp, TLS and WebSocket run on either.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct rte_mempool;
struct rte_mbuf;

namespace altair::net {

class DpdkPort {
public:
    /// Initialise the EAL once per process with `eal_args` (split on spaces),
    /// then open `port` (a PCI address or a vdev name).
    static std::unique_ptr<DpdkPort> open(const std::string& eal_args, const std::string& port, std::string& err);
    ~DpdkPort();
    DpdkPort(const DpdkPort&) = delete;
    DpdkPort& operator=(const DpdkPort&) = delete;

    const std::uint8_t* mac() const noexcept { return mac_; }
    std::uint16_t port_id() const noexcept { return port_; }

    /// Hand every waiting frame to on_frame(const uint8_t*, size_t); never blocks.
    template <typename F>
    unsigned poll(F&& on_frame) {
        const unsigned n = rx_burst();
        for (unsigned i = 0; i < n; ++i) on_frame(rx_data_[i], rx_len_[i]);
        rx_free(n);
        return n;
    }

    /// Copy one Ethernet frame into an mbuf and transmit it.
    bool send(const std::uint8_t* frame, std::size_t len);

    /// Release DPDK's process-wide state (after every port is destroyed).
    static void shutdown();

    std::uint64_t rx_frames() const noexcept { return rx_frames_; }
    std::uint64_t tx_frames() const noexcept { return tx_frames_; }

private:
    DpdkPort() = default;
    unsigned rx_burst();
    void rx_free(unsigned n);

    static constexpr unsigned kBurst = 32;
    std::uint16_t port_ = 0;
    rte_mempool* pool_ = nullptr;
    rte_mbuf* rx_[kBurst] = {};
    const std::uint8_t* rx_data_[kBurst] = {};
    std::size_t rx_len_[kBurst] = {};
    std::uint8_t mac_[6] = {};
    std::uint64_t rx_frames_ = 0, tx_frames_ = 0;
    bool started_ = false;
};

} // namespace altair::net
