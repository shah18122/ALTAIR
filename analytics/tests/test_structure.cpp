// P6-02 acceptance tests for analytics/structure.hpp.
//
// Test 1 is the card: the Chikou span is today's close drawn 26 bars in the
// past, so a strategy that reads it where it is DRAWN is standing 26 bars ago
// holding today's close. The test measures what that is worth by running the
// same rule both ways on the same series.
//
// Tests 2 and 3 are the same defect wearing different clothes: pivots computed
// from today's own high, and swing points declared at the bar they occur.
//
// Test 4: Heikin-Ashi closes are not prices, and filling at one is filling
// inside the bar.
//
// No check description here may contain the substring FAIL.

#include <analytics/structure.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>

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

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

struct Lcg {
    std::uint64_t s;
    double uniform()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & ((1ULL << 53) - 1))
               / static_cast<double>(1ULL << 53);
    }
    double normal()
    {
        const double u1 = uniform();
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1 + 1e-300))
               * std::cos(6.283185307179586 * u2);
    }
};

} // namespace

using namespace altair;

namespace {

constexpr std::size_t kBars = 3000;

void make_bars(Bar* out, std::size_t n, std::uint64_t seed)
{
    Lcg g{seed};
    double p = 2'400'000.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double o = p;
        p += g.normal() * 2000.0;
        const double c = p;
        const double wick = std::fabs(g.normal()) * 800.0;
        out[i].ts = Timestamp{static_cast<std::int64_t>(i)};
        out[i].open = Price{static_cast<std::int64_t>(o)};
        out[i].close = Price{static_cast<std::int64_t>(c)};
        out[i].high = Price{static_cast<std::int64_t>((o > c ? o : c) + wick)};
        out[i].low = Price{static_cast<std::int64_t>((o < c ? o : c) - wick)};
        out[i].volume = Qty{1000};
    }
}

Bar bars[kBars];

// ── 1 ────────────────────────────────────────────────────────────────────
// THE card.
void the_chikou_span_is_drawn_in_a_past_it_did_not_know()
{
    std::printf("\n1 the_chikou_span_is_drawn_in_a_past_it_did_not_know\n");
    make_bars(bars, kBars, 0x1C40D0A1);

    Ichimoku<256> ich{9, 26, 52, 26};
    // Two versions of the same rule: "go long when the Chikou span is above
    // the price it is drawn over." One acts at the bar the comparison is
    // KNOWN at; the other acts at the bar it is DRAWN at.
    double honest_pnl = 0.0, cheating_pnl = 0.0;
    int honest_trades = 0, cheating_trades = 0;
    std::size_t max_hindsight = 0;

    for (std::size_t i = 0; i < kBars; ++i) {
        (void)ich.update(bars[i]);
        const auto ch = ich.chikou();
        const auto ref = ich.chikou_reference();
        if (!ch || !ref) { continue; }
        if (ch->hindsight() > max_hindsight) {
            max_hindsight = ch->hindsight();
        }
        const bool bullish = ch->value > ref->value;

        // HONEST: act at ch->known_at, which is the current bar. Hold one bar.
        if (bullish && ch->known_at + 1 < kBars) {
            honest_pnl += static_cast<double>(
                bars[ch->known_at + 1].close.raw()
                - bars[ch->known_at].close.raw());
            ++honest_trades;
        }
        // CHEATING: act at ch->describes -- the bar the line is DRAWN at,
        // which is 26 bars before the value existed.
        if (bullish && ch->describes + 1 < kBars) {
            cheating_pnl += static_cast<double>(
                bars[ch->describes + 1].close.raw()
                - bars[ch->describes].close.raw());
            ++cheating_trades;
        }
    }

    std::printf("    the SAME Chikou rule, on the SAME %zu bars:\n"
                "      acted on at known_at  (honest)   %5d trades,"
                " Rs %10.2f\n"
                "      acted on at describes (drawn)    %5d trades,"
                " Rs %10.2f\n"
                "      hindsight carried by every value: %zu bars\n",
                kBars, honest_trades, honest_pnl / 100.0,
                cheating_trades, cheating_pnl / 100.0, max_hindsight);

    check(max_hindsight == 26,
          "every Chikou value carries exactly 26 bars of hindsight, and says"
          " so: known_at minus describes is the displacement");
    check(cheating_pnl > 4.0 * std::fabs(honest_pnl),
          "acting where the line is DRAWN is enormously more profitable than"
          " acting where it became known -- on a random walk, which has no"
          " edge in it at all");
    std::printf("    -> and that is the whole trap. The line is drawn in the"
                " PAST, so reading it\n       there looks like reading history."
                " It is not history; it is today's close\n       standing"
                " twenty-six bars ago. The most profitable indicator ever"
                " backtested,\n       and it has never made anyone a rupee.\n");

    check(honest_trades == cheating_trades,
          "the two versions take the same NUMBER of trades on the same signal"
          " -- only the bar they act on differs, which is what makes the gap"
          " pure look-ahead and not a different strategy");

    // The reference half was known when it happened.
    Ichimoku<256> j{9, 26, 52, 26};
    for (std::size_t i = 0; i < 60; ++i) { (void)j.update(bars[i]); }
    const auto ref = j.chikou_reference().value();
    check(ref.hindsight() == 0,
          "the price the Chikou is compared AGAINST carries no hindsight -- it"
          " was known when it happened, so only one half of the comparison is"
          " the problem");
    check(j.chikou().value().describes == ref.describes,
          "and the two halves describe the same bar, which is what makes the"
          " comparison meaningful once you stand in the right place");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void pivots_come_from_the_session_that_finished()
{
    std::printf("\n2 pivots_come_from_the_session_that_finished\n");
    Bar yesterday{};
    yesterday.open = Price{2'395'000};
    yesterday.high = Price{2'412'000};
    yesterday.low  = Price{2'388'000};
    yesterday.close = Price{2'405'000};

    const auto p = pivots_for_next_session(yesterday, 41);
    check(p.has_value(), "a completed session yields levels");
    if (!p) { return; }
    std::printf("    session 41 H %.2f L %.2f C %.2f  ->  levels for session"
                " %zu:\n      R2 %.2f  R1 %.2f  P %.2f  S1 %.2f  S2 %.2f\n",
                static_cast<double>(yesterday.high.raw()) / 100.0,
                static_cast<double>(yesterday.low.raw()) / 100.0,
                static_cast<double>(yesterday.close.raw()) / 100.0,
                p->for_session, p->r2 / 100.0, p->r1 / 100.0,
                p->pivot / 100.0, p->s1 / 100.0, p->s2 / 100.0);

    check(near(p->pivot, (2'412'000.0 + 2'388'000.0 + 2'405'000.0) / 3.0, 1e-9),
          "the pivot is the mean of the previous session's high, low and"
          " close");
    check(p->from_session == 41 && p->for_session == 42,
          "and the result says which session it came FROM and which it is FOR"
          " -- two fields, because collapsing them is the bug");
    check(p->r1 > p->pivot && p->r2 > p->r1 && p->r3 > p->r2
          && p->s1 < p->pivot && p->s2 < p->s1 && p->s3 < p->s2,
          "the levels are strictly ordered around the pivot");
    std::printf("    -> there is deliberately no overload taking today's bar"
                " and returning today's\n       levels. That function cannot be"
                " written honestly: it needs today's high\n       before the"
                " session opens, and the signature is the only place to say"
                " so.\n");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void a_swing_high_is_not_a_swing_until_the_bars_after_it_agree()
{
    std::printf("\n3 a_swing_high_is_not_a_swing_until_the_bars_after_it"
                "_agree\n");
    make_bars(bars, kBars, 0x5017465);

    SwingDetector<3> det;
    int swings = 0;
    std::size_t lag_min = 999, lag_max = 0;
    for (std::size_t i = 0; i < 800; ++i) {
        const auto s = det.update(bars[i]);
        if (!s) { continue; }
        ++swings;
        const std::size_t lag = s->confirmed_at - s->at;
        if (lag < lag_min) { lag_min = lag; }
        if (lag > lag_max) { lag_max = lag; }
    }
    std::printf("    %d confirmed swings in 800 bars, confirmation lag %zu to"
                " %zu bars (K = %zu)\n",
                swings, lag_min, lag_max,
                SwingDetector<3>::confirmation_lag());
    check(swings > 20, "a usable number of swings was found");
    check(lag_min == 3 && lag_max == 3,
          "EVERY swing is confirmed exactly K bars after it happened -- never"
          " sooner, because 'no later bar exceeded it' is a statement about"
          " bars that have not printed");
    check(det.state() != StructureState::Unknown,
          "and higher-high / higher-low structure is derived from confirmed"
          " swings only");

    // The detector cannot emit anything until it has both sides.
    SwingDetector<3> fresh;
    int emitted = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        if (fresh.update(bars[i]).has_value()) { ++emitted; }
    }
    check(emitted == 0,
          "with fewer than 2K+1 bars it emits nothing at all, rather than"
          " guessing from the left-hand side it does have");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void a_heikin_ashi_close_is_not_a_price()
{
    std::printf("\n4 a_heikin_ashi_close_is_not_a_price\n");
    make_bars(bars, kBars, 0xA0A0FF11);

    HeikinAshi ha;
    int outside = 0, inside = 0;
    double sum_edge = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < 1500; ++i) {
        const auto h = ha.update(bars[i]);
        if (!h) { continue; }
        const double lo = static_cast<double>(h->real.low.raw());
        const double hi = static_cast<double>(h->real.high.raw());
        if (h->close < lo || h->close > hi) { ++outside; } else { ++inside; }
        // The edge a TREND FOLLOWER gets, which is the case that matters: it
        // buys on a bullish synthetic bar, and on those the HA close sits
        // below the real one because it averages in the open and the low.
        if (h->close > h->open) {
            sum_edge += static_cast<double>(h->real.close.raw()) - h->close;
            ++n;
        }
    }
    std::printf("    %zu synthetic bars:\n"
                "      HA close inside the real bar's range   %d\n"
                "      HA close outside it                    %d\n"
                "      mean (real close - HA close)           Rs %.2f\n",
                n, inside, outside, (sum_edge / static_cast<double>(n)) / 100.0);
    check(outside == 0,
          "the HA close always sits INSIDE the real bar's range, which is"
          " exactly why filling at it is flattering: it is an average of four"
          " numbers and never a level anything traded at");
    check(inside == 1500, "on every one of the 1500 bars");
    check(sum_edge / static_cast<double>(n) > 0.0,
          "and on the bullish bars a trend follower buys, the HA close sits"
          " BELOW the real one -- so filling there hands the backtest a better"
          " entry than the exchange offered, on exactly the bars it trades");
    std::printf("    -> so the synthetic bar carries `real` alongside it. A"
                " backtest fills against\n       the bar that happened; the"
                " smoothing is for the signal, not for the fill.\n");

    // The recurrence, and what it means for a caller starting mid-series.
    HeikinAshi a, b;
    (void)a.update(bars[0]);
    for (std::size_t i = 1; i < 40; ++i) {
        (void)a.update(bars[i]);
        (void)b.update(bars[i]);        // started one bar later
    }
    const auto va = a.update(bars[40]).value();
    const auto vb = b.update(bars[40]).value();
    std::printf("    two HA series over the same bar, started one bar apart:"
                " opens differ by Rs %.3g\n"
                "      (the open recurrence carries a factor of 1/2, so a"
                " different seed halves every bar)\n",
                std::fabs(va.open - vb.open) / 100.0);
    check(va.close == vb.close,
          "the HA CLOSE is memoryless -- it depends only on the current bar, so"
          " two series that started at different points agree on it exactly");
    check(va.open != vb.open,
          "the HA OPEN is a recurrence and the two still differ 40 bars later");
    check(std::fabs(va.open - vb.open) < 1.0,
          "though by less than a paisa: the seed halves every bar, so it washes"
          " out in about 40. The honest statement is that joining mid-series"
          " matters for the first few dozen bars and not after -- a warmup, not"
          " a permanent divergence, and the difference between those two is"
          " whether a live feed can be joined at all");
    check(va.index != vb.index,
          "and the bar index says how many synthetic bars each has produced,"
          " which is how a caller knows whether it is past that warmup");
}

} // namespace

int main()
{
    std::printf("altair market structure tests\n");
    the_chikou_span_is_drawn_in_a_past_it_did_not_know();
    pivots_come_from_the_session_that_finished();
    a_swing_high_is_not_a_swing_until_the_bars_after_it_agree();
    a_heikin_ashi_close_is_not_a_price();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
