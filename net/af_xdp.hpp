#pragma once

// net/af_xdp.hpp -- AF_XDP sockets: frames straight from the NIC's ring into
// ALTAIR's memory, no kernel network stack, no copy into a socket buffer.
//
//   XdpFilter   loads net/xdp/altair_flow.bpf.o onto an interface and owns
//               the list of flows it steers. Everything not listed passes to
//               the kernel, so SSH, DCV and the recorder are unaffected.
//   XskQueue    one AF_XDP socket on one NIC queue, with its own UMEM (the
//               frame memory the NIC writes into, on hugepages when it can).
//               poll() hands each frame to a callback in place; send() puts a
//               frame on the wire through the same ring.
//
// RSS spreads flows over the NIC's queues, so a steered flow can arrive on any
// of them: open one XskQueue per queue (ENA on this server has 8) and poll all.
//
// SAFETY ON THE LIVE SERVER. Attaching XDP in native (driver) mode makes ENA
// reconfigure its queues: the link drops for a moment, which would cut SSH and
// the recorder's feed. Generic (SKB) mode attaches without a reset. Never
// attach on the live interface during market hours; ops/linux/xdp_netns_test.sh
// tests the whole path on a veth pair in a private network namespace.

#include <xdp/xsk.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace altair::net {

struct XdpOptions {
    std::string iface;
    std::string bpf_object;          // path to altair_flow.bpf.o
    bool native = false;             // driver mode (fast, resets ENA) vs generic SKB mode
    bool zero_copy = false;          // XDP_ZEROCOPY: needs native mode and driver support
    std::uint32_t frames = 4096;     // UMEM frames per queue: half receive, half send
    std::uint32_t frame_size = 4096; // 2048 or 4096
    bool hugepages = true;           // UMEM on 2 MiB pages (vm.nr_hugepages), else 4 KiB
    int busy_poll_us = 20;           // SO_BUSY_POLL for the socket; 0 leaves it off
};

class XdpFilter {
public:
    static std::unique_ptr<XdpFilter> attach(const XdpOptions& o, std::string& err);
    ~XdpFilter();
    XdpFilter(const XdpFilter&) = delete;
    XdpFilter& operator=(const XdpFilter&) = delete;

    /// Steer a flow to the AF_XDP sockets. All values in NETWORK order.
    bool add_flow(std::uint32_t remote_ip, std::uint16_t remote_port, std::uint16_t local_port);
    bool remove_flow(std::uint32_t remote_ip, std::uint16_t remote_port, std::uint16_t local_port);

    int xsks_map_fd() const noexcept { return xsks_fd_; }
    int ifindex() const noexcept { return ifindex_; }
    /// Frames sent to AF_XDP and frames passed to the kernel, summed over CPUs.
    void counters(std::uint64_t& redirected, std::uint64_t& passed) const;

private:
    XdpFilter() = default;
    struct bpf_object* obj_ = nullptr;
    int ifindex_ = 0;
    unsigned attach_flags_ = 0;
    int flows_fd_ = -1;
    int xsks_fd_ = -1;
    int counters_fd_ = -1;
};

struct XskStats {
    std::uint64_t rx_frames = 0;
    std::uint64_t rx_bytes = 0;
    std::uint64_t tx_frames = 0;
    std::uint64_t tx_no_frame = 0;   // send() found no free frame
    std::uint64_t wakeups = 0;       // syscalls the kernel asked for
};

class XskQueue {
public:
    static std::unique_ptr<XskQueue> open(const XdpOptions& o, std::uint32_t queue, XdpFilter& filter,
                                          std::string& err);
    ~XskQueue();
    XskQueue(const XskQueue&) = delete;
    XskQueue& operator=(const XskQueue&) = delete;

    std::uint32_t queue() const noexcept { return queue_; }
    int fd() const noexcept { return fd_; }
    const XskStats& stats() const noexcept { return stats_; }

    /// Hand every waiting frame to on_frame(const uint8_t* frame, size_t len),
    /// then give the frames back to the NIC. Never blocks; returns the count.
    template <typename F>
    std::uint32_t poll(F&& on_frame, std::uint32_t batch = 64) {
        std::uint32_t idx_rx = 0;
        const std::uint32_t n = xsk_ring_cons__peek(&rx_, batch, &idx_rx);
        if (n == 0) {
            if (xsk_ring_prod__needs_wakeup(&fill_)) wake_rx();
            return 0;
        }
        std::uint32_t idx_fill = 0;
        while (xsk_ring_prod__reserve(&fill_, n, &idx_fill) != n) {
            if (xsk_ring_prod__needs_wakeup(&fill_)) wake_rx();
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            const xdp_desc* d = xsk_ring_cons__rx_desc(&rx_, idx_rx + i);
            const std::uint64_t addr = xsk_umem__add_offset_to_addr(d->addr);
            on_frame(static_cast<const std::uint8_t*>(xsk_umem__get_data(area_, addr)),
                     static_cast<std::size_t>(d->len));
            stats_.rx_bytes += d->len;
            *xsk_ring_prod__fill_addr(&fill_, idx_fill + i) = xsk_umem__extract_addr(d->addr);
        }
        xsk_ring_prod__submit(&fill_, n);
        xsk_ring_cons__release(&rx_, n);
        stats_.rx_frames += n;
        return n;
    }

    /// Copy one complete Ethernet frame into a free send frame and queue it.
    bool send(const std::uint8_t* frame, std::size_t len);

private:
    XskQueue() = default;
    void wake_rx();
    void reclaim_tx();

    struct xsk_umem* umem_ = nullptr;
    struct xsk_socket* xsk_ = nullptr;
    struct xsk_ring_prod fill_{};
    struct xsk_ring_cons comp_{};
    struct xsk_ring_cons rx_{};
    struct xsk_ring_prod tx_{};
    void* area_ = nullptr;
    std::size_t area_size_ = 0;
    std::uint32_t frame_size_ = 0;
    std::vector<std::uint64_t> tx_free_;   // free send frames (UMEM addresses)
    std::uint32_t queue_ = 0;
    int fd_ = -1;
    XskStats stats_{};
};

} // namespace altair::net
