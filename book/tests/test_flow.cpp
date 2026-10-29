// Tests for book/flow.hpp, the live order-book imbalance engine.
//
// No check description here may contain the substring FAIL.

#include <book/flow.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

bool near(double a, double b, double tol = 1e-12) { return std::fabs(a - b) <= tol; }

using altair::flow::FlowConfig;
using altair::flow::FlowEngine;
using altair::flow::Level;

constexpr std::int64_t kMs = 1'000'000;

/// A book of `n` levels a side, 5 paise apart, every level `bq` / `aq`.
struct TestBook {
    std::vector<Level> bid, ask;
};
TestBook ladder(std::int64_t best_bid, std::int64_t best_ask, std::size_t n, std::int64_t bq, std::int64_t aq)
{
    TestBook b;
    for (std::size_t k = 0; k < n; ++k) {
        b.bid.push_back({best_bid - 5 * static_cast<std::int64_t>(k), bq});
        b.ask.push_back({best_ask + 5 * static_cast<std::int64_t>(k), aq});
    }
    return b;
}
void feed(FlowEngine& e, const TestBook& b, std::int64_t ns)
{
    e.on_book(b.bid.data(), b.bid.size(), b.ask.data(), b.ask.size(), ns);
}

void worked_examples()
{
    std::printf("\n1 worked examples\n");
    const auto obi = altair::flow::imbalance(12'000, 4'000);
    check(obi && near(*obi, 0.5), "OBI (12,000 - 4,000) / (12,000 + 4,000) = +0.50");
    check(!altair::flow::imbalance(0, 0), "no size at all is no reading, not 0.00");
    // Bid 999.95 x 8,000, ask 1,000.00 x 2,000.
    const auto mp = altair::flow::microprice(99'995, 8'000, 100'000, 2'000);
    check(mp && mp->num == 2'000LL * 99'995 + 8'000LL * 100'000 && mp->den == 10'000, "microprice is an exact rational");
    check(mp && near(mp->value(), 99'999.0), "microprice = 999.99 rupees");
    check(mp && mp->value() > 0.5 * (99'995 + 100'000), "and it sits ABOVE the mid 999.975: heavy bids lean up");
    check(!altair::flow::microprice(100'000, 1, 100'000, 1), "a locked book has no microprice");
    check(!altair::flow::microprice(100'005, 1, 100'000, 1), "a crossed book has no microprice");
    // OFI: bid steps up with 500, ask unchanged price, size 300 -> 200.
    // Bid 100 -> 103 with 500 (up: +500); ask 105 -> 110 (up: + the old 300).
    const auto e = altair::flow::ofi_level(100, 400, 105, 300, 103, 500, 110, 200);
    check(e == 800, "CKS OFI: bid up adds its new size, ask up adds the old ask size");
    check(altair::flow::ofi_level(100, 400, 105, 300, 100, 400, 105, 300) == 0, "an unchanged book has zero OFI");
    check(altair::flow::ofi_level(100, 400, 105, 300, 100, 100, 105, 300) == -300, "300 off the bid is -300");
}

void levels_and_weights()
{
    std::printf("\n2 five levels and fifty\n");
    FlowConfig c;
    c.decay = 0.5;
    FlowEngine e(c);
    TestBook b = ladder(100'000, 100'005, 50, 100, 100);
    for (std::size_t k = 0; k < 5; ++k) b.bid[k].qty = 300;   // heavy near the touch
    for (std::size_t k = 5; k < 50; ++k) b.ask[k].qty = 1'000; // heavy far away
    feed(e, b, 1 * kMs);
    const auto& s = e.signals();
    check(s.valid && s.short_used == 5 && s.deep_used == 50, "5 and 50 levels both used");
    check(near(s.obi_short, (1500.0 - 500.0) / 2000.0), "5-level OBI is exact: +0.50");
    const double deep_b = 1500.0 + 45 * 100.0, deep_a = 500.0 + 45 * 1000.0;
    check(near(s.obi_deep, (deep_b - deep_a) / (deep_b + deep_a)), "50-level unweighted OBI is exact");
    check(s.obi_deep < 0.0 && s.obi_deep_w > 0.0, "far size dominates unweighted, the weighted one keeps the touch");
    double wb = 0, wa = 0;
    for (std::size_t k = 0; k < 50; ++k) {
        const double w = std::exp(-0.5 * static_cast<double>(k));
        wb += w * static_cast<double>(b.bid[k].qty);
        wa += w * static_cast<double>(b.ask[k].qty);
    }
    check(near(s.obi_deep_w, (wb - wa) / (wb + wa), 1e-12), "weighted OBI matches w_i = exp(-lambda (i-1)) by hand");

    FlowEngine f(c);
    feed(f, ladder(100'000, 100'005, 5, 100, 100), 1 * kMs);
    check(f.signals().deep_used == 5, "a 5-level book asked for 50 sums the 5 it has");
    TestBook thin = ladder(100'000, 100'005, 5, 100, 100);
    thin.ask.resize(2);
    feed(f, thin, 2 * kMs);
    check(f.signals().short_used == 2, "both sides are summed over the same number of levels");
}

void malformed_and_untradable()
{
    std::printf("\n3 what is not used\n");
    FlowEngine e;
    TestBook b = ladder(100'000, 100'005, 5, 100, 100);
    b.bid[3].px = b.bid[1].px;   // out of order
    feed(e, b, 1 * kMs);
    check(e.signals().bid_levels == 3 && e.signals().malformed == 1, "levels from the first out-of-order one are dropped");
    TestBook pad = ladder(100'000, 100'005, 5, 100, 100);
    pad.ask[2].qty = 0;
    feed(e, pad, 2 * kMs);
    check(e.signals().ask_levels == 2, "zero padding ends the side");
    feed(e, ladder(100'005, 100'000, 5, 100, 100), 3 * kMs);
    check(!e.signals().valid && e.signals().untradable == 1, "a crossed book gives no reading");
    feed(e, ladder(100'000, 100'005, 5, 100, 100), 4 * kMs);
    check(e.signals().valid && e.signals().ofi_last == 0, "and no OFI is measured across it");
}

void executed_cancelled_hidden()
{
    std::printf("\n4 executed, cancelled, hidden\n");
    FlowConfig c;
    c.settle_ns = 500 * kMs;
    {   // trade first, then the book that shows it
        FlowEngine e(c);
        feed(e, ladder(100'000, 100'005, 5, 1'000, 1'000), 1 * kMs);
        e.on_trade(100'005, 10'000, 0, 2 * kMs);   // the volume baseline
        e.on_trade(100'005, 10'400, 0, 3 * kMs);   // a 400 buy at the ask
        TestBook b = ladder(100'000, 100'005, 5, 1'000, 1'000);
        b.ask[0].qty = 600;
        feed(e, b, 4 * kMs);
        const auto& s = e.signals();
        check(s.buy_exec == 400 && s.sell_exec == 0, "the volume change is the print size; at the ask it is a buy");
        check(s.ask_exec_drop == 400 && s.ask_cancel == 0, "the 400 off the ask is executed, not cancelled");
        e.on_trade(100'005, 10'400, 0, 2'000 * kMs);   // time passes
        check(e.signals().hidden_buy == 0 && e.signals().ask_cancel == 0, "nothing left over to settle");
    }
    {   // the book first, then the print
        FlowEngine e(c);
        feed(e, ladder(100'000, 100'005, 5, 1'000, 1'000), 1 * kMs);
        e.on_trade(100'000, 5'000, 0, 2 * kMs);
        TestBook b = ladder(100'000, 100'005, 5, 1'000, 1'000);
        b.bid[0].qty = 700;
        feed(e, b, 3 * kMs);
        e.on_trade(100'000, 5'300, 0, 4 * kMs);   // a 300 sell at the bid, arriving after
        check(e.signals().bid_exec_drop == 300 && e.signals().sell_exec == 300, "a print after its drop still matches it");
        e.on_trade(100'000, 5'300, 0, 900 * kMs);
        check(e.signals().bid_cancel == 0, "so nothing becomes a cancel");
    }
    {   // a drop no print explains, and a print no drop explains
        FlowEngine e(c);
        feed(e, ladder(100'000, 100'005, 5, 1'000, 1'000), 1 * kMs);
        e.on_trade(100'005, 1'000, 0, 2 * kMs);
        TestBook b = ladder(100'000, 100'005, 5, 1'000, 1'000);
        b.ask[1].qty = 250;   // 750 pulled from the second ask level
        feed(e, b, 3 * kMs);
        e.on_trade(100'000, 1'200, 0, 4 * kMs);   // a 200 sell, and the bid never drops
        check(e.signals().ask_cancel == 0, "a drop waits settle_ns before it is called a cancel");
        e.on_trade(100'000, 1'200, 0, 600 * kMs);
        check(e.signals().ask_cancel == 750, "then 750 cancelled from the ask");
        check(e.signals().hidden_sell == 200, "and the 200 sell no visible bid explained is hidden execution");
    }
    {   // a sell print does not explain an ask drop, nor a print that never reached the level
        FlowEngine e(c);
        feed(e, ladder(100'000, 100'005, 5, 1'000, 1'000), 1 * kMs);
        e.on_trade(100'005, 0, 0, 2 * kMs);
        e.on_trade(100'005, 100, 0, 3 * kMs);   // a 100 buy at 100.05
        TestBook b = ladder(100'000, 100'005, 5, 1'000, 1'000);
        b.ask[2].qty = 900;   // 100 off 100.15: the print did not reach it
        feed(e, b, 4 * kMs);
        check(e.signals().ask_exec_drop == 0, "a buy at 100.05 cannot take size at 100.15");
    }
}

void shown_depth_edge()
{
    std::printf("\n5 a level that slides out of sight is not a cancel\n");
    FlowConfig c;
    c.settle_ns = 0;
    FlowEngine e(c);
    feed(e, ladder(100'000, 100'005, 5, 1'000, 1'000), 1 * kMs);
    // A new best bid one tick up: the 5-level window now ends a tick higher,
    // so the old fifth level is out of view.
    feed(e, ladder(100'005, 100'010, 5, 1'000, 1'000), 2 * kMs);
    e.on_trade(0, -1, 0, 3 * kMs);
    check(e.signals().bid_cancel == 0, "the old fifth bid is out of view, not cancelled");
    check(e.signals().ask_cancel == 1'000, "the old best ask, gone with no print, is cancelled");
    check(e.signals().bid_add == 1'000, "the new best bid is an add");
}

void persistence_and_breakout()
{
    std::printf("\n6 persistence, EWMA, breakout\n");
    FlowEngine e;
    for (int i = 0; i < 4; ++i) feed(e, ladder(100'000, 100'005, 5, 900, 300), (1 + i) * 100 * kMs);
    check(e.signals().persist == 4 && e.signals().persist_ns == 300 * kMs, "four updates beyond +0.30, 300 ms");
    check(e.signals().obi_ewma > 0.4, "the EWMA follows");
    feed(e, ladder(100'000, 100'005, 5, 300, 900), 600 * kMs);
    check(e.signals().persist == -1, "a flip restarts the run on the other side");
    feed(e, ladder(100'010, 100'015, 5, 300, 300), 700 * kMs);
    check(near(e.signals().prior_high, 100'002.5) && e.signals().mid > e.signals().prior_high,
          "the high is from before this update, so a new mid can break it");
    check(near(e.signals().momentum_ticks, 2.0), "momentum: the mid rose two ticks over the window");
}

void absorption_and_withdrawal()
{
    std::printf("\n7 absorption and withdrawal\n");
    FlowConfig c;
    c.settle_ns = 100 * kMs;
    {
        FlowEngine e(c);
        TestBook b = ladder(100'000, 100'005, 5, 1'000, 1'000);
        feed(e, b, 1 * kMs);
        e.on_trade(100'000, 0, 0, 2 * kMs);
        std::int64_t vol = 0;
        for (int i = 0; i < 5; ++i) {   // 5 x 800 sold into the bid; the bid is refilled each time
            vol += 800;
            e.on_trade(100'000, vol, 0, (10 + 20 * i) * kMs);
            feed(e, b, (11 + 20 * i) * kMs);
        }
        e.on_trade(100'000, vol, 0, 300 * kMs);   // let every print settle
        const auto& s = e.signals();
        check(s.sell_exec == 4'000 && s.bid_window_low == 100'000, "4,000 sold and the bid never fell");
        check(s.hidden_sell == 4'000, "no displayed drop explained the prints: hidden size reloaded the bid");
        check(s.absorption == 1, "buyers absorbed the selling: +1");
    }
    {
        FlowEngine e(c);
        feed(e, ladder(100'000, 100'005, 5, 1'000, 1'000), 1 * kMs);
        e.on_trade(100'005, 0, 0, 2 * kMs);
        std::int64_t vol = 0;
        for (int i = 1; i <= 3; ++i) {   // asks pulled, buyers lift each new ask
            vol += 200;
            e.on_trade(100'000 + 5 * i, vol, 0, (10 * i) * kMs);
            feed(e, ladder(100'000 + 5 * i, 100'005 + 5 * i, 5, 1'000, 300), (10 * i + 1) * kMs);
        }
        e.on_trade(100'015, vol, 0, 300 * kMs);
        const auto& s = e.signals();
        check(s.ask - s.ask_window_start >= 10 && s.buy_exec > 0, "asks lifted two ticks with real buys");
        check(s.withdrawal == 1, "liquidity withdrawal on the ask: +1");
    }
}

void volume_and_eviction()
{
    std::printf("\n8 volume re-base, a full window\n");
    FlowEngine e;
    feed(e, ladder(100'000, 100'005, 5, 1'000, 1'000), 1 * kMs);
    e.on_trade(100'005, 50'000, 0, 2 * kMs);
    check(e.signals().buy_exec == 0, "the first volume is a baseline, not a print");
    e.on_trade(100'005, 100, 0, 3 * kMs);
    check(e.signals().buy_exec == 0, "volume that went backwards re-bases; nothing invented");
    e.on_trade(100'005, -1, 70, 4 * kMs);
    check(e.signals().buy_exec == 70, "with no volume the print's own size is used");

    FlowConfig c;
    c.capacity = 64;
    FlowEngine f(c);
    for (int i = 0; i < 200; ++i) feed(f, ladder(100'000, 100'005, 5, 1'000 + i, 1'000), (1 + i) * kMs);
    check(f.signals().evicted > 0, "more events than the ring holds are evicted and counted");
}

void random_invariants()
{
    std::printf("\n9 random books: invariants\n");
    std::mt19937_64 rng(7);
    FlowEngine e;
    std::int64_t bid = 2'500'000, vol = 0;
    bool ok_obi = true, ok_mp = true, ok_sums = true, ok_norm = true;
    for (int i = 0; i < 100'000; ++i) {
        bid += 5 * static_cast<std::int64_t>(rng() % 3) - 5;
        TestBook b = ladder(bid, bid + 5 * static_cast<std::int64_t>(1 + rng() % 2), 1 + rng() % 50, 0, 0);
        for (auto& l : b.bid) l.qty = 1 + static_cast<std::int64_t>(rng() % 5'000);
        for (auto& l : b.ask) l.qty = 1 + static_cast<std::int64_t>(rng() % 5'000);
        const std::int64_t ns = (1 + i) * kMs;
        feed(e, b, ns);
        if (rng() % 2) {
            vol += static_cast<std::int64_t>(rng() % 500);
            e.on_trade(rng() % 2 ? b.ask[0].px : b.bid[0].px, vol, 0, ns + kMs / 2);
        }
        const auto& s = e.signals();
        if (!s.valid) continue;
        for (const double v : {s.obi1, s.obi_short, s.obi_deep, s.obi_deep_w, s.trade_imbalance})
            ok_obi = ok_obi && v >= -1.0 && v <= 1.0;
        ok_mp = ok_mp && s.microprice.value() >= static_cast<double>(s.bid) && s.microprice.value() <= static_cast<double>(s.ask);
        ok_sums = ok_sums && s.buy_exec >= 0 && s.sell_exec >= 0 && s.bid_cancel >= 0 && s.ask_cancel >= 0
               && s.ask_exec_drop <= s.buy_exec + s.ask_exec_drop && s.bid_add >= 0 && s.ask_add >= 0;
        ok_norm = ok_norm && s.ofi_norm >= -1.0 && s.ofi_norm <= 1.0 && std::isfinite(s.score);
    }
    check(ok_obi, "every OBI and the trade imbalance lie in [-1, +1]");
    check(ok_mp, "every microprice lies within [bid, ask]");
    check(ok_sums, "window sums are never negative");
    check(ok_norm, "OFI_norm lies in [-1, +1]; S is finite");
}

void speed()
{
    std::printf("\n10 speed\n");
    FlowEngine e;
    std::vector<TestBook> books;
    std::mt19937_64 rng(11);
    std::int64_t bid = 2'500'000;
    for (int i = 0; i < 1'024; ++i) {
        bid += 5 * static_cast<std::int64_t>(rng() % 3) - 5;
        TestBook b = ladder(bid, bid + 5, 50, 0, 0);
        for (auto& l : b.bid) l.qty = 1 + static_cast<std::int64_t>(rng() % 5'000);
        for (auto& l : b.ask) l.qty = 1 + static_cast<std::int64_t>(rng() % 5'000);
        books.push_back(std::move(b));
    }
    constexpr int kN = 200'000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kN; ++i) feed(e, books[static_cast<std::size_t>(i) & 1023u], (1 + i) * kMs);
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / kN;
    std::printf("  50-level book, every level's size new each update (worst case): %.0f ns per update\n", ns);
    // The usual TBT diff: one to three levels change.
    FlowEngine g;
    TestBook b = books[0];
    std::vector<TestBook> diffs;
    for (int i = 0; i < 1'024; ++i) {
        const std::size_t n = 1 + rng() % 3;
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t lv = rng() % 10;
            (rng() % 2 ? b.bid : b.ask)[lv].qty = 1 + static_cast<std::int64_t>(rng() % 5'000);
        }
        diffs.push_back(b);
    }
    const auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < kN; ++i) feed(g, diffs[static_cast<std::size_t>(i) & 1023u], (1 + i) * kMs);
    const auto t3 = std::chrono::steady_clock::now();
    const double ns2 = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count()) / kN;
    std::printf("  50-level book, 1-3 levels changed (typical diff): %.0f ns per update\n", ns2);
    check(e.signals().valid, "the engine kept up");
}

} // namespace

int main()
{
    worked_examples();
    levels_and_weights();
    malformed_and_untradable();
    executed_cancelled_hidden();
    shown_depth_edge();
    persistence_and_breakout();
    absorption_and_withdrawal();
    volume_and_eviction();
    random_invariants();
    speed();
    std::printf("\n%s (%d)\n", failures == 0 ? "ALL PASSED" : "SOME CHECKS DID NOT PASS", failures);
    return failures == 0 ? 0 : 1;
}
