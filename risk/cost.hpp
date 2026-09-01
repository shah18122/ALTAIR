// risk/cost.hpp — the cost calculator.
//
// P3-09. CLAUDE.md rule 5: "Every signal is priced net of full cost before it
// exists. No strategy sees a pre-cost number." This is the file that makes
// that enforceable, so everything downstream depends on it being right.
//
// EXACT INTEGER ARITHMETIC, and that is the point.
//
// The carried debt from P0-01 says `apply_bps` uses `long double` -- 64-bit on
// MSVC -- and "must be revisited with scaled-integer arithmetic in P3-09 where
// session-accumulated turnover is involved." This file therefore does NOT use
// apply_bps. Rates are integers scaled by 1e9 and every product goes through a
// 128-bit intermediate, so a session's accumulated charges are exact to the
// paisa no matter how large the turnover grows. A cost that drifts is a P&L
// that drifts, and a strategy graded on drifting P&L learns the drift.
//
// WHAT IT PRICES: one trade -- a buy or a sell of one contract. Exercise and
// assignment are charged differently (STT on an exercised option is on
// intrinsic value, not premium) and are a separate card.
//
// Decisions D1..D7 are fixed in prompts/P3-09_cost.md.

#pragma once

#include <core/types/units.hpp>
#include <core/time/timestamp.hpp>
#include <instruments/contract_spec.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace altair {

/// A rate as an integer scaled by 1e9. 0.0000297 -> 29'700.
///
/// Nano rather than basis points because exchange transaction charges are
/// finer than a basis point: 0.0000297 is 0.297 bp. Nano-fractions hold every
/// published rate exactly, and exactly is the whole requirement.
using RateNano = std::int64_t;
inline constexpr std::int64_t kRateScale = 1'000'000'000;

/// Build a RateNano from a decimal fraction at COMPILE TIME only.
/// Runtime rates come from the schedule as integers; this exists so a test can
/// write 0.0005 and mean it.
[[nodiscard]] constexpr RateNano rate_from(long double f) noexcept {
    return static_cast<RateNano>(f * static_cast<long double>(kRateScale)
                                 + (f < 0 ? -0.5L : 0.5L));
}

enum class ChargeSide : std::uint8_t { None, Buy, Sell, Both };
enum class Side : std::uint8_t { Buy, Sell };

/// Which quantity a rate is applied to.
///
/// D1 — THE TRAP THIS FILE EXISTS AROUND. For options, STT and exchange
/// charges are levied on PREMIUM, not notional. A NIFTY 25000 call trading at
/// Rs 50 has a premium turnover of Rs 50 x lot and a notional of Rs 25'000 x
/// lot -- a factor of 500. Charging notional would make every option look 500x
/// too expensive and kill every options strategy before it was written;
/// charging notional where premium was meant on a live fill would understate
/// the bill by the same factor.
///
/// This calculator takes the TRADED PRICE, which for an option IS the premium,
/// so it computes premium turnover by construction. The field is carried so a
/// caller cannot quietly hand it a strike and be believed.
enum class TurnoverBasis : std::uint8_t { Notional, Premium };

enum class CostError : std::uint8_t {
    NoSchedule,     // no schedule covers the trade date
    BadQuantity,    // zero or negative
    BadPrice,       // negative
    Overflow,       // turnover or a charge exceeds int64 paise
    UnknownSegment  // no charge line for this segment
};

/// Charges for one segment under one schedule.
struct SegmentCharges {
    RateNano      stt = 0;
    ChargeSide    stt_side = ChargeSide::None;
    RateNano      exch_txn_nse = 0;
    RateNano      exch_txn_bse = 0;
    RateNano      stamp = 0;
    ChargeSide    stamp_side = ChargeSide::Buy;
    RateNano      ipft = 0;
    TurnoverBasis basis = TurnoverBasis::Notional;
    /// Flat, sell side only, delivery only. UNIT: paise.
    Notional      dp_per_scrip_sell{};
    bool          present = false;   // false == this schedule has no such line
};

/// One effective-dated schedule.
///
/// D6 — chosen by the TRADE DATE, never by today. A backtest of March must be
/// charged March's rates. CLAUDE.md: STT rose on 2026-04-01, so "every
/// pre-April backtest is optimistic until re-run" -- which is only true if the
/// backtester can actually reach the old schedule.
struct ChargeSchedule {
    Timestamp valid_from{};
    Timestamp valid_to{};

    SegmentCharges equity_delivery{};
    SegmentCharges equity_intraday{};
    SegmentCharges equity_futures{};
    SegmentCharges equity_options{};
    SegmentCharges currency_futures{};
    SegmentCharges currency_options{};
    SegmentCharges commodity_futures{};

    /// On (brokerage + exchange txn + SEBI + IPFT). NOT on STT or stamp duty.
    RateNano gst = rate_from(0.18L);
    /// SEBI turnover fee: Rs 10 per Rs 1 crore == 1e-6 of turnover.
    RateNano sebi = rate_from(0.000001L);

    /// D7 — carried through to every result. A P&L figure whose cost basis was
    /// never checked against a circular should say so rather than look like
    /// any other number.
    bool verified = false;
};

/// Broker charges. Separate from the schedule because they are commercial, not
/// regulatory, and change without a circular.
struct BrokerageRule {
    /// UNIT: paise. Zero for delivery on a discount broker.
    Notional flat_per_order{};
    /// Percentage alternative.
    RateNano pct = 0;
    /// The charge is min(flat, pct x turnover) when both are non-zero --
    /// Zerodha's "Rs 20 or 0.03%, whichever is lower".
    bool take_lower = true;
};

struct Trade {
    Segment   segment = Segment::Cash;
    Exchange  exchange = Exchange::NSE;
    Side      side = Side::Buy;
    Qty       qty{};
    /// The TRADED price. For an option this is the premium (D1).
    Price     price{};
    /// Cash only: delivery nets differently and is taxed differently.
    bool      delivery = false;
    /// The instant the trade happened. Selects the schedule (D6).
    Timestamp trade_ts{};
};

/// D5 — itemised, never a single number. A cost you cannot decompose is a cost
/// you cannot audit, dispute, or explain to a strategy that just lost money.
struct CostBreakdown {
    Notional turnover{};
    Notional brokerage{};
    Notional stt{};
    Notional exchange_txn{};
    Notional sebi{};
    Notional stamp{};
    Notional ipft{};
    Notional gst{};
    Notional dp{};
    Notional total{};
    TurnoverBasis basis = TurnoverBasis::Notional;
    bool schedule_verified = false;
};

namespace detail {

/// (a * b) / kRateScale, rounded half away from zero, through a 128-bit
/// intermediate. Returns false on overflow of the int64 result.
///
/// The 128-bit step is not decoration. Turnover of Rs 1000 crore is 1e12
/// paise; multiplied by a rate of 1.0 (1e9 nano) that is 1e21, which overflows
/// int64 by two orders of magnitude. Session-accumulated turnover reaches that
/// scale, which is exactly the case the P0-01 debt named.
[[nodiscard]] inline bool mul_rate(std::int64_t v, RateNano r,
                                   std::int64_t& out) noexcept {
    if (v == 0 || r == 0) {
        out = 0;
        return true;
    }
    const bool neg = (v < 0) != (r < 0);
    const std::uint64_t a = v < 0 ? (~static_cast<std::uint64_t>(v) + 1u)
                                  : static_cast<std::uint64_t>(v);
    const std::uint64_t b = r < 0 ? (~static_cast<std::uint64_t>(r) + 1u)
                                  : static_cast<std::uint64_t>(r);

#if defined(_MSC_VER) && defined(_M_X64)
    std::uint64_t hi = 0;
    std::uint64_t lo = _umul128(a, b, &hi);
    // Round half away from zero: add kRateScale/2 before dividing.
    const std::uint64_t half = static_cast<std::uint64_t>(kRateScale) / 2u;
    const std::uint64_t lo2 = lo + half;
    if (lo2 < lo) {
        ++hi;
    }
    if (hi >= static_cast<std::uint64_t>(kRateScale)) {
        return false;                       // quotient would exceed 64 bits
    }
    std::uint64_t rem = 0;
    const std::uint64_t q = _udiv128(hi, lo2, static_cast<std::uint64_t>(kRateScale),
                                     &rem);
#else
    const unsigned __int128 prod = static_cast<unsigned __int128>(a) * b
                                 + static_cast<unsigned __int128>(kRateScale / 2);
    const unsigned __int128 qq = prod / static_cast<unsigned __int128>(kRateScale);
    if (qq > static_cast<unsigned __int128>(
                 std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    const std::uint64_t q = static_cast<std::uint64_t>(qq);
#endif

    if (q > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    out = neg ? -static_cast<std::int64_t>(q) : static_cast<std::int64_t>(q);
    return true;
}

[[nodiscard]] constexpr bool side_applies(ChargeSide cs, Side s) noexcept {
    switch (cs) {
        case ChargeSide::None: return false;
        case ChargeSide::Both: return true;
        case ChargeSide::Buy:  return s == Side::Buy;
        case ChargeSide::Sell: return s == Side::Sell;
    }
    return false;
}

} // namespace detail

/// Pick the schedule covering `when`. Returns nullptr when none does.
///
/// Half-open on the upper bound would be wrong here: charge circulars are
/// written as "effective from D1 to D2 inclusive", and a trade ON the last day
/// of a regime is charged that regime's rates.
[[nodiscard]] inline const ChargeSchedule*
schedule_for(const ChargeSchedule* schedules, std::size_t n,
             Timestamp when) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const ChargeSchedule& s = schedules[i];
        if (!(when < s.valid_from) && !(s.valid_to < when)) {
            return &s;
        }
    }
    return nullptr;
}

/// The charge line for a trade's segment.
[[nodiscard]] inline const SegmentCharges*
line_for(const ChargeSchedule& s, const Trade& t) noexcept {
    switch (t.segment) {
        case Segment::Cash:
            return t.delivery ? &s.equity_delivery : &s.equity_intraday;
        case Segment::Fut:       return &s.equity_futures;
        case Segment::Opt:       return &s.equity_options;
        case Segment::Currency:  return &s.currency_futures;
        case Segment::Commodity: return &s.commodity_futures;
    }
    return nullptr;
}

/// Price one trade, itemised. UNIT: paise throughout.
///
/// D3 — every component is rounded half away from zero to a whole paisa, and
/// the total is the SUM OF ROUNDED COMPONENTS rather than a rounded sum. That
/// is how a contract note is actually computed, and matching it means our
/// number reconciles against the broker's rather than differing by a paisa
/// nobody can explain.
[[nodiscard]] ALTAIR_HOT inline std::expected<CostBreakdown, CostError>
compute_cost(const Trade& t, const ChargeSchedule& sch,
             const BrokerageRule& br) noexcept {
    if (t.qty.raw() <= 0) {
        return std::unexpected(CostError::BadQuantity);
    }
    if (t.price.raw() < 0) {
        return std::unexpected(CostError::BadPrice);
    }
    const SegmentCharges* line = line_for(sch, t);
    if (line == nullptr || !line->present) {
        return std::unexpected(CostError::UnknownSegment);
    }

    // Turnover from the TRADED price. For an option that price is the premium,
    // so this is premium turnover by construction (D1).
    const auto turn = notional_of(t.price, t.qty);
    if (!turn) {
        return std::unexpected(CostError::Overflow);
    }
    const std::int64_t T = turn->raw();

    CostBreakdown out{};
    out.turnover = *turn;
    out.basis = line->basis;
    out.schedule_verified = sch.verified;

    std::int64_t v = 0;

    // ── brokerage ────────────────────────────────────────────────────────
    std::int64_t brok = br.flat_per_order.raw();
    if (br.pct != 0) {
        if (!detail::mul_rate(T, br.pct, v)) {
            return std::unexpected(CostError::Overflow);
        }
        if (br.take_lower) {
            brok = (brok == 0) ? v : (v < brok ? v : brok);
        } else {
            brok = v;
        }
    }
    out.brokerage = Notional{brok};

    // ── STT / CTT ────────────────────────────────────────────────────────
    // D2 — the SIDE is load-bearing. STT is sell-side for intraday, futures
    // and options, but BOTH sides for delivery. A calculator that charged both
    // everywhere would double the largest single line on the bill.
    if (detail::side_applies(line->stt_side, t.side)) {
        if (!detail::mul_rate(T, line->stt, v)) {
            return std::unexpected(CostError::Overflow);
        }
        out.stt = Notional{v};
    }

    // ── exchange transaction charges ─────────────────────────────────────
    const RateNano ex = (t.exchange == Exchange::BSE) ? line->exch_txn_bse
                                                      : line->exch_txn_nse;
    if (!detail::mul_rate(T, ex, v)) {
        return std::unexpected(CostError::Overflow);
    }
    out.exchange_txn = Notional{v};

    // ── SEBI turnover fee ────────────────────────────────────────────────
    if (!detail::mul_rate(T, sch.sebi, v)) {
        return std::unexpected(CostError::Overflow);
    }
    out.sebi = Notional{v};

    // ── IPFT ─────────────────────────────────────────────────────────────
    if (!detail::mul_rate(T, line->ipft, v)) {
        return std::unexpected(CostError::Overflow);
    }
    out.ipft = Notional{v};

    // ── stamp duty: BUY side only ────────────────────────────────────────
    if (detail::side_applies(line->stamp_side, t.side)) {
        if (!detail::mul_rate(T, line->stamp, v)) {
            return std::unexpected(CostError::Overflow);
        }
        out.stamp = Notional{v};
    }

    // ── DP charge: delivery SELL only, flat per scrip ────────────────────
    if (t.segment == Segment::Cash && t.delivery && t.side == Side::Sell) {
        out.dp = line->dp_per_scrip_sell;
    }

    // ── GST on (brokerage + exchange + SEBI + IPFT) ──────────────────────
    // D4 — NOT on STT and NOT on stamp duty. Those are taxes; GST is not
    // levied on a tax. Including them inflates every bill by roughly the STT
    // again at 18%, which on options is the difference between an edge and a
    // loss.
    const std::int64_t gst_base = out.brokerage.raw() + out.exchange_txn.raw()
                                + out.sebi.raw() + out.ipft.raw();
    if (!detail::mul_rate(gst_base, sch.gst, v)) {
        return std::unexpected(CostError::Overflow);
    }
    out.gst = Notional{v};

    const std::int64_t total = out.brokerage.raw() + out.stt.raw()
                             + out.exchange_txn.raw() + out.sebi.raw()
                             + out.stamp.raw() + out.ipft.raw()
                             + out.gst.raw() + out.dp.raw();
    out.total = Notional{total};
    return out;
}

/// Round-trip cost: buy then sell the same quantity at the same price.
///
/// The number a strategy actually needs. A signal is worth taking only if its
/// expected move exceeds this, and rule 5 says it must be netted BEFORE the
/// signal exists rather than subtracted from a backtest afterwards.
[[nodiscard]] inline std::expected<Notional, CostError>
round_trip_cost(const Trade& t, const ChargeSchedule& sch,
                const BrokerageRule& br) noexcept {
    Trade b = t;
    b.side = Side::Buy;
    Trade s = t;
    s.side = Side::Sell;
    const auto cb = compute_cost(b, sch, br);
    if (!cb) {
        return std::unexpected(cb.error());
    }
    const auto cs = compute_cost(s, sch, br);
    if (!cs) {
        return std::unexpected(cs.error());
    }
    return Notional{cb->total.raw() + cs->total.raw()};
}

} // namespace altair
