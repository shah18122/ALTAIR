#pragma once

// net/bypass_https.hpp -- the order line with no kernel TCP under it.
//
// A kept-alive HTTPS/1.1 connection on ALTAIR's own stack: an Ethernet link the
// kernel does not own (a DPDK port, net/dpdk_link.hpp; or a raw packet socket in
// the tests) -> ALTAIR's TCP (net/user_tcp.hpp) -> TLS on memory BIOs
// (net/tls_stream.hpp) -> one HTTP request at a time. It is the twin of
// broker/https_keepalive.hpp, with the same safety rules, and the order router
// uses it in front of that one:
//
//   * Before a request, the link is serviced and the connection checked: not
//     established, idle past `max_idle`, or ANY unasked byte after a reply
//     (FIN, close_notify) -- and it is replaced BEFORE a byte is written.
//     Those failures are ResolveFailed / ConnectFailed / TlsFailed: nothing was
//     sent, so the router may send the same order on the kernel line instead.
//   * Once the request is written, any failure is TransportFailed and is never
//     retried: the router treats it as uncertain and asks the order book.
//
// ARP is ALTAIR's own on such a link: it answers for its address and learns
// the next hop's MAC (net/arp.hpp). DNS goes through the kernel (getaddrinfo),
// at connect only.
//
// One thread. Busy-polls while a request is out (the router's core is its own).

#include <net/arp.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace altair::net {

/// An Ethernet link ALTAIR drives itself: hand it frames, poll it for frames.
class NetLink {
public:
    using OnFrame = std::function<void(const std::uint8_t*, std::size_t)>;
    virtual ~NetLink() = default;
    virtual unsigned poll(const OnFrame& on_frame) = 0;   ///< never blocks
    virtual bool send(const std::uint8_t* frame, std::size_t len) = 0;
    [[nodiscard]] virtual Mac mac() const = 0;
};

struct BypassHttpsConfig {
    std::string host;                     ///< SNI, certificate name, Host header
    std::string remote_ip;                ///< empty: resolve `host` (kernel DNS)
    std::uint16_t remote_port = 443;
    std::string local_ip;                 ///< the link's address (the spare ENI's private IP)
    std::string next_hop_ip;              ///< the subnet's gateway (172.31.32.1 on the server)
    std::string ca_file;                  ///< empty: the system store
    bool verify = true;
    std::uint16_t mss = 1460;
    std::chrono::seconds max_idle{50};
};

struct BypassHttpsResponse {
    unsigned status = 0;
    std::string body;
};

enum class BypassHttpsError : std::uint8_t {
    NotSent,      ///< nothing of the request left: the link, ARP, TCP or TLS did not come up
    Transport,    ///< the request was (or may have been) written; the reply did not complete
};

class BypassHttps {
public:
    BypassHttps(NetLink& link, BypassHttpsConfig cfg);
    ~BypassHttps();
    BypassHttps(const BypassHttps&) = delete;
    BypassHttps& operator=(const BypassHttps&) = delete;

    /// Answer ARP, take in ACKs and the peer's close, retransmit: call it often.
    void service();
    /// Connect (ARP, TCP, TLS) now, so the first order does not pay for it.
    bool warm(std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] std::chrono::steady_clock::duration idle() const noexcept;

    /// One request. `ok` false: `error` says whether anything was sent.
    struct Result {
        bool ok = false;
        BypassHttpsError error = BypassHttpsError::NotSent;
        BypassHttpsResponse response;
        std::string why;
    };
    Result request(std::string_view method, std::string_view target, std::string_view body,
                   std::string_view authorization, std::chrono::milliseconds timeout = std::chrono::milliseconds{10000});

    void close();
    /// The same numbers as HttpsKeepAlive::json().
    [[nodiscard]] std::string json() const;
    [[nodiscard]] const std::string& last_error() const noexcept { return err_; }

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
    std::string err_;
};

} // namespace altair::net
