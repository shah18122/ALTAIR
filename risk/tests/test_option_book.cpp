// Tests for risk/option_book.hpp -- the engine behind the GETS Greek screens.

#include <risk/option_book.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using namespace altair;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

constexpr std::int64_t kDayNs = 86'400LL * 1'000'000'000LL;
const Timestamp kNow{1'790'000'000LL * 1'000'000'000LL};
const Timestamp kExpiry30{kNow.ns_since_epoch() + 30 * kDayNs};
const Timestamp kExpiry60{kNow.ns_since_epoch() + 60 * kDayNs};
const Price kSpot{2'500'000};   // 25,000.00

BookParams params() {
    BookParams p{};
    p.now = kNow;
    p.rate = 0.065;
    return p;
}

/// The BS price at 15% vol, rounded to the paisa: a mark the solver must invert.
Price mark_at(OptionRight right, Price strike, Timestamp expiry, double vol) {
    const double years = static_cast<double>(expiry.ns_since_epoch() - kNow.ns_since_epoch())
                       / (365.0 * 86'400.0 * 1e9);
    const auto g = black_scholes(right, kSpot, strike, Years{years}, Vol{vol}, 0.065, 0.0);
    return Price{static_cast<std::int64_t>(g->price + 0.5)};
}

BookLeg option_leg(std::uint32_t id, OptionRight right, Price strike, Timestamp expiry,
                   std::int64_t units, Price average) {
    BookLeg leg{};
    leg.id = id;
    leg.underlying = 1;
    leg.kind = BookLegKind::Option;
    leg.right = right;
    leg.strike = strike;
    leg.expiry = expiry;
    leg.expiry_known = true;
    leg.units = Qty{units};
    leg.average = average;
    leg.mark = mark_at(right, strike, expiry, 0.15);
    leg.lot = LotSize{75};
    return leg;
}

void test_leg_iv_round_trip() {
    const auto leg = option_leg(1, OptionRight::Call, Price{2'500'000}, kExpiry30, 75, Price{30'000});
    const auto v = value_book_leg(leg, kSpot, params());
    check(v.issue == BookLegIssue::None && v.greeks_ok, "an ATM call with a mark is valued");
    check(v.implied && near(v.implied->raw(), 0.15, 2e-4), "IV solved from the mark is ~15%");
    check(near(v.days, 30.0, 1e-9), "30 calendar days to expiry");
    const auto mtm = (leg.mark->raw() - 30'000) * 75;
    check(v.mtm && v.mtm->raw() == mtm, "MTM is exact (mark - average) x units");
    check(v.unit && near(v.delta, v.unit->delta * 75.0, 1e-9), "position delta = unit delta x units");
    check(v.delta > 0.0 && v.gamma > 0.0 && v.vega > 0.0 && v.theta < 0.0,
          "long call: +delta, +gamma, +vega, -theta");
    check(v.time_value && v.intrinsic && v.intrinsic->raw() == 0
              && v.time_value->raw() == leg.mark->raw(),
          "ATM: intrinsic zero, time value is the whole premium");
}

void test_user_iv_override() {
    auto leg = option_leg(2, OptionRight::Put, Price{2'450'000}, kExpiry30, -150, Price{20'000});
    leg.user_vol = Vol{0.20};
    const auto v = value_book_leg(leg, kSpot, params());
    check(v.used && near(v.used->raw(), 0.20, 1e-12), "user IV drives the Greeks");
    check(v.implied && near(v.implied->raw(), 0.15, 2e-4), "solved IV is still reported");
    check(v.delta > 0.0, "short put has positive delta");
}

void test_refusals() {
    auto leg = option_leg(3, OptionRight::Call, Price{2'500'000}, kExpiry30, 75, Price{30'000});
    leg.mark.reset();
    auto v = value_book_leg(leg, kSpot, params());
    check(v.issue == BookLegIssue::NoMark && !v.mtm && !v.greeks_ok, "no mark: no MTM, no Greeks");
    leg = option_leg(4, OptionRight::Call, Price{2'500'000}, kExpiry30, 75, Price{30'000});
    leg.expiry_known = false;
    v = value_book_leg(leg, kSpot, params());
    check(v.issue == BookLegIssue::NoExpiry && !v.greeks_ok, "unknown expiry refuses Greeks");
    check(v.mtm.has_value(), "unknown expiry still has an MTM");
    leg = option_leg(5, OptionRight::Call, Price{2'500'000}, kExpiry30, 75, Price{30'000});
    v = value_book_leg(leg, std::nullopt, params());
    check(v.issue == BookLegIssue::NoSpot && !v.greeks_ok, "no spot refuses Greeks");
    leg.expiry = Timestamp{kNow.ns_since_epoch() - 1};
    v = value_book_leg(leg, kSpot, params());
    check(v.issue == BookLegIssue::Expired, "past expiry is Expired");
    leg = option_leg(6, OptionRight::Call, Price{2'400'000}, kExpiry30, 75, Price{30'000});
    leg.mark = Price{50'000};   // Rs 500 for a Rs 1,000 in-the-money call
    v = value_book_leg(leg, kSpot, params());
    check(v.issue == BookLegIssue::IvUnsolved && !v.greeks_ok,
          "a mark below intrinsic has no IV and is not clamped");
}

void test_futures_and_parity() {
    BookLeg fut{};
    fut.id = 7;
    fut.underlying = 1;
    fut.kind = BookLegKind::Future;
    fut.units = Qty{-75};
    fut.average = Price{2'510'000};
    fut.mark = Price{2'512'000};
    fut.lot = LotSize{75};
    const auto v = value_book_leg(fut, kSpot, params());
    check(v.greeks_ok && v.delta == -75.0 && v.mtm && v.mtm->raw() == -75 * 2'000,
          "a short future: delta -units, exact MTM");

    const auto call = option_leg(8, OptionRight::Call, Price{2'500'000}, kExpiry30, 75, Price{0});
    const auto put = option_leg(9, OptionRight::Put, Price{2'500'000}, kExpiry30, -75, Price{0});
    const auto vc = value_book_leg(call, kSpot, params());
    const auto vp = value_book_leg(put, kSpot, params());
    check(near(vc.delta + vp.delta, 75.0, 0.05), "long call + short put is a synthetic long (delta ~ units)");
    check(near(vc.gamma + vp.gamma, 0.0, 1e-3), "and has ~no gamma");
}

void test_summary() {
    std::vector<BookLeg> legs{
        option_leg(10, OptionRight::Call, Price{2'500'000}, kExpiry30, 75, Price{30'000}),
        option_leg(11, OptionRight::Put, Price{2'450'000}, kExpiry30, -75, Price{20'000}),
        option_leg(12, OptionRight::Call, Price{2'600'000}, kExpiry60, -150, Price{15'000}),
    };
    BookLeg cash{};
    cash.id = 13;
    cash.underlying = 1;
    cash.kind = BookLegKind::Cash;
    cash.units = Qty{10};
    cash.average = Price{2'490'000};
    cash.mark = Price{2'500'000};
    legs.push_back(cash);
    BookLeg other = legs[0];
    other.id = 14;
    other.underlying = 2;
    legs.push_back(other);

    std::vector<BookLegView> views;
    for (const auto& l : legs) views.push_back(value_book_leg(l, kSpot, params()));
    const auto rows = summarise_book(legs, views, [](std::uint32_t) { return std::optional<Price>{kSpot}; });
    check(rows.size() == 6, "underlying 1: two expiries + cash + total; underlying 2: expiry + total");
    check(rows[0].expiry == kExpiry30 && rows[0].legs == 2 && !rows[0].total, "first group is the 30-day expiry");
    check(rows[2].legs == 1 && rows[2].cash_units == 10 && !rows[2].expiry_known, "cash is its own group");
    const auto& total = rows[3];
    check(total.total && total.legs == 4, "total row covers every leg of the underlying");
    const double d = views[0].delta + views[1].delta + views[2].delta + views[3].delta;
    check(near(total.delta, d, 1e-9), "total delta is the sum of legs");
    std::int64_t mtm = 0;
    for (std::size_t i = 0; i < 4; ++i) mtm += views[i].mtm->raw();
    check(total.mtm && total.mtm->raw() == mtm && total.complete, "total MTM is the exact sum");
    check(total.hedge_lots && near(*total.hedge_lots, -d / 75.0, 1e-9), "hedge lots = -delta / lot");
    check(total.call_iv && near(total.call_iv->raw(), 0.15, 3e-4), "call IV is the units-weighted mean");
    check(near(total.days, 30.0, 1e-9), "days left is the nearest expiry");

    legs[1].mark.reset();
    views[1] = value_book_leg(legs[1], kSpot, params());
    const auto partial = summarise_book(legs, views, [](std::uint32_t) { return std::optional<Price>{kSpot}; });
    check(!partial[0].mtm && !partial[0].complete && !partial[3].complete,
          "a leg without a mark makes its group and total incomplete, not smaller");
}

void test_simulation() {
    std::vector<BookLeg> legs{
        option_leg(20, OptionRight::Call, Price{2'500'000}, kExpiry30, 75, Price{30'000}),
        option_leg(21, OptionRight::Put, Price{2'500'000}, kExpiry30, 75, Price{25'000}),
    };
    std::vector<BookLegView> views;
    for (const auto& l : legs) views.push_back(value_book_leg(l, kSpot, params()));
    const std::vector<BookShift> shifts{{0.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {-2.0, 0.0, 0.0},
                                        {0.0, 0.0, 5.0}, {0.0, 2.0, 0.0}};
    const auto rows = simulate_book(legs, views, 1, kSpot, params(), shifts);
    check(rows.size() == 5 && rows[0].complete, "one row per shift");
    check(rows[0].pnl && std::llabs(rows[0].pnl->raw()) <= 150,
          "no move, no change: P&L is within the mark's rounding");
    check(rows[1].pnl && rows[2].pnl && rows[1].pnl->raw() > 0 && rows[2].pnl->raw() > 0,
          "a long straddle gains on a 2% move either way");
    check(rows[3].pnl && rows[3].pnl->raw() < 0, "five days of decay cost a long straddle");
    check(rows[4].pnl && rows[4].pnl->raw() > 0, "two vol points help a long straddle");
    const std::int64_t payoff_now =
        75 * (0 - legs[0].mark->raw()) + 75 * (0 - legs[1].mark->raw());
    check(rows[0].pnl_at_expiry && rows[0].pnl_at_expiry->raw() == payoff_now,
          "P&L at expiry with spot unchanged is minus both premiums (ATM)");
    check(rows[0].hedge_lots && near(*rows[0].hedge_lots, -rows[0].delta / 75.0, 1e-12),
          "scenario hedge lots use the lot size");
}

void test_expenses() {
    ChargeSchedule sch{};
    sch.valid_from = Timestamp{0};
    sch.valid_to = Timestamp{4'000'000'000LL * 1'000'000'000LL};
    sch.verified = true;
    sch.equity_options.present = true;
    sch.equity_options.basis = TurnoverBasis::Premium;
    sch.equity_options.stt = rate_from(0.001L);
    sch.equity_options.stt_side = ChargeSide::Sell;
    sch.equity_options.exch_txn_nse = rate_from(0.0003503L);
    sch.equity_options.stamp = rate_from(0.00003L);
    BrokerageRule br{};
    br.flat_per_order = Notional{2'000};
    br.take_lower = false;

    std::vector<BookFill> fills{
        {1, Side::Buy, Qty{75}, Notional{75 * 30'000}, Segment::Opt, Exchange::NSE, false, kNow},
        {1, Side::Buy, Qty{75}, Notional{75 * 31'000}, Segment::Opt, Exchange::NSE, false, kNow},
        {1, Side::Sell, Qty{150}, Notional{150 * 35'050}, Segment::Opt, Exchange::NSE, false, kNow},
        {2, Side::Sell, Qty{75}, Notional{75 * 12'345}, Segment::Opt, Exchange::NSE, false, kNow},
    };
    const auto rows = book_expenses(fills, [&](Timestamp) { return &sch; },
                                    [&](const BookFill&) { return br; });
    check(rows.size() == 2, "fills are summed per instrument");
    check(rows[0].buy_qty == 150 && rows[0].buy_value.raw() == 75 * 30'000 + 75 * 31'000
              && rows[0].buy_avg == Price{30'500},
          "buy quantity, value and average");
    check(rows[0].sell_qty == 150 && rows[0].sell_avg == Price{35'050} && rows[0].orders == 3,
          "sell side and order count");
    std::int64_t expected = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        Trade t{};
        t.segment = Segment::Opt;
        t.side = fills[i].side;
        t.qty = fills[i].qty;
        t.price = Price{fills[i].value.raw() / fills[i].qty.raw()};
        t.trade_ts = kNow;
        expected += compute_cost(t, sch, br)->total.raw();
    }
    check(rows[0].expense && rows[0].expense->raw() == expected,
          "expense is the sum of each order's itemised cost");
    check(rows[1].buy_qty == 0 && !rows[1].buy_avg && rows[1].sell_avg == Price{12'345},
          "a sell-only row has no buy average");

    const auto none = book_expenses(fills, [](Timestamp) { return static_cast<const ChargeSchedule*>(nullptr); },
                                    [&](const BookFill&) { return br; });
    check(!none[0].expense && none[0].expense_error == CostError::NoSchedule,
          "without a schedule the expense is absent and says why");
}

} // namespace

int main() {
    std::printf("Option book (GETS Greek screens)\n");
    test_leg_iv_round_trip();
    test_user_iv_override();
    test_refusals();
    test_futures_and_parity();
    test_summary();
    test_simulation();
    test_expenses();
    std::printf("Option book: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
