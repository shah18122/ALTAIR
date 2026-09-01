// P5-06 acceptance tests for strategies/parity.hpp.
//
// Test 1 is the card: the textbook butterfly condition assumes an evenly
// spaced strike ladder, NIFTY's ladder is 50 near the money and 100 in the
// wings, and three consecutive listed strikes routinely straddle the step
// change. On a perfectly convex Black-76 chain the unweighted form reports a
// large, confident, entirely fictional arbitrage. Measured, not argued.
//
// Test 2 is the second way to manufacture option arbitrage: a mid-to-mid
// parity deviation is the spread, not an opportunity.
//
// No check description here may contain the substring FAIL.

#include <analytics/greeks.hpp>
#include <strategies/parity.hpp>

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
double rupees(std::int64_t p) { return static_cast<double>(p) / 100.0; }

} // namespace

using namespace altair;

namespace {

ChargeSchedule schedule()
{
    ChargeSchedule s{};
    s.valid_from = Timestamp{0};
    s.valid_to = Timestamp{std::numeric_limits<std::int64_t>::max()};
    s.verified = true;

    s.equity_options.present  = true;
    s.equity_options.stt      = rate_from(0.0015L);   // 0.15% post-April, SELL
    s.equity_options.stt_side = ChargeSide::Sell;
    s.equity_options.exch_txn_nse = rate_from(0.0003503L);
    s.equity_options.stamp    = rate_from(0.00003L);
    s.equity_options.stamp_side = ChargeSide::Buy;
    s.equity_options.ipft     = rate_from(0.000005L);
    s.equity_options.basis    = TurnoverBasis::Premium;

    s.equity_futures.present  = true;
    s.equity_futures.stt      = rate_from(0.0005L);
    s.equity_futures.stt_side = ChargeSide::Sell;
    s.equity_futures.exch_txn_nse = rate_from(0.0000173L);
    s.equity_futures.stamp    = rate_from(0.00002L);
    s.equity_futures.stamp_side = ChargeSide::Buy;
    s.equity_futures.ipft     = rate_from(0.0000005L);

    s.equity_delivery.present  = true;
    s.equity_delivery.stt      = rate_from(0.001L);
    s.equity_delivery.stt_side = ChargeSide::Both;
    s.equity_delivery.exch_txn_nse = rate_from(0.0000297L);
    s.equity_delivery.stamp    = rate_from(0.00015L);
    s.equity_delivery.ipft     = rate_from(0.000001L);
    return s;
}

BrokerageRule broker()
{
    BrokerageRule b{};
    b.flat_per_order = Notional{2000};      // Rs 20 per leg
    b.pct = 0;
    b.take_lower = false;
    return b;
}

/// A clean Black-76 call price in paise, from a FLAT surface. Flat is the
/// point: a flat vol surface is convex in strike by construction, so any
/// butterfly the scanner finds on it is an artefact of the scanner.
std::int64_t call_paise(double F, double K, double T, double vol)
{
    const auto g = black76(OptionRight::Call, Price{static_cast<std::int64_t>(F)},
                           Price{static_cast<std::int64_t>(K)}, Years{T},
                           Vol{vol}, 0.0);
    return g ? static_cast<std::int64_t>(g->price + 0.5) : 0;
}

/// Wrap a fair value in a symmetric two-sided quote `half` paise wide.
Touch quote(std::int64_t fair, std::int64_t half)
{
    return Touch{Price{fair - half}, Price{fair + half}};
}

/// The mid of a quote. Defined HERE in the test and nowhere in the header,
/// because the header has no business forming one -- this exists only to
/// measure what the mid would have claimed.
double detail_mid(const Touch& t)
{
    return 0.5 * static_cast<double>(t.bid.raw() + t.ask.raw());
}

// ── 1 ────────────────────────────────────────────────────────────────────
// THE card.
void the_textbook_butterfly_assumes_an_even_ladder()
{
    std::printf("\n1 the_textbook_butterfly_assumes_an_even_ladder\n");
    // NIFTY forward 24,000.00, 30 days, flat 14 vol. Prices in paise.
    const double F = 2'400'000.0, T = 30.0 / 365.0, vol = 0.14;

    // First, the EVEN ladder: 24000 / 24100 / 24200, step 100 throughout.
    {
        ButterflyQuote q{};
        q.k1 = Price{2'400'000}; q.k2 = Price{2'410'000}; q.k3 = Price{2'420'000};
        q.c1 = quote(call_paise(F, 2'400'000, T, vol), 0);
        q.c2 = quote(call_paise(F, 2'410'000, T, vol), 0);
        q.c3 = quote(call_paise(F, 2'420'000, T, vol), 0);
        const auto r = butterfly_margin(q);
        check(r.has_value(), "an even ladder measures");
        if (r) {
            std::printf("    EVEN ladder 24000/24100/24200 (gaps %ld/%ld):"
                        "  weighted %+.2f   unweighted %+.2f\n",
                        static_cast<long>(r->lower_gap / 100),
                        static_cast<long>(r->upper_gap / 100),
                        r->margin / 100.0, r->unweighted / 100.0);
            check(r->equally_spaced, "and is recognised as equally spaced");
            check(r->margin > 0.0 && r->unweighted > 0.0,
                  "both forms agree the surface is convex, because on an even"
                  " ladder they are the same test");
        }
    }

    // Now the ladder as NSE actually lists it across the step change:
    // 24000 / 24100 / 24300. Gaps of 100 and 200.
    ButterflyQuote q{};
    q.k1 = Price{2'400'000}; q.k2 = Price{2'410'000}; q.k3 = Price{2'430'000};
    q.c1 = quote(call_paise(F, 2'400'000, T, vol), 0);
    q.c2 = quote(call_paise(F, 2'410'000, T, vol), 0);
    q.c3 = quote(call_paise(F, 2'430'000, T, vol), 0);
    const auto r = butterfly_margin(q);
    check(r.has_value(), "the uneven ladder measures");
    if (!r) { return; }

    std::printf("    UNEVEN ladder 24000/24100/24300 (gaps %ld/%ld) on the"
                " SAME flat, convex,\n    arbitrage-free surface:\n"
                "      weighted    %+9.2f  <- correct\n"
                "      unweighted  %+9.2f  <- textbook form\n",
                static_cast<long>(r->lower_gap / 100),
                static_cast<long>(r->upper_gap / 100),
                r->margin / 100.0, r->unweighted / 100.0);
    check(!r->equally_spaced, "the ladder is correctly seen as uneven");
    check(r->margin > 0.0,
          "the SPACING-WEIGHTED margin is positive -- the surface is convex,"
          " which it is by construction, being a flat Black-76 chain");
    check(r->unweighted < 0.0,
          "while the UNWEIGHTED textbook form reports a NEGATIVE butterfly:"
          " an arbitrage, on a surface that has none");
    std::printf("    -> a scanner using C(K1) - 2C(K2) + C(K3) on this triple"
                " would flag Rs %.2f\n       of free money per unit, every"
                " tick, on every strike where NIFTY's ladder\n       steps"
                " from 50 to 100. The strikes come from the spec store; the"
                " STEP\n       must never be assumed (rule 1).\n",
                -r->unweighted / 100.0);

    // The magnitude is a full strike gap times the local slope, so it scales
    // with how uneven the ladder is rather than being a rounding wobble.
    check(-r->unweighted > 100.0,
          "and it is not a small artefact -- it is the strike asymmetry times"
          " the slope of the call curve, which is rupees, not paise");

    // Genuinely negative convexity is still caught by the correct form.
    ButterflyQuote bent = q;
    bent.c2 = quote(q.c2.bid.raw() + 15'000, 0);   // body Rs 150 too rich
    const auto br2 = butterfly_margin(bent);
    check(br2.has_value() && br2->margin < 0.0,
          "and when the body really is too rich the weighted form goes"
          " negative, so the correction did not blunt the test");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void parity_is_checked_at_the_touch_never_at_the_mid()
{
    std::printf("\n2 parity_is_checked_at_the_touch_never_at_the_mid\n");
    const ChargeSchedule sch = schedule();
    const BrokerageRule br = broker();
    const Qty lot{75};                  // from the spec store, not a literal
    ShortCashCapability cap{};

    const double F = 2'400'000.0, T = 30.0 / 365.0, vol = 0.14;
    const std::int64_t K = 2'400'000;
    const std::int64_t c_fair = call_paise(F, K, T, vol);
    // Put from parity at zero rates: P = C - (F - K) = C, at the money.
    const std::int64_t p_fair = c_fair;

    // Both options quoted Rs 2.00 wide; the future Rs 0.50 wide. Perfectly
    // fair mids, so parity holds EXACTLY at the mid and by construction there
    // is no opportunity at all.
    ParityQuote q{};
    q.call = quote(c_fair, 200);
    q.put = quote(p_fair, 200);
    q.hedge = quote(static_cast<std::int64_t>(F), 50);
    q.hedge_leg = HedgeLeg::Future;
    q.strike = Price{K};
    q.t = Years{T};
    q.rate = 0.0;

    const auto o = scan_parity(q, lot, Exchange::NSE, Timestamp{0}, cap, sch,
                               br);
    check(o.has_value(), "the parity triple scans");
    if (!o) { return; }
    std::printf("    a chain where parity holds EXACTLY at the mid, options"
                " Rs 2.00 wide:\n"
                "      mid deviation    %+8.2f\n"
                "      touch deviation  %+8.2f\n",
                o->mid_deviation / 100.0, o->touch_deviation / 100.0);
    check(near(o->mid_deviation, 0.0, 1.0),
          "the mid-to-mid deviation is zero, as constructed");
    check(o->touch_deviation < 0.0,
          "and the TOUCH deviation is negative -- crossing four spreads costs"
          " more than the parity relation is off by, which is the normal state"
          " of a well-quoted chain");
    check(!o->actionable(),
          "so there is nothing to do, which a mid-based scanner would not have"
          " been able to say");

    // Now push the call Rs 8 rich at the BID, so the touch deviation is real.
    ParityQuote rich = q;
    rich.call = Touch{Price{c_fair + 800}, Price{c_fair + 1200}};
    const auto ro = scan_parity(rich, lot, Exchange::NSE, Timestamp{0}, cap,
                                sch, br);
    check(ro.has_value(), "the dislocated chain scans");
    if (ro) {
        std::printf("    call bid pushed Rs 8.00 rich: touch %+.2f, gross"
                    " Rs %.2f, cost Rs %.2f, NET Rs %.2f\n",
                    ro->touch_deviation / 100.0,
                    rupees(ro->gross_value.raw()), rupees(ro->cost.raw()),
                    rupees(ro->net.raw()));
        check(ro->trade == ParityTrade::Conversion,
              "the synthetic is rich, so the trade is a conversion: sell the"
              " call, buy the put, buy the future");
        check(ro->touch_deviation > 0.0,
              "and the deviation survives crossing every spread");
        check(ro->mid_deviation > ro->touch_deviation,
              "while the mid still reports a LARGER number than is reachable"
              " -- the mid is always the optimistic one, which is why it is"
              " carried for comparison and never decided on");
    }
}

// ── 3 ────────────────────────────────────────────────────────────────────
void a_reversal_inherits_the_short_cash_problem_only_against_spot()
{
    std::printf("\n3 a_reversal_inherits_the_short_cash_problem_only_against"
                "_spot\n");
    const ChargeSchedule sch = schedule();
    const BrokerageRule br = broker();
    const Qty lot{75};
    ShortCashCapability retail{};       // no borrow

    const double F = 2'400'000.0, T = 30.0 / 365.0, vol = 0.14;
    const std::int64_t K = 2'400'000;
    const std::int64_t c_fair = call_paise(F, K, T, vol);

    // Call CHEAP at the ask -> the synthetic is cheap -> reversal: buy the
    // synthetic, SELL the hedge.
    ParityQuote q{};
    q.call = Touch{Price{c_fair - 1200}, Price{c_fair - 800}};
    q.put = quote(c_fair, 200);
    q.hedge = quote(static_cast<std::int64_t>(F), 50);
    q.strike = Price{K};
    q.t = Years{T};
    q.rate = 0.0;

    q.hedge_leg = HedgeLeg::Future;
    const auto fut = scan_parity(q, lot, Exchange::NSE, Timestamp{0}, retail,
                                 sch, br);
    q.hedge_leg = HedgeLeg::Spot;
    const auto spot = scan_parity(q, lot, Exchange::NSE, Timestamp{0}, retail,
                                  sch, br);
    check(fut.has_value() && spot.has_value(), "both hedges scan");
    if (!fut || !spot) { return; }

    check(fut->trade == ParityTrade::Reversal
          && spot->trade == ParityTrade::Reversal,
          "the same dislocation implies a reversal against either hedge");
    check(fut->executability == Executability::Executable,
          "hedged with a SHORT FUTURE it is executable -- futures short"
          " freely, so option parity against a future is symmetric in a way"
          " the cash-futures carry of P5-05 is not");
    check(spot->executability == Executability::ShortCashUnavailable,
          "hedged by SHORTING THE STOCK it is the same unavailable trade as"
          " P5-05: the constraint is on the hedge instrument, not on the"
          " option legs");
    check(!spot->net_edge().has_value() && fut->net_edge().has_value(),
          "so only the future-hedged version reports an edge at all");

    ParityQuote unknown = q;
    unknown.hedge_leg = HedgeLeg::Unknown;
    const auto u = scan_parity(unknown, lot, Exchange::NSE, Timestamp{0},
                               retail, sch, br);
    check(!u && u.error() == ParityError::HedgeUnknown,
          "and a caller who did not say what the hedge is gets REFUSED, not a"
          " guess: the hedge decides both the segment the bill is computed"
          " under and whether the trade is reachable, so an unnamed one would"
          " have produced a fully populated opportunity priced against an"
          " instrument nobody chose");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void a_box_is_a_loan_and_reports_its_rate()
{
    std::printf("\n4 a_box_is_a_loan_and_reports_its_rate\n");
    const ChargeSchedule sch = schedule();
    const BrokerageRule br = broker();
    const Qty lot{75};
    const double T = 30.0 / 365.0, vol = 0.14;

    // A box between 24000 and 24200, priced off a flat surface at a 6.5%
    // discount rate, quoted Rs 1.00 wide on every leg.
    const double r = 0.065;
    const double df = std::exp(-r * T);
    auto c = [&](std::int64_t K) {
        const auto g = black76(OptionRight::Call, Price{2'400'000}, Price{K},
                               Years{T}, Vol{vol}, r);
        return g ? static_cast<std::int64_t>(g->price + 0.5) : 0;
    };
    auto p = [&](std::int64_t K) {
        const auto g = black76(OptionRight::Put, Price{2'400'000}, Price{K},
                               Years{T}, Vol{vol}, r);
        return g ? static_cast<std::int64_t>(g->price + 0.5) : 0;
    };

    BoxQuote q{};
    q.k1 = Price{2'400'000};
    q.k2 = Price{2'420'000};
    q.c1 = quote(c(2'400'000), 100);
    q.c2 = quote(c(2'420'000), 100);
    q.p1 = quote(p(2'400'000), 100);
    q.p2 = quote(p(2'420'000), 100);
    q.t = Years{T};
    q.rate = r;

    // What the box costs at the MID -- i.e. if spreads did not exist. This is
    // the honest 6.5% loan the chain is quoting.
    const double mid_cost = detail_mid(q.c1) - detail_mid(q.c2)
                          - detail_mid(q.p1) + detail_mid(q.p2);
    const double payoff = static_cast<double>(q.k2.raw() - q.k1.raw());
    const double mid_rate = std::log(payoff / mid_cost) / T;

    const auto o = scan_box(q, lot, Exchange::NSE, Timestamp{0}, sch, br);
    check(o.has_value(), "the box scans");
    if (!o) { return; }
    std::printf("    24000/24200 box, 30 days, every leg Rs 1.00 wide,"
                " lot %ld:\n"
                "      cost at the touch  Rs %8.2f  (payoff is Rs %.2f"
                " certain)\n"
                "      implied rate       %8.3f%%\n"
                "      discounted payoff  Rs %8.2f\n"
                "      charges (4 legs)   Rs %8.2f\n"
                "      NET                Rs %8.2f\n",
                static_cast<long>(lot.raw()),
                o->touch_cost / 100.0,
                static_cast<double>(q.k2.raw() - q.k1.raw()) / 100.0,
                o->implied_rate * 100.0, o->discounted_payoff / 100.0,
                rupees(o->cost.raw()), rupees(o->net.raw()));

    check(o->executability == Executability::Executable,
          "a box needs no short cash anywhere -- all four legs are options --"
          " so it is the one scanner in Phase 5 with no executability caveat");
    check(mid_cost < payoff && mid_rate > 0.0,
          "at the MID the box costs less than its certain payoff, which is"
          " what makes it a loan: the chain is quoting a positive rate");
    check(near(mid_rate, r, 0.01),
          "and that rate is the 6.5% the options were priced with, recovered"
          " from four quotes -- so the measurement is sound before any spread"
          " is crossed");

    std::printf("    -> AT THE MID this box lends at %.2f%%. Crossing four"
                " Rs 1.00 spreads costs\n       Rs %.2f and turns it into"
                " %.2f%% -- a swing of %.0f PERCENTAGE POINTS of\n"
                "       annualised rate, out of four rupees.\n",
                mid_rate * 100.0, (o->touch_cost - mid_cost) / 100.0,
                o->implied_rate * 100.0,
                (mid_rate - o->implied_rate) * 100.0);
    check(o->implied_rate < 0.0,
          "at the TOUCH the same box lends at a NEGATIVE rate -- four spreads"
          " cost more than a month of financing is worth, so a fairly quoted"
          " box is never an opportunity and the spread is the whole story");
    check(df > 0.0 && df < 1.0 && near(o->discounted_payoff, payoff * df, 1.0),
          "and the discounted payoff is the certain amount times exp(-rT),"
          " which is the only part of a box that is not a market price");

    // The same four rupees at a shorter tenor. The rupee cost is identical;
    // the rate it destroys is not.
    BoxQuote short_dated = q;
    short_dated.t = Years{6.0 / 365.0};
    const auto sd = scan_box(short_dated, lot, Exchange::NSE, Timestamp{0},
                             sch, br);
    if (sd) {
        std::printf("    the SAME Rs 4.00 of spread at 6 days instead of 30:"
                    " implied rate %.1f%%\n       (was %.1f%%). Identical"
                    " rupees, %.1fx the damage in rate terms.\n",
                    sd->implied_rate * 100.0, o->implied_rate * 100.0,
                    sd->implied_rate / o->implied_rate);
        check(sd->implied_rate < o->implied_rate,
              "a shorter box is hurt far worse by the same rupee spread --"
              " which is why the scanner ranks on implied RATE and not on the"
              " rupee gap, and why the two order a chain differently");
    }

    check(o->net.raw() < 0,
          "and after the four legs' charges it is negative by more still");

    // The degenerate box a naive scanner loves: strikes the wrong way round.
    BoxQuote bad = q;
    bad.k1 = Price{2'420'000};
    bad.k2 = Price{2'400'000};
    check(scan_box(bad, lot, Exchange::NSE, Timestamp{0}, sch, br).error()
          == ParityError::BadStrikes,
          "inverted strikes are refused rather than producing a negative"
          " payoff that would read as a large opportunity");
}

} // namespace

int main()
{
    std::printf("altair parity / box / butterfly scanner tests\n");
    the_textbook_butterfly_assumes_an_even_ladder();
    parity_is_checked_at_the_touch_never_at_the_mid();
    a_reversal_inherits_the_short_cash_problem_only_against_spot();
    a_box_is_a_loan_and_reports_its_rate();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
