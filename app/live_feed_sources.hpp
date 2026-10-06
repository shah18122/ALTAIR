// app/live_feed_sources.hpp -- the price service's FYERS and simulated sources.
//
// altair_price_service owns the feed and republishes it on 127.0.0.1:7421.
// It had two sources: Kite live (--go) and a one-series replay. These are the
// two the live terminal runs on:
//
//   --fyers --go   FYERS' data socket for the whole live universe
//                  (live/universe.hpp): indices, futures, the option chains,
//                  the NIFTY 50 -- ticks, quotes and 5-level depth;
//   --sim          the same universe from live/sim.hpp, flagged SIM on every
//                  frame, for building and showing the terminal when the
//                  market is shut.
//
// A LATE SUBSCRIBER GETS THE WHOLE BOARD -- AND ONLY IT. The bus is a stream
// of changes; a terminal that connects at 11:00 would otherwise show an
// illiquid strike as blank until it next trades. So the last trade, quote and
// book of every instrument is kept and sent to the NEW subscriber alone, as
// Snapshot frames (server/price_bus.hpp). Subscribers already running never
// see them: to them a replayed trade would be a fresh print.
//
// ONE THREAD OWNS THE BUS. The bus is not thread-safe, and the source loops
// (FYERS, Kite, the simulator) block on their sockets. A source does not lock
// anything: it pushes each update into a single-producer ring, and the bus's
// owner thread drains the ring, keeps the board, publishes, accepts
// subscribers and flushes outboxes. Nothing is dropped between a source and
// the bus: a full ring makes the source wait (and counts it), because a lost
// trade would be a gap every subscriber then has to recover from. Slow
// SUBSCRIBERS are the bus's business (its outbox caps), not the source's.

#pragma once

#include <live/sim.hpp>
#include <live/universe.hpp>
#include <server/price_bus.hpp>

#include <lockfree/spsc_ring.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace altair::live_sources {

/// The last of everything, per token, for late subscribers.
class BoardCache {
public:
    void trade(const PricePayload& p, std::int64_t ns) { auto& s = slot(p.token); s.trade = p; s.trade_ns = ns; s.has_trade = true; }
    void quote(const QuotePayload& q, std::int64_t ns) { auto& s = slot(q.token); s.quote = q; s.quote_ns = ns; s.has_quote = true; }
    void book(const PricePayload& p, const PriceLevel* b, const PriceLevel* a, std::int64_t ns) {
        auto& s = slot(p.token);
        s.book = p; s.book_ns = ns; s.has_book = true;
        for (std::size_t i = 0; i < kMaxDepthLevels; ++i) { s.bids[i] = b[i]; s.asks[i] = a[i]; }
    }
    /// The board, to one new client, as Snapshot frames. Quotes first, so a
    /// row has its previous close before its first price; by token, so two
    /// joiners see the same order.
    void replay_to(PriceBus& bus, std::uint64_t client) const {
        std::vector<std::uint32_t> toks;
        toks.reserve(slots_.size());
        for (const auto& [tok, s] : slots_) toks.push_back(tok);
        std::sort(toks.begin(), toks.end());
        for (const auto t : toks) { const Slot& s = slots_.at(t); if (s.has_quote) bus.publish_quote_snapshot_to(client, s.quote, s.quote_ns); }
        for (const auto t : toks) { const Slot& s = slots_.at(t); if (s.has_trade) bus.publish_snapshot_to(client, kTopicTrades, s.trade, nullptr, nullptr, s.trade_ns); }
        for (const auto t : toks) { const Slot& s = slots_.at(t); if (s.has_book) bus.publish_snapshot_to(client, kTopicBook, s.book, s.bids, s.asks, s.book_ns); }
    }
    [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }

private:
    struct Slot {
        PricePayload trade{}, book{};
        QuotePayload quote{};
        PriceLevel bids[kMaxDepthLevels]{}, asks[kMaxDepthLevels]{};
        std::int64_t trade_ns = 0, quote_ns = 0, book_ns = 0;
        bool has_trade = false, has_quote = false, has_book = false;
    };
    Slot& slot(std::uint32_t tok) { return slots_[tok]; }
    std::unordered_map<std::uint32_t, Slot> slots_;
};

/// One update from a source, as it crosses to the bus's owner thread.
struct BusEvent {
    enum Kind : std::uint8_t { Trade = 1, Quote = 2, Book = 3 };
    std::uint8_t kind = 0;
    std::int64_t ns = 0;
    PricePayload price{};
    QuotePayload quote{};
    PriceLevel bids[kMaxDepthLevels]{}, asks[kMaxDepthLevels]{};
};

/// The bus, its board and its owner thread: everything a source publishes
/// through. trade(), quote() and book() are called from ONE source thread at
/// a time (the sources run one after another, never together); second_book()
/// from ONE other thread (the FYERS 50-level book's), on a ring of its own,
/// so each ring keeps exactly one producer. Everything else is safe from any
/// thread.
class SharedBus {
public:
    static constexpr std::size_t kRing = 4096;

    /// `owner_init` runs first on the owner thread (its core and priority).
    explicit SharedBus(PriceBus& bus, std::function<void()> owner_init = {})
        : bus_(bus), ring_(std::make_unique<Ring>()), second_(std::make_unique<Ring>()), init_(std::move(owner_init)),
          th_([this] {
              if (init_) init_();
              run();
          }) {}
    ~SharedBus() {
        stop_.store(true, std::memory_order_release);
        th_.join();
    }
    SharedBus(const SharedBus&) = delete;
    SharedBus& operator=(const SharedBus&) = delete;

    void trade(const PricePayload& p, std::int64_t ns) {
        BusEvent e;
        e.kind = BusEvent::Trade; e.ns = ns; e.price = p;
        push(e);
        trades_.fetch_add(1, std::memory_order_relaxed);
    }
    void quote(const QuotePayload& q, std::int64_t ns) {
        BusEvent e;
        e.kind = BusEvent::Quote; e.ns = ns; e.quote = q;
        push(e);
        quotes_.fetch_add(1, std::memory_order_relaxed);
    }
    void book(const PricePayload& p, const PriceLevel* b, const PriceLevel* a, std::int64_t ns) {
        BusEvent e;
        e.kind = BusEvent::Book; e.ns = ns; e.price = p;
        for (std::size_t i = 0; i < kMaxDepthLevels; ++i) { e.bids[i] = b[i]; e.asks[i] = a[i]; }
        push(e);
        books_.fetch_add(1, std::memory_order_relaxed);
    }
    /// A book from the second producer (the 50-level book's thread).
    void second_book(const PricePayload& p, const PriceLevel* b, const PriceLevel* a, std::int64_t ns) {
        BusEvent e;
        e.kind = BusEvent::Book; e.ns = ns; e.price = p;
        for (std::size_t i = 0; i < kMaxDepthLevels; ++i) { e.bids[i] = b[i]; e.asks[i] = a[i]; }
        push(*second_, e);
        books_.fetch_add(1, std::memory_order_relaxed);
    }
    /// Block until everything pushed so far has been published (tests, shutdown).
    void drain() {
        const std::uint64_t want = ring_->pushed(), want2 = second_->pushed();
        while (published_.load(std::memory_order_acquire) < want || published2_.load(std::memory_order_acquire) < want2)
            std::this_thread::yield();
    }

    [[nodiscard]] std::size_t clients() const noexcept { return clients_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t trades() const noexcept { return trades_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t quotes() const noexcept { return quotes_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t books() const noexcept { return books_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t coalesced() const noexcept { return coalesced_.load(std::memory_order_relaxed); }
    /// Times a source found the ring full and waited for the owner thread.
    [[nodiscard]] std::uint64_t waits() const noexcept { return waits_.load(std::memory_order_relaxed); }
    /// Baselines sent to late joiners.
    [[nodiscard]] std::uint64_t snapshots_sent() const noexcept { return snapshots_.load(std::memory_order_relaxed); }

private:
    using Ring = SpscRing<BusEvent, kRing>;

    void push(const BusEvent& e) { push(*ring_, e); }
    void push(Ring& r, const BusEvent& e) {
        if (r.try_push(e)) return;
        waits_.fetch_add(1, std::memory_order_relaxed);
        // Backpressure, never loss: the owner thread does not block, so this
        // wait is bounded by how long it takes to publish what is queued.
        while (!r.try_push(e)) std::this_thread::yield();
    }

    void run() {
        BusEvent e;
        auto last_poll = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        for (;;) {
            const bool stopping = stop_.load(std::memory_order_acquire);
            std::size_t n = 0;
            while (n < 1024 && ring_->try_pop(e)) {
                apply(e);
                ++n;
            }
            published_.store(ring_->popped(), std::memory_order_release);
            for (std::size_t k = 0; k < 1024 && second_->try_pop(e); ++k) {
                apply(e);
                ++n;
            }
            published2_.store(second_->popped(), std::memory_order_release);
            const auto now = std::chrono::steady_clock::now();
            if (n == 0 || now - last_poll >= std::chrono::milliseconds(5)) {
                last_poll = now;
                bus_.poll();
                for (const auto id : bus_.take_joined()) { cache_.replay_to(bus_, id); snapshots_.fetch_add(1, std::memory_order_relaxed); }
                clients_.store(bus_.clients(), std::memory_order_relaxed);
                coalesced_.store(bus_.coalesced(), std::memory_order_relaxed);
            }
            if (stopping && ring_->empty_approx() && second_->empty_approx()) break;
            if (n == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        bus_.poll();   // a last flush of what will go
    }

    void apply(const BusEvent& e) {
        switch (e.kind) {
        case BusEvent::Trade:
            cache_.trade(e.price, e.ns);
            bus_.publish(kTopicTrades, e.price, nullptr, nullptr, e.ns);
            break;
        case BusEvent::Quote:
            cache_.quote(e.quote, e.ns);
            bus_.publish_quote(e.quote, e.ns);
            break;
        case BusEvent::Book:
            cache_.book(e.price, e.bids, e.asks, e.ns);
            bus_.publish(kTopicBook, e.price, e.bids, e.asks, e.ns);
            break;
        default: break;
        }
    }

    PriceBus& bus_;                 ///< owner thread only
    BoardCache cache_;              ///< owner thread only
    std::unique_ptr<Ring> ring_;
    std::unique_ptr<Ring> second_;  ///< the second producer's ring (second_book)
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> published_{0}, published2_{0};
    std::atomic<std::size_t> clients_{0};
    std::atomic<std::uint64_t> trades_{0}, quotes_{0}, books_{0}, coalesced_{0}, waits_{0}, snapshots_{0};
    std::function<void()> init_;
    std::thread th_;                ///< last: starts once everything above exists
};

/// A small JSON status file for the desktop: what the feed is doing and why
/// nothing ticks, if nothing does. Temp then rename, like every status file here.
struct FeedStatus {
    std::string source;          ///< "fyers" | "sim"
    std::string state;           ///< "connecting", "streaming", "reconnecting", "stopped", "refused"
    std::string error;
    std::size_t instruments = 0, unknown_symbols = 0, clients = 0;
    std::uint64_t trades = 0, quotes = 0, books = 0, reconnects = 0;
    std::int64_t engine_ns = 0;
};

inline void write_status(const std::string& path, const FeedStatus& s) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path{path}.parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return;
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        f << "{\n  \"written_unix\": " << now << ",\n  \"source\": \"" << s.source << "\",\n  \"state\": \"" << s.state
          << "\",\n  \"error\": \"" << s.error << "\",\n  \"instruments\": " << s.instruments
          << ",\n  \"unknown_symbols\": " << s.unknown_symbols << ",\n  \"clients\": " << s.clients
          << ",\n  \"trades\": " << s.trades << ",\n  \"quotes\": " << s.quotes << ",\n  \"books\": " << s.books
          << ",\n  \"reconnects\": " << s.reconnects << ",\n  \"engine_ns\": " << s.engine_ns << "\n}\n";
    }
    // Replaces in one step (std::filesystem::rename overwrites on every
    // platform): never deleted first, so a reader sees the old file or the new.
    std::filesystem::rename(tmp, path, ec);
}

/// Seeds for the simulator: yesterday's closes from dataset/ and data/pairs/.
/// With `day_iso` ("YYYY-MM-DD"), the closes of the session before that day.
[[nodiscard]] inline double sim_close(const std::string& dir, const std::string& day_iso) {
    return day_iso.empty() ? live::last_close(dir) : live::last_close_before(dir, day_iso);
}

[[nodiscard]] inline live::LiveSimSeeds sim_seeds(const std::string& source_dir, const std::string& dataset_dir,
                                                  const std::vector<live::LiveInstrument>& u,
                                                  const std::string& day_iso = {}) {
    live::LiveSimSeeds s;
    const double n = sim_close(dataset_dir + "/spot/nifty/1d", day_iso);
    const double b = sim_close(dataset_dir + "/spot/banknifty/1d", day_iso);
    const double v = sim_close(dataset_dir + "/spot/indiavix/1d", day_iso);
    if (n > 0.0) s.nifty = n;
    if (b > 0.0) s.banknifty = b;
    if (v > 0.0) s.vix = v;
    for (const auto& i : u) {
        if (i.kind != live::LiveKind::Equity) continue;
        std::string dir = i.symbol;
        for (auto& c : dir) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        const double c = sim_close(source_dir + "/data/pairs/" + dir + "/1d", day_iso);
        if (c > 0.0) s.stocks[i.symbol] = c;
    }
    return s;
}

/// Run the simulator: `speed` simulated seconds per wall second, from
/// `start_ns`, until 15:30 IST that day or `stop()` says so. `setup(sim)` runs
/// once before the first step (a past day's anchors); `added()` is asked every
/// couple of seconds for instruments to add mid-session (the market watch's
/// scrip search), as (instrument, equity close or 0) pairs.
template <class Stop, class Setup, class Added>
inline void run_sim(SharedBus& bus, const std::vector<live::LiveInstrument>& u, const live::LiveSimSeeds& seeds,
                    std::uint64_t seed, std::int64_t start_ns, double speed, const std::string& status_path,
                    Stop&& stop, Setup&& setup, Added&& added) {
    live::LiveSim sim(u, seeds, seed, start_ns);
    setup(sim);
    const std::int64_t close_ns = (live::ist_today(start_ns / 1'000'000'000LL) * 86400 + 10 * 3600) * 1'000'000'000LL;
    constexpr std::int64_t kStep = 100'000'000;   // 100 ms of simulated time
    const auto wall_step = std::chrono::nanoseconds(static_cast<std::int64_t>(static_cast<double>(kStep) / speed));
    auto next = std::chrono::steady_clock::now();
    auto last_status = next;
    FeedStatus st;
    st.source = "sim";
    st.state = "streaming";
    st.instruments = u.size();
    // Opening board: one quote per instrument so every row has its previous close.
    sim.board([&](const live::LiveInstrument&, const live::LiveSimEvent& ev) { bus.quote(ev.quote, sim.now_ns()); });
    while (!stop()) {
        if (sim.now_ns() < close_ns) {
            sim.step(kStep, [&](const live::LiveInstrument&, const live::LiveSimEvent& ev) {
                if (ev.trade) bus.trade(ev.price, ev.price.exchange_ts_ns);
                bus.quote(ev.quote, sim.now_ns());
                if (ev.book) bus.book(live::live_book_payload(ev), ev.bids, ev.asks, sim.now_ns());
            });
        } else {
            st.state = "closed";
        }
        next += wall_step;
        std::this_thread::sleep_until(next);
        const auto now = std::chrono::steady_clock::now();
        if (now - last_status > std::chrono::seconds(2)) {
            last_status = now;
            for (auto& [in, close] : added()) {
                const std::size_t before = sim.universe().size();
                sim.add(in, close);
                if (sim.universe().size() == before) continue;
                // Its opening quote at once, so the new row has a previous close.
                sim.board_one(sim.universe().size() - 1, [&](const live::LiveInstrument&, const live::LiveSimEvent& ev) {
                    bus.quote(ev.quote, sim.now_ns());
                });
            }
            st.instruments = sim.universe().size();
            st.clients = bus.clients();
            st.trades = bus.trades(); st.quotes = bus.quotes(); st.books = bus.books();
            st.engine_ns = sim.now_ns();
            write_status(status_path, st);
        }
        if (next < now - std::chrono::seconds(1)) next = now;   // RULE 11: safe-side -- after a stall, resume at the requested speed instead of racing to catch up.
    }
    st.state = "stopped";
    write_status(status_path, st);
}

template <class Stop>
inline void run_sim(SharedBus& bus, const std::vector<live::LiveInstrument>& u, const live::LiveSimSeeds& seeds,
                    std::uint64_t seed, std::int64_t start_ns, double speed, const std::string& status_path,
                    Stop&& stop) {
    run_sim(bus, u, seeds, seed, start_ns, speed, status_path, std::forward<Stop>(stop), [](live::LiveSim&) {},
            [] { return std::vector<std::pair<live::LiveInstrument, double>>{}; });
}

} // namespace altair::live_sources
