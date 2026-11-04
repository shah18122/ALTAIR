// app/live_feed_reader.hpp -- the engine's socket, drained on its own thread.
//
// The engine used to read the price bus from its main loop: a non-blocking
// read, then decode and run the models, then sleep 5 ms whenever the socket
// was momentarily empty. Reading in bursts like that, a busy feed (a SIM day
// at 30x is over 100,000 frames a second) filled the bus's per-client outbox
// between reads, and the bus -- correctly, for a reader that has fallen
// behind -- dropped the oldest frames. Those drops are trade-stream gaps, and
// the engine pauses its decisions on a gap: a reader that drains slowly
// makes the models stop trading.
//
// So the socket gets a thread whose only job is to read it, continuously, into
// chunks the engine's thread takes when it is ready. A slow minute in a model
// no longer reaches the bus. The queue is bounded (`max_queued`): past it the
// reader stops reading, the bus's coalescing takes over, and the gaps it
// causes are counted and acted on exactly as before -- nothing is hidden.
//
// The thread runs an io_context in 100 ms slices so a stop is seen promptly;
// it reconnects every 2 s while the service is away and says so in order,
// between the chunks, so the engine knows where one stream ends.
//
// SPIN MODE (config/latency.toml spin = true). Both waits above are scheduler
// wake-ups: epoll waking the reader, then the condition variable waking the
// engine, tens of microseconds each. With spin the reader busy-polls a
// non-blocking socket and take() busy-polls the queue, so a chunk crosses in
// well under a microsecond. Each spinning thread holds its core at 100%, so the
// caller pins both on isolated cores (the "reader" and "engine" roles).

#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace altair::live_feed {

struct FeedEvent {
    enum Kind : std::uint8_t { Data, Connected, Disconnected };
    Kind kind = Data;
    std::vector<std::uint8_t> bytes;   ///< Data only
    std::string note;                  ///< Disconnected: why
    std::int64_t recv_ns = 0;          ///< steady-clock ns when the reader took it off the socket
};

class FeedReader {
public:
    /// `spin`: busy-poll (see above). `on_start` runs first on the reader's
    /// own thread: where the caller pins it.
    FeedReader(unsigned short port, std::size_t max_queued = 256u << 20, bool spin = false,
               std::function<void()> on_start = {})
        : port_(port), max_(max_queued), spin_(spin), on_start_(std::move(on_start)), th_([this] { run(); }) {}
    ~FeedReader() {
        stop_.store(true);
        th_.join();
    }
    FeedReader(const FeedReader&) = delete;
    FeedReader& operator=(const FeedReader&) = delete;

    /// Everything queued so far, in order. Waits up to `wait` for something
    /// to arrive when the queue is empty.
    std::vector<FeedEvent> take(std::chrono::milliseconds wait) {
        if (spin_ && pending_.load(std::memory_order_acquire) == 0) {
            const auto until = std::chrono::steady_clock::now() + wait;
            for (unsigned i = 0; pending_.load(std::memory_order_acquire) == 0; ++i) {
                cpu_relax();
                if ((i & 1023) == 1023 && std::chrono::steady_clock::now() >= until) break;
            }
        }
        std::unique_lock lk(m_);
        if (!spin_ && q_.empty()) cv_.wait_for(lk, wait, [this] { return !q_.empty(); });
        pending_.store(0, std::memory_order_relaxed);
        std::vector<FeedEvent> out(std::make_move_iterator(q_.begin()), std::make_move_iterator(q_.end()));
        q_.clear();
        queued_ = 0;
        return out;
    }

    /// Drop this connection and make a new one: a stream that lost its
    /// framing cannot be resynchronised by guessing, only by starting over.
    /// Data queued before the new Connected event belongs to the old stream.
    void reconnect() noexcept { reset_.store(true); }

    [[nodiscard]] bool connected() const noexcept { return connected_.load(); }
    /// Bytes waiting for the engine's thread right now.
    [[nodiscard]] std::size_t queued() const {
        std::lock_guard lk(m_);
        return queued_;
    }
    /// Times the queue was full and the reader stopped reading for a while.
    [[nodiscard]] std::uint64_t throttled() const noexcept { return throttled_.load(); }

private:
    void push(FeedEvent e) {
        e.recv_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        {
            std::lock_guard lk(m_);
            queued_ += e.bytes.size();
            q_.push_back(std::move(e));
            pending_.store(q_.size(), std::memory_order_release);
        }
        if (!spin_) cv_.notify_one();
    }
    [[nodiscard]] bool full() const {
        std::lock_guard lk(m_);
        return queued_ >= max_;
    }

    static void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }

    void run() {
        if (on_start_) on_start_();
        namespace net = boost::asio;
        using tcp = net::ip::tcp;
        net::io_context io;
        auto last_try = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        while (!stop_.load()) {
            if (std::chrono::steady_clock::now() - last_try < std::chrono::seconds(2)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            last_try = std::chrono::steady_clock::now();
            tcp::socket sock(io);
            boost::system::error_code ec;
            sock.connect({net::ip::make_address("127.0.0.1"), port_}, ec);
            if (ec) continue;
            connected_.store(true);
            push(FeedEvent{FeedEvent::Connected, {}, {}});
            if (spin_) {
                if (spin_read(sock)) last_try = std::chrono::steady_clock::now() - std::chrono::seconds(10);
                continue;
            }

            std::vector<std::uint8_t> chunk(1u << 18);
            bool reading = false, done = false, alive = true;
            std::size_t got = 0;
            boost::system::error_code rec;
            while (alive && !stop_.load()) {
                if (reset_.exchange(false)) { last_try = std::chrono::steady_clock::now() - std::chrono::seconds(10); break; }
                if (!reading) {
                    if (full()) {                         // the engine is far behind: let the bus coalesce
                        throttled_.fetch_add(1);
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        continue;
                    }
                    reading = true;
                    done = false;
                    sock.async_read_some(net::buffer(chunk), [&](boost::system::error_code e, std::size_t n) {
                        rec = e; got = n; done = true;
                    });
                }
                io.restart();
                io.run_for(std::chrono::milliseconds(100));
                if (!done) continue;
                reading = false;
                if (rec) {
                    alive = false;
                    break;
                }
                push(FeedEvent{FeedEvent::Data, std::vector<std::uint8_t>(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got)), {}});
            }
            boost::system::error_code ignored;
            sock.close(ignored);
            io.restart();
            io.run_for(std::chrono::milliseconds(50));   // let a cancelled read complete
            if (connected_.exchange(false))
                push(FeedEvent{FeedEvent::Disconnected, {}, stop_.load() ? std::string("stopping") : rec.message()});
        }
    }

    // Spin mode's read loop: a non-blocking socket polled without sleeping.
    // True when it ended for reconnect() (reconnect at once).
    bool spin_read(boost::asio::ip::tcp::socket& sock) {
        bool reset = false;
        std::vector<std::uint8_t> chunk(1u << 18);
        boost::system::error_code ec;
        sock.non_blocking(true, ec);
        while (!stop_.load(std::memory_order_relaxed)) {
            if (reset_.exchange(false)) {
                reset = true;
                break;
            }
            if (full()) {
                throttled_.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            const std::size_t n = sock.read_some(boost::asio::buffer(chunk), ec);
            if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
                cpu_relax();
                continue;
            }
            if (ec) break;
            push(FeedEvent{FeedEvent::Data, std::vector<std::uint8_t>(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n)), {}});
        }
        boost::system::error_code ignored;
        sock.close(ignored);
        if (connected_.exchange(false))
            push(FeedEvent{FeedEvent::Disconnected, {}, stop_.load() ? std::string("stopping") : reset ? std::string("resync") : ec.message()});
        return reset;
    }

    unsigned short port_;
    std::size_t max_;
    bool spin_ = false;
    std::function<void()> on_start_;
    std::atomic<std::size_t> pending_{0};   ///< events queued: spin mode's lock-free "anything yet?"
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<FeedEvent> q_;
    std::size_t queued_ = 0;
    std::atomic<bool> stop_{false}, connected_{false}, reset_{false};
    std::atomic<std::uint64_t> throttled_{0};
    std::thread th_;   ///< last: starts once everything above exists
};

} // namespace altair::live_feed
