// analytics/vix.hpp -- India VIX by variance-swap replication.
//
// P3-08. The NSE India VIX methodology, which is the CBOE VIX methodology
// applied to NIFTY options.
//
// IT IS MODEL-FREE, AND THAT IS THE ENTIRE POINT.
//
// Nothing in this header prices an option. There is no Black-76 here, no
// implied vol, no smile. The formula comes from the fact that a LOG CONTRACT
// -- a claim paying -2*ln(S_T/F) -- can be replicated exactly and statically
// by a strip of out-of-the-money options weighted 1/K^2, and the fair value of
// that strip is the expected variance. So VIX measures what the option market
// is charging for variance, without assuming any model of how prices move.
//
// That independence is what makes it useful HERE: it is a check on P3-05's SVI
// fit that shares none of its assumptions. If the fitted surface and the
// replication disagree, one of them is wrong, and they cannot be wrong the
// same way.
//
// THREE THINGS THAT MAKE THE NUMBER WRONG, all of which look fine:
//
//   1. USING IN-THE-MONEY OPTIONS. The strip must be OTM only -- puts below
//      the forward, calls above, and the average of the two at K0. An ITM call
//      is an OTM put plus a forward by parity, so including both counts the
//      same variance twice. Nothing errors; the number is just too big.
//
//   2. COARSE STRIKES. The integral over K is approximated by a sum over the
//      strikes that happen to be listed, so the answer depends on the ladder.
//      Measured on a flat 15-vol surface, where the true answer is exactly
//      15.00:
//
//          strike step     VIX      bias
//          Rs   50       15.0029   +0.0029      (NIFTY's actual step)
//          Rs  100       15.0117   +0.0117
//          Rs  250       15.0735   +0.0735
//          Rs  500       15.2923   +0.2923      100x worse
//
//      NIFTY's real ladder is fine. A stock with a Rs 500 step is not, and the
//      bias is upward, so a thin chain reports more fear than there is.
//
//   3. TRUNCATION. Stopping the strip early drops real variance, always
//      downward. The wings are cheap and easy to dismiss, and they are exactly
//      where a variance swap gets its convexity.
//
// AND ONE THING PEOPLE READ WRONG: VIX IS NOT THE AT-THE-MONEY VOL. It is the
// whole strip, and skew lifts it. On the realistic NIFTY slice used in P3-05 --
// 15.06 at the money -- this returns 16.70. That 1.64-point gap IS the skew,
// not a signal. Comparing India VIX to NIFTY ATM IV and trading the difference
// is trading the shape of the smile while believing you are trading its level.

#pragma once

#include <core/types/units.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class VixError : std::uint8_t {
    /// Fewer than three usable strikes. The strip needs a K0 with a neighbour
    /// on each side before a spacing can even be defined.
    TooFewStrikes,
    /// No strike carries both a call and a put, so the parity forward cannot
    /// be derived.
    NoParityStrike,
    /// Every listed strike is above the derived forward, so there is no K0.
    /// A chain that does not straddle the money cannot price variance.
    NoStrikeBelowForward,
    /// Strikes are not strictly ascending. Refused rather than sorted in
    /// place: the caller's order is information, and silently reordering
    /// hides a broken chain builder.
    UnsortedStrikes,
    /// T <= 0, or the two terms of an interpolation are not ordered.
    NonPositiveTime,
    NotFinite
};

/// One strike on the chain, with mid quotes.
///
/// Prices are MIDPOINTS in paise. A missing side is `has_call`/`has_put`
/// false, NOT a zero price -- a zero-priced option is a real quote (a
/// worthless wing) and means something different from an absent one. That
/// distinction is what the truncation rule keys on.
struct VixQuote {
    Price strike{0};
    Price call{0};
    Price put{0};
    bool has_call = false;
    bool has_put = false;
};

/// The variance of one expiry, and the working that produced it.
struct VixTerm {
    /// Annualised variance, sigma^2. UNIT: 1/years.
    double variance = 0.0;
    /// Forward implied by put-call parity at the strike where |C - P| is
    /// smallest. UNIT: paise.
    double forward = 0.0;
    /// The first listed strike at or below the forward. UNIT: paise.
    double k0 = 0.0;
    /// The -(1/T)*(F/K0 - 1)^2 term, kept separately because it is small,
    /// always negative, and the first thing to check when a number looks off.
    double correction = 0.0;
    /// Strikes that entered the strip.
    int strikes_used = 0;
    /// Strikes dropped by the truncation rule, below and above K0.
    int truncated_low = 0;
    int truncated_high = 0;
};

/// Consecutive missing quotes that end the strip on one side.
///
/// CBOE stops after two consecutive zero BIDS. Working from mids, the
/// analogue is two consecutive absent quotes. Two rather than one because a
/// single illiquid strike in the middle of a live wing is a gap, not an end.
inline constexpr int kVixTruncateAfter = 2;

/// 30 days, in years. The constant the index interpolates to.
inline constexpr double kVixTargetYears = 30.0 / 365.0;

/// NSE rolls the near term out when it has 7 days or fewer to expiry.
///
/// Not cosmetic: inside a week the near-term variance is dominated by a
/// handful of strikes with almost no time value, the parity forward gets
/// noisy, and the interpolation weight on that term goes negative -- so the
/// index starts EXTRAPOLATING from a term it should have dropped.
[[nodiscard]] inline bool vix_should_roll(Years t_near) noexcept {
    return t_near.raw() <= 7.0 / 365.0;
}

namespace detail {

/// Half-width of the strike interval around index i. UNIT: paise.
///
/// Interior strikes get half the distance between their neighbours; the two
/// ends get the full distance to their single neighbour. Using a half-width at
/// the ends instead loses half the outermost contribution, which biases the
/// wings -- downward, and only in the tails, so it is invisible near the money
/// where anyone would look.
[[nodiscard]] inline double vix_delta_k(const VixQuote* q, std::size_t n,
                                        std::size_t i) noexcept {
    const double lo = static_cast<double>(q[i > 0 ? i - 1 : 0].strike.raw());
    const double hi = static_cast<double>(
        q[i + 1 < n ? i + 1 : n - 1].strike.raw());
    if (i == 0) { return hi - static_cast<double>(q[0].strike.raw()); }
    if (i + 1 == n) { return static_cast<double>(q[n - 1].strike.raw()) - lo; }
    return 0.5 * (hi - lo);
}

} // namespace detail

/// Variance of one expiry, by OTM strip replication.
///
/// `quotes` must be sorted by strike, strictly ascending. `t` in years,
/// `rate` continuously compounded.
[[nodiscard]] inline std::expected<VixTerm, VixError>
vix_term_variance(const VixQuote* quotes, std::size_t n, Years t,
                  double rate) noexcept {
    if (n < 3) { return std::unexpected(VixError::TooFewStrikes); }
    if (!(t.raw() > 0.0) || !std::isfinite(t.raw())
        || !std::isfinite(rate)) {
        return std::unexpected(VixError::NonPositiveTime);
    }
    for (std::size_t i = 1; i < n; ++i) {
        if (quotes[i].strike.raw() <= quotes[i - 1].strike.raw()) {
            return std::unexpected(VixError::UnsortedStrikes);
        }
    }

    const double T = t.raw();
    const double carry = std::exp(rate * T);

    // --- the forward, from put-call parity -----------------------------
    // At the strike where a call and a put are closest in price, parity gives
    // F = K + e^{rT}(C - P). Deriving it from the option market rather than
    // taking the index level matters: the index is a spot, and the strip
    // prices variance around the FORWARD. Using spot shifts K0 by the carry
    // and biases the correction term.
    std::size_t i_star = n;
    double best = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!quotes[i].has_call || !quotes[i].has_put) { continue; }
        const double d = std::fabs(static_cast<double>(quotes[i].call.raw())
                                 - static_cast<double>(quotes[i].put.raw()));
        if (i_star == n || d < best) { best = d; i_star = i; }
    }
    if (i_star == n) { return std::unexpected(VixError::NoParityStrike); }

    VixTerm out{};
    out.forward = static_cast<double>(quotes[i_star].strike.raw())
                + carry * (static_cast<double>(quotes[i_star].call.raw())
                         - static_cast<double>(quotes[i_star].put.raw()));
    if (!std::isfinite(out.forward) || out.forward <= 0.0) {
        return std::unexpected(VixError::NotFinite);
    }

    // K0: the first strike AT OR BELOW the forward.
    std::size_t i0 = n;
    for (std::size_t i = 0; i < n; ++i) {
        if (static_cast<double>(quotes[i].strike.raw()) <= out.forward) {
            i0 = i;
        }
    }
    if (i0 == n) { return std::unexpected(VixError::NoStrikeBelowForward); }
    out.k0 = static_cast<double>(quotes[i0].strike.raw());

    // --- the strip -----------------------------------------------------
    // Walk OUTWARD from K0 in both directions, so the truncation rule counts
    // consecutive gaps from the money outward, which is the direction
    // liquidity actually dies in. Walking the array start-to-end instead would
    // truncate on the far wing first and let a hole near the money through.
    double total = 0.0;
    int used = 0;

    auto contribution = [&](std::size_t i, double q_paise) {
        const double K = static_cast<double>(quotes[i].strike.raw());
        total += detail::vix_delta_k(quotes, n, i) / (K * K) * carry * q_paise;
        ++used;
    };

    // At K0 the strip takes the AVERAGE of the call and the put. Both are
    // near the money and neither is unambiguously out of it.
    if (quotes[i0].has_call && quotes[i0].has_put) {
        contribution(i0, 0.5 * (static_cast<double>(quotes[i0].call.raw())
                              + static_cast<double>(quotes[i0].put.raw())));
    } else if (quotes[i0].has_put) {
        contribution(i0, static_cast<double>(quotes[i0].put.raw()));
    } else if (quotes[i0].has_call) {
        contribution(i0, static_cast<double>(quotes[i0].call.raw()));
    }

    // Downward from K0: PUTS.
    int gap = 0;
    for (std::size_t j = i0; j-- > 0;) {
        if (!quotes[j].has_put) {
            ++gap;
            ++out.truncated_low;
            if (gap >= kVixTruncateAfter) { break; }
            continue;
        }
        gap = 0;
        contribution(j, static_cast<double>(quotes[j].put.raw()));
    }

    // Upward from K0: CALLS.
    gap = 0;
    for (std::size_t j = i0 + 1; j < n; ++j) {
        if (!quotes[j].has_call) {
            ++gap;
            ++out.truncated_high;
            if (gap >= kVixTruncateAfter) { break; }
            continue;
        }
        gap = 0;
        contribution(j, static_cast<double>(quotes[j].call.raw()));
    }

    if (used < 3) { return std::unexpected(VixError::TooFewStrikes); }
    out.strikes_used = used;

    // The correction for K0 not being exactly the forward. Always negative,
    // and small when the ladder is fine -- but it is the difference between
    // measuring variance around F and around K0, and dropping it biases the
    // index upward by the square of the ladder's own coarseness.
    const double moneyness = out.forward / out.k0 - 1.0;
    out.correction = -(1.0 / T) * moneyness * moneyness;
    out.variance = (2.0 / T) * total + out.correction;

    if (!std::isfinite(out.variance)) {
        return std::unexpected(VixError::NotFinite);
    }
    return out;
}

/// The published index: two terms interpolated to a constant 30 days.
///
/// `t_near` must be strictly less than `t_next`. The interpolation is in TOTAL
/// variance (T*sigma^2), not in vol and not in variance -- because total
/// variance is what is additive in time. Interpolating vol directly is the
/// standard error, and it understates the index whenever the term structure is
/// not flat.
///
/// When 30 days sits outside [t_near, t_next] this EXTRAPOLATES, and says so
/// by the weights going outside [0,1]. `vix_should_roll` exists so a caller
/// can avoid getting there.
[[nodiscard]] inline std::expected<double, VixError>
india_vix(const VixTerm& near_term, Years t_near,
          const VixTerm& next_term, Years t_next) noexcept {
    const double t1 = t_near.raw();
    const double t2 = t_next.raw();
    if (!(t1 > 0.0) || !(t2 > t1)) {
        return std::unexpected(VixError::NonPositiveTime);
    }
    const double w1 = (t2 - kVixTargetYears) / (t2 - t1);
    const double w2 = (kVixTargetYears - t1) / (t2 - t1);
    const double total = t1 * near_term.variance * w1
                       + t2 * next_term.variance * w2;
    if (!std::isfinite(total)) {
        return std::unexpected(VixError::NotFinite);
    }
    // A negative interpolated variance means the two terms are inconsistent --
    // calendar arbitrage in the raw chain, or a term that should have been
    // rolled. Refuse rather than return a NaN from the square root.
    if (total <= 0.0) { return std::unexpected(VixError::NotFinite); }
    return 100.0 * std::sqrt(total / kVixTargetYears);
}

/// The index from a single expiry, for when only one term is usable.
///
/// Reported as the vol of THAT expiry, not interpolated to 30 days: with one
/// term there is nothing to interpolate, and scaling a 45-day variance to 30
/// days pretends the term structure is flat. Callers must label it as what it
/// is.
[[nodiscard]] inline std::expected<double, VixError>
vix_single_term(const VixTerm& term) noexcept {
    if (!(term.variance > 0.0) || !std::isfinite(term.variance)) {
        return std::unexpected(VixError::NotFinite);
    }
    return 100.0 * std::sqrt(term.variance);
}

} // namespace altair
