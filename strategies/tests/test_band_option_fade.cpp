// Tests for strategies/band_option_fade.hpp -- the band-touch short option.

#include <strategies/band_option_fade.hpp>

#include <cmath>
#include <cstdio>

namespace {

using namespace altair;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

constexpr std::int64_t kDay0 = 20'500LL * 86'400;   // some session, 00:00 IST
constexpr std::int64_t kOpen = kDay0 + (9 * 3600 + 15 * 60);

/// 75 five-minute bars from 09:15 at `level`, IV 15 %; `shape(k)` moves the close.
template <class F>
FadeDay day(double level, F shape) {
    FadeDay d;
    for (std::size_t k = 0; k < 75; ++k) {
        const double c = level + shape(k);
        const double o = k == 0 ? level : level + shape(k - 1);
        d.bars.push_back({kOpen + static_cast<std::int64_t>(k) * 300, o, std::max(o, c) + 2.0, std::min(o, c) - 2.0, c, 0.15});
    }
    d.decide_bar = 0;                                  // the 09:15 bar closes at 09:20
    d.exit_bar = 72;                                   // the 15:15 bar closes at 15:20
    d.expiry_ts = kDay0 + 3 * 86'400 + (15 * 3600 + 30 * 60);
    return d;
}

FadePolicy policy() {
    FadePolicy p;
    p.strike_step = 50.0;
    p.lot_size = 65.0;
    p.lots = 1;
    p.rate = 0.065;
    p.slippage_pts = 0.5;
    return p;
}

void test_quiet_day() {
    const auto d = day(25'000.0, [](std::size_t) { return 0.0; });
    const auto t = fade_day(d, 25'000.0, 0.004, 3, policy());
    check(t.has_value() && t->empty(), "a day that never reaches the band places no trade");
}

void test_call_fade() {
    // Rises 1 point a bar to +150 by bar 30 (above the +100.2 upper edge), then
    // falls back to the anchor by the close.
    const auto d = day(25'000.0, [](std::size_t k) {
        return k <= 30 ? 5.0 * static_cast<double>(k) : 150.0 - 5.0 * static_cast<double>(k - 30);
    });
    const double half = 0.004;   // 25,000 x e^0.004 = 25,100.2
    const auto t = fade_day(d, 25'000.0, half, 7, policy());
    check(t.has_value() && t->size() == 1 && (*t)[0].side == FadeSide::Call, "a touch of the upper band sells one call");
    if (!t || t->empty()) { return; }
    const FadeTrade& c = (*t)[0];
    check(c.strike == 25'150.0 && c.band_edge > 25'100.0 && c.band_edge < 25'101.0,
          "at the first strike at or above the band edge (25,100.2 -> 25,150)");
    // Bar 20's close is +100 (below the edge) but its high, +102, is above it.
    check(c.entry_t == kOpen + 21 * 300 && c.exit_t == kOpen + 73 * 300,
          "sold at the close of the first touching bar, bought back at 15:20");
    check(c.model == 7 && c.qty == 65.0, "one lot, and the trade knows which model placed it");
    check(c.entry_premium > c.exit_premium && c.gross > 0.0,
          "the move fades: the call is bought back for less than it was sold");
    check(std::fabs(c.gross - (c.entry_premium - c.exit_premium) * 65.0) < 1e-9, "gross = premium kept x quantity");
}

void test_put_and_slippage() {
    // Breaks the lower band at bar 40 and keeps falling: 200 points more by the close.
    const auto d = day(25'000.0, [](std::size_t k) {
        return k < 40 ? 0.0 : -200.0 - 200.0 * static_cast<double>(k - 40) / 32.0;
    });
    auto p = policy();
    const auto a = fade_day(d, 25'000.0, 0.004, 0, p);
    check(a.has_value() && a->size() == 1 && (*a)[0].side == FadeSide::Put && (*a)[0].strike == 24'900.0,
          "a touch of the lower band sells a put at the first strike at or below it");
    check(a.has_value() && !a->empty() && (*a)[0].gross < 0.0,
          "a break that extends loses: the put is bought back for more");
    p.slippage_pts = 0.0;
    const auto b = fade_day(d, 25'000.0, 0.004, 0, p);
    check(b.has_value() && !b->empty() && !a->empty()
              && std::fabs((b->front().gross - a->front().gross) - 2 * 0.5 * 65.0) < 1e-6,
          "slippage costs half a point each way: one point of premium a round trip");
}

void test_both_sides_and_refusals() {
    const auto d = day(25'000.0, [](std::size_t k) {
        return k < 10 ? 0.0 : (k < 30 ? 200.0 : -200.0);
    });
    const auto t = fade_day(d, 25'000.0, 0.004, 1, policy());
    check(t.has_value() && t->size() == 2, "a day that touches both edges sells a call and a put, once each");
    auto bad = policy();
    bad.strike_step = 0.0;
    check(!fade_day(d, 25'000.0, 0.004, 1, bad).has_value(), "no strike step: refused, not guessed");
    auto late = d;
    late.expiry_ts = late.bars[72].t + 300;   // expires at the square-off
    check(!fade_day(late, 25'000.0, 0.004, 1, policy()).has_value(), "an option that expires by the square-off is refused");
    check(!fade_day(d, 25'000.0, -0.004, 1, policy()).has_value(), "a negative band is refused");
}

} // namespace

int main() {
    std::printf("Band option fade\n");
    test_quiet_day();
    test_call_fade();
    test_put_and_slippage();
    test_both_sides_and_refusals();
    std::printf("Band option fade: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
