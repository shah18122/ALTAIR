// P3-09 acceptance tests for risk/cost.hpp.
//
// The schedules here mirror config/charges.toml, which is why that file had to
// parse before this card could be written.
//
// No check description here may contain the substring FAIL.

#include <risk/cost.hpp>

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>

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

constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept
{
    y -= (m <= 2);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned shifted = (m > 2) ? (m - 3u) : (m + 9u);
    const unsigned doy = (153u * shifted + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

constexpr Timestamp ist_date(std::int64_t y, unsigned m, unsigned d) noexcept
{
    return Timestamp{days_from_civil(y, m, d) * 86'400'000'000'000LL
                     - kIstOffset.raw()};
}

// ── the two real schedules from config/charges.toml ──────────────────────
ChargeSchedule pre_april()
{
    ChargeSchedule s{};
    s.valid_from = ist_date(2000, 1, 1);
    s.valid_to   = ist_date(2026, 3, 31);
    s.verified   = false;                       // charges.toml says UNVERIFIED

    s.equity_delivery.present   = true;
    s.equity_delivery.stt       = rate_from(0.001L);
    s.equity_delivery.stt_side  = ChargeSide::Both;
    s.equity_delivery.exch_txn_nse = rate_from(0.0000297L);
    s.equity_delivery.stamp     = rate_from(0.00015L);
    s.equity_delivery.ipft      = rate_from(0.000001L);
    s.equity_delivery.dp_per_scrip_sell = Notional{1534};   // Rs 15.34

    s.equity_intraday.present   = true;
    s.equity_intraday.stt       = rate_from(0.00025L);
    s.equity_intraday.stt_side  = ChargeSide::Sell;
    s.equity_intraday.exch_txn_nse = rate_from(0.0000297L);
    s.equity_intraday.stamp     = rate_from(0.00003L);
    s.equity_intraday.ipft      = rate_from(0.000001L);

    s.equity_futures.present    = true;
    s.equity_futures.stt        = rate_from(0.0002L);       // 0.02%
    s.equity_futures.stt_side   = ChargeSide::Sell;
    s.equity_futures.exch_txn_nse = rate_from(0.0000173L);
    s.equity_futures.stamp      = rate_from(0.00002L);
    s.equity_futures.ipft       = rate_from(0.0000005L);

    s.equity_options.present    = true;
    s.equity_options.basis      = TurnoverBasis::Premium;
    s.equity_options.stt        = rate_from(0.001L);        // 0.10%
    s.equity_options.stt_side   = ChargeSide::Sell;
    s.equity_options.exch_txn_nse = rate_from(0.0003503L);
    s.equity_options.stamp      = rate_from(0.00003L);
    s.equity_options.ipft       = rate_from(0.000005L);
    return s;
}

ChargeSchedule post_april()
{
    ChargeSchedule s = pre_april();
    s.valid_from = ist_date(2026, 4, 1);
    s.valid_to   = ist_date(2099, 12, 31);
    s.equity_futures.stt = rate_from(0.0005L);   // RAISED 0.02% -> 0.05%
    s.equity_options.stt = rate_from(0.0015L);   // RAISED 0.10% -> 0.15%
    return s;
}

// Zerodha: Rs 20 or 0.03%, whichever is LOWER, for intraday and F&O.
BrokerageRule zerodha_fo()
{
    BrokerageRule b{};
    b.flat_per_order = Notional{2000};           // Rs 20 in paise
    b.pct = rate_from(0.0003L);
    b.take_lower = true;
    return b;
}

BrokerageRule zerodha_delivery()
{
    return BrokerageRule{};                      // free
}

Trade fut_trade(Side side, std::int64_t px_paise, std::int64_t qty)
{
    Trade t{};
    t.segment = Segment::Fut;
    t.exchange = Exchange::NSE;
    t.side = side;
    t.qty = Qty{qty};
    t.price = Price{px_paise};
    t.trade_ts = ist_date(2026, 9, 1);
    return t;
}

// ── 1 ────────────────────────────────────────────────────────────────────
// D1. The factor-of-500 trap.
void option_turnover_is_premium_not_notional()
{
    std::printf("\n1 option_turnover_is_premium_not_notional\n");
    const ChargeSchedule s = post_april();

    // A NIFTY 25000 call trading at Rs 50, lot 65.
    Trade t{};
    t.segment = Segment::Opt;
    t.side = Side::Sell;                          // STT is sell side
    t.qty = Qty{65};
    t.price = Price{5'000};                       // Rs 50 PREMIUM, in paise
    t.trade_ts = ist_date(2026, 9, 1);

    const auto c = compute_cost(t, s, zerodha_fo());
    check(c.has_value(), "the option trade prices");
    if (!c) { return; }

    check(c->turnover == Notional{5'000 * 65},
          "turnover is PREMIUM x qty = Rs 3'250, from the traded price");
    check(c->basis == TurnoverBasis::Premium, "and the basis says so");

    // What a notional-based calculation would have produced.
    const std::int64_t notional = 2'500'000LL * 65;      // strike x qty
    check(notional / c->turnover.raw() == 500,
          "notional would have been 500x larger -- Rs 16'25'000 against "
          "Rs 3'250");

    // STT at 0.15% of each.
    const std::int64_t stt_premium = c->stt.raw();
    std::int64_t stt_notional = 0;
    (void)detail::mul_rate(notional, rate_from(0.0015L), stt_notional);
    check(stt_premium > 0, "STT on premium is charged");
    // 325'000 x 0.0015 is 487.5, which rounds to 488, so the ratio is 499.5
    // rather than a clean 500. The rounding lives at the small end, which is
    // exactly where it should: the premium charge is the one that must be
    // right to the paisa.
    const double ratio = static_cast<double>(stt_notional)
                       / static_cast<double>(stt_premium);
    std::printf("       STT premium %lld paise, notional %lld paise, ratio %.1f\n",
                static_cast<long long>(stt_premium),
                static_cast<long long>(stt_notional), ratio);
    check(ratio > 499.0 && ratio < 500.0,
          "and STT on notional would be ~500x it -- enough to kill every "
          "options strategy before it was written, or to understate a live "
          "bill by the same factor");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// D2. Side is load-bearing.
void side_decides_stt_and_stamp()
{
    std::printf("\n2 side_decides_stt_and_stamp\n");
    const ChargeSchedule s = post_april();
    const BrokerageRule b = zerodha_fo();

    const auto buy  = compute_cost(fut_trade(Side::Buy,  2'408'000, 65), s, b);
    const auto sell = compute_cost(fut_trade(Side::Sell, 2'408'000, 65), s, b);
    check(buy.has_value() && sell.has_value(), "both sides price");

    check(buy->stt == Notional{0},
          "a futures BUY pays no STT -- it is a sell-side tax");
    check(sell->stt.raw() > 0, "the SELL pays it");
    check(buy->stamp.raw() > 0, "while stamp duty is BUY side only");
    check(sell->stamp == Notional{0}, "and the sell pays none");
    check(sell->total.raw() > buy->total.raw(),
          "so the sell costs more overall, which is the shape of the real "
          "bill");

    // Delivery is the exception: STT applies to BOTH sides.
    Trade d{};
    d.segment = Segment::Cash;
    d.delivery = true;
    d.qty = Qty{100};
    d.price = Price{150'000};
    d.trade_ts = ist_date(2026, 9, 1);
    d.side = Side::Buy;
    const auto db = compute_cost(d, s, zerodha_delivery());
    d.side = Side::Sell;
    const auto ds = compute_cost(d, s, zerodha_delivery());
    check(db.has_value() && ds.has_value(), "delivery prices both ways");
    check(db->stt.raw() > 0 && ds->stt.raw() > 0,
          "DELIVERY charges STT on BOTH sides -- a calculator that assumed "
          "sell-only everywhere would halve the largest line on the bill");
    check(ds->dp.raw() > 0 && db->dp.raw() == 0,
          "and the DP charge is delivery-SELL only, flat per scrip");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// D6. The schedule follows the trade date.
void the_schedule_follows_the_trade_date()
{
    std::printf("\n3 the_schedule_follows_the_trade_date\n");
    const ChargeSchedule scheds[2] = {pre_april(), post_april()};

    const Timestamp march = ist_date(2026, 3, 15);
    const Timestamp april = ist_date(2026, 4, 15);
    const auto* m = schedule_for(scheds, 2, march);
    const auto* a = schedule_for(scheds, 2, april);
    check(m != nullptr && a != nullptr, "both dates find a schedule");
    check(m != a, "and they are different schedules");

    const BrokerageRule b = zerodha_fo();
    const Trade t = fut_trade(Side::Sell, 2'408'000, 65);
    const auto cm = compute_cost(t, *m, b);
    const auto ca = compute_cost(t, *a, b);
    check(cm.has_value() && ca.has_value(), "both price");
    check(ca->stt.raw() > cm->stt.raw(),
          "the same futures sell costs MORE from April -- STT went 0.02% to "
          "0.05%");
    check(ca->stt.raw() == cm->stt.raw() * 5 / 2
              || ca->stt.raw() / cm->stt.raw() == 2,
          "roughly 2.5x, which is 0.05 over 0.02");
    check(ca->total.raw() > cm->total.raw(),
          "so a March backtest priced with April's schedule would be "
          "PESSIMISTIC, and an April session priced with March's optimistic "
          "-- CLAUDE.md's warning made checkable");

    // The last day of a regime is charged that regime.
    check(schedule_for(scheds, 2, ist_date(2026, 3, 31)) == m,
          "31 March is still the old regime -- circulars say 'to D2 "
          "inclusive', so the upper bound is closed");
    check(schedule_for(scheds, 2, ist_date(2026, 4, 1)) == a,
          "and 1 April is the new one");
    check(schedule_for(scheds, 2, ist_date(1990, 1, 1)) == nullptr,
          "a date before every schedule finds none rather than the nearest");
}

// ── 4 ────────────────────────────────────────────────────────────────────
// D4. GST is not levied on a tax.
void gst_excludes_stt_and_stamp()
{
    std::printf("\n4 gst_excludes_stt_and_stamp\n");
    const ChargeSchedule s = post_april();
    const auto c = compute_cost(fut_trade(Side::Sell, 2'408'000, 65), s,
                                zerodha_fo());
    check(c.has_value(), "the trade prices");
    if (!c) { return; }

    std::int64_t expect = 0;
    const std::int64_t base = c->brokerage.raw() + c->exchange_txn.raw()
                            + c->sebi.raw() + c->ipft.raw();
    (void)detail::mul_rate(base, rate_from(0.18L), expect);
    check(c->gst.raw() == expect,
          "GST is 18% of (brokerage + exchange + SEBI + IPFT) exactly");

    std::int64_t wrong = 0;
    (void)detail::mul_rate(base + c->stt.raw() + c->stamp.raw(),
                           rate_from(0.18L), wrong);
    check(wrong > c->gst.raw(),
          "including STT and stamp duty would inflate it -- GST is not levied "
          "on a tax, and on options that difference is the gap between an "
          "edge and a loss");
}

// ── 5 ────────────────────────────────────────────────────────────────────
// The P0-01 carried debt, discharged.
void arithmetic_is_exact_at_session_scale()
{
    std::printf("\n5 arithmetic_is_exact_at_session_scale\n");

    // Rs 1000 crore of turnover, in paise. This is the magnitude the P0-01
    // debt named: "session-accumulated turnover".
    const std::int64_t huge = 1'000'000'000'000LL;      // 1e12 paise
    std::int64_t v = 0;
    check(detail::mul_rate(huge, rate_from(0.0000297L), v),
          "a rate applied to Rs 1000 crore does not overflow");
    check(v == 29'700'000LL,
          "and is EXACT: 1e12 x 0.0000297 = 29'700'000 paise, to the paisa");

    // The naive int64 product would have overflowed two orders of magnitude
    // ago; long double would have lost the low digits.
    const long double approx = static_cast<long double>(huge) * 0.0000297L;
    check(static_cast<std::int64_t>(approx) == v
              || static_cast<std::int64_t>(approx) == v - 1,
          "long double lands within a paisa here, but it is APPROXIMATE -- "
          "the integer path is exact by construction and stays exact as "
          "turnover grows");

    // Rounding is half away from zero.
    check(detail::mul_rate(15, rate_from(0.1L), v) && v == 2,
          "15 x 0.1 = 1.5 rounds to 2, away from zero");
    check(detail::mul_rate(5, rate_from(0.1L), v) && v == 1,
          "5 x 0.1 = 0.5 rounds to 1");
    check(detail::mul_rate(-15, rate_from(0.1L), v) && v == -2,
          "and -1.5 rounds to -2, not -1");

    // Genuine overflow is reported, not wrapped.
    check(!detail::mul_rate(std::numeric_limits<std::int64_t>::max() / 2,
                            rate_from(100.0L), v),
          "a product that cannot fit int64 returns false rather than a "
          "wrapped number that would look like a plausible charge");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void brokerage_takes_the_lower()
{
    std::printf("\n6 brokerage_takes_the_lower\n");
    const ChargeSchedule s = post_april();
    const BrokerageRule b = zerodha_fo();          // Rs 20 or 0.03%, lower

    // Small trade: 0.03% is under Rs 20.
    const auto small = compute_cost(fut_trade(Side::Buy, 100'000, 1), s, b);
    check(small.has_value() && small->brokerage.raw() == 30,
          "on a Rs 1000 turnover, 0.03% is 30 paise and beats the Rs 20 flat");

    // Large trade: the flat wins.
    const auto big = compute_cost(fut_trade(Side::Buy, 2'408'000, 65), s, b);
    check(big.has_value() && big->brokerage == Notional{2000},
          "on a Rs 15.65 lakh turnover, 0.03% exceeds Rs 20 so the FLAT "
          "applies -- 'whichever is lower' is a min, not a sum");

    const auto free = compute_cost(fut_trade(Side::Buy, 2'408'000, 65), s,
                                   zerodha_delivery());
    check(free.has_value() && free->brokerage == Notional{0},
          "and a zero rule charges nothing");
}

// ── 7 ────────────────────────────────────────────────────────────────────
// What a strategy actually needs, and rule 5's whole point.
void round_trip_is_what_a_signal_must_beat()
{
    std::printf("\n7 round_trip_is_what_a_signal_must_beat\n");
    const ChargeSchedule s = post_april();
    const BrokerageRule b = zerodha_fo();

    const Trade t = fut_trade(Side::Buy, 2'408'000, 65);
    const auto rt = round_trip_cost(t, s, b);
    check(rt.has_value(), "the round trip prices");
    if (!rt) { return; }

    const auto buy  = compute_cost(fut_trade(Side::Buy,  2'408'000, 65), s, b);
    const auto sell = compute_cost(fut_trade(Side::Sell, 2'408'000, 65), s, b);
    check(rt->raw() == buy->total.raw() + sell->total.raw(),
          "and equals buy plus sell exactly");

    // Expressed as a move in the underlying, which is the number that decides
    // whether a signal is worth taking.
    const std::int64_t turnover = 2'408'000LL * 65;
    const double bps = 10'000.0 * static_cast<double>(rt->raw())
                     / static_cast<double>(turnover);
    std::printf("       round trip on Rs %.2f turnover: Rs %.2f (%.2f bps)\n",
                turnover / 100.0, rt->raw() / 100.0, bps);
    check(bps > 0.0 && bps < 100.0,
          "a plausible round-trip cost in basis points");
    check(rt->raw() > buy->total.raw(),
          "and it is strictly more than one leg -- a strategy that netted only "
          "the entry would think it had an edge it does not have");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void refusals_and_provenance()
{
    std::printf("\n8 refusals_and_provenance\n");
    const ChargeSchedule s = post_april();
    const BrokerageRule b = zerodha_fo();

    const auto zero = compute_cost(fut_trade(Side::Buy, 2'408'000, 0), s, b);
    check(!zero.has_value() && zero.error() == CostError::BadQuantity,
          "a zero quantity is refused, not priced at zero cost");
    const auto neg = compute_cost(fut_trade(Side::Buy, -1, 65), s, b);
    check(!neg.has_value() && neg.error() == CostError::BadPrice,
          "a negative price is refused");

    // A segment the schedule has no line for.
    ChargeSchedule bare = s;
    bare.equity_futures.present = false;
    const auto miss = compute_cost(fut_trade(Side::Buy, 2'408'000, 65), bare, b);
    check(!miss.has_value() && miss.error() == CostError::UnknownSegment,
          "a segment with no charge line is UnknownSegment -- never priced at "
          "zero, which would make an untaxed segment look like free money");

    const auto c = compute_cost(fut_trade(Side::Buy, 2'408'000, 65), s, b);
    check(c.has_value() && !c->schedule_verified,
          "and the result carries schedule_verified = false, because "
          "charges.toml still says UNVERIFIED -- a P&L whose cost basis was "
          "never checked against a circular should say so");

    ChargeSchedule ok = s;
    ok.verified = true;
    const auto v = compute_cost(fut_trade(Side::Buy, 2'408'000, 65), ok, b);
    check(v.has_value() && v->schedule_verified,
          "a verified schedule says so too");
}

} // namespace

int main()
{
    std::printf("altair risk cost tests\n");
    option_turnover_is_premium_not_notional();
    side_decides_stt_and_stamp();
    the_schedule_follows_the_trade_date();
    gst_excludes_stt_and_stamp();
    arithmetic_is_exact_at_session_scale();
    brokerage_takes_the_lower();
    round_trip_is_what_a_signal_must_beat();
    refusals_and_provenance();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
