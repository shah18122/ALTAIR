// features/options_calendar.hpp -- the options, cross-asset and calendar
// families.
//
// P5-04.
//
// KNOW WHAT ACTUALLY WRAPS BEFORE YOU ENCODE IT AS A CIRCLE.
//
// The standard advice for time features is "encode them as sin/cos, because
// time is cyclical". Half of that is right and applying it uniformly is a bug,
// so this file separates the two cases explicitly.
//
// The session clock DOES NOT WRAP. NSE trades 09:15 to 15:30 and then stops.
// 15:30 and the next 09:15 are seventeen and a half hours apart, and more to
// the point the open and the close are the two LEAST alike moments of the day:
// the open is overnight news being priced in, the close is square-off and
// settlement pressure. Put session progress on a circle and sin/cos map both
// to (0, 1) -- identical coordinates for the two most different states in the
// session. `tests/test_options_calendar.cpp` measures it: a circular encoding
// calls the open and the close a distance of 0.000 apart. That is a worse
// error than the linear encoding it was supposed to fix.
//
// So session progress here is LINEAR and monotone, which is what it is.
//
// What genuinely wraps is position in a REPEATING cycle: the trading week
// (Friday's close is followed by Monday's open, and the market treats them as
// contiguous), and the expiry cycle (a weekly option's Friday is the previous
// Thursday's tomorrow). Those get the sin/cos pair, and the test measures the
// linear encoding failing on exactly those -- calling the last day of one
// expiry cycle and the first of the next 96% of a cycle apart when they are
// consecutive sessions.
//
// THE TRADING CALENDAR IS NOT DERIVED HERE. Both cyclical positions are passed
// IN. Working out which day of the trading week a timestamp falls on needs the
// exchange holiday calendar, and a Diwali muhurat session or an unscheduled
// closure would silently shift every one of these features. That calendar
// lives in instruments/ (rule 1: no expiry as a literal, and no calendar
// reconstructed from a timestamp either).
//
// TIME COMES OFF THE TICK (rule 7). Nothing here calls a clock, so a replay of
// last Tuesday produces last Tuesday's calendar features.
//
// AN IV WITH A WIDE ERROR BAR IS NOT AN IV.
//
// On a 30-day NIFTY chain, one 5-paise tick resolves the 24000 implied vol to
// +- 0.000018 and the 28000 to +- 0.00045 -- 25x wider for the same tick. And
// that is the FLATTERING case, because the wing is quoted at 21 vol: price the
// same strike flat at the ATM's 14 and the band opens to +- 0.038, which is
// 2070x. The skew claws back a factor of 84 and the wing is still a far softer
// measurement than the money strike.
//
// So every options feature deriving from an implied vol carries the
// uncertainty alongside it, and a model is given the means to discount the
// wing rather than being handed a number that looks as solid as the ATM's.

#pragma once

#include <analytics/greeks.hpp>
#include <analytics/iv.hpp>
#include <analytics/svi.hpp>
#include <features/vector.hpp>

#include <cmath>
#include <cstdint>
#include <expected>

namespace altair {

inline constexpr double kTau = 6.28318530717958647692;   // 2*pi

enum class CalendarError : std::uint8_t {
    /// The session window is empty or inverted.
    BadSession,
    /// The timestamp is outside the session.
    OutOfSession,
    /// A cycle was described with a non-positive length.
    BadCycle
};

// ---------------------------------------------------------------------------
// Cyclical encoding -- the primitive, so the two cases can be told apart
// ---------------------------------------------------------------------------

/// A position within a repeating cycle, as a fraction in [0, 1).
///
/// `length` has no default and never will. A weekly expiry cycle and a monthly
/// one are different cycles, and a feature that guessed which would be a
/// different feature depending on the instrument (rule 1).
struct CyclePosition {
    /// How far into the cycle, in the same unit as `length`.
    double elapsed = 0.0;
    /// The full cycle. Must be positive.
    double length = 0.0;
};

/// sin/cos of a cycle position. Both, always: sin alone maps the quarter and
/// three-quarter points to the same value, which is the original bug wearing a
/// trigonometric hat.
struct CyclicPair {
    double sin = 0.0;
    double cos = 0.0;
};

[[nodiscard]] inline std::expected<CyclicPair, CalendarError>
cyclic_encode(const CyclePosition& c) noexcept {
    if (!(c.length > 0.0)) { return std::unexpected(CalendarError::BadCycle); }
    const double theta = kTau * (c.elapsed / c.length);
    return CyclicPair{std::sin(theta), std::cos(theta)};
}

/// Distance between two cycle fractions on the circle, in [0, 0.5].
///
/// Exposed so the two encodings can be MEASURED against each other rather than
/// argued about. On a circle 0.99 and 0.01 are 0.02 apart; as plain numbers
/// they are 0.98 apart.
[[nodiscard]] inline double cyclic_distance(double a, double b) noexcept {
    const double d = std::fabs(a - b);
    return d > 0.5 ? 1.0 - d : d;
}

/// Euclidean distance between two sin/cos encodings, in [0, 2].
///
/// This is what a model with a distance-based kernel actually sees, so it is
/// the thing worth measuring. It is a monotone function of `cyclic_distance`,
/// which is the point: the encoding preserves the circular metric.
[[nodiscard]] inline double pair_distance(const CyclicPair& a,
                                          const CyclicPair& b) noexcept {
    const double ds = a.sin - b.sin;
    const double dc = a.cos - b.cos;
    return std::sqrt(ds * ds + dc * dc);
}

// ---------------------------------------------------------------------------
// Calendar
// ---------------------------------------------------------------------------

struct CalendarSlots {
    /// Fraction of the session elapsed, in [0, 1]. LINEAR AND MONOTONE, and
    /// deliberately not on a circle -- see the header.
    FeatureIndex session_progress = kSkip;
    /// Seconds since the session opened, and until it closes. Raw, because
    /// "how long until the square-off" is a real quantity in its own right and
    /// does not scale with session length the way the fraction does.
    FeatureIndex since_open_s = kSkip;
    FeatureIndex until_close_s = kSkip;
    /// Position in the trading WEEK, as a sin/cos pair. This one wraps:
    /// Friday's close is followed by Monday's open.
    FeatureIndex week_sin = kSkip;
    FeatureIndex week_cos = kSkip;
    /// Position in the EXPIRY cycle, as a sin/cos pair. This one wraps too:
    /// the day after a weekly expiry is day one of the next.
    FeatureIndex expiry_cycle_sin = kSkip;
    FeatureIndex expiry_cycle_cos = kSkip;
};

/// One trading session, in tick time.
struct Session {
    Timestamp open{};
    Timestamp close{};
};

/// Everything about where this tick sits in the calendar, supplied by the
/// caller from the exchange calendar rather than reconstructed here.
struct CalendarPosition {
    Session session{};
    /// Which session of the trading week this is, and how many sessions the
    /// week holds. A holiday-shortened week is 4, not 5, and getting that from
    /// the calendar rather than from a modulus is the whole reason this is an
    /// input.
    CyclePosition week{};
    /// Which session of the expiry cycle this is, and how many the cycle
    /// holds. Zero length means "not supplied" and the pair stays absent.
    CyclePosition expiry_cycle{};
};

/// Write the calendar family from the TICK's timestamp.
[[nodiscard]] inline std::expected<int, CalendarError>
build_calendar(Timestamp now, const CalendarPosition& p,
               const CalendarSlots& sl, FeatureVector& out) noexcept {
    const std::int64_t span = (p.session.close - p.session.open).raw();
    if (span <= 0) { return std::unexpected(CalendarError::BadSession); }
    const std::int64_t since = (now - p.session.open).raw();
    if (since < 0 || since > span) {
        return std::unexpected(CalendarError::OutOfSession);
    }

    int written = 0;
    auto put = [&](FeatureIndex i, double v) {
        if (i != kSkip && out.set(i, v)) { ++written; }
    };

    put(sl.session_progress,
        static_cast<double>(since) / static_cast<double>(span));
    put(sl.since_open_s, static_cast<double>(since) / 1e9);
    put(sl.until_close_s, static_cast<double>(span - since) / 1e9);

    // A cycle the caller did not describe stays ABSENT. Defaulting a missing
    // week position to zero would place every such tick at Monday's open --
    // a specific, wrong, and entirely plausible-looking claim.
    if (const auto w = cyclic_encode(p.week)) {
        put(sl.week_sin, w->sin);
        put(sl.week_cos, w->cos);
    }
    if (const auto e = cyclic_encode(p.expiry_cycle)) {
        put(sl.expiry_cycle_sin, e->sin);
        put(sl.expiry_cycle_cos, e->cos);
    }
    return written;
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct OptionSlots {
    /// Implied vol, and the width of the band it is actually resolved to.
    /// The second is not optional decoration -- see the header.
    FeatureIndex iv = kSkip;
    FeatureIndex iv_uncertainty = kSkip;
    /// Log-moneyness against the forward. The x-axis of every smile, and the
    /// form that makes a 24000 strike on NIFTY comparable to a 2400 one on a
    /// stock.
    FeatureIndex log_moneyness = kSkip;
    /// Years to expiry, and its square root, which is the form vol scales in.
    FeatureIndex time_to_expiry = kSkip;
    FeatureIndex sqrt_time = kSkip;
    /// First-order greeks, per unit of the underlying.
    FeatureIndex delta = kSkip;
    FeatureIndex gamma = kSkip;
    FeatureIndex vega = kSkip;
    FeatureIndex theta = kSkip;
    /// Total implied variance from the fitted surface at this strike, and the
    /// slice's own butterfly margin. A model should be able to see that the
    /// surface it is reading is about to violate no-arbitrage.
    FeatureIndex total_variance = kSkip;
    FeatureIndex butterfly_margin = kSkip;
};

/// One option's inputs.
struct OptionPoint {
    OptionRight right = OptionRight::Call;
    Price forward{0};
    Price strike{0};
    Years t{0.0};
    /// Observed mid, in paise. Zero or less means no quote.
    Price mid{0};
    /// Half the bid-ask spread, in paise -- the price uncertainty P3-04 turns
    /// into an IV uncertainty.
    double price_uncertainty = 0.0;
    double rate = 0.0;
};

/// Write the options family.
///
/// Every number comes from Phase 3; nothing is recomputed here, because a
/// second copy of a formula is the copy that goes wrong. What this adds is
/// that Phase 3's REFUSALS -- expired, non-positive vol, a price outside the
/// no-arbitrage bounds -- are carried through as ABSENCE rather than being
/// replaced with a limit value.
[[nodiscard]] inline int
build_options(const OptionPoint& o, const OptionSlots& s,
              FeatureVector& out) noexcept {
    int written = 0;
    auto put = [&](FeatureIndex i, double v) {
        if (i != kSkip && out.set(i, v)) { ++written; }
    };

    if (o.forward.raw() > 0 && o.strike.raw() > 0) {
        put(s.log_moneyness,
            std::log(static_cast<double>(o.strike.raw())
                     / static_cast<double>(o.forward.raw())));
    }
    // T <= 0 fills NOTHING time-related and stops. On expiry day the greeks
    // are not large, they are undefined: gamma at the money is a delta
    // function, and P3-01 refuses rather than returning a big number that a
    // sizer would happily divide by.
    if (!(o.t.raw() > 0.0)) { return written; }
    put(s.time_to_expiry, o.t.raw());
    put(s.sqrt_time, std::sqrt(o.t.raw()));

    if (o.mid.raw() <= 0) { return written; }
    const auto iv = implied_vol_black76(o.right, o.mid, o.forward, o.strike,
                                        o.t, o.rate);
    if (!iv) { return written; }        // outside the no-arb bounds: no IV
    put(s.iv, iv->vol.raw());
    // The band the price uncertainty actually resolves. `iv_uncertainty`
    // returns +infinity where vega has collapsed, and `set` refuses
    // non-finite -- so the slot stays ABSENT exactly where the IV is a
    // number the price does not constrain.
    put(s.iv_uncertainty, iv_uncertainty(iv->vega, o.price_uncertainty));

    if (const auto g = black76(o.right, o.forward, o.strike, o.t, iv->vol,
                               o.rate)) {
        put(s.delta, g->delta);
        put(s.gamma, g->gamma);
        put(s.vega, g->vega);
        put(s.theta, g->theta);
    }
    return written;
}

/// Add the fitted-surface features for this strike.
///
/// Separate from `build_options` because the surface is a per-EXPIRY object
/// fitted once while the above is per-strike. Passing the slice in rather than
/// refitting per option is not an optimisation, it is correctness: two strikes
/// must be priced off the SAME slice or their greeks do not belong to one book.
[[nodiscard]] inline int
build_surface(const SviParams& slice, double log_moneyness,
              const OptionSlots& s, FeatureVector& out) noexcept {
    int written = 0;
    auto put = [&](FeatureIndex i, double v) {
        if (i != kSkip && out.set(i, v)) { ++written; }
    };
    put(s.total_variance, svi_total_variance(slice, log_moneyness));
    // Gatheral's g. Negative means the surface implies negative probability at
    // this strike -- P3-05a confirmed g < 0 IS a negative butterfly price on
    // 42 of 42 strike-slice pairs.
    put(s.butterfly_margin, svi_g(slice, log_moneyness));
    return written;
}

// ---------------------------------------------------------------------------
// Cross-asset
// ---------------------------------------------------------------------------

struct CrossAssetSlots {
    /// Correlation of this instrument's returns with a reference series.
    FeatureIndex correlation = kSkip;
    /// Beta to the reference.
    FeatureIndex beta = kSkip;
    /// Mean return relative to the reference over the same window.
    FeatureIndex relative_return = kSkip;
    /// How many paired observations the estimates rest on. A beta from six
    /// ticks is not a beta, and the count is what lets a model say so.
    FeatureIndex pair_count = kSkip;
};

/// Correlation and beta against a reference series.
///
/// BOTH SERIES MUST BE SAMPLED AT THE SAME INSTANTS, and the caller pairs
/// them. This does not interpolate, because interpolating one series onto
/// another's timestamps invents observations, and the correlation of invented
/// data is a correlation with the interpolator -- which is high, stable, and
/// entirely fictional.
///
/// Takes RETURNS, not prices. Two random walks are correlated at 0.9 in levels
/// and 0.0 in increments, and the first number is the one that gets a strategy
/// funded.
[[nodiscard]] inline int
build_cross_asset(const double* asset_ret, const double* ref_ret,
                  std::size_t n, const CrossAssetSlots& s,
                  FeatureVector& out) noexcept {
    int written = 0;
    auto put = [&](FeatureIndex i, double v) {
        if (i != kSkip && out.set(i, v)) { ++written; }
    };
    put(s.pair_count, static_cast<double>(n));
    if (n < 3) { return written; }

    double ma = 0.0, mr = 0.0;
    for (std::size_t i = 0; i < n; ++i) { ma += asset_ret[i]; mr += ref_ret[i]; }
    ma /= static_cast<double>(n);
    mr /= static_cast<double>(n);

    double saa = 0.0, srr = 0.0, sar = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double da = asset_ret[i] - ma;
        const double dr = ref_ret[i] - mr;
        saa += da * da;
        srr += dr * dr;
        sar += da * dr;
    }
    // A reference that did not move has no beta and no correlation. Left
    // ABSENT rather than reported as zero: a beta of zero is the claim "moves
    // independently of the index", and this is the absence of any claim.
    if (srr > 0.0) {
        put(s.beta, sar / srr);
        if (saa > 0.0) { put(s.correlation, sar / std::sqrt(saa * srr)); }
    }
    put(s.relative_return, ma - mr);
    return written;
}

} // namespace altair
