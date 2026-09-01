// strategies/calendar.hpp -- the calendar spread scanner, options and futures.
//
// P5-07. Detection only; oms/ decides what to do.
//
// TWO EXPIRIES DO NOT SHARE A FORWARD, SO THEY DO NOT SHARE A MONEYNESS.
//
// This is the card, and it is the same shape as P5-06's: a no-arbitrage
// condition stated over one variable, evaluated over a different one.
//
// The calendar condition is on TOTAL IMPLIED VARIANCE at fixed log-moneyness:
//
//     w(k, T2) >= w(k, T1)   for T2 > T1, at every k = ln(K / F(T))
//
// and the forward is not the same at the two expiries. Fixed k therefore means
// a DIFFERENT STRIKE on each expiry -- K on the near one, K*F2/F1 on the far
// one. Iterating over strikes and reading each slice at that strike's own
// moneyness feels like aligning by moneyness and is exactly the strike-aligned
// comparison wearing the other label. An earlier draft of this file had the
// two branches swapped for that reason; the test caught it.
//
// WHAT THE MISALIGNMENT ACTUALLY DOES, MEASURED:
//
// The distortion is the forward shift times the local skew, so its SIGN
// follows the direction the forward moved -- and it does not scale with the
// calendar margin, so it dominates whenever the margin is small.
//
//   Index calendar, 7d vs 21d, pure carry. The far forward is ABOVE the near
//   one, k shifts 0.0025, and the strike-aligned margin comes out at 100.6%
//   of the correct one. Real, biased, and second order. Reporting that as a
//   headline would be as wrong as ignoring it.
//
//   Single stock going ex-dividend between the expiries. The far forward
//   drops BELOW the near one, k shifts -0.0086 -- three times as far, the
//   other way. On a surface whose total variance genuinely falls 0.5% from
//   near to far, the k-aligned scan finds the violation at all 401 sampled
//   strikes and the strike-aligned scan finds 320. EIGHTY-ONE GENUINE
//   VIOLATIONS HIDDEN.
//
// So the failure is not the one it is natural to assume. On these pairs the
// strike-aligned test does not invent arbitrage -- it HIDES it, above the
// smile's minimum where w rises with k. That is the worse of the two: an
// invented opportunity costs money once and then gets fixed, while a hidden
// one is simply never seen.
//
// VOL FALLING WITH MATURITY IS NOT ARBITRAGE. TOTAL VARIANCE FALLING IS.
//
// The other trap in this family, and the reason nothing here compares implied
// vols. In a stressed market the front month can trade at 30 vol against 18
// two months out, and that is a perfectly ordinary term structure -- w still
// rises, because T rises faster than sigma^2 falls. A scanner comparing VOLS
// would flag every stressed market as an arbitrage.
//
// A FUTURES CALENDAR SPREAD IS SYMMETRIC. Both legs are futures, both short
// freely, so unlike P5-05's carry and P5-06's spot-hedged reversal, both
// directions are reachable. It joins the box as the second structure in Phase
// 5 with no executability caveat.

#pragma once

#include <analytics/svi.hpp>
#include <risk/cost.hpp>
#include <strategies/basis.hpp>
#include <strategies/parity.hpp>

#include <cmath>
#include <cstdint>
#include <expected>

namespace altair {

enum class CalendarSpreadError : std::uint8_t {
    /// The expiries were not strictly ordered.
    BadTenors,
    /// A price was zero or negative.
    BadPrice,
    /// The two legs were observed too far apart.
    StaleLeg,
    /// The caller did not say how stale is too stale.
    NoSkewLimit,
    /// An SVI slice was not admissible.
    BadSlice,
    /// The cost calculator refused.
    CostUnavailable
};

// ---------------------------------------------------------------------------
// Option calendars
// ---------------------------------------------------------------------------

/// Two slices of one surface, each with the forward it was fitted against.
///
/// The forward is carried WITH the slice, not alongside it. A slice and the
/// forward it was fitted to are one object; separating them is how they end up
/// mismatched, and a slice evaluated against the wrong forward is a different
/// surface.
struct DatedSlice {
    SviParams slice{};
    Price forward{0};
    Years t{0.0};
    Timestamp observed{};
};

/// How the two expiries were lined up before being compared.
enum class CalendarAlignment : std::uint8_t {
    Unknown = 0,
    /// Compared at equal log-moneyness. The correct one.
    Moneyness,
    /// Compared at equal strike. Carried only so the difference can be
    /// measured; never the basis for a decision.
    Strike
};

struct CalendarCheck {
    CalendarAlignment alignment = CalendarAlignment::Unknown;
    /// The smallest w_long - w_short found, and where. Negative is arbitrage.
    double worst = 0.0;
    /// The strike, in paise, at which `worst` occurred.
    std::int64_t worst_strike = 0;
    /// How many sampled strikes reported a violation.
    int violations = 0;
    int points = 0;
    [[nodiscard]] bool clean() const noexcept { return violations == 0; }
};

/// Scan a strike range for calendar arbitrage between two expiries.
///
/// `alignment` is REQUIRED and has no default. The two answers differ, one of
/// them is wrong, and a caller who did not choose should not silently get
/// either.
[[nodiscard]] inline std::expected<CalendarCheck, CalendarSpreadError>
scan_option_calendar(const DatedSlice& shorter, const DatedSlice& longer,
                     Price k_lo, Price k_hi, int points,
                     CalendarAlignment alignment) noexcept {
    if (!(longer.t.raw() > shorter.t.raw())) {
        return std::unexpected(CalendarSpreadError::BadTenors);
    }
    if (shorter.forward.raw() <= 0 || longer.forward.raw() <= 0
        || k_lo.raw() <= 0 || k_hi.raw() <= k_lo.raw() || points < 2) {
        return std::unexpected(CalendarSpreadError::BadPrice);
    }
    if (alignment == CalendarAlignment::Unknown) {
        return std::unexpected(CalendarSpreadError::BadSlice);
    }
    if (const auto ok = svi_check_params(shorter.slice); !ok) {
        return std::unexpected(CalendarSpreadError::BadSlice);
    }
    if (const auto ok = svi_check_params(longer.slice); !ok) {
        return std::unexpected(CalendarSpreadError::BadSlice);
    }

    const double f_short = static_cast<double>(shorter.forward.raw());
    const double f_long = static_cast<double>(longer.forward.raw());

    CalendarCheck out{};
    out.alignment = alignment;
    out.points = points;
    out.worst = std::numeric_limits<double>::infinity();

    const double lo = static_cast<double>(k_lo.raw());
    const double hi = static_cast<double>(k_hi.raw());
    const double step = (hi - lo) / static_cast<double>(points - 1);

    for (int i = 0; i < points; ++i) {
        const double K = lo + step * static_cast<double>(i);
        const double k_short = std::log(K / f_short);
        // Gatheral's condition is dw/dT >= 0 AT FIXED k, with k measured from
        // each expiry's OWN forward. So:
        //
        //  Moneyness: both slices at the SAME k. Fixed k means a DIFFERENT
        //             strike on each expiry -- K on the near one, K*F2/F1 on
        //             the far one -- and that is the point, not a defect.
        //  Strike:    each slice at its own k for a SHARED strike, which is
        //             two different k, which is not the condition.
        //
        // Getting these the right way round is the whole card, and an earlier
        // draft of this file had them swapped: iterating strikes and mapping
        // each to its own k per expiry FEELS like aligning by moneyness and
        // is exactly the strike-aligned comparison wearing the other label.
        const double k_long = alignment == CalendarAlignment::Moneyness
                                ? k_short
                                : std::log(K / f_long);
        const double d = svi_total_variance(longer.slice, k_long)
                       - svi_total_variance(shorter.slice, k_short);
        if (d < out.worst) {
            out.worst = d;
            out.worst_strike = static_cast<std::int64_t>(K + 0.5);
        }
        if (d < 0.0) { ++out.violations; }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Futures calendars
// ---------------------------------------------------------------------------

/// Near and far futures on one underlying, each with its own touch.
struct FuturesCalendarQuote {
    Touch near_leg{};
    Timestamp near_ts{};
    Years near_t{0.0};
    Touch far_leg{};
    Timestamp far_ts{};
    Years far_t{0.0};
    /// Continuously-compounded carry between the two expiries, per year.
    double rate = 0.0;
    double dividend_yield = 0.0;
};

struct FuturesCalendarOpportunity {
    Executability executability = Executability::Unknown;
    /// far - near, at the touch, in paise. Signed; which touch depends on the
    /// direction, so this is already the tradable number.
    double touch_spread = 0.0;
    /// What carry alone says the spread should be, in paise.
    double fair_spread = 0.0;
    /// The carry rate the spread is actually quoting, per year. Comparable
    /// across pairs of expiries in a way the point spread is not.
    double implied_carry = 0.0;
    Duration skew{0};
    Notional cost{};
    Notional gross_value{};
    Notional net{};

    [[nodiscard]] std::expected<Notional, Executability> net_edge() const
        noexcept {
        if (executability != Executability::Executable) {
            return std::unexpected(executability);
        }
        return net;
    }
    [[nodiscard]] bool actionable() const noexcept {
        return executability == Executability::Executable && net.raw() > 0;
    }
};

/// Scan a near/far futures pair.
///
/// Both legs are futures, so both directions are reachable and executability
/// never depends on borrow. What it DOES depend on is the two legs having been
/// seen at the same instant: the far month is thinner and its last print is
/// routinely older, which biases a naive spread toward whatever the near month
/// has done since.
[[nodiscard]] inline std::expected<FuturesCalendarOpportunity,
                                   CalendarSpreadError>
scan_futures_calendar(const FuturesCalendarQuote& q, Qty qty, Exchange ex,
                      Duration max_skew, const ChargeSchedule& sch,
                      const BrokerageRule& br) noexcept {
    if (max_skew.raw() <= 0) {
        return std::unexpected(CalendarSpreadError::NoSkewLimit);
    }
    if (!q.near_leg.live() || !q.far_leg.live()) {
        return std::unexpected(CalendarSpreadError::BadPrice);
    }
    if (!(q.far_t.raw() > q.near_t.raw()) || !(q.near_t.raw() > 0.0)) {
        return std::unexpected(CalendarSpreadError::BadTenors);
    }
    const std::int64_t raw = (q.far_ts - q.near_ts).raw();
    const Duration skew{raw < 0 ? -raw : raw};
    if (skew.raw() > max_skew.raw()) {
        return std::unexpected(CalendarSpreadError::StaleLeg);
    }

    FuturesCalendarOpportunity o{};
    o.skew = skew;
    o.executability = Executability::Executable;

    const double dt = q.far_t.raw() - q.near_t.raw();
    const double carry = std::expm1((q.rate - q.dividend_yield) * dt);

    // Buying the spread means buying the far and selling the near, so the far
    // crosses at its ASK and the near at its BID. Selling it is the mirror.
    const double buy_spread = static_cast<double>(q.far_leg.ask.raw())
                            - static_cast<double>(q.near_leg.bid.raw());
    const double sell_spread = static_cast<double>(q.far_leg.bid.raw())
                             - static_cast<double>(q.near_leg.ask.raw());

    // Fair is measured off the leg being sold in each case, so the comparison
    // is against a price we could actually have.
    const double fair_from_bid =
        static_cast<double>(q.near_leg.bid.raw()) * carry;
    const double fair_from_ask =
        static_cast<double>(q.near_leg.ask.raw()) * carry;

    const double sell_edge = sell_spread - fair_from_ask;   // spread rich
    const double buy_edge = fair_from_bid - buy_spread;     // spread cheap
    if (sell_edge >= buy_edge) {
        o.touch_spread = sell_spread;
        o.fair_spread = fair_from_ask;
        o.gross_value = Notional{static_cast<std::int64_t>(
            sell_edge * static_cast<double>(qty.raw()) + 0.5)};
    } else {
        o.touch_spread = buy_spread;
        o.fair_spread = fair_from_bid;
        o.gross_value = Notional{static_cast<std::int64_t>(
            buy_edge * static_cast<double>(qty.raw()) + 0.5)};
    }
    // ln(F_far / F_near) / dt. The comparable number: a 40-point spread over
    // one month and over three are different carries, exactly as in P5-05.
    o.implied_carry =
        std::log(static_cast<double>(q.far_leg.bid.raw() + q.far_leg.ask.raw())
                 / static_cast<double>(q.near_leg.bid.raw()
                                       + q.near_leg.ask.raw()))
        / dt + q.dividend_yield;

    // Four futures legs: open both, close both.
    std::int64_t total = 0;
    const Price legs[4] = {q.near_leg.bid, q.near_leg.ask, q.far_leg.ask,
                           q.far_leg.bid};
    const Side sides[4] = {Side::Sell, Side::Buy, Side::Buy, Side::Sell};
    for (int i = 0; i < 4; ++i) {
        Trade t{};
        t.segment = Segment::Fut;
        t.exchange = ex;
        t.side = sides[i];
        t.qty = qty;
        t.price = legs[i];
        t.trade_ts = q.far_ts;
        const auto c = compute_cost(t, sch, br);
        if (!c) { return std::unexpected(CalendarSpreadError::CostUnavailable); }
        total += c->total.raw();
    }
    o.cost = Notional{total};
    o.net = Notional{o.gross_value.raw() - o.cost.raw()};
    return o;
}

} // namespace altair
