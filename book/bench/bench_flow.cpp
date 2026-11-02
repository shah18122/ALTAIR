// book/bench/bench_flow.cpp -- the OBI hot path, timed: FlowEngine::on_book /
// on_trade on a 50-level book, and the per-tick trade-type check the OBI
// trader runs (live/obi_signals.hpp) for A-D.
//
// The stream is synthetic but shaped like a FYERS TBT feed: most updates
// change a few sizes at the same prices, some shift the touch a tick, and a
// print arrives every few books. Seeded, so every run sees the same events.
//
// It also prints a CHECKSUM of every FlowSignals field after every event and
// of every A-D decision. A speed change must not move it: run it before and
// after an edit to flow.hpp or obi_signals.hpp and compare.
//
// NOT A CTEST TEST, on purpose (see analytics/bench/bench_chain.cpp): timing
// under a parallel test run is noise. Run it by hand on a quiet machine.

#include <book/flow.hpp>
#include <live/obi_signals.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using altair::flow::FlowEngine;
using altair::flow::FlowSignals;
using altair::flow::Level;

struct Event {
    bool book = false;
    std::int64_t ns = 0;
    std::uint8_t nb = 0, na = 0;
    Level bid[50], ask[50];
    std::int64_t px = 0, volume = 0;
};

std::vector<Event> make_stream(std::size_t n, std::size_t levels) {
    std::mt19937_64 rng(20261102);
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<std::int64_t> size(15, 1500);
    std::vector<Event> out(n);
    std::int64_t bid = 5'000'000, ns = 33'300'000'000'000, vol = 0;   // BANKNIFTY-ish, paise
    Level b[50], a[50];
    for (std::size_t k = 0; k < levels; ++k) {
        b[k] = {bid - 5 * static_cast<std::int64_t>(k), size(rng)};
        a[k] = {bid + 5 + 5 * static_cast<std::int64_t>(k), size(rng)};
    }
    for (auto& e : out) {
        ns += 1'000'000 + 1'000 * pct(rng);   // ~1-1.1 ms apart
        e.ns = ns;
        if (pct(rng) < 20) {                  // a print
            vol += 15 * (1 + pct(rng) % 10);
            const int r = pct(rng);
            e.px = r < 45 ? a[0].px : (r < 90 ? b[0].px : (a[0].px + b[0].px) / 2);
            e.volume = vol;
            continue;
        }
        e.book = true;
        const int r = pct(rng);
        if (r < 8) {                          // the touch moves a tick
            const std::int64_t d = (r < 4 ? 5 : -5);
            bid += d;
            for (std::size_t k = 0; k < levels; ++k) {
                b[k] = {bid - 5 * static_cast<std::int64_t>(k), size(rng)};
                a[k] = {bid + 5 + 5 * static_cast<std::int64_t>(k), size(rng)};
            }
        } else {                              // a few sizes change in place
            const int m = 1 + pct(rng) % 4;
            for (int i = 0; i < m; ++i) {
                const std::size_t k = static_cast<std::size_t>(pct(rng)) % std::min<std::size_t>(levels, 10);
                if (pct(rng) < 50) b[k].qty = size(rng); else a[k].qty = size(rng);
            }
        }
        e.nb = e.na = static_cast<std::uint8_t>(levels);
        std::copy(b, b + levels, e.bid);
        std::copy(a, a + levels, e.ask);
    }
    return out;
}

/// FNV-1a over the bytes of every numeric field.
struct Hash {
    std::uint64_t h = 1469598103934665603ull;
    template <class T> void add(const T& v) {
        unsigned char c[sizeof(T)];
        std::memcpy(c, &v, sizeof(T));
        for (unsigned char x : c) { h ^= x; h *= 1099511628211ull; }
    }
};

void hash_signals(Hash& h, const FlowSignals& s) {
    h.add(s.ns); h.add(s.valid); h.add(s.bid_levels); h.add(s.ask_levels);
    h.add(s.bid); h.add(s.ask); h.add(s.bid_qty); h.add(s.ask_qty); h.add(s.mid); h.add(s.spread_ticks);
    h.add(s.microprice.num); h.add(s.microprice.den); h.add(s.micro_offset_ticks);
    h.add(s.obi1); h.add(s.obi_short); h.add(s.obi_deep); h.add(s.obi_deep_w); h.add(s.short_used); h.add(s.deep_used);
    h.add(s.ofi_last); h.add(s.ofi_multi_last); h.add(s.ofi_window); h.add(s.ofi_multi_window); h.add(s.ofi_norm);
    h.add(s.buy_exec); h.add(s.sell_exec); h.add(s.unknown_exec); h.add(s.ask_exec_drop); h.add(s.bid_exec_drop);
    h.add(s.ask_cancel); h.add(s.bid_cancel); h.add(s.ask_add); h.add(s.bid_add); h.add(s.hidden_buy); h.add(s.hidden_sell);
    h.add(s.trade_imbalance); h.add(s.momentum_ticks); h.add(s.mom_norm); h.add(s.score); h.add(s.persist); h.add(s.persist_ns);
    h.add(s.obi_ewma); h.add(s.prior_high); h.add(s.prior_low); h.add(s.bid_window_low); h.add(s.ask_window_high);
    h.add(s.bid_window_start); h.add(s.ask_window_start); h.add(s.mean_bid_qty); h.add(s.mean_ask_qty);
    h.add(s.absorption); h.add(s.withdrawal); h.add(s.books); h.add(s.trades); h.add(s.malformed); h.add(s.evicted);
    h.add(s.untradable);
    const auto& t = s.total;
    h.add(t.buy); h.add(t.sell); h.add(t.unknown); h.add(t.bid_exec_drop); h.add(t.ask_exec_drop); h.add(t.bid_cancel);
    h.add(t.ask_cancel); h.add(t.bid_add); h.add(t.ask_add); h.add(t.hidden_buy); h.add(t.hidden_sell);
}

/// The stream is a short pattern (it stays in cache, as a live book does),
/// replayed with time and volume shifted forward each lap.
struct Stream {
    std::vector<Event> base;
    std::size_t total = 0;
    std::int64_t ns_span = 0, vol_span = 0;
};

Stream make(std::size_t total, std::size_t levels) {
    Stream s;
    s.base = make_stream(std::min<std::size_t>(total, 2000), levels);
    s.total = total;
    s.ns_span = s.base.back().ns - s.base.front().ns + 1'000'000;
    for (const auto& e : s.base) if (!e.book) s.vol_span = e.volume;
    return s;
}

void feed(FlowEngine& f, const Event& e, std::int64_t dns, std::int64_t dvol) {
    if (e.book) f.on_book(e.bid, e.nb, e.ask, e.na, e.ns + dns);
    else f.on_trade(e.px, e.volume + dvol, 0, e.ns + dns);
}

template <class Fn>
void replay(const Stream& s, Fn&& fn) {
    std::size_t done = 0;
    for (std::int64_t lap = 0; done < s.total; ++lap)
        for (std::size_t i = 0; i < s.base.size() && done < s.total; ++i, ++done) fn(s.base[i], lap * s.ns_span, lap * s.vol_span);
}

double best_ns_per_event(const Stream& st, int reps, int mode) {
    using namespace altair::live;
    const ObiSignalRules rules{};
    double best = 1e300;
    volatile int sink = 0;
    for (int r = 0; r < reps; ++r) {
        FlowEngine f;
        std::string why;
        const auto t0 = Clock::now();
        replay(st, [&](const Event& e, std::int64_t dns, std::int64_t dvol) {
            feed(f, e, dns, dvol);
            // What the 8 OBI models of one underlying do on each tick of its
            // future: mode 1 the old way (reason text every time), mode 2 the
            // gate (the bare decision; text only when a type fires).
            if (mode == 1)
                for (int m = 0; m < 2; ++m)
                    for (const ObiType t : kObiTypes) sink = sink + obi_signal(t, f.signals(), rules, why);
            if (mode == 2)
                for (int m = 0; m < 2; ++m)
                    for (const ObiType t : kObiTypes)
                        if (obi_decide(t, f.signals(), rules) != 0) sink = sink + obi_signal(t, f.signals(), rules, why);
        });
        const double ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / static_cast<double>(st.total);
        best = std::min(best, ns);
    }
    return best;
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 400'000;
    for (const std::size_t levels : {std::size_t{5}, std::size_t{50}}) {
        if (argc > 2 && std::strtoull(argv[2], nullptr, 10) != levels) continue;
        const Stream st = make(n, levels);
        // Checksum: every signal after every event, and every A-D answer --
        // and obi_decide() must agree with obi_signal() on every one.
        Hash h;
        FlowEngine f;
        std::string why;
        int fired = 0, disagree = 0;
        replay(st, [&](const Event& e, std::int64_t dns, std::int64_t dvol) {
            feed(f, e, dns, dvol);
            hash_signals(h, f.signals());
            for (const auto t : altair::live::kObiTypes) {
                const int d = altair::live::obi_signal(t, f.signals(), {}, why);
                disagree += d != altair::live::obi_decide(t, f.signals(), {});
                h.add(d);
                fired += d != 0;
                if (d != 0) for (char c : why) h.add(c);
            }
        });
        const double engine = best_ns_per_event(st, 7, 0);
        const double old_way = best_ns_per_event(st, 7, 1) - engine;
        const double gated = best_ns_per_event(st, 7, 2) - engine;
        std::printf("%2zu levels, %zu events: engine %.1f ns/event; 8 A-D checks: with text %.1f ns, gated %.1f ns; "
                    "fired %d, decide/signal disagree %d, checksum %016llx\n",
                    levels, n, engine, old_way, gated, fired, disagree, static_cast<unsigned long long>(h.h));
    }
    return 0;
}
