// analytics/svi.hpp -- the raw SVI volatility slice, and the two arbitrage
// conditions it has to satisfy before anything is allowed to quote off it.
//
// P3-05a. Evaluation and validation only; calibration is P3-05b.
//
// TWO UNITS THAT ARE CONSTANTLY CONFUSED, so they are named everywhere:
//
//   * w is TOTAL IMPLIED VARIANCE, sigma^2 * T. Not variance, not vol. SVI is
//     parameterised in w because that is the quantity in which the no-calendar
//     condition is simply "increasing in T"; in vol it is not.
//   * k is LOG-MONEYNESS ln(K/F) against the FORWARD, matching black76 in
//     analytics/greeks.hpp. Not ln(K/S). Using spot here shifts the whole
//     smile by the carry -- 0.44% on a 30-day NIFTY -- which reads as a skew
//     that is not there.
//
// The raw parameterisation (Gatheral):
//
//     w(k) = a + b * ( rho*(k-m) + sqrt((k-m)^2 + sigma^2) )
//
//   a      vertical level    (raw variance)
//   b      overall slope     (b >= 0)
//   rho    skew, in [-1,1]   (equity indices sit near -0.7: puts bid)
//   m      horizontal shift  (where the smile bottoms out)
//   sigma  curvature at the bottom (sigma > 0; sigma -> 0 gives a kink)
//
// WHY THE ARBITRAGE CHECKS ARE NOT OPTIONAL.
//
// A fitted surface is an interpolator, and an interpolator that is a little
// too curved implies a NEGATIVE PROBABILITY DENSITY. That is not an abstract
// defect: the butterfly spread C(K-dK) - 2C(K) + C(K+dK) prices negative, and
// a strategy scanning for mispricings will find it, size into it, and trade
// against a number the surface invented. The engine's own fit is then its
// counterparty. `tests/test_svi.cpp` prices real butterflies off both a clean
// and a violating slice and shows exactly that.
//
// Two conditions, and they are different failures:
//
//   * BUTTERFLY (static, one expiry). Gatheral-Jacquier: the density is
//     non-negative iff g(k) >= 0 everywhere, where
//
//         g = (1 - k*w'/(2w))^2 - (w'^2/4)*(1/w + 1/4) + w''/2
//
//   * CALENDAR (across expiries). Total variance must not decrease in T at
//     any fixed k. If it does, a calendar spread is free money -- buy the
//     cheap long-dated option, sell the dear short-dated one at the same
//     strike, and the surface says you are paid to hold optionality.
//
// BOTH ARE CHECKED ON A GRID, AND THE GRID IS AN ARGUMENT. A surface verified
// on [-0.2, 0.2] says nothing about k = -0.6, and the wings are where a fit is
// least constrained by data and most likely to be extrapolating. Callers pass
// the range they will actually quote over, and the scan reports WHERE the
// worst point was, not just whether it passed.

#pragma once

#include <core/types/units.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class SviError : std::uint8_t {
    /// b < 0. Negative slope inverts the smile.
    NegativeB,
    /// |rho| >= 1. At exactly 1 the slice has a corner and w' is
    /// discontinuous; beyond it the square root term is dominated and w goes
    /// negative in one wing.
    RhoOutOfRange,
    /// sigma <= 0. At zero the smile has a kink at k = m and w'' is a delta.
    NonPositiveSigma,
    /// a + b*sigma*sqrt(1-rho^2) < 0, i.e. the minimum of w is negative.
    /// A negative total variance is not a small error to be clamped: it has
    /// no square root, so the slice has no implied vol at all near its
    /// bottom -- which is the money strike.
    NegativeMinVariance,
    /// A non-finite parameter or evaluation point.
    NotFinite,
    /// A scan was asked for over an empty or inverted k range, or with fewer
    /// than two points.
    BadRange
};

/// Raw SVI parameters for ONE expiry. Fitting these is P3-05b.
struct SviParams {
    double a = 0.0;
    double b = 0.0;
    double rho = 0.0;
    double m = 0.0;
    double sigma = 0.0;
};

/// Total variance and its first two derivatives at one k.
///
/// Returned together because all three share the same `y` and `r`, and
/// because every arbitrage condition needs all three. Computing them in
/// separate calls triples the work and invites one of them being evaluated at
/// a different k than the others.
struct SviLocal {
    /// w(k), total implied variance. UNIT: dimensionless (vol^2 * years).
    double w = 0.0;
    /// dw/dk. UNIT: dimensionless.
    double w_prime = 0.0;
    /// d2w/dk2. UNIT: dimensionless.
    double w_double_prime = 0.0;
};

/// Domain validation. Call before anything else uses a parameter set.
///
/// These are the conditions under which w(k) is positive and smooth for every
/// real k. They are cheap, they are necessary, and they are NOT sufficient --
/// a slice can satisfy all of them and still admit butterfly arbitrage, which
/// is what `svi_butterfly_scan` is for.
[[nodiscard]] inline std::expected<void, SviError>
svi_check_params(const SviParams& p) noexcept {
    if (!std::isfinite(p.a) || !std::isfinite(p.b) || !std::isfinite(p.rho)
        || !std::isfinite(p.m) || !std::isfinite(p.sigma)) {
        return std::unexpected(SviError::NotFinite);
    }
    if (p.b < 0.0) { return std::unexpected(SviError::NegativeB); }
    if (!(p.rho > -1.0 && p.rho < 1.0)) {
        return std::unexpected(SviError::RhoOutOfRange);
    }
    if (!(p.sigma > 0.0)) {
        return std::unexpected(SviError::NonPositiveSigma);
    }
    if (p.a + p.b * p.sigma * std::sqrt(1.0 - p.rho * p.rho) < 0.0) {
        return std::unexpected(SviError::NegativeMinVariance);
    }
    return {};
}

/// The minimum of w over all k, attained at k = m - rho*sigma/sqrt(1-rho^2).
///
/// Equal to `a + b*sigma*sqrt(1-rho^2)`, which is why that expression is the
/// domain condition rather than an arbitrary-looking inequality.
[[nodiscard]] inline double svi_min_total_variance(const SviParams& p) noexcept {
    return p.a + p.b * p.sigma * std::sqrt(1.0 - p.rho * p.rho);
}

/// The k at which w is smallest -- the bottom of the smile.
[[nodiscard]] inline double svi_argmin_k(const SviParams& p) noexcept {
    return p.m - p.rho * p.sigma / std::sqrt(1.0 - p.rho * p.rho);
}

/// Evaluate w, w' and w'' at one log-moneyness.
///
/// PRECONDITION: `svi_check_params` has passed. Unchecked here because this
/// is called once per strike per surface update and the parameters do not
/// change between those calls.
[[nodiscard]] ALTAIR_HOT inline SviLocal
svi_local(const SviParams& p, double k) noexcept {
    const double y = k - p.m;
    const double r = std::sqrt(y * y + p.sigma * p.sigma);
    SviLocal out{};
    out.w = p.a + p.b * (p.rho * y + r);
    out.w_prime = p.b * (p.rho + y / r);
    // d/dy (y/r) = (r^2 - y^2)/r^3 = sigma^2/r^3. Always positive, so w is
    // strictly convex for sigma > 0 -- which is why sigma = 0 is refused
    // rather than treated as a limit.
    out.w_double_prime = p.b * p.sigma * p.sigma / (r * r * r);
    return out;
}

/// Total implied variance at one log-moneyness. UNIT: vol^2 * years.
[[nodiscard]] ALTAIR_HOT inline double
svi_total_variance(const SviParams& p, double k) noexcept {
    const double y = k - p.m;
    return p.a + p.b * (p.rho * y + std::sqrt(y * y + p.sigma * p.sigma));
}

/// Black-76 implied volatility at one log-moneyness. UNIT: annualised.
///
/// The conversion w -> sigma is where the T goes, and it is the single place
/// the two units meet. `t` in years, and it must be the SAME expiry the slice
/// was fitted to; a slice does not carry its own T, because a surface is a
/// collection of slices indexed by it.
[[nodiscard]] inline std::expected<Vol, SviError>
svi_implied_vol(const SviParams& p, double k, Years t) noexcept {
    if (!std::isfinite(k) || !(t.raw() > 0.0)) {
        return std::unexpected(SviError::NotFinite);
    }
    const double w = svi_total_variance(p, k);
    if (!(w > 0.0) || !std::isfinite(w)) {
        return std::unexpected(SviError::NegativeMinVariance);
    }
    return Vol{std::sqrt(w / t.raw())};
}

/// Gatheral-Jacquier g(k). The risk-neutral density is proportional to it,
/// so g >= 0 everywhere is exactly "no butterfly arbitrage".
///
/// PRECONDITION: w(k) > 0, which `svi_check_params` guarantees.
[[nodiscard]] inline double svi_g(const SviParams& p, double k) noexcept {
    const SviLocal L = svi_local(p, k);
    const double t1 = 1.0 - k * L.w_prime / (2.0 * L.w);
    return t1 * t1
         - (L.w_prime * L.w_prime / 4.0) * (1.0 / L.w + 0.25)
         + L.w_double_prime / 2.0;
}

/// The risk-neutral density of log-moneyness implied by the slice.
///
/// p(k) = g(k) / sqrt(2*pi*w) * exp(-d2^2 / 2),  d2 = -k/sqrt(w) - sqrt(w)/2.
///
/// Exposed because "the density is negative" is a more legible statement of a
/// butterfly violation than "g is negative", and because integrating it over
/// the quoting range is a useful independent sanity check on a fit.
[[nodiscard]] inline double svi_density(const SviParams& p, double k) noexcept {
    constexpr double kTwoPi = 6.28318530717958647692528676655901;
    const double w = svi_total_variance(p, k);
    if (!(w > 0.0)) { return 0.0; }
    const double sw = std::sqrt(w);
    const double d2 = -k / sw - 0.5 * sw;
    return svi_g(p, k) / std::sqrt(kTwoPi * w) * std::exp(-0.5 * d2 * d2);
}

/// Result of scanning a condition across a range of log-moneyness.
///
/// Carries WHERE the worst point was, not just whether it passed. A fit that
/// fails at k = -0.55 and one that fails at k = 0.01 are different problems:
/// the first is an over-extrapolated wing, the second is a broken fit.
struct SviScan {
    /// True when the condition held at every sampled point.
    bool clean = false;
    /// The most negative value found -- min g for a butterfly scan, min
    /// (w_long - w_short) for a calendar scan. Positive when clean.
    double worst = 0.0;
    /// The k at which `worst` occurred.
    double worst_k = 0.0;
    /// Points sampled.
    int points = 0;
};

/// Scan g(k) for butterfly arbitrage over [k_lo, k_hi].
///
/// PASS THE RANGE YOU WILL QUOTE OVER. A clean scan on [-0.2, 0.2] is not
/// evidence about k = -0.6, and the wings are simultaneously where the fit has
/// least data and where a scanning strategy looks hardest for mispricings.
[[nodiscard]] inline std::expected<SviScan, SviError>
svi_butterfly_scan(const SviParams& p, double k_lo, double k_hi,
                   int points = 401) noexcept {
    if (const auto ok = svi_check_params(p); !ok) {
        return std::unexpected(ok.error());
    }
    if (!(k_hi > k_lo) || points < 2 || !std::isfinite(k_lo)
        || !std::isfinite(k_hi)) {
        return std::unexpected(SviError::BadRange);
    }
    SviScan out{};
    out.points = points;
    out.worst = svi_g(p, k_lo);
    out.worst_k = k_lo;
    const double step = (k_hi - k_lo) / static_cast<double>(points - 1);
    for (int i = 1; i < points; ++i) {
        const double k = k_lo + step * static_cast<double>(i);
        const double g = svi_g(p, k);
        if (g < out.worst) { out.worst = g; out.worst_k = k; }
    }
    out.clean = out.worst >= 0.0;
    return out;
}

/// Scan two slices for calendar arbitrage.
///
/// `shorter` and `longer` are slices of the SAME underlying at two expiries,
/// with `longer` the later one. Total variance must not decrease in T at any
/// fixed k -- so `w_longer(k) - w_shorter(k) >= 0` everywhere.
///
/// Note the direction: this is a condition on TOTAL variance, not on vol.
/// Implied VOL routinely falls with maturity in a stressed market and that is
/// not arbitrage; total variance falling is.
[[nodiscard]] inline std::expected<SviScan, SviError>
svi_calendar_scan(const SviParams& shorter, const SviParams& longer,
                  double k_lo, double k_hi, int points = 401) noexcept {
    if (const auto ok = svi_check_params(shorter); !ok) {
        return std::unexpected(ok.error());
    }
    if (const auto ok = svi_check_params(longer); !ok) {
        return std::unexpected(ok.error());
    }
    if (!(k_hi > k_lo) || points < 2 || !std::isfinite(k_lo)
        || !std::isfinite(k_hi)) {
        return std::unexpected(SviError::BadRange);
    }
    SviScan out{};
    out.points = points;
    out.worst = svi_total_variance(longer, k_lo)
              - svi_total_variance(shorter, k_lo);
    out.worst_k = k_lo;
    const double step = (k_hi - k_lo) / static_cast<double>(points - 1);
    for (int i = 1; i < points; ++i) {
        const double k = k_lo + step * static_cast<double>(i);
        const double d = svi_total_variance(longer, k)
                       - svi_total_variance(shorter, k);
        if (d < out.worst) { out.worst = d; out.worst_k = k; }
    }
    out.clean = out.worst >= 0.0;
    return out;
}

/// The asymptotic slopes of w: b*(1-rho) as k -> -inf, b*(1+rho) as k -> +inf.
///
/// Lee's moment formula caps both at 2: a total-variance slope above 2 implies
/// the underlying has no finite moment of the corresponding order, which no
/// traded asset does. A necessary condition, and much cheaper than the
/// butterfly scan -- worth checking first, because a slice that fails it
/// cannot be rescued by narrowing the scan range.
[[nodiscard]] inline bool svi_slopes_within_lee(const SviParams& p) noexcept {
    return p.b * (1.0 + std::fabs(p.rho)) <= 2.0;
}

} // namespace altair
