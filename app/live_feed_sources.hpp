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
// A LATE SUBSCRIBER GETS THE WHOLE BOARD. The bus is a stream of changes; a
// terminal that connects at 11:00 would otherwise show an illiquid strike as
// blank until it next trades. So the last trade, quote and book of every
// instrument is kept, and republished whenever a new subscriber connects.
//
// THE BUS IS NOT THREAD-SAFE and the FYERS socket loop blocks, so every bus
// call goes through one mutex, and a second thread polls the bus (accepting
// subscribers, flushing outboxes) while the socket is quiet.

#pragma once

#include <live/sim.hpp>
#include <live/universe.hpp>
#include <server/price_bus.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
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
    /// Republish the board. Quotes first, so a row has its previous close
    /// before its first price.
    void replay(PriceBus& bus) const {
        for (const auto& [tok, s] : slots_) if (s.has_quote) bus.publish_quote(s.quote, s.quote_ns);
        for (const auto& [tok, s] : slots_) if (s.has_trade) bus.publish(kTopicTrades, s.trade, nullptr, nullptr, s.trade_ns);
        for (const auto& [tok, s] : slots_) if (s.has_book) bus.publish(kTopicBook, s.book, s.bids, s.asks, s.book_ns);
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

/// The bus, its cache and its lock: everything a source publishes through.
class SharedBus {
public:
    explicit SharedBus(PriceBus& bus) : bus_(bus) {}

    void trade(const PricePayload& p, std::int64_t ns) {
        std::lock_guard lk(m_);
        cache_.trade(p, ns);
        bus_.publish(kTopicTrades, p, nullptr, nullptr, ns);
        ++trades_;
    }
    void quote(const QuotePayload& q, std::int64_t ns) {
        std::lock_guard lk(m_);
        cache_.quote(q, ns);
        bus_.publish_quote(q, ns);
        ++quotes_;
    }
    void book(const PricePayload& p, const PriceLevel* b, const PriceLevel* a, std::int64_t ns) {
        std::lock_guard lk(m_);
        cache_.book(p, b, a, ns);
        bus_.publish(kTopicBook, p, b, a, ns);
        ++books_;
    }
    /// Accept and flush; replay the board to everyone when someone new joined.
    void poll() {
        std::lock_guard lk(m_);
        bus_.poll();
        const std::size_t n = bus_.clients();
        if (n > clients_) cache_.replay(bus_);
        clients_ = n;
    }
    [[nodiscard]] std::size_t clients() { std::lock_guard lk(m_); return bus_.clients(); }
    [[nodiscard]] std::uint64_t trades() const noexcept { return trades_; }
    [[nodiscard]] std::uint64_t quotes() const noexcept { return quotes_; }
    [[nodiscard]] std::uint64_t books() const noexcept { return books_; }
    [[nodiscard]] std::uint64_t coalesced() { std::lock_guard lk(m_); return bus_.coalesced(); }

private:
    PriceBus& bus_;
    std::mutex m_;
    BoardCache cache_;
    std::size_t clients_ = 0;
    std::atomic<std::uint64_t> trades_{0}, quotes_{0}, books_{0};
};

/// Keeps the bus polled from its own thread until stopped.
class Poller {
public:
    explicit Poller(SharedBus& bus) : bus_(bus), th_([this] {
        while (!stop_.load()) { bus_.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    }) {}
    ~Poller() { stop_.store(true); th_.join(); }
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

private:
    SharedBus& bus_;
    std::atomic<bool> stop_{false};
    std::thread th_;
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
    std::remove(path.c_str());
    (void)std::rename(tmp.c_str(), path.c_str());
}

/// Seeds for the simulator: yesterday's closes from dataset/ and data/pairs/.
[[nodiscard]] inline live::LiveSimSeeds sim_seeds(const std::string& source_dir, const std::string& dataset_dir,
                                                  const std::vector<live::LiveInstrument>& u) {
    live::LiveSimSeeds s;
    const double n = live::last_close(dataset_dir + "/spot/nifty/1d");
    const double b = live::last_close(dataset_dir + "/spot/banknifty/1d");
    const double v = live::last_close(dataset_dir + "/spot/indiavix/1d");
    if (n > 0.0) s.nifty = n;
    if (b > 0.0) s.banknifty = b;
    if (v > 0.0) s.vix = v;
    for (const auto& i : u) {
        if (i.kind != live::LiveKind::Equity) continue;
        std::string dir = i.symbol;
        for (auto& c : dir) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        const double c = live::last_close(source_dir + "/data/pairs/" + dir + "/1d");
        if (c > 0.0) s.stocks[i.symbol] = c;
    }
    return s;
}

/// Run the simulator: `speed` simulated seconds per wall second, from
/// `start_ns`, until 15:30 IST that day or `stop()` says so.
template <class Stop>
inline void run_sim(SharedBus& bus, const std::vector<live::LiveInstrument>& u, const live::LiveSimSeeds& seeds,
                    std::uint64_t seed, std::int64_t start_ns, double speed, const std::string& status_path,
                    Stop&& stop) {
    live::LiveSim sim(u, seeds, seed, start_ns);
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

} // namespace altair::live_sources
