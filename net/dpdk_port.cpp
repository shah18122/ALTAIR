// net/dpdk_port.cpp -- see dpdk_port.hpp.

#include <net/dpdk_port.hpp>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

#include <cstring>
#include <sstream>

namespace altair::net {

namespace {
bool g_eal_ready = false;
}

std::unique_ptr<DpdkPort> DpdkPort::open(const std::string& eal_args, const std::string& port, std::string& err) {
    if (!g_eal_ready) {
        std::vector<std::string> words{"altair"};
        std::istringstream in(eal_args);
        for (std::string w; in >> w;) words.push_back(w);
        std::vector<char*> argv;
        for (auto& w : words) argv.push_back(w.data());
        if (rte_eal_init(static_cast<int>(argv.size()), argv.data()) < 0) {
            err = std::string("DPDK EAL refused: ") + rte_strerror(rte_errno);
            return nullptr;
        }
        g_eal_ready = true;
    }
    std::unique_ptr<DpdkPort> p(new DpdkPort());
    if (rte_eth_dev_get_port_by_name(port.c_str(), &p->port_) != 0) {
        err = "no DPDK port " + port + " (bound to vfio-pci? named in --eal?)";
        return nullptr;
    }
    const int socket = rte_eth_dev_socket_id(p->port_) < 0 ? 0 : rte_eth_dev_socket_id(p->port_);
    p->pool_ = rte_pktmbuf_pool_create(("altair" + std::to_string(p->port_)).c_str(), 8191, 256, 0,
                                       RTE_MBUF_DEFAULT_BUF_SIZE, socket);
    if (!p->pool_) {
        err = std::string("no mbuf pool (hugepages?): ") + rte_strerror(rte_errno);
        return nullptr;
    }
    rte_eth_conf conf{};
    if (rte_eth_dev_configure(p->port_, 1, 1, &conf) != 0 ||
        rte_eth_rx_queue_setup(p->port_, 0, 1024, static_cast<unsigned>(socket), nullptr, p->pool_) != 0 ||
        rte_eth_tx_queue_setup(p->port_, 0, 1024, static_cast<unsigned>(socket), nullptr) != 0) {
        err = "cannot configure port " + port;
        return nullptr;
    }
    if (rte_eth_dev_start(p->port_) != 0) {
        err = "cannot start port " + port;
        return nullptr;
    }
    p->started_ = true;
    rte_ether_addr a{};
    rte_eth_macaddr_get(p->port_, &a);
    std::memcpy(p->mac_, a.addr_bytes, 6);
    return p;
}

void DpdkPort::shutdown() {
    if (g_eal_ready) rte_eal_cleanup();
    g_eal_ready = false;
}

DpdkPort::~DpdkPort() {
    if (started_) {
        rte_eth_dev_stop(port_);
        rte_eth_dev_close(port_);
    }
    if (pool_) rte_mempool_free(pool_);
}

unsigned DpdkPort::rx_burst() {
    const unsigned n = rte_eth_rx_burst(port_, 0, rx_, kBurst);
    for (unsigned i = 0; i < n; ++i) {
        rx_data_[i] = rte_pktmbuf_mtod(rx_[i], const std::uint8_t*);
        rx_len_[i] = rte_pktmbuf_data_len(rx_[i]);   // one segment: frames <= the 2 KiB mbuf
    }
    rx_frames_ += n;
    return n;
}

void DpdkPort::rx_free(unsigned n) {
    for (unsigned i = 0; i < n; ++i) rte_pktmbuf_free(rx_[i]);
}

bool DpdkPort::send(const std::uint8_t* frame, std::size_t len) {
    rte_mbuf* m = rte_pktmbuf_alloc(pool_);
    if (!m) return false;
    char* p = rte_pktmbuf_append(m, static_cast<std::uint16_t>(len));
    if (!p) {
        rte_pktmbuf_free(m);
        return false;
    }
    std::memcpy(p, frame, len);
    if (rte_eth_tx_burst(port_, 0, &m, 1) != 1) {
        rte_pktmbuf_free(m);
        return false;
    }
    ++tx_frames_;
    return true;
}

} // namespace altair::net
