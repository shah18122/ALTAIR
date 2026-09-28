// P5-05 acceptance tests for strategies/basis.hpp.
//
// Test 2 is the card: the basis is arithmetically symmetric and the market is
// not. A scanner written from the formula reports both directions and looks
// twice as productive as it is, because the reverse trade needs a short cash
// position that a retail account cannot carry.
//
// Test 3 is the reality check CLAUDE.md asks Phase 5 to produce: a real
// NIFTY-scale cash-and-carry priced through P3-09, with the answer reported
// rather than hoped for.
//
// Test 6 is P7-03's remainder: a price is not a size, and an edge that does not
// clear measured impact and latency is not an edge. Both refuse rather than
// shrinking the trade or reporting a number that could not be reached.
//
// No check description here may contain the substring FAIL.

#include <strategies/basis.hpp>

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

double rupees(std::int64_t paise) { return static_cast<double>(paise) / 100.0; }

} // namespace

using namespace altair;

namespace {

constexpr std::int64_t kNs = 1'000'000'000LL;

/// The post-April-2026 schedule, which is the one that matters now.
/// Rates as published; the point of this test is what they add up to.
ChargeSchedule schedule()
{
    ChargeSchedule s{};
    s.valid_from = Timestamp{0};
    s.valid_to = Timestamp{std::numeric_limits<std::int64_t>::max()};
    s.verified = true;

    s.equity_delivery.present   = true;
    s.equity_delivery.stt       = rate_from(0.001L);       // 0.1% BOTH sides
    s.equity_delivery.stt_side  = ChargeSide::Both;
    s.equity_delivery.exch_txn_nse = rate_from(0.0000297L);
    s.equity_delivery.exch_txn_bse = rate_from(0.0000375L);
    s.equity_delivery.stamp     = rate_from(0.00015L);
    s.equity_delivery.stamp_side = ChargeSide::Buy;
    s.equity_delivery.ipft      = rate_from(0.000001L);
    s.equity_delivery.dp_per_scrip_sell = Notional{1534};

    s.equity_intraday.present   = true;
    s.equity_intraday.stt       = rate_from(0.00025L);     // 0.025% SELL only
    s.equity_intraday.stt_side  = ChargeSide::Sell;
    s.equity_intraday.exch_txn_nse = rate_from(0.0000297L);
    s.equity_intraday.exch_txn_bse = rate_from(0.0000375L);
    s.equity_intraday.stamp     = rate_from(0.00003L);
    s.equity_intraday.stamp_side = ChargeSide::Buy;
    s.equity_intraday.ipft      = rate_from(0.000001L);

    s.equity_futures.present    = true;
    s.equity_futures.stt        = rate_from(0.0005L);      // 0.05% post-April
    s.equity_futures.stt_side   = ChargeSide::Sell;
    s.equity_futures.exch_txn_nse = rate_from(0.0000173L);
    s.equity_futures.exch_txn_bse = rate_from(0.0000173L);
    s.equity_futures.stamp      = rate_from(0.00002L);
    s.equity_futures.stamp_side = ChargeSide::Buy;
    s.equity_futures.ipft       = rate_from(0.0000005L);
    return s;
}

BrokerageRule discount_broker()
{
    BrokerageRule b{};
    b.flat_per_order = Notional{2000};      // Rs 20
    b.pct = rate_from(0.0003L);             // or 0.03%
    b.take_lower = true;
    return b;
}

// ── 1 ────────────────────────────────────────────────────────────────────
void the_basis_is_measured_in_rate_terms_not_points()
{
    std::printf("\n1 the_basis_is_measured_in_rate_terms_not_points\n");
    // RELIANCE at 2,950.00, a 30-day future at 2,962.00. 12-rupee basis.
    BasisQuote q{};
    q.spot = Price{295'000};
    q.future = Price{296'200};
    q.spot_ts = Timestamp{100 * kNs};
    q.future_ts = Timestamp{100 * kNs};
    q.t = Years{30.0 / 365.0};
    q.rate = 0.065;
    q.dividend_yield = 0.008;

    const auto b = measure_basis(q, Duration{kNs});
    check(b.has_value(), "a same-instant pair measures");
    if (!b) { return; }
    std::printf("    basis %.2f  fair %.2f  mispricing %+.2f  implied repo"
                " %.3f%%\n",
                rupees(b->basis), b->fair_basis / 100.0,
                b->mispricing / 100.0, b->implied_repo * 100.0);
    check(b->basis == 1200, "the raw basis is Rs 12.00");
    check(near(b->fair_basis, 1385.0, 5.0),
          "carry alone accounts for Rs 13.85 of it at 6.5% financing against a"
          " 0.8% yield");
    check(b->mispricing < 0.0,
          "so a Rs 12.00 premium is a DISCOUNT: the future looks rich on a"
          " screen and is priced below carry. The sign of the tradable number"
          " is the opposite of the sign of the one on the screen, which is why"
          " nothing downstream is ever handed the raw basis");
    std::printf("    -> Rs 12.00 over spot LOOKS like a rich future. Against"
                " Rs 13.85 of carry it is\n       Rs 1.85 cheap. A scanner"
                " ranking on raw basis has the sign backwards.\n");

    // The same 12-rupee basis on a 90-day future is a completely different
    // rate. This is why implied repo is the comparable number.
    BasisQuote longer = q;
    longer.t = Years{90.0 / 365.0};
    const auto lb = measure_basis(longer, Duration{kNs});
    check(lb.has_value(), "the longer-dated pair measures too");
    if (lb) {
        std::printf("    the SAME Rs 12.00 basis at 90 days -> implied repo"
                    " %.3f%% (was %.3f%%)\n",
                    lb->implied_repo * 100.0, b->implied_repo * 100.0);
        check(lb->basis == b->basis, "the basis in points is identical");
        check(lb->implied_repo < b->implied_repo / 2.0,
              "but the rate it implies is less than half -- which is why the"
              " scanner compares repo rates and not point differences, and why"
              " a screen showing basis in points ranks tenors wrongly");
    }
}

// ── 2 ────────────────────────────────────────────────────────────────────
// THE card.
void half_the_directions_cannot_be_traded()
{
    std::printf("\n2 half_the_directions_cannot_be_traded\n");
    const ChargeSchedule sch = schedule();
    const BrokerageRule br = discount_broker();
    const Qty qty{500};                  // one RELIANCE lot
    const Duration skew{100 * 1'000'000LL};   // 100 ms

    // Retail: no borrow, intraday short allowed.
    ShortCashCapability retail{};
    retail.intraday_short = true;
    retail.borrow_available = false;

    BasisQuote rich{};
    rich.spot = Price{295'000};
    rich.future = Price{298'000};        // future RICH
    rich.t = Years{30.0 / 365.0};
    rich.rate = 0.065;
    rich.dividend_yield = 0.008;

    BasisQuote cheap = rich;
    cheap.future = Price{292'000};       // future CHEAP -- mirror image

    const auto a = scan_cash_futures(rich, qty, Exchange::NSE, skew, retail,
                                     sch, br);
    const auto b = scan_cash_futures(cheap, qty, Exchange::NSE, skew, retail,
                                     sch, br);
    check(a.has_value() && b.has_value(), "both directions scan");
    if (!a || !b) { return; }

    std::printf("    future RICH  by %+.2f -> %s, mispricing value Rs %.2f\n",
                a->basis.mispricing / 100.0, "cash-and-carry",
                rupees(a->mispricing_value.raw()));
    std::printf("    future CHEAP by %+.2f -> %s, mispricing value Rs %.2f\n",
                b->basis.mispricing / 100.0, "reverse (short cash)",
                rupees(b->mispricing_value.raw()));

    check(a->direction == BasisDirection::CashAndCarry
          && b->direction == BasisDirection::ReverseCashAndCarry,
          "the two mispricings point in opposite directions");
    check(a->executability == Executability::Executable,
          "the cash-and-carry is executable: buy cash, sell the future");
    check(b->executability == Executability::ShortCashUnavailable,
          "the REVERSE is not -- carrying it to expiry means holding a short"
          " delivery position, which is stock we do not have");

    const auto ea = a->net_edge();
    const auto eb = b->net_edge();
    check(ea.has_value(), "so the carry reports an edge");
    check(!eb && eb.error() == Executability::ShortCashUnavailable,
          "and the reverse reports WHY it has none, rather than reporting a"
          " number -- an unexecutable mispricing is arithmetic, not edge, and"
          " a scanner returning it would claim profit on a trade that cannot"
          " be placed");
    check(!b->actionable(),
          "the reverse is never actionable however large the mispricing gets");

    // With borrow, it becomes reachable. The arithmetic never changed.
    ShortCashCapability institutional = retail;
    institutional.borrow_available = true;
    const auto c = scan_cash_futures(cheap, qty, Exchange::NSE, skew,
                                     institutional, sch, br);
    check(c.has_value() && c->executability == Executability::Executable,
          "an account WITH stock borrow can reach the same mispricing -- so"
          " executability is a fact about the account, passed in, and not"
          " something the formula could have known");
    check(c->mispricing_value.raw() == b->mispricing_value.raw(),
          "and the mispricing itself is byte-identical between the two: only"
          " the reachability changed");

    // The phantom default. A zeroed result must not read as tradable.
    BasisOpportunity zeroed{};
    check(zeroed.executability == Executability::Unknown
          && zeroed.direction == BasisDirection::Unknown,
          "a zeroed opportunity is Unknown in both fields, not Executable and"
          " not a direction");
    check(!zeroed.actionable() && !zeroed.net_edge().has_value(),
          "so it is not actionable and yields no edge -- ordinal 0 is the"
          " unset state precisely so an uninitialised struct cannot pass for a"
          " signal");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void a_real_carry_priced_through_the_cost_calculator()
{
    std::printf("\n3 a_real_carry_priced_through_the_cost_calculator\n");
    const ChargeSchedule sch = schedule();
    const BrokerageRule br = discount_broker();
    const Qty qty{500};
    ShortCashCapability retail{};
    retail.intraday_short = true;

    // A generous basis: 40 rupees on a 2,950 spot over 30 days. That is an
    // implied repo well above any real financing rate -- the kind of number a
    // screen flags as an obvious opportunity.
    BasisQuote q{};
    q.spot = Price{295'000};
    q.future = Price{299'000};
    q.t = Years{30.0 / 365.0};
    q.rate = 0.065;
    q.dividend_yield = 0.008;

    const auto o = scan_cash_futures(q, qty, Exchange::NSE,
                                     Duration{100 * 1'000'000LL}, retail, sch,
                                     br);
    check(o.has_value(), "the carry scans");
    if (!o) { return; }

    const double turn = static_cast<double>(o->cash_buy.turnover.raw());
    std::printf("    RELIANCE 2950.00 cash / 2990.00 future, 500 shares,"
                " 30 days\n"
                "      implied repo        %.2f%%  (financing is %.2f%%)\n"
                "      mispricing value    Rs %10.2f\n"
                "      ---- cost of the four legs ----\n"
                "      cash buy  (deliv)   Rs %10.2f   of which STT Rs %.2f\n"
                "      cash sell (deliv)   Rs %10.2f   of which STT Rs %.2f\n"
                "      future sell         Rs %10.2f   of which STT Rs %.2f\n"
                "      future buy          Rs %10.2f   of which STT Rs %.2f\n"
                "      TOTAL COST          Rs %10.2f  (%.3f%% of turnover)\n"
                "      NET EDGE            Rs %10.2f\n",
                o->basis.implied_repo * 100.0, q.rate * 100.0,
                rupees(o->mispricing_value.raw()),
                rupees(o->cash_buy.total.raw()), rupees(o->cash_buy.stt.raw()),
                rupees(o->cash_sell.total.raw()), rupees(o->cash_sell.stt.raw()),
                rupees(o->fut_sell.total.raw()), rupees(o->fut_sell.stt.raw()),
                rupees(o->fut_buy.total.raw()), rupees(o->fut_buy.stt.raw()),
                rupees(o->cost.raw()),
                100.0 * static_cast<double>(o->cost.raw()) / turn,
                rupees(o->net.raw()));

    check(o->cash_buy.stt.raw() > 0 && o->cash_sell.stt.raw() > 0,
          "delivery STT is charged on BOTH sides of the cash leg -- 0.1% each"
          " way, which is the single largest line in the bill");
    check(o->fut_buy.stt.raw() == 0 && o->fut_sell.stt.raw() > 0,
          "while futures STT is SELL side only, so the futures leg pays it"
          " once");
    check(o->cash_buy.stt.raw() + o->cash_sell.stt.raw()
          > 3 * o->fut_sell.stt.raw(),
          "the cash leg's STT alone exceeds three times the futures leg's --"
          " 0.2% round trip against 0.05% one-way");
    check(o->cost.raw() == o->cash_buy.total.raw() + o->cash_sell.total.raw()
                         + o->fut_sell.total.raw() + o->fut_buy.total.raw(),
          "and the total is the sum of the four itemised legs, exactly, in"
          " paise");
    check(o->net.raw() == o->mispricing_value.raw() - o->cost.raw(),
          "net is mispricing minus cost, with no rounding slack");
    check(o->cash_buy.schedule_verified && o->fut_sell.schedule_verified,
          "the schedule this was priced against is marked verified, and that"
          " marking travels with the answer");

    // How big does the basis have to be?
    const double breakeven_per_share =
        static_cast<double>(o->cost.raw()) / static_cast<double>(qty.raw());
    const double fair = o->basis.fair_basis;
    std::printf("    -> BREAKEVEN: the future must be rich by Rs %.2f per"
                " share ABOVE fair\n       (fair basis here is Rs %.2f), i.e."
                " a total basis of Rs %.2f on a\n       Rs 2950 spot -- %.2f%%"
                " over %d days, an implied repo of about %.1f%%.\n",
                breakeven_per_share / 100.0, fair / 100.0,
                (fair + breakeven_per_share) / 100.0,
                100.0 * (fair + breakeven_per_share) / 295'000.0, 30,
                100.0 * ((fair + breakeven_per_share) / 295'000.0)
                    * (365.0 / 30.0));
    check(breakeven_per_share > 0.0, "the breakeven is a positive number");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void a_stale_leg_is_refused_not_measured()
{
    std::printf("\n4 a_stale_leg_is_refused_not_measured\n");
    BasisQuote q{};
    q.spot = Price{295'000};
    q.future = Price{296'200};
    q.spot_ts = Timestamp{100 * kNs};
    q.future_ts = Timestamp{109 * kNs};       // NINE SECONDS later
    q.t = Years{30.0 / 365.0};
    q.rate = 0.065;

    const Duration tight{100 * 1'000'000LL};   // 100 ms
    const auto r = measure_basis(q, tight);
    check(!r && r.error() == BasisError::StaleLeg,
          "a nine-second gap between the two legs is refused at a 100 ms"
          " tolerance");
    std::printf("    -> a basis from a fresh future and a nine-second-old cash"
                " print measures how\n       far the market moved in nine"
                " seconds. Its largest readings appear exactly\n       when"
                " the market is moving fastest and the fill is least likely.\n");

    const auto loose = measure_basis(q, Duration{30 * kNs});
    check(loose.has_value() && loose->skew.raw() == 9 * kNs,
          "at a looser tolerance it measures, and reports the skew it accepted"
          " -- so a downstream reader can see what it was given");

    const auto none = measure_basis(q, Duration{0});
    check(!none && none.error() == BasisError::NoSkewLimit,
          "and a caller who did not say how stale is too stale is refused"
          " rather than given a default: the tolerance is a decision about"
          " the market, not about the arithmetic");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void cross_venue_is_touch_to_touch_and_symmetric()
{
    std::printf("\n5 cross_venue_is_touch_to_touch_and_symmetric\n");
    const ChargeSchedule sch = schedule();
    const BrokerageRule br = discount_broker();
    const Qty qty{500};
    ShortCashCapability retail{};
    retail.intraday_short = true;           // permitted, same-session
    CrossVenueExecutionConstraints eligible{};
    eligible.sell_inventory = Qty{500};
    eligible.settlement = CrossVenueSettlement::Eligible;

    // NSE ask 2950.00, BSE bid 2950.60. A 60-paise cross.
    CrossVenueQuote q{};
    q.buy_venue = Exchange::NSE;
    q.buy_ask = Price{295'000};
    q.buy_ask_qty = Qty{500};          // the touch holds the scanned size
    q.sell_venue = Exchange::BSE;
    q.sell_bid = Price{295'060};
    q.sell_bid_qty = Qty{500};

    // A MEASURED allowance: 5 bps of impact and 3 bps of latency. It is passed
    // in, not invented here -- risk/slippage.hpp is where the bps is fitted.
    ExecutionAllowance allow{};
    allow.impact_bps = 5.0;
    allow.latency_bps = 3.0;

    const auto o = scan_cross_venue(q, qty, Duration{50 * 1'000'000LL}, retail,
                                    allow, eligible, sch, br);
    check(o.has_value(), "the cross-venue pair scans");
    if (!o) { return; }
    std::printf("    NSE ask 2950.00 / BSE bid 2950.60 x 500\n"
                "      gross Rs %.2f   cost Rs %.2f   allowance Rs %.2f"
                "   NET Rs %.2f\n",
                rupees(o->gross_value.raw()), rupees(o->cost.raw()),
                rupees(o->allowance.raw()), rupees(o->net.raw()));
    check(o->cross_spread == 60, "the cross is 60 paise, touch to touch");
    check(o->executable_size.raw() == 500 && o->size_short.raw() == 0,
          "the touch held the scanned size, so nothing is short");
    check(o->allowance.raw() > 0
          && o->net.raw() == o->gross_value.raw() - o->cost.raw()
                                - o->allowance.raw(),
          "the allowance is charged on top of the schedule and shows in the"
          " net");
    check(o->cost.raw() == o->buy_leg.total.raw() + o->sell_leg.total.raw(),
          "and it does not contaminate the charge total, which stays a charge"
          " total");
    check(o->executability == Executability::Executable,
          "and BOTH directions are reachable here, because an intraday short"
          " in the cash segment is permitted -- unlike the carry, this one is"
          " symmetric");
    check(o->sell_leg.stt.raw() > 0 && o->buy_leg.stt.raw() == 0,
          "intraday STT is sell-side only, so only the selling venue's leg"
          " pays it");
    check(o->net.raw() < o->gross_value.raw(),
          "and the net is below the gross, as it must be");
    check(!o->actionable(),
          "but 8 bps of measured allowance makes a 1-bps cross"
          " non-actionable -- the edge was in the price, not in the trade");

    // Turn off intraday short and it becomes unreachable in this direction.
    ShortCashCapability no_short{};
    CrossVenueExecutionConstraints requires_short = eligible;
    requires_short.sell_inventory = Qty{0};
    requires_short.permitted_short = Qty{500};
    const auto blocked = scan_cross_venue(q, qty, Duration{50 * 1'000'000LL},
                                          no_short, allow, requires_short, sch, br);
    check(blocked.has_value()
          && blocked->executability == Executability::ShortCashUnavailable,
          "an account without intraday short cannot reach it at all");
    check(!blocked->net_edge().has_value(),
          "and gets no edge number back, only the reason");
}

// ── 6 ────────────────────────────────────────────────────────────────────
// P7-03. The two things a touch-to-touch comparison still leaves out: the
// size actually resting, and the cost of crossing to it and waiting for the
// second leg. Both change whether an opportunity is real.
void size_and_allowance_decide_the_answer()
{
    std::printf("\n6 size_and_allowance_decide_the_answer\n");
    const ChargeSchedule sch = schedule();
    const BrokerageRule br = discount_broker();
    ShortCashCapability retail{};
    retail.intraday_short = true;
    const Duration tol{50 * 1'000'000LL};
    const ExecutionAllowance none{};
    CrossVenueExecutionConstraints eligible{};
    eligible.sell_inventory = Qty{500};
    eligible.settlement = CrossVenueSettlement::Eligible;

    CrossVenueQuote q{};
    q.buy_venue = Exchange::NSE;
    q.buy_ask = Price{295'000};
    q.sell_venue = Exchange::BSE;
    q.sell_bid = Price{295'060};

    // (a) The buy venue holds less than the scan asks for. A two-leg trade
    //     cannot take 200 of one leg and 500 of the other, so this refuses.
    q.buy_ask_qty = Qty{200};
    q.sell_bid_qty = Qty{500};
    const auto short_size =
        scan_cross_venue(q, Qty{500}, tol, retail, none, sch, br);
    check(short_size.has_value(), "an undersized touch still scans");
    if (short_size) {
        check(short_size->executability == Executability::InsufficientSize,
              "but it is refused for size, not reported as a smaller trade");
        check(short_size->executable_size.raw() == 200,
              "and the size that WAS there is reported back");
        check(short_size->size_short.raw() == 300,
              "with the shortfall, so the caller can size to it");
        check(!short_size->actionable()
              && !short_size->net_edge().has_value(),
              "a size refusal is never actionable and yields no edge number");
    }

    // (b) A size the caller never supplied cannot be checked.
    CrossVenueQuote unsized{};
    unsized.buy_venue = Exchange::NSE;
    unsized.buy_ask = Price{295'000};
    unsized.sell_venue = Exchange::BSE;
    unsized.sell_bid = Price{295'060};
    const auto no_size =
        scan_cross_venue(unsized, Qty{500}, tol, retail, none, sch, br);
    check(no_size.has_value()
          && no_size->executability == Executability::InsufficientSize
          && no_size->executable_size.raw() == 0,
          "an unreported size refuses rather than assuming depth that was"
          " never quoted");

    // (c) Asking for exactly the smaller side is fine.
    q.buy_ask_qty = Qty{500};
    q.sell_bid_qty = Qty{200};
    const auto exact =
        scan_cross_venue(q, Qty{200}, tol, retail, none, sch, br);
    check(exact.has_value()
          && exact->executability == Executability::Executable
          && exact->size_short.raw() == 0 && exact->executable_size.raw() == 200,
          "asking for exactly the smaller side is executable");

    // (d) The allowance is real money, charged on turnover, and it is the
    //     difference between an actionable observation and a dead one. The
    //     cross must first survive its own bill, or the allowance would just
    //     be flattering a trade that was never on: at a 60-paise cross the
    //     charges are Rs 584 against Rs 300 gross, so the edge is gone before
    //     any allowance is applied. Use a wider cross.
    CrossVenueQuote wide{};
    wide.buy_venue = Exchange::NSE;
    wide.buy_ask = Price{295'000};
    wide.buy_ask_qty = Qty{500};
    wide.sell_venue = Exchange::BSE;
    wide.sell_bid = Price{295'200};        // a 200-paise cross, not 60
    wide.sell_bid_qty = Qty{500};

    const auto free_edge =
        scan_cross_venue(wide, Qty{500}, tol, retail, none, sch, br);
    ExecutionAllowance measured{};
    measured.impact_bps = 5.0;
    measured.latency_bps = 3.0;
    const auto charged =
        scan_cross_venue(wide, Qty{500}, tol, retail, measured, sch, br);
    check(free_edge.has_value() && charged.has_value(),
          "the scan runs with and without an allowance");
    if (free_edge && charged) {
        // turnover = (295000 + 295200) paise x 500 = 295,100,000 paise.
        // 8 bps of it = 236,080 paise.
        check(charged->allowance.raw() == 236'080,
              "8 bps on 295,100,000 paise of turnover is 236,080 paise");
        check(free_edge->allowance.raw() == 0,
              "and a zero allowance charges nothing");
        check(charged->cost.raw() == free_edge->cost.raw(),
              "the allowance does NOT touch the charge schedule's total");
        check(charged->net.raw() == charged->gross_value.raw()
                                    - charged->cost.raw()
                                    - charged->allowance.raw(),
              "the net is gross minus charges minus allowance, exactly");
        check(free_edge->actionable() && !charged->actionable(),
              "and it flips actionability -- without it the cross clears its"
              " bill and is actionable; with it, it is not");
        std::printf("    a 200-paise cross: gross Rs %.2f, charges Rs %.2f"
                    " -> NET Rs %.2f\n"
                    "      the same cross with 8 bps of measured allowance"
                    " -> NET Rs %.2f\n",
                    rupees(free_edge->gross_value.raw()),
                    rupees(free_edge->cost.raw()),
                    rupees(free_edge->net.raw()),
                    rupees(charged->net.raw()));
    }

    // (e) and (f) use a quote whose touch holds the scanned size.
    q.buy_ask_qty = Qty{500};
    q.sell_bid_qty = Qty{500};

    // (e) A negative allowance is a caller error, not a zero.
    ExecutionAllowance bad{};
    bad.impact_bps = -1.0;
    const auto neg =
        scan_cross_venue(q, Qty{500}, tol, retail, bad, sch, br);
    check(!neg && neg.error() == BasisError::BadAllowance,
          "a negative allowance is refused, not silently treated as free");

    // (f) A non-positive size is a caller error, distinct from a refusal.
    const auto zero_qty =
        scan_cross_venue(q, Qty{0}, tol, retail, none, eligible, sch, br);
    check(!zero_qty && zero_qty.error() == BasisError::BadQuantity,
          "a zero scanned quantity is a caller error, not a size refusal");
}

void inventory_and_settlement_constraints_refuse_conservatively()
{
    std::printf("\n7 inventory_and_settlement_constraints_refuse_conservatively\n");
    const ChargeSchedule sch = schedule();
    const BrokerageRule br = discount_broker();
    const Duration tol{50 * 1'000'000LL};
    const ExecutionAllowance none{};
    ShortCashCapability cap{};
    cap.intraday_short = true;
    CrossVenueQuote q{};
    q.buy_ask = Price{295'000}; q.buy_ask_qty = Qty{500};
    q.sell_bid = Price{295'200}; q.sell_bid_qty = Qty{500};

    CrossVenueExecutionConstraints unknown{};
    unknown.sell_inventory = Qty{500};
    const auto no_settlement =
        scan_cross_venue(q, Qty{500}, tol, cap, none, unknown, sch, br);
    check(no_settlement && no_settlement->executability
              == Executability::SettlementUnavailable,
          "unknown settlement eligibility refuses instead of assuming safe delivery");

    CrossVenueExecutionConstraints ineligible = unknown;
    ineligible.settlement = CrossVenueSettlement::Ineligible;
    const auto bad_settlement =
        scan_cross_venue(q, Qty{500}, tol, cap, none, ineligible, sch, br);
    check(bad_settlement && bad_settlement->executability
              == Executability::SettlementUnavailable,
          "explicitly ineligible settlement refuses the pair");

    CrossVenueExecutionConstraints no_inventory{};
    no_inventory.settlement = CrossVenueSettlement::Eligible;
    const auto empty_inventory =
        scan_cross_venue(q, Qty{500}, tol, cap, none, no_inventory, sch, br);
    check(empty_inventory && empty_inventory->executability
              == Executability::InventoryUnavailable,
          "zero inventory and zero short capacity refuse the sell leg");

    CrossVenueExecutionConstraints partial = no_inventory;
    partial.sell_inventory = Qty{200};
    partial.permitted_short = Qty{299};
    const auto partial_capacity =
        scan_cross_venue(q, Qty{500}, tol, cap, none, partial, sch, br);
    check(partial_capacity && partial_capacity->executability
              == Executability::InventoryUnavailable,
          "partial inventory and insufficient short capacity refuse without shrinking");

    CrossVenueExecutionConstraints exact_short = no_inventory;
    exact_short.sell_inventory = Qty{200};
    exact_short.permitted_short = Qty{300};
    const auto covered =
        scan_cross_venue(q, Qty{500}, tol, cap, none, exact_short, sch, br);
    check(covered && covered->executability == Executability::Executable,
          "inventory plus exactly permitted intraday short capacity is executable");

    cap.intraday_short = false;
    const auto unavailable_short =
        scan_cross_venue(q, Qty{500}, tol, cap, none, exact_short, sch, br);
    check(unavailable_short && unavailable_short->executability
              == Executability::ShortCashUnavailable,
          "permitted short capacity still requires intraday-short account capability");
}

void executable_cash_futures_uses_directional_touches_and_borrow()
{
    std::printf("\n8 executable_cash_futures_uses_directional_touches_and_borrow\n");
    const ChargeSchedule sch = schedule();
    const BrokerageRule br = discount_broker();
    ShortCashCapability cap{};
    cap.borrow_available = true;
    CashFuturesExecutionQuote q{};
    q.cash_bid = Price{100'00}; q.cash_ask = Price{101'00};
    q.future_bid = Price{105'00}; q.future_ask = Price{106'00};
    q.cash_ts = Timestamp{1'000'000'000};
    q.future_ts = Timestamp{1'100'000'000};
    q.t = Years{30.0 / 365.0}; q.rate = 0.06; q.dividend_yield = 0.0;
    q.borrow_cost_per_unit = Notional{2};
    const auto rich = scan_cash_futures_executable(
        q, Qty{100}, Exchange::NSE, Duration{1'000'000'000}, cap, sch, br);
    check(rich && rich->classification == PremiumDiscount::Premium,
          "future above fair carry is classified as premium");
    check(rich && rich->direction == BasisDirection::CashAndCarry,
          "premium direction buys cash at ask and sells future at bid");
    check(rich && rich->entry_edge.raw() == 40'000,
          "cash-and-carry entry edge uses future bid minus cash ask");

    q.future_bid = Price{95'00}; q.future_ask = Price{96'00};
    const auto cheap = scan_cash_futures_executable(
        q, Qty{100}, Exchange::NSE, Duration{1'000'000'000}, cap, sch, br);
    check(cheap && cheap->classification == PremiumDiscount::Discount,
          "future below fair carry is classified as discount");
    check(cheap && cheap->direction == BasisDirection::ReverseCashAndCarry,
          "discount direction buys future at ask and sells cash at bid");
    check(cheap && cheap->entry_edge.raw() == 40'000,
          "reverse-carry entry edge uses cash bid minus future ask");
    check(cheap && cheap->borrow_cost.raw() == 200,
          "reverse carry includes explicit borrow cost");

    cap.borrow_available = false;
    const auto refused = scan_cash_futures_executable(
        q, Qty{100}, Exchange::NSE, Duration{1'000'000'000}, cap, sch, br);
    check(refused && refused->executability == Executability::ShortCashUnavailable,
          "reverse carry refuses without borrow capability");

    q.future_bid = Price{101'00}; q.future_ask = Price{101'00};
    cap.borrow_available = true;
    const auto fair = scan_cash_futures_executable(
        q, Qty{100}, Exchange::NSE, Duration{1'000'000'000}, cap, sch, br);
    check(fair && fair->classification == PremiumDiscount::Fair,
          "near-carry quote is explicitly classified fair");
}

} // namespace

int main()
{
    std::printf("altair cash-futures basis and cross-venue scanner tests\n");
    the_basis_is_measured_in_rate_terms_not_points();
    half_the_directions_cannot_be_traded();
    a_real_carry_priced_through_the_cost_calculator();
    a_stale_leg_is_refused_not_measured();
    cross_venue_is_touch_to_touch_and_symmetric();
    size_and_allowance_decide_the_answer();
    inventory_and_settlement_constraints_refuse_conservatively();
    executable_cash_futures_uses_directional_touches_and_borrow();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
