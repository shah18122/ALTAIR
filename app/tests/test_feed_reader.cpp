// app/live_feed_reader.hpp: the engine's socket drained on its own thread.
//   * a stream published while the engine's thread is busy (a 1.5 s stall,
//     about 3 MB) still arrives whole: no sequence gap, nothing coalesced;
//   * reconnect() drops the stream and makes a new one, in order;
//   * the reader stops promptly;
// all of it twice: as it always ran, and in spin mode (busy-polling, the
// config/latency.toml `spin = true` path). The time from the socket to the
// engine's take() is printed for both.
// Runs threads: label `concurrency`.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <app/live_feed_reader.hpp>
#include <app/live_feed_sources.hpp>
#include <server/protocol.hpp>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <vector>
#include <cstdio>
#include <thread>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

using altair::live_feed::FeedEvent;

// Spin mode is meant for two isolated cores; with 4+ cores the test puts the
// reader and this thread on the last two, as config/latency.toml would.
bool pin_to(long core) {
    const long n = sysconf(_SC_NPROCESSORS_CONF);
    if (n < 4 || core < 0 || core >= n) return false;
    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(static_cast<std::size_t>(core), &one);
    return pthread_setaffinity_np(pthread_self(), sizeof one, &one) == 0;
}

std::int64_t steady_now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void suite(bool spin) {
    using namespace altair;
    std::printf("feed reader%s\n", spin ? " (spin mode)" : "");
    boost::asio::io_context io;
    PriceBus bus(io, 0);
    if (!bus.ok()) { std::printf("  could not bind\n"); ++failures; return; }
    live_sources::SharedBus shared(bus);
    {
        const long last_core = sysconf(_SC_NPROCESSORS_CONF) - 1;
        cpu_set_t was;
        pthread_getaffinity_np(pthread_self(), sizeof was, &was);
        const bool pinned = spin && pin_to(last_core);
        auto owned = std::make_unique<live_feed::FeedReader>(bus.port(), std::size_t{256u << 20}, spin,
                                                             [spin, last_core] { if (spin) pin_to(last_core - 1); });
        if (spin) std::printf("  %s\n", pinned ? "reader and engine thread on the last two cores" : "not pinned (fewer than 4 cores)");
        live_feed::FeedReader& reader = *owned;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool connected = false;
        while (!connected && std::chrono::steady_clock::now() < until)
            for (const auto& e : reader.take(std::chrono::milliseconds(50))) connected = connected || e.kind == FeedEvent::Connected;
        check(connected, "the reader connects on its own thread");
        while (shared.clients() != 1 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // 32,000 trades (about 3 MB) at 200 every 10 ms -- some 20,000 a
        // second, several times a live session -- while this thread, the
        // engine's, does not read for 1.5 s. A reader on this thread would
        // have let about 2.9 MB pile up: far past the bus's 1 MiB outbox and
        // the kernel's buffers, so the bus would have coalesced. The reader
        // thread has half a second of slack per outbox-full, so a busy CI
        // machine scheduling it late still loses nothing.
        constexpr std::uint64_t kN = 32000;
        const auto t_burst = std::chrono::steady_clock::now();
        std::thread source([&] {
            auto next = std::chrono::steady_clock::now();
            for (std::uint64_t k = 0; k < kN; ++k) {
                PricePayload p;
                p.token = static_cast<std::uint32_t>(1 + k % 100);
                p.last_paise = 100 + static_cast<std::int64_t>(k);
                p.exchange_ts_ns = 1;
                shared.trade(p, 1);
                if ((k + 1) % 200 == 0) { next += std::chrono::milliseconds(10); std::this_thread::sleep_until(next); }
            }
        });
        std::this_thread::sleep_until(t_burst + std::chrono::milliseconds(1500));   // the engine is busy

        std::vector<std::uint8_t> buf;
        std::uint64_t frames = 0, last = 0, gaps = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (frames < kN && std::chrono::steady_clock::now() < deadline) {
            for (auto& e : reader.take(std::chrono::milliseconds(50))) {
                if (e.kind != FeedEvent::Data) continue;
                buf.insert(buf.end(), e.bytes.begin(), e.bytes.end());
            }
            std::size_t at = 0;
            while (buf.size() - at >= kFrameHeaderBytes) {
                const auto h = decode_header(buf.data() + at, buf.size() - at);
                if (!h || buf.size() - at < kFrameHeaderBytes + h->payload_len) break;
                if (h->kind == FrameKind::Delta) {
                    if (last != 0 && h->seq != last + 1) ++gaps;
                    last = h->seq;
                    ++frames;
                }
                at += kFrameHeaderBytes + h->payload_len;
            }
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(at));
        }
        source.join();
        check(frames == kN && gaps == 0 && last == kN, "a stream during a 1.5 s stall arrives whole: every frame, in sequence");
        check(shared.coalesced() == 0, "the bus coalesced nothing for this reader");

        // A quiet feed, one trade every 2 ms: how long from the socket (the
        // reader's recv_ns) until this thread's take() has it.
        std::vector<std::int64_t> handoff;
        for (int k = 0; k < 500; ++k) {
            PricePayload p;
            p.token = 1;
            p.last_paise = 100;
            shared.trade(p, 1);
            const auto until_one = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            bool got = false;
            while (!got && std::chrono::steady_clock::now() < until_one)
                for (const auto& e : reader.take(std::chrono::milliseconds(5)))
                    if (e.kind == FeedEvent::Data) {
                        handoff.push_back(steady_now() - e.recv_ns);
                        got = true;
                    }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::sort(handoff.begin(), handoff.end());
        check(handoff.size() >= 490, "a quiet feed's trades all reach the engine's thread");
        if (!handoff.empty())
            std::printf("  socket -> engine thread: p50 %lld / p99 %lld ns (%zu)\n",
                        static_cast<long long>(handoff[handoff.size() / 2]),
                        static_cast<long long>(handoff[handoff.size() * 99 / 100]), handoff.size());

        reader.reconnect();
        int disc = 0, conn = 0;
        const auto until2 = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (conn == 0 && std::chrono::steady_clock::now() < until2)
            for (const auto& e : reader.take(std::chrono::milliseconds(50))) {
                if (e.kind == FeedEvent::Disconnected) ++disc;
                if (e.kind == FeedEvent::Connected && disc > 0) ++conn;
            }
        check(disc == 1 && conn == 1, "reconnect() ends the stream and starts a new one, in that order");
        const auto t0 = std::chrono::steady_clock::now();
        owned.reset();
        check(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1), "and stops within a second");
        pthread_setaffinity_np(pthread_self(), sizeof was, &was);
    }
}

}  // namespace

int main() {
    suite(false);
    suite(true);
    std::printf("%s\n", failures == 0 ? "all feed reader checks passed" : "feed reader checks did not pass");
    return failures == 0 ? 0 : 1;
}
