// P5-07 acceptance tests for strategies/calendar.hpp.
//
// Test 1 is the card, and it is measured rather than assumed. The calendar
// no-arbitrage condition is stated over log-moneyness, the two expiries have
// different forwards, and so the same STRIKE is two different points on the
// surface.
//
// The first thing the measurement says is that on a plain index calendar the
// error is SMALL -- a few percent of the calendar margin. Saying so is part of
// the card: an effect that is real and second-order should be reported as
// real and second-order.
//
// The second thing it says is that on a stock going ex-dividend between the
// two expiries the forward gap is seven times larger and points the other way,
// and there the strike-aligned test does not invent arbitrage -- it MASKS a
// genuine one. That is the worse failure. An invented opportunity costs money
// once and then gets fixed; a masked one is never seen at all.
//
// Test 2 is the other trap: implied vol falling with maturity is an ordinary
// stressed term structure, not an arbitrage. Only total variance falling is.
//
// No check description here may contain the substring FAIL.

#include <strategies/calendar.hpp>

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

double rupees(std::int64_t p) { return static_cast<double>(p) / 100.0; }

} // namespace

using namespace altair;

namespace {

constexpr std::int64_t kNs = 1'000'000'000LL;

ChargeSchedule schedule()
{
    ChargeSchedule s{};
    s.valid_from = Timestamp{0};
    s.valid_to = Timestamp{std::numeric_limits<std::int64_t>::max()};
    s.verified = true;
    s.equity_futures.present  = true;
    s.equity_futures.stt      = rate_from(0.0005L);
    s.equity_futures.stt_side = ChargeSide::Sell;
    s.equity_futures.exch_txn_nse = rate_from(0.0000173L);
    s.equity_futures.stamp    = rate_from(0.00002L);
    s.equity_futures.stamp_side = ChargeSide::Buy;
    s.equity_futures.ipft     = rate_from(0.0000005L);
    return s;
}

BrokerageRule broker()
{
    BrokerageRule b{};
    b.flat_per_order = Notional{2000};
    b.pct = 0;
    b.take_lower = false;
    return b;
}

/// An SVI slice with a given ATM total variance and a given SKEW IN VOL SPACE.
///
/// Parametrised by dsigma/dk rather than by SVI's `b` directly, because `b` is
/// in variance units and scales with T: the same `b` that gives a realistic
/// skew at 30 days gives an absurd one at 7, and an inadmissible slice into
/// the bargain. This is the parametrisation that stays meaningful across the
/// term structure.
SviParams slice(double atm_vol, double t, double dsig_dk, double rho)
{
    SviParams p{};
    p.rho = rho;
    p.m = 0.0;
    p.sigma = 0.10;
    const double w0 = atm_vol * atm_vol * t;
    // dw/dk at k = 0 (with m = 0) is b*rho; and dw/dk = 2*sigma*T*dsigma/dk.
    p.b = 2.0 * atm_vol * t * dsig_dk / rho;
    p.a = w0 - p.b * p.sigma;
    return p;
}

/// The same smile with every total variance multiplied by `lambda`.
///
/// w(k) = a + b*(...), so scaling a and b scales w exactly. Used to build a
/// calendar pair whose margin is (lambda - 1) * w_short(k) at EVERY k -- so
/// whether it is clean or not is settled by construction rather than by the
/// scan under test.
SviParams scaled(const SviParams& p, double lambda)
{
    SviParams q = p;
    q.a = p.a * lambda;
    q.b = p.b * lambda;
    return q;
}

// ── 1a ───────────────────────────────────────────────────────────────────
void on_an_index_calendar_the_misalignment_is_second_order()
{
    std::printf("\n1a on_an_index_calendar_the_misalignment_is_second_order\n");
    const double r = 0.065, T1 = 7.0 / 365.0, T2 = 21.0 / 365.0;
    const std::int64_t F1 = 2'400'000;
    const std::int64_t F2 = static_cast<std::int64_t>(
        static_cast<double>(F1) * std::exp(r * (T2 - T1)) + 0.5);

    DatedSlice s1{}, s2{};
    s1.slice = slice(0.14, T1, -0.70, -0.72);
    s1.forward = Price{F1};
    s1.t = Years{T1};
    // Total variance up 60% -- a normal, mildly flat term structure. Clean by
    // construction: the margin is 0.60 * w_short(k) at every k.
    s2.slice = scaled(s1.slice, 1.60);
    s2.forward = Price{F2};
    s2.t = Years{T2};

    const Price lo{2'160'000}, hi{2'640'000};
    const auto by_k = scan_option_calendar(s1, s2, lo, hi, 401,
                                           CalendarAlignment::Moneyness);
    const auto by_K = scan_option_calendar(s1, s2, lo, hi, 401,
                                           CalendarAlignment::Strike);
    check(by_k.has_value() && by_K.has_value(), "both alignments scan");
    if (!by_k || !by_K) { return; }

    const double delta = std::log(static_cast<double>(F2)
                                  / static_cast<double>(F1));
    std::printf("    near forward %.2f (7d), far %.2f (21d) -- the forward"
                " moves %.4f in k.\n"
                "      aligned by MONEYNESS: %3d/%d violations, worst margin"
                " %+.6f\n"
                "      aligned by STRIKE   : %3d/%d violations, worst margin"
                " %+.6f\n"
                "      the strike-aligned margin is %.1f%% of the correct one"
                " at its worst point.\n",
                static_cast<double>(F1) / 100.0,
                static_cast<double>(F2) / 100.0, delta,
                by_k->violations, by_k->points, by_k->worst,
                by_K->violations, by_K->points, by_K->worst,
                100.0 * by_K->worst / by_k->worst);

    check(by_k->clean(),
          "aligned by moneyness the surface is clean everywhere, which it is"
          " by construction -- the far slice is the near one scaled by 1.6");
    check(by_K->clean(),
          "and aligned by STRIKE it is ALSO clean here: on a plain index"
          " calendar with 14 days of carry between the expiries, the"
          " misalignment is NOT enough to invent an opportunity");
    check(by_K->worst > by_k->worst,
          "and here it OVERSTATES the margin, because an index far forward sits"
          " ABOVE the near one: that reads the far slice at a LOWER k, and on a"
          " downside skew a lower k means a higher total variance");
    std::printf("    -> the distortion is the forward shift times the local"
                " skew, so its SIGN follows\n       the direction the forward"
                " moved. Here it is +0.6%% of the margin: real, biased,\n"
                "       and second order. Writing that up as a headline would"
                " be as wrong as\n       ignoring it. What it does is fix the"
                " threshold at which it starts to matter --\n       and a"
                " dividend, in 1b, moves the forward the other way and three"
                " times as far.\n");

    check(scan_option_calendar(s1, s2, lo, hi, 401,
                               CalendarAlignment::Unknown).error()
          == CalendarSpreadError::BadSlice,
          "and a caller who did not choose an alignment is refused: the two"
          " answers differ, one of them is wrong, and there is no default that"
          " is not a decision");
}

// ── 1b ───────────────────────────────────────────────────────────────────
// THE card.
void a_dividend_makes_the_strike_aligned_test_mask_a_real_violation()
{
    std::printf("\n1b a_dividend_makes_the_strike_aligned_test_mask_a_real"
                "_violation\n");
    // A single stock at 2,950 with a Rs 40 dividend going ex between the two
    // expiries. The far forward is BELOW the near one, which is the ordinary
    // state of affairs for a dividend payer and not an anomaly.
    const double r = 0.065, T1 = 7.0 / 365.0, T2 = 35.0 / 365.0;
    const double S = 295'000.0, div = 4'000.0;
    const std::int64_t F1 = static_cast<std::int64_t>(S * std::exp(r * T1));
    const std::int64_t F2 = static_cast<std::int64_t>(S * std::exp(r * T2)
                                                      - div);
    const double delta = std::log(static_cast<double>(F2)
                                  / static_cast<double>(F1));

    DatedSlice s1{}, s2{};
    s1.slice = slice(0.28, T1, -0.60, -0.60);
    s1.forward = Price{F1};
    s1.t = Years{T1};
    // A GENUINE violation: total variance FALLS by half a percent from the
    // near expiry to the far one, at every k. Small, real, and exactly the
    // kind a scanner exists to find.
    s2.slice = scaled(s1.slice, 0.995);
    s2.forward = Price{F2};
    s2.t = Years{T2};

    // Rs 2,500 to Rs 3,400 -- the ladder around a 2,950 STOCK. (An earlier
    // draft of this test used index-scale strikes here against a stock-scale
    // forward, which put every sample at k = 2.1, out on the right wing where
    // the skew has the opposite sign. The measurement was real; it was just
    // measuring a part of the surface nobody quotes.)
    const Price lo{250'000}, hi{340'000};
    const auto by_k = scan_option_calendar(s1, s2, lo, hi, 401,
                                           CalendarAlignment::Moneyness);
    const auto by_K = scan_option_calendar(s1, s2, lo, hi, 401,
                                           CalendarAlignment::Strike);
    check(by_k.has_value() && by_K.has_value(), "both alignments scan");
    if (!by_k || !by_K) { return; }

    std::printf("    stock 2950, Rs 40 ex-dividend between the expiries:\n"
                "      near forward %.2f (7d), far forward %.2f (35d)\n"
                "      the far forward is BELOW the near one, so k moves"
                " %+.5f -- %.1fx the\n      pure-carry shift of 1a, and in the"
                " OPPOSITE direction.\n"
                "      total variance falls 0.5%% from near to far: a REAL"
                " calendar arbitrage\n      at every strike.\n\n"
                "      aligned by MONEYNESS: %3d/%d violations found\n"
                "      aligned by STRIKE   : %3d/%d violations found\n",
                static_cast<double>(F1) / 100.0,
                static_cast<double>(F2) / 100.0, delta,
                std::fabs(delta) / 0.00249,
                by_k->violations, by_k->points,
                by_K->violations, by_K->points);

    check(by_k->violations == by_k->points,
          "aligned by moneyness EVERY sampled strike reports the violation,"
          " which is right -- the margin is (lambda - 1) * w at every k and"
          " lambda is below one");
    check(by_K->violations < by_k->violations,
          "aligned by STRIKE the scanner finds FEWER of them: it reads the far"
          " slice at a strike's own far-moneyness rather than at the near"
          " slice's k, and where the smile rises in k that reports a higher"
          " total variance than the far expiry has at the k being tested");
    std::printf("    -> %d of %d genuine violations are MASKED, and they are"
                " the ones ABOVE the\n       smile's minimum, where w rises"
                " with k. Below it the distortion pushes the\n       other way"
                " and the violation was already visible. So on this pair the"
                "\n       strike-aligned scan does not invent arbitrage -- it"
                " HIDES it, on one wing.\n\n       That is the worse of the two"
                " failures. An invented opportunity costs money\n       once"
                " and then gets fixed; a hidden one is simply never seen.\n",
                by_k->violations - by_K->violations,
                by_k->violations);
    check(by_k->violations - by_K->violations > 40,
          "and it is not a handful of boundary strikes -- the masking covers a"
          " wide band of the ladder");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void falling_vol_is_not_falling_variance()
{
    std::printf("\n2 falling_vol_is_not_falling_variance\n");
    const double T1 = 7.0 / 365.0, T2 = 60.0 / 365.0;
    const std::int64_t F = 2'400'000;

    // A stressed market: front month at 30 vol, two months out at 18.
    DatedSlice s1{}, s2{};
    s1.slice = slice(0.30, T1, -0.70, -0.72);
    s1.forward = Price{F};
    s1.t = Years{T1};
    s2.slice = slice(0.18, T2, -0.70, -0.72);
    s2.forward = Price{F};      // same forward, so alignment is not the
    s2.t = Years{T2};           // variable under test here

    const double w1 = svi_total_variance(s1.slice, 0.0);
    const double w2 = svi_total_variance(s2.slice, 0.0);
    std::printf("    front 30 vol over 7 days, back 18 vol over 60 days:\n"
                "      implied VOL      0.30 -> 0.18   (FALLS by %.0f%%)\n"
                "      total VARIANCE   %.5f -> %.5f   (RISES by %.0f%%)\n",
                100.0 * (0.30 - 0.18) / 0.30, w1, w2,
                100.0 * (w2 - w1) / w1);

    const auto r = scan_option_calendar(s1, s2, Price{2'280'000},
                                        Price{2'520'000}, 201,
                                        CalendarAlignment::Moneyness);
    check(r.has_value(), "the stressed pair scans");
    if (!r) { return; }
    check(w2 > w1,
          "total variance RISES even though vol falls by 40%, because T rises"
          " by more than sigma squared falls");
    check(r->clean(),
          "so there is no calendar arbitrage here -- a scanner comparing"
          " implied VOLS would flag every stressed market in the sample, which"
          " is precisely the sample anyone bothers to scan");

    // And a genuine violation is still caught.
    DatedSlice bad = s2;
    bad.slice = slice(0.10, T2, -0.70, -0.72);
    const double wb = svi_total_variance(bad.slice, 0.0);
    const auto v = scan_option_calendar(s1, bad, Price{2'280'000},
                                        Price{2'520'000}, 201,
                                        CalendarAlignment::Moneyness);
    check(v.has_value() && !v->clean() && wb < w1,
          "while a back month whose total variance is genuinely BELOW the"
          " front month's is caught, so nothing here blunted the test");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void a_futures_calendar_is_symmetric_and_priced_in_carry()
{
    std::printf("\n3 a_futures_calendar_is_symmetric_and_priced_in_carry\n");
    const ChargeSchedule sch = schedule();
    const BrokerageRule br = broker();
    const Qty lot{75};
    const double r = 0.065;

    FuturesCalendarQuote q{};
    q.near_t = Years{7.0 / 365.0};
    q.far_t = Years{35.0 / 365.0};
    q.rate = r;
    q.dividend_yield = 0.012;
    q.near_leg = Touch{Price{2'399'975}, Price{2'400'025}};
    // Carry over 28 days at 6.5% less a 1.2% yield is ~Rs 97.8 on a 24,000
    // future. Quote the far Rs 97.00 over: a little cheap, but only a little.
    q.far_leg = Touch{Price{2'409'675}, Price{2'409'725}};

    const auto o = scan_futures_calendar(q, lot, Exchange::NSE,
                                         Duration{100 * 1'000'000LL}, sch, br);
    check(o.has_value(), "the futures calendar scans");
    if (!o) { return; }
    std::printf("    near %.2f / far %.2f, 28 days apart, carry %.2f%% less"
                " %.2f%% yield:\n"
                "      touch spread   %8.2f\n"
                "      fair spread    %8.2f\n"
                "      implied carry  %8.3f%%\n"
                "      gross Rs %.2f   cost Rs %.2f   NET Rs %.2f\n",
                static_cast<double>(q.near_leg.bid.raw()) / 100.0,
                static_cast<double>(q.far_leg.ask.raw()) / 100.0,
                r * 100.0, q.dividend_yield * 100.0,
                o->touch_spread / 100.0, o->fair_spread / 100.0,
                o->implied_carry * 100.0,
                rupees(o->gross_value.raw()), rupees(o->cost.raw()),
                rupees(o->net.raw()));

    check(o->executability == Executability::Executable,
          "both legs are futures and both short freely, so a futures calendar"
          " is reachable in BOTH directions -- the second structure in Phase 5"
          " with no executability caveat, after the box");
    check(o->implied_carry > 0.0, "the spread is quoting a positive carry");
    check(o->cost.raw() > 0, "four futures legs cost something");
    check(o->net.raw() == o->gross_value.raw() - o->cost.raw(),
          "and net is gross minus cost exactly, in paise");
    check(o->net.raw() < 0,
          "a spread one rupee away from its fair carry does not pay for four"
          " futures legs -- the usual answer, and the reason the scanner"
          " reports it rather than filtering it away");

    // The stale far month. It is thinner, its last print is older, and a
    // naive spread measures whatever the near month did in between.
    FuturesCalendarQuote stale = q;
    stale.far_ts = Timestamp{4 * kNs};
    check(scan_futures_calendar(stale, lot, Exchange::NSE,
                                Duration{100 * 1'000'000LL}, sch, br).error()
          == CalendarSpreadError::StaleLeg,
          "a far leg four seconds behind the near one is refused -- the far"
          " month is always the thinner of the two, so this is the normal case"
          " rather than an edge case");
    check(scan_futures_calendar(q, lot, Exchange::NSE, Duration{0}, sch, br)
              .error() == CalendarSpreadError::NoSkewLimit,
          "and a caller who set no staleness limit is refused rather than"
          " given one");

    FuturesCalendarQuote inverted = q;
    inverted.near_t = Years{35.0 / 365.0};
    inverted.far_t = Years{7.0 / 365.0};
    check(scan_futures_calendar(inverted, lot, Exchange::NSE,
                                Duration{100 * 1'000'000LL}, sch, br).error()
          == CalendarSpreadError::BadTenors,
          "and expiries the wrong way round are refused rather than producing"
          " a negative carry interval that would read as a large edge");
}

} // namespace

int main()
{
    std::printf("altair calendar spread scanner tests\n");
    on_an_index_calendar_the_misalignment_is_second_order();
    a_dividend_makes_the_strike_aligned_test_mask_a_real_violation();
    falling_vol_is_not_falling_variance();
    a_futures_calendar_is_symmetric_and_priced_in_carry();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
