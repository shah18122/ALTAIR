// net/af_xdp.cpp -- see af_xdp.hpp.

#include <net/af_xdp.hpp>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>

#ifndef SO_PREFER_BUSY_POLL
#define SO_PREFER_BUSY_POLL 69
#endif
#ifndef SO_BUSY_POLL_BUDGET
#define SO_BUSY_POLL_BUDGET 70
#endif

namespace altair::net {

namespace {

struct FlowKey4 {
    std::uint32_t remote_ip;
    std::uint16_t remote_port;
    std::uint16_t local_port;
};
static_assert(sizeof(FlowKey4) == 8, "must match struct flow_key in altair_flow.bpf.c");

std::string errno_text(int e) { return std::strerror(e < 0 ? -e : e); }

} // namespace

std::unique_ptr<XdpFilter> XdpFilter::attach(const XdpOptions& o, std::string& err) {
    std::unique_ptr<XdpFilter> f(new XdpFilter());
    f->ifindex_ = static_cast<int>(if_nametoindex(o.iface.c_str()));
    if (f->ifindex_ == 0) {
        err = "no interface " + o.iface;
        return nullptr;
    }
    f->obj_ = bpf_object__open_file(o.bpf_object.c_str(), nullptr);
    if (!f->obj_) {
        err = "cannot open " + o.bpf_object + ": " + errno_text(errno);
        return nullptr;
    }
    if (const int r = bpf_object__load(f->obj_); r != 0) {
        err = "the kernel refused the XDP program: " + errno_text(r);
        return nullptr;
    }
    bpf_program* prog = bpf_object__find_program_by_name(f->obj_, "altair_flow");
    f->flows_fd_ = bpf_object__find_map_fd_by_name(f->obj_, "flows");
    f->xsks_fd_ = bpf_object__find_map_fd_by_name(f->obj_, "xsks");
    f->counters_fd_ = bpf_object__find_map_fd_by_name(f->obj_, "counters");
    if (!prog || f->flows_fd_ < 0 || f->xsks_fd_ < 0 || f->counters_fd_ < 0) {
        err = o.bpf_object + " is not ALTAIR's flow filter";
        return nullptr;
    }
    // UPDATE_IF_NOEXIST: never replace a program someone else attached.
    f->attach_flags_ = XDP_FLAGS_UPDATE_IF_NOEXIST | (o.native ? XDP_FLAGS_DRV_MODE : XDP_FLAGS_SKB_MODE);
    if (const int r = bpf_xdp_attach(f->ifindex_, bpf_program__fd(prog), f->attach_flags_, nullptr); r != 0) {
        f->attach_flags_ = 0;
        err = "attach on " + o.iface + (o.native ? " (native)" : " (generic)") + " failed: " + errno_text(r);
        return nullptr;
    }
    return f;
}

XdpFilter::~XdpFilter() {
    if (attach_flags_) bpf_xdp_detach(ifindex_, attach_flags_ & ~XDP_FLAGS_UPDATE_IF_NOEXIST, nullptr);
    if (obj_) bpf_object__close(obj_);
}

bool XdpFilter::add_flow(std::uint32_t remote_ip, std::uint16_t remote_port, std::uint16_t local_port) {
    const FlowKey4 k{remote_ip, remote_port, local_port};
    const std::uint32_t one = 1;
    return bpf_map_update_elem(flows_fd_, &k, &one, BPF_ANY) == 0;
}

bool XdpFilter::remove_flow(std::uint32_t remote_ip, std::uint16_t remote_port, std::uint16_t local_port) {
    const FlowKey4 k{remote_ip, remote_port, local_port};
    return bpf_map_delete_elem(flows_fd_, &k) == 0;
}

void XdpFilter::counters(std::uint64_t& redirected, std::uint64_t& passed) const {
    redirected = passed = 0;
    const int cpus = libbpf_num_possible_cpus();
    if (cpus <= 0) return;
    std::vector<std::uint64_t> v(static_cast<std::size_t>(cpus));
    for (std::uint32_t i = 0; i < 2; ++i) {
        if (bpf_map_lookup_elem(counters_fd_, &i, v.data()) != 0) continue;
        std::uint64_t sum = 0;
        for (const auto x : v) sum += x;
        (i == 0 ? redirected : passed) = sum;
    }
}

std::unique_ptr<XskQueue> XskQueue::open(const XdpOptions& o, std::uint32_t queue, XdpFilter& filter,
                                         std::string& err) {
    if (o.frame_size != 2048 && o.frame_size != 4096) {
        err = "frame_size is 2048 or 4096";
        return nullptr;
    }
    if (o.frames < 64 || (o.frames & (o.frames - 1))) {
        err = "frames is a power of two, at least 64";
        return nullptr;
    }
    std::unique_ptr<XskQueue> q(new XskQueue());
    q->queue_ = queue;
    q->frame_size_ = o.frame_size;
    q->area_size_ = std::size_t{o.frames} * o.frame_size;
    constexpr std::size_t huge = 2u << 20;
    void* area = MAP_FAILED;
    if (o.hugepages) {
        const std::size_t rounded = (q->area_size_ + huge - 1) / huge * huge;
        area = mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0);
        if (area != MAP_FAILED) q->area_size_ = rounded;
    }
    if (area == MAP_FAILED)
        area = mmap(nullptr, q->area_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (area == MAP_FAILED) {
        err = "cannot map the UMEM: " + errno_text(errno);
        return nullptr;
    }
    q->area_ = area;

    const std::uint32_t half = o.frames / 2;
    xsk_umem_config ucfg{};
    ucfg.fill_size = half;
    ucfg.comp_size = half;
    ucfg.frame_size = o.frame_size;
    ucfg.frame_headroom = 0;
    ucfg.flags = 0;
    if (const int r = xsk_umem__create(&q->umem_, area, std::uint64_t{o.frames} * o.frame_size, &q->fill_, &q->comp_, &ucfg);
        r != 0) {
        err = "UMEM refused: " + errno_text(r);
        return nullptr;
    }
    xsk_socket_config scfg{};
    scfg.rx_size = half;
    scfg.tx_size = half;
    scfg.libxdp_flags = XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD;   // XdpFilter owns the program
    scfg.xdp_flags = 0;
    scfg.bind_flags = static_cast<std::uint16_t>(XDP_USE_NEED_WAKEUP | (o.zero_copy ? XDP_ZEROCOPY : XDP_COPY));
    if (const int r = xsk_socket__create(&q->xsk_, o.iface.c_str(), queue, q->umem_, &q->rx_, &q->tx_, &scfg); r != 0) {
        err = "AF_XDP socket on " + o.iface + " queue " + std::to_string(queue) + " refused: " + errno_text(r);
        return nullptr;
    }
    q->fd_ = xsk_socket__fd(q->xsk_);

    // The first half of the frames go to the NIC to receive into; the second half are for sending.
    std::uint32_t idx = 0;
    if (xsk_ring_prod__reserve(&q->fill_, half, &idx) != half) {
        err = "the fill ring is smaller than asked";
        return nullptr;
    }
    for (std::uint32_t i = 0; i < half; ++i) *xsk_ring_prod__fill_addr(&q->fill_, idx + i) = std::uint64_t{i} * o.frame_size;
    xsk_ring_prod__submit(&q->fill_, half);
    q->tx_free_.reserve(half);
    for (std::uint32_t i = half; i < o.frames; ++i) q->tx_free_.push_back(std::uint64_t{i} * o.frame_size);

    if (o.busy_poll_us > 0) {
        const int one = 1, us = o.busy_poll_us, budget = 64;
        // Best effort: older kernels lack the last two; the socket still works.
        setsockopt(q->fd_, SOL_SOCKET, SO_PREFER_BUSY_POLL, &one, sizeof one);
        setsockopt(q->fd_, SOL_SOCKET, SO_BUSY_POLL, &us, sizeof us);
        setsockopt(q->fd_, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &budget, sizeof budget);
    }
    if (const int r = xsk_socket__update_xskmap(q->xsk_, filter.xsks_map_fd()); r != 0) {
        err = "cannot register the socket with the filter: " + errno_text(r);
        return nullptr;
    }
    return q;
}

XskQueue::~XskQueue() {
    if (xsk_) xsk_socket__delete(xsk_);
    if (umem_) xsk_umem__delete(umem_);
    if (area_) munmap(area_, area_size_);
}

void XskQueue::wake_rx() {
    ++stats_.wakeups;
    recvfrom(fd_, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
}

void XskQueue::reclaim_tx() {
    std::uint32_t idx = 0;
    const std::uint32_t n = xsk_ring_cons__peek(&comp_, 64, &idx);
    for (std::uint32_t i = 0; i < n; ++i) tx_free_.push_back(*xsk_ring_cons__comp_addr(&comp_, idx + i));
    if (n) xsk_ring_cons__release(&comp_, n);
}

bool XskQueue::send(const std::uint8_t* frame, std::size_t len) {
    reclaim_tx();
    if (len > frame_size_ || tx_free_.empty()) {
        ++stats_.tx_no_frame;
        return false;
    }
    std::uint32_t idx = 0;
    if (xsk_ring_prod__reserve(&tx_, 1, &idx) != 1) {
        ++stats_.tx_no_frame;
        return false;
    }
    const std::uint64_t addr = tx_free_.back();
    tx_free_.pop_back();
    std::memcpy(xsk_umem__get_data(area_, addr), frame, len);
    xdp_desc* d = xsk_ring_prod__tx_desc(&tx_, idx);
    d->addr = addr;
    d->len = static_cast<std::uint32_t>(len);
    xsk_ring_prod__submit(&tx_, 1);
    if (xsk_ring_prod__needs_wakeup(&tx_)) {
        ++stats_.wakeups;
        sendto(fd_, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
    }
    ++stats_.tx_frames;
    return true;
}

} // namespace altair::net
