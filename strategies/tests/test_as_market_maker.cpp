// Tests for strategies/as_market_maker.hpp -- Avellaneda-Stoikov quoting on
// one-minute futures bars, on paper.

#include <strategies/as_market_maker.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <vector>

namespace {

using namespace altair;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

/// `days` sessions of 375 one-minute bars, 09:15-15:29 IST, a random walk with
/// sd `vol` points per minute and an overnight gap.
std::vector<AsBar> walk(int days, double vol, unsigned seed) {
    std::mt19937 g(seed);
    std::normal_distribution<double> z(0.0, 1.0);
    std::vector<AsBar> v;
    double px = 25'000.0;
    const std::int64_t day0 = 20'000;
    for (int d = 0; d < days; ++d) {
        px += 40.0 * z(g);
        for (int m = 0; m < 375; ++m) {
            AsBar b;
            b.t = (day0 + d) * 86'400 + (9 * 60 + 15 + m) * 60;
            b.o = px;
            double hi = px, lo = px;
            for (int s = 0; s < 4; ++s) {
                px += vol * 0.5 * z(g);
                hi = std::max(hi, px);
                lo = std::min(lo, px);
            }
            b.h = hi; b.l = lo; b.c = px;
            v.push_back(b);
        }
    }
    return v;
}

} // namespace

int main() {
    std::printf("Avellaneda-Stoikov market maker on futures bars\n");
    const auto bars = walk(30, 6.0, 3);
    AsMmParams p;
    const auto r = run_as_market_maker(bars, p);
    std::printf("    %zu quotes, %zu fills, %zu sessions\n", r.quotes.size(), r.fills.size(), r.days.size());
    check(!r.fills.empty(), "it trades");
    check(r.days.size() == 30, "one row per session");

    // ---- flat every session, the cap held ----------------------------------------
    {
        std::map<std::int64_t, int> net;
        bool cap = true, reasons = true, sell_buy = true;
        for (const auto& f : r.fills) {
            const int lots = std::abs(f.q_after - f.q_before);
            net[f.t / 86'400] += f.side * lots;
            if (std::abs(f.q_after) > p.max_lots) { cap = false; }
            if (f.reason.empty()) { reasons = false; }
            if (f.side != 1 && f.side != -1) { sell_buy = false; }
        }
        bool flat = true;
        for (const auto& [d, n] : net) { if (n != 0) { flat = false; } }
        check(flat, "every session ends flat: nothing carried overnight");
        check(cap, "inventory never beyond the cap");
        check(reasons, "every fill carries its reason");
        check(sell_buy, "every fill is one side or the other");
    }
    // ---- quotes: inside the window, post-only, leaning against inventory -----------
    {
        bool window = true, post_only = true, lean = true;
        std::size_t i = 0;
        for (const auto& q : r.quotes) {
            const int m = static_cast<int>((q.t % 86'400) / 60);
            if (m < p.first_quote_min || m >= p.last_quote_min) { window = false; }
            while (i < bars.size() && bars[i].t < q.t) { ++i; }
            if (q.bid_on && q.bid > bars[i].c - p.tick + 1e-9) { post_only = false; }
            if (q.ask_on && q.ask < bars[i].c + p.tick - 1e-9) { post_only = false; }
            if (q.q > 0 && q.bid_on && q.ask_on && (q.ask - bars[i].c) > (bars[i].c - q.bid) + 2 * p.tick) { lean = false; }
            if (q.q >= p.max_lots && q.bid_on) { window = false; }
        }
        check(window, "no quote outside 09:20-15:15, no bid at the cap");
        check(post_only, "post-only: bid under the close, ask over it");
        check(lean, "long: the ask is never further from the mid than the bid");
    }
    // ---- nothing decided from a later bar ------------------------------------------
    {
        std::size_t total = 0;
        for (const std::size_t cut : {bars.size() / 3, bars.size() / 2 + 37, bars.size() - 200}) {
            total += as_leak_check(bars, p, cut);
        }
        check(total == 0, "leak check: quotes and fills up to each cut unchanged by another future");
    }
    // ---- the regime scales risk aversion, from the day BEFORE ----------------------
    {
        std::map<std::int64_t, double> stress;
        for (int d = 0; d < 30; ++d) { stress[20'000 + d] = 1.0; }
        const auto rs = run_as_market_maker(bars, p, &stress);
        double w0 = 0.0, w1 = 0.0;
        std::size_t n0 = 0, n1 = 0;
        for (const auto& q : r.quotes) { if (q.bid_on && q.ask_on && q.q == 0) { w0 += q.ask - q.bid; ++n0; } }
        for (const auto& q : rs.quotes) { if (q.bid_on && q.ask_on && q.q == 0) { w1 += q.ask - q.bid; ++n1; } }
        check(n0 > 0 && n1 > 0 && w1 / static_cast<double>(n1) > w0 / static_cast<double>(n0),
              "P(stress) 1 widens the flat-book spread");
        bool first_day_plain = true;
        for (const auto& f : rs.fills) { if (f.t / 86'400 == 20'000 && f.p_stress != 0.0) { first_day_plain = false; } }
        check(first_day_plain, "the first session has no earlier regime and is quoted plain");
        check(as_leak_check(bars, p, bars.size() / 2, &stress) == 0, "leak check with the regime");
    }
    // ---- markouts are for the report only ------------------------------------------
    {
        std::size_t with = 0;
        for (const auto& f : r.fills) { if (std::isfinite(f.markout_pts)) { ++with; } }
        check(with > 0, "markouts written");
    }
    std::printf(failures == 0 ? "all passed\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
