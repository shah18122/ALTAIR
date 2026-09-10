// server/price_bus.hpp -- one process owns the feed, everybody else reads it.
//
// P37-02. Today every live thing in the UI is a subprocess that runs, writes a
// JSON file and exits: the tick feed samples for N seconds, the quote refresh
// fetches once, the account panel fetches once. Each holds the credential for
// as long as it runs and none of them is a stream.
//
// This is the stream. One process holds ONE Kite connection and republishes
// every tick as a protocol frame on a local port. The window, the options
// chain and anything else become read-only subscribers.
//
// WHAT THIS BUYS BEYOND CONVENIENCE.
//
// The credential surface NARROWS. Four subprocesses each reading
// data/kite_session.json becomes one process that reads it, and the UI still
// links neither broker/ nor oms/ -- gate 3 is untouched, and the client
// vocabulary in protocol.hpp has no word for placing an order.
//
// PLAIN TCP, NOT WEBSOCKET, AND THAT IS A DECISION.
//
// ROADMAP §10 specifies binary WebSocket frames for the REMOTE client, and
// that still holds -- a browser cannot open a raw socket. This is a loopback
// bus between processes on one machine, where the WebSocket handshake, masking
// and close protocol buy nothing and cost a dependency. The framing is
// identical either way, because protocol.hpp's 48-byte header was written to
// be transport-agnostic and is already covered by conformance vectors.
//
// A SLOW READER MUST NOT STALL THE FEED.
//
// The failure this is built around: a subscriber that stops reading (a window
// being dragged, a debugger breakpoint) fills its socket buffer, and a
// blocking write would then stall the thread that is decoding ticks. The whole
// feed would stop because one reader did.
//
// So sockets are non-blocking and each client has its own outbox with a byte
// cap. Past the cap the OLDEST frames are dropped, in whole-frame units, and
// counted -- which is `Overflow::CoalesceNewest`, the policy protocol.hpp
// already requires for the State channel. A price is state: the newest one is
// the true one and an old one has no value. Dropping partial bytes would
// desynchronise the stream, so the outbox holds complete frames and an offset
// into the head, never a byte count.

#pragma once

#include <server/price_payload.hpp>
#include <server/protocol.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace altair {

/// Per-client outbox cap. Roughly 3,600 index frames, or a few seconds of a
/// busy chain -- enough to ride out a repaint, far short of enough to hide a
/// reader that has actually stopped.
///
/// RULE 11: this bound TRUNCATES, and every dropped frame is counted in
/// `coalesced()`. It cannot refuse (there is nobody to refuse to -- the tick
/// has already happened) and it cannot be proved unreachable (a stopped reader
/// is exactly what it exists for). Dropping the OLDEST is the safe direction
/// for state: the subscriber ends up with the most recent price rather than a
/// stale one, and the sequence gap tells it what it missed.
inline constexpr std::size_t kOutboxBytesCap = 1u << 20;   // 1 MiB

/// Kernel send buffer per client.
///
/// Bounded on purpose: see the note where it is applied. An unbounded socket
/// buffer makes the outbox cap above meaningless, because the backlog a
/// stalled reader accumulates is the sum of the two and only one of them is
/// under this file's control.
inline constexpr std::size_t kSendBufferBytes = 64u * 1024u;

class PriceBus {
public:
    PriceBus(boost::asio::io_context& io, unsigned short port)
        : acceptor_(io) {
        namespace ip = boost::asio::ip;
        boost::system::error_code ec;
        const ip::tcp::endpoint ep(ip::make_address("127.0.0.1"), port);
        acceptor_.open(ep.protocol(), ec);
        if (ec) { fail_ = ec; return; }
        acceptor_.set_option(ip::tcp::acceptor::reuse_address(true), ec);
        acceptor_.bind(ep, ec);
        if (ec) { fail_ = ec; return; }
        acceptor_.listen(ip::tcp::socket::max_listen_connections, ec);
        if (ec) { fail_ = ec; return; }
        acceptor_.non_blocking(true, ec);
        if (ec) { fail_ = ec; return; }
        port_ = acceptor_.local_endpoint(ec).port();
    }

    /// LOOPBACK ONLY, and deliberately. This carries live market data derived
    /// from a credential; binding 0.0.0.0 would publish it to the network with
    /// no authentication, which protocol.hpp's session layer is supposed to
    /// provide and does not exist yet.
    [[nodiscard]] bool ok() const noexcept { return !fail_; }
    [[nodiscard]] boost::system::error_code error() const noexcept {
        return fail_;
    }
    [[nodiscard]] unsigned short port() const noexcept { return port_; }
    [[nodiscard]] std::size_t clients() const noexcept {
        return clients_.size();
    }
    [[nodiscard]] std::uint64_t frames_sent() const noexcept { return sent_; }
    [[nodiscard]] std::uint64_t coalesced() const noexcept { return dropped_; }
    [[nodiscard]] std::uint64_t dropped_clients() const noexcept {
        return gone_;
    }

    /// Accept anything waiting and push whatever each outbox will take.
    /// Non-blocking throughout; call it often.
    void poll() {
        namespace ip = boost::asio::ip;
        for (;;) {
            boost::system::error_code ec;
            ip::tcp::socket s(acceptor_.get_executor());
            acceptor_.accept(s, ec);
            if (ec) { break; }
            s.non_blocking(true, ec);
            s.set_option(ip::tcp::no_delay(true), ec);
            // THE KERNEL'S SEND BUFFER IS PART OF THE BACKLOG, so it has to
            // be bounded too.
            //
            // Without this the outbox cap above is a fiction. Loopback send
            // buffers are large and elastic: a first version of the test
            // flooded 32,766 frames at a reader that had stopped and the
            // outbox never filled once, because the kernel quietly absorbed
            // all 2.9 MB and every write_some succeeded. Nothing coalesced,
            // and a subscriber resuming after a pause would have been fed
            // megabytes of stale prices in order, oldest first -- the exact
            // opposite of what a State channel should do.
            //
            // Bounding it puts the coalescing policy back in charge of what a
            // slow reader sees, and caps the kernel memory one stalled client
            // can pin.
            s.set_option(boost::asio::socket_base::send_buffer_size(
                             static_cast<int>(kSendBufferBytes)), ec);
            clients_.push_back(std::make_unique<Client>(std::move(s)));
        }
        flush();
    }

    /// Encode one update and queue it for every subscriber.
    ///
    /// `engine_ns` is the TICK's own time, not now. protocol.hpp carries both
    /// so a replay is visibly a replay: in live trading the two track, and on
    /// historical data they differ by years.
    void publish(std::uint32_t topic, const PricePayload& p,
                 const PriceLevel* bids, const PriceLevel* asks,
                 std::int64_t engine_ns) {
        std::vector<std::uint8_t> frame(kFrameHeaderBytes
                                        + price_frame_bytes(
                                              p.has(kPriceHasBook)
                                                  ? p.depth_levels : 0));
        const auto body = encode_price(p, bids, asks,
                                       frame.data() + kFrameHeaderBytes,
                                       frame.size() - kFrameHeaderBytes);
        if (!body) { ++refused_; return; }

        FrameHeader h;
        h.kind = FrameKind::Delta;
        h.channel = Channel::State;
        h.topic = topic;
        h.seq = ++seq_[topic];
        h.payload_len = static_cast<std::uint32_t>(*body);
        h.engine_time_ns = engine_ns;
        h.server_time_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        if (!encode_header(h, frame.data(), kFrameHeaderBytes)) {
            ++refused_;
            return;
        }
        frame.resize(kFrameHeaderBytes + *body);

        for (auto& c : clients_) {
            c->out.push_back(frame);
            c->bytes += frame.size();
            // Whole frames, oldest first. A byte-level trim would leave the
            // reader mid-header and every frame after it would be garbage.
            while (c->bytes > kOutboxBytesCap && c->out.size() > 1) {
                c->bytes -= c->out.front().size();
                c->out.pop_front();
                c->head = 0;
                ++dropped_;
            }
        }
        flush();
    }

private:
    struct Client {
        explicit Client(boost::asio::ip::tcp::socket s)
            : sock(std::move(s)) {}
        boost::asio::ip::tcp::socket sock;
        std::deque<std::vector<std::uint8_t>> out;
        std::size_t head = 0;     ///< bytes of out.front() already written
        std::size_t bytes = 0;
    };

    void flush() {
        for (std::size_t i = 0; i < clients_.size();) {
            Client& c = *clients_[i];
            bool dead = false;
            while (!c.out.empty()) {
                const std::vector<std::uint8_t>& f = c.out.front();
                boost::system::error_code ec;
                const std::size_t n = c.sock.write_some(
                    boost::asio::buffer(f.data() + c.head, f.size() - c.head),
                    ec);
                if (ec == boost::asio::error::would_block
                    || ec == boost::asio::error::try_again) {
                    break;                    // full for now; try again later
                }
                if (ec) { dead = true; break; }
                c.head += n;
                if (c.head >= f.size()) {
                    c.bytes -= f.size();
                    c.out.pop_front();
                    c.head = 0;
                    ++sent_;
                }
            }
            if (dead) {
                clients_.erase(clients_.begin()
                               + static_cast<std::ptrdiff_t>(i));
                ++gone_;
            } else {
                ++i;
            }
        }
    }

    boost::asio::ip::tcp::acceptor acceptor_;
    std::vector<std::unique_ptr<Client>> clients_;
    std::uint64_t seq_[3]{};          ///< per topic; index 0 unused
    std::uint64_t sent_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t gone_ = 0;
    std::uint64_t refused_ = 0;
    unsigned short port_ = 0;
    boost::system::error_code fail_;
};

}  // namespace altair
