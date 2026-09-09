// P4-02 acceptance tests for risk/limits.hpp.
//
// Two demonstrations carry this file:
//
//   * Test 5 does the conservation check in DOUBLE alongside the integer one,
//     on a real F&O-scale breach, and watches the floating-point version
//     report zero. A conservation check in double is not a weaker version of
//     the integer one; it is a different check that answers yes when the
//     answer is no.
//
//   * Test 3 shows the kill switch refusing an order that passes every other
//     check, and continuing to refuse after the condition that tripped it has
//     gone away. A switch that re-arms itself is a warning light.
//
// No check description here may contain the substring FAIL.

#include <risk/limits.hpp>

#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cmath>

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

} // namespace

using namespace altair;

namespace {

constexpr std::int64_t kMs = 1'000'000;
const Timestamp kNow{1'000'000 * kMs};

// A clean NIFTY order: 2 lots of 75 at Rs 24,080, on a Rs 0.05 tick.
ProposedOrder good_order()
{
    ProposedOrder o{};
    o.qty = Qty{150};
    o.limit_price = Price{2'408'000};
    o.quote_ts = kNow - duration::millis(50);
    o.lot_size = LotSize{75};
    o.tick_size = Price{5};
    o.freeze_qty = Qty{1800};
    o.band_lower = Price{2'167'200};
    o.band_upper = Price{2'648'800};
    return o;
}

RiskLimits limits()
{
    RiskLimits l{};
    l.max_order_qty = Qty{750};
    l.max_position_qty = Qty{1500};
    l.max_position_notional = Notional{5'000'000'000};
    l.max_gross_notional = Notional{20'000'000'000};
    l.max_daily_loss = Notional{500'000'000};       // Rs 5 lakh
    l.max_open_orders = 20;
    l.max_quote_age = duration::millis(500);
    return l;
}

// ── 1 ────────────────────────────────────────────────────────────────────
void a_clean_order_passes_and_a_broken_one_reports_everything()
{
    std::printf("\n1 a_clean_order_passes_and_a_broken_one_reports_everything\n");
    KillSwitch k;
    AccountState a{};
    check(clean(check_order(good_order(), a, limits(), k, kNow)),
          "a well-formed order passes every check");

    // One order that is wrong four ways at once. A fail-fast checker would
    // report one of them and leave three to be discovered one build at a time.
    ProposedOrder bad = good_order();
    bad.qty = Qty{160};                       // not a whole 75-lot
    bad.limit_price = Price{2'408'003};       // not on a 5-paise tick
    bad.quote_ts = kNow - duration::seconds(3);   // stale
    bad.band_upper = Price{2'400'000};        // above the band
    const Violation v = check_order(bad, a, limits(), k, kNow);
    std::printf("    a four-way broken order reports %d violations (mask 0x%x)\n",
                violation_count(v), static_cast<unsigned>(v));
    check(has(v, Violation::NotWholeLots), "not a whole number of lots");
    check(has(v, Violation::NotOnTick), "not on a tick boundary");
    check(has(v, Violation::StaleQuote), "priced off a stale quote");
    check(has(v, Violation::OutsidePriceBand), "outside the price band");
    check(violation_count(v) == 4,
          "all four are reported together -- one violation is a sizing bug,"
          " four is a broken upstream, and only the full set says which");
    check(!clean(v), "and the order is refused");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// Rule 1 and rule 9: a missing spec BLOCKS. And two zeros that mean opposite
// things must not be conflated.
void a_missing_spec_blocks_but_an_absent_band_does_not()
{
    std::printf("\n2 a_missing_spec_blocks_but_an_absent_band_does_not\n");
    KillSwitch k;
    AccountState a{};

    ProposedOrder no_lot = good_order();
    no_lot.lot_size = LotSize{0};
    check(has(check_order(no_lot, a, limits(), k, kNow),
              Violation::SpecUnavailable),
          "a zero LOT SIZE blocks -- it means the spec store was read before"
          " it was loaded, not that any quantity is acceptable");

    ProposedOrder no_tick = good_order();
    no_tick.tick_size = Price{0};
    check(has(check_order(no_tick, a, limits(), k, kNow),
              Violation::SpecUnavailable),
          "and so does a zero TICK SIZE");

    // A zero band is the OPPOSITE: the exchange genuinely publishes none.
    ProposedOrder no_band = good_order();
    no_band.band_lower = Price{0};
    no_band.band_upper = Price{0};
    const Violation v = check_order(no_band, a, limits(), k, kNow);
    check(clean(v),
          "but a zero PRICE BAND means the exchange publishes none, and the"
          " order passes -- two zeros, two meanings, and conflating them is"
          " the phantom-default defect");
    check(!has(v, Violation::OutsidePriceBand),
          "specifically, no band violation is raised against an absent band");

    // A one-sided band is honoured on the side that exists.
    ProposedOrder upper_only = good_order();
    upper_only.band_lower = Price{0};
    upper_only.band_upper = Price{2'400'000};
    check(has(check_order(upper_only, a, limits(), k, kNow),
              Violation::OutsidePriceBand),
          "a band published on one side only is still enforced on that side");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void the_kill_switch_is_sticky_and_short_circuits()
{
    std::printf("\n3 the_kill_switch_is_sticky_and_short_circuits\n");
    KillSwitch k;
    AccountState a{};
    check(!k.tripped() && k.reason() == KillReason::NotTripped,
          "a fresh switch is not tripped");
    check(clean(check_order(good_order(), a, limits(), k, kNow)),
          "and a good order passes");

    k.trip(KillReason::ConservationBreach, kNow);
    check(k.tripped(), "it trips");
    const Violation v = check_order(good_order(), a, limits(), k, kNow);
    check(v == Violation::KillSwitchTripped,
          "and the SAME order that just passed is now refused");
    check(violation_count(v) == 1,
          "with exactly one bit set -- nothing else was evaluated, because"
          " there is no order it would approve");

    // The first reason is kept. Later trips are consequences.
    k.trip(KillReason::DailyLoss, kNow + duration::seconds(1));
    check(k.reason() == KillReason::ConservationBreach,
          "a second trip does not overwrite the reason -- the first cause is"
          " kept and the later one is its consequence");
    check(k.trips() == 2, "though both attempts are counted");

    // Stickiness: the condition going away changes nothing.
    check(check_order(good_order(), a, limits(), k,
                      kNow + duration::seconds(3600))
          == Violation::KillSwitchTripped,
          "an hour later, with nothing wrong, it still refuses");

    // Reset needs the acknowledgement. A wrong token is a no-op.
    k.reset(1, kNow);
    check(k.tripped(), "reset(1) does nothing");
    k.reset(0, kNow);
    check(k.tripped(), "and neither does reset(0)");
    check(k.resets() == 0, "neither counted as a reset");
    k.reset(KillSwitch::kResetAcknowledgement, kNow);
    check(!k.tripped(), "only the acknowledgement token clears it");
    check(k.resets() == 1, "and that one is counted");
    check(clean(check_order(good_order(), a, limits(), k, kNow)),
          "after which orders flow again");
}

// ── 4 ────────────────────────────────────────────────────────────────────
// A cap must be checked against the RESULTING position, not the current one.
void caps_are_checked_against_the_resulting_position()
{
    std::printf("\n4 caps_are_checked_against_the_resulting_position\n");
    KillSwitch k;
    RiskLimits l = limits();
    l.max_position_qty = Qty{300};

    AccountState a{};
    a.position = Qty{225};                    // 3 lots already on
    ProposedOrder o = good_order();
    o.qty = Qty{150};                         // 2 more -> 375, over the 300 cap

    const Violation v = check_order(o, a, l, k, kNow);
    std::printf("    holding 225, adding 150 -> 375 against a cap of 300\n");
    check(has(v, Violation::MaxPositionQty),
          "the order is refused on the RESULTING position, not the current"
          " one -- a cap checked against what you already hold approves the"
          " order that breaches it");

    o.qty = Qty{75};                          // 1 more -> 300, exactly at cap
    check(!has(check_order(o, a, l, k, kNow), Violation::MaxPositionQty),
          "landing exactly on the cap is allowed");

    // Selling DOWN from an over-cap position must be allowed, or a book that
    // somehow got too big could never be reduced.
    a.position = Qty{600};
    o.qty = Qty{-150};                        // reduce to 450
    const Violation red = check_order(o, a, l, k, kNow);
    std::printf("    holding 600 (over cap), selling 150 -> 450\n");
    check(has(red, Violation::MaxPositionQty),
          "reducing from 600 to 450 is still over the 300 cap and is flagged");
    o.qty = Qty{-450};                        // reduce to 150, under the cap
    check(!has(check_order(o, a, l, k, kNow), Violation::MaxPositionQty),
          "but a reduction that lands UNDER the cap passes, so an oversized"
          " book can always be brought back");

    // The short side is capped by magnitude, not by sign.
    a.position = Qty{0};
    o.qty = Qty{-375};
    check(has(check_order(o, a, l, k, kNow), Violation::MaxPositionQty),
          "a short of 375 breaches a 300 cap just as a long would");
}

// ── 5 ────────────────────────────────────────────────────────────────────
// THE demonstration. Integer paise versus double, on a real breach.
void conservation_is_exact_and_double_would_miss_it()
{
    std::printf("\n5 conservation_is_exact_and_double_would_miss_it\n");
    KillSwitch k;

    // A balanced tick: bought 150 units at 24,080 (cash out), paid 5.95 bps
    // of cost, and the ledger recorded the matching cash movement.
    const std::int64_t fills = -361'200'000;      // 150 * 2,408,000, cash OUT
    const std::int64_t costs = -214'914;          // 5.95 bps
    const std::int64_t cash = 361'414'914;        // what the ledger moved
    check(conservation_residual(Notional{fills}, Notional{costs},
                                Notional{cash}) == 0,
          "a balanced tick has a residual of exactly zero");
    check(enforce_conservation(k, Notional{fills}, Notional{costs},
                               Notional{cash}, kNow) == 0
          && !k.tripped(),
          "and does not trip the switch");

    // Now a ONE PAISA breach, at F&O notional scale where a double cannot
    // represent it. Rs 4 lakh crore of daily notional is 4e16 paise; the ulp
    // of a double there is 8.
    const std::int64_t big_fills = -40'000'000'000'000'001LL;   // 4e16 + 1 paisa
    const std::int64_t big_costs = 0;
    const std::int64_t big_cash = 40'000'000'000'000'000LL;

    const std::int64_t exact = conservation_residual(
        Notional{big_fills}, Notional{big_costs}, Notional{big_cash});
    const double as_double = static_cast<double>(big_fills)
                           + static_cast<double>(big_costs)
                           + static_cast<double>(big_cash);
    std::printf("    a ONE PAISA breach on Rs 4 lakh crore of notional:\n");
    std::printf("      integer paise : residual %lld  -> BREACH\n",
                static_cast<long long>(exact));
    std::printf("      same in double: residual %.1f  -> looks balanced\n",
                as_double);
    check(exact == -1, "the integer check finds the missing paisa");
    check(as_double == 0.0,
          "and the double check reports exactly zero -- it is not a weaker"
          " version of the same check, it answers yes when the answer is no");

    KillSwitch k2;
    check(enforce_conservation(k2, Notional{big_fills}, Notional{big_costs},
                               Notional{big_cash}, kNow) == -1,
          "enforce_conservation returns the residual so its size and sign"
          " survive into the log");
    check(k2.tripped() && k2.reason() == KillReason::ConservationBreach,
          "and trips the switch as a side effect, because a breach that is"
          " merely returned is a breach someone can forget to act on");
    check(clean(check_order(good_order(), AccountState{}, limits(), k2, kNow))
          == false,
          "after which nothing trades");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void staleness_rejects_both_old_and_future_quotes()
{
    std::printf("\n6 staleness_rejects_both_old_and_future_quotes\n");
    KillSwitch k;
    AccountState a{};
    RiskLimits l = limits();                       // 500 ms budget

    ProposedOrder fresh = good_order();
    fresh.quote_ts = kNow - duration::millis(499);
    check(!has(check_order(fresh, a, l, k, kNow), Violation::StaleQuote),
          "a quote 499 ms old is inside a 500 ms budget");

    ProposedOrder old = good_order();
    old.quote_ts = kNow - duration::millis(501);
    check(has(check_order(old, a, l, k, kNow), Violation::StaleQuote),
          "and one 501 ms old is not");

    // A quote from the FUTURE is as broken as one too old, and accepting it
    // would be look-ahead (rule 7).
    ProposedOrder future = good_order();
    future.quote_ts = kNow + duration::millis(10);
    check(has(check_order(future, a, l, k, kNow), Violation::StaleQuote),
          "a quote stamped in the FUTURE is refused too -- accepting it would"
          " be look-ahead, which rule 7 forbids outright");

    // Time comes off the tick. Advancing `now` alone makes the same order
    // stale, with nothing else changed.
    check(!has(check_order(fresh, a, l, k, kNow), Violation::StaleQuote)
          && has(check_order(fresh, a, l, k, kNow + duration::seconds(1)),
                 Violation::StaleQuote),
          "and staleness is measured against the tick's own clock, so the"
          " same order goes stale purely by time passing on the feed");
}

// ── 7 ────────────────────────────────────────────────────────────────────
void portfolio_and_loss_limits_bind()
{
    std::printf("\n7 portfolio_and_loss_limits_bind\n");
    KillSwitch k;
    RiskLimits l = limits();

    AccountState a{};
    a.gross_notional = Notional{19'900'000'000};   // just under the 200 cr cap
    ProposedOrder o = good_order();                  // adds 36.12 cr
    check(has(check_order(o, a, l, k, kNow), Violation::MaxGrossNotional),
          "an order that would push gross notional past the portfolio cap is"
          " refused on the resulting total");

    a = AccountState{};
    a.day_pnl = Notional{-500'000'000};            // exactly at the limit
    check(has(check_order(o, a, l, k, kNow), Violation::DailyLossLimit),
          "hitting the daily loss limit exactly is a breach, not a near miss");
    a.day_pnl = Notional{-499'999'999};
    check(!has(check_order(o, a, l, k, kNow), Violation::DailyLossLimit),
          "one paisa short of it is not");
    a.day_pnl = Notional{5'000'000};               // a profit
    check(!has(check_order(o, a, l, k, kNow), Violation::DailyLossLimit),
          "and a PROFIT does not trip a loss limit -- the sign convention"
          " holds, which a magnitude comparison would have got wrong");

    a = AccountState{};
    a.open_orders = 20;
    check(has(check_order(o, a, l, k, kNow), Violation::MaxOpenOrders),
          "the twenty-first order is refused when twenty are live");
    a.open_orders = 19;
    check(!has(check_order(o, a, l, k, kNow), Violation::MaxOpenOrders),
          "the twentieth is not");

    // A zero cap means "not configured" and must not block everything.
    RiskLimits none{};
    none.max_quote_age = duration::millis(500);
    check(clean(check_order(good_order(), AccountState{}, none, k, kNow)),
          "unconfigured caps are zero and do not block -- otherwise an empty"
          " config would refuse every order and look like a broken engine");
}

} // namespace

int main()
{
    std::printf("altair risk limits tests\n");
    a_clean_order_passes_and_a_broken_one_reports_everything();
    a_missing_spec_blocks_but_an_absent_band_does_not();
    the_kill_switch_is_sticky_and_short_circuits();
    caps_are_checked_against_the_resulting_position();
    conservation_is_exact_and_double_would_miss_it();
    staleness_rejects_both_old_and_future_quotes();
    portfolio_and_loss_limits_bind();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
