// Tests for strategies/band_option_fade.hpp -- the band-touch short option.

#include <strategies/band_option_fade.hpp>

#include <cmath>
#include <cstdio>
#include <string>

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


void test_strangle() {
    const double half = 0.004;   // edges 24,900.2 and 25,100.2 -> strikes 24,900 and 25,150
    const auto quiet = day(25'000.0, [](std::size_t) { return 0.0; });
    const auto q = strangle_day(quiet, 25'000.0, half, 4, policy(), StranglePolicy{});
    check(q.has_value() && q->has_value(), "a strangle is sold at the decision without waiting for a touch");
    if (!q || !*q) { return; }
    const StrangleDay& a = **q;
    check(a.call.strike == 25'150.0 && a.put.strike == 24'900.0 && a.call.entry_t == kOpen + 300,
          "call at the first strike at or above the upper edge, put at or below the lower, sold at 09:20");
    check(a.gross > 0.0 && a.call.gross > 0.0 && a.put.gross > 0.0 && a.fills.empty(),
          "a day that stays inside the band keeps time decay on both legs");

    // Trends up 2 points a bar from 09:20: 144 points by 15:20, far past the call.
    const auto trend = day(25'000.0, [](std::size_t k) { return 2.0 * static_cast<double>(k); });
    const auto held = strangle_day(trend, 25'000.0, half, 4, policy(), StranglePolicy{});
    StranglePolicy stop;
    stop.stop_multiple = 1.5;
    const auto stopped = strangle_day(trend, 25'000.0, half, 4, policy(), stop);
    check(held && *held && stopped && *stopped && (*held)->call.gross < 0.0,
          "a trend through the upper edge loses on the call");
    if (held && *held && stopped && *stopped) {
        const auto& c = (*stopped)->call;
        check(std::string{c.exit_reason} == "stop" && c.exit_t < (*held)->call.exit_t
                  && c.gross > (*held)->call.gross,
              "the stop buys the call back early, for a smaller loss than holding");
    }
    // Whole lots: 144 points leaves the call's delta under half a lot, so no hedge
    // is placed. 5 points a bar (360 by 15:20) takes it well past.
    StranglePolicy hedge;
    hedge.hedge = true;
    const auto none = strangle_day(trend, 25'000.0, half, 4, policy(), hedge);
    check(none && *none && (*none)->fills.empty(), "under half a lot of delta, the whole-lot hedge holds nothing");
    const auto steep = day(25'000.0, [](std::size_t k) { return 5.0 * static_cast<double>(k); });
    const auto bare = strangle_day(steep, 25'000.0, half, 4, policy(), StranglePolicy{});
    const auto hedged = strangle_day(steep, 25'000.0, half, 4, policy(), hedge);
    if (hedged && *hedged && bare && *bare) {
        long net = 0;
        for (const auto& f : (*hedged)->fills) { net += f.lots; }
        check(!(*hedged)->fills.empty() && net == 0 && (*hedged)->fills.front().lots > 0,
              "the hedge buys futures as the call's delta grows and is flat by the close");
        check((*hedged)->hedge_gross > 0.0 && (*hedged)->gross > (*bare)->gross,
              "hedged in whole lots, the trend costs less than unhedged");
    } else {
        check(false, "hedged strangle priced");
    }
    StranglePolicy bad;
    bad.stop_multiple = 0.8;
    check(!strangle_day(quiet, 25'000.0, half, 4, policy(), bad).has_value(),
          "a stop below the sale price is refused, not read as no stop");
}


void test_clock() {
    FadeClock c;
    c.trading = true;
    c.session_close = kDay0 + 15 * 3600 + 30 * 60;
    c.sessions_after = 0.0;
    const std::int64_t open = kDay0 + 9 * 3600 + 15 * 60;
    check(std::fabs(fade_detail::years(c, open, c.session_close) - 1.0 / 252.0) < 1e-12,
          "trading clock: a whole session is one 252nd of a year");
    check(std::fabs(fade_detail::years(FadeClock{}, open, c.session_close) - 22'500.0 / (365.0 * 86'400.0)) < 1e-12,
          "calendar clock: the same session is 6h15m of 365 days");
    c.sessions_after = 3.0;
    check(std::fabs(fade_detail::years(c, c.session_close + 3600, c.session_close + 3 * 86'400) - 3.0 / 252.0) < 1e-12,
          "after the close nothing of today is left, only the sessions to come");

    // A quiet day: decay from 09:20 to 15:20 is about a session's worth on the
    // trading clock and about a quarter of a day's on the calendar clock.
    auto quiet = day(25'000.0, [](std::size_t) { return 0.0; });
    quiet.expiry_ts = kDay0 + 3 * 86'400 + 15 * 3600 + 30 * 60;
    const auto cal = strangle_day(quiet, 25'000.0, 0.004, 0, policy(), StranglePolicy{});
    quiet.clock.trading = true;
    quiet.clock.session_close = kDay0 + 15 * 3600 + 30 * 60;
    quiet.clock.sessions_after = 3.0;
    const auto trd = strangle_day(quiet, 25'000.0, 0.004, 0, policy(), StranglePolicy{});
    check(cal && *cal && trd && *trd && (*trd)->gross > 2.0 * (*cal)->gross,
          "the trading clock credits a quiet session with far more decay than the calendar clock");
    quiet.clock.intraday_share = 0.65;
    const auto var = strangle_day(quiet, 25'000.0, 0.004, 0, policy(), StranglePolicy{});
    check(var && *var && trd && *trd && cal && *cal && (*var)->gross < (*trd)->gross && (*var)->gross > (*cal)->gross,
          "the variance clock, with a third of the variance overnight, credits decay between the two");
    c.intraday_share = 0.65;
    c.sessions_after = 0.0;
    check(std::fabs(fade_detail::years(c, open, c.session_close) - 0.65 / 252.0) < 1e-12,
          "variance clock: the session holds its share of the day's unit");
}

} // namespace

int main() {
    std::printf("Band option fade\n");
    test_quiet_day();
    test_call_fade();
    test_put_and_slippage();
    test_both_sides_and_refusals();
    test_strangle();
    test_clock();
    std::printf("Band option fade: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
