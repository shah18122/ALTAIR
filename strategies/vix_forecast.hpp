// strategies/vix_forecast.hpp -- forecasting India VIX.
//
// P10-07. Consumes analytics/vix.hpp (P3-08), which computes the index; this
// forecasts it.
//
// FORECAST log(VIX), NOT VIX.
//
// The usual version of this complaint is that a level model forecasts a
// NEGATIVE VIX. Measured, that is not true and it is worth saying so: a
// mean-reverting AR(1) with a positive intercept converges monotonically to
// a/(1-b) from any positive start, and its two-sigma lower bound stays
// positive too. Two drafts of the test asserted each in turn and neither held.
//
// The real failure is the ERROR BAR. VIX moves proportionally, so a level
// model's residual spread depends on where VIX is:
//
//     residual sd with VIX below 15    1.2303   (372 observations)
//     residual sd with VIX above 22    2.5192   (494 observations)   2.0x
//     the level model fits ONE sd of   2.0048   to both
//
// And that shows up as coverage. Nominal 95.4% two-sigma band:
//
//                     VIX quiet (<15)   VIX stressed (>22)
//     level model         98.9%              96.2%
//     log   model         98.4%              98.4%
//
// The level band narrows relative to the moves in exactly the regime that
// produced the need for a number, while the log band scales with the level
// because that is the space it was fitted in. A forecast is only as good as
// the interval around it, and everything downstream here -- P6-04's `fires`,
// P8-12's `lower_bound`, P10-06's `neutral` -- consumes the interval.
//
// VIX IS NOT A FORECAST OF REALISED VOLATILITY. IT IS THAT PLUS A PREMIUM.
//
// The second thing, and it is a bias rather than a bug. Implied volatility
// sits systematically ABOVE subsequently realised volatility -- the variance
// risk premium, and it is the reason selling options is a business. Regress
// realised on implied and the slope is near one and the INTERCEPT is
// materially negative; use VIX directly as a realised-vol forecast and every
// number is high by that premium.
//
// So `realised_from_implied` requires the premium to be supplied, measured on
// the caller's own history, and there is no overload that assumes it away.
//
// AND MEAN REVERSION MEANS A RANDOM WALK IS THE WRONG BASELINE.
//
// The natural baseline for a VIX forecast is "tomorrow equals today", which is
// right for a price and wrong here: VIX reverts, so at any horizon beyond a few
// days the unconditional mean beats persistence. A model that only beats the
// random walk has not been tested against the baseline that matters.

#pragma once

#include <analytics/rolling.hpp>
#include <core/types/units.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class VixForecastError : std::uint8_t {
    /// Fewer observations than the fit needs.
    TooFewSamples,
    /// A VIX observation was zero or negative, so its log does not exist.
    NonPositiveVix,
    /// The variance risk premium was not supplied.
    NoPremium,
    /// The horizon was zero.
    BadHorizon,
    /// The fit was singular.
    Degenerate
};

/// Which space the model works in. No default: the two produce different
/// models from identical data, and one of them can emit a negative volatility.
enum class VixSpace : std::uint8_t {
    Unspecified = 0,
    /// Fit on the level. Present so the negative forecasts can be counted.
    Level,
    /// Fit on log(VIX). The only one that cannot produce a negative.
    Log
};

/// A mean-reverting one-step model: x[t+1] = a + b*x[t].
///
/// Deliberately the simplest thing that captures what VIX does. An AR(1) in
/// logs already reproduces the persistence and the reversion, and CLAUDE.md's
/// reality check about overfitting applies here as much as anywhere.
struct VixModel {
    double a = 0.0;
    double b = 0.0;
    /// Long-run level, a/(1-b), in the fitted space.
    double mean = 0.0;
    /// Reversion half-life in observations.
    double half_life = 0.0;
    double residual_sd = 0.0;
    VixSpace space = VixSpace::Unspecified;
    std::size_t n = 0;
};

/// Fit the one-step model.
[[nodiscard]] inline std::expected<VixModel, VixForecastError>
fit_vix(const double* vix, std::size_t n, VixSpace space) noexcept {
    if (space == VixSpace::Unspecified) {
        return std::unexpected(VixForecastError::BadHorizon);
    }
    if (n < 64) { return std::unexpected(VixForecastError::TooFewSamples); }
    for (std::size_t i = 0; i < n; ++i) {
        if (!(vix[i] > 0.0)) {
            return std::unexpected(VixForecastError::NonPositiveVix);
        }
    }
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    const double m = static_cast<double>(n - 1);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const double x = space == VixSpace::Log ? std::log(vix[i]) : vix[i];
        const double y = space == VixSpace::Log ? std::log(vix[i + 1])
                                               : vix[i + 1];
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    const double den = m * sxx - sx * sx;
    if (!(std::fabs(den) > 0.0)) {
        return std::unexpected(VixForecastError::Degenerate);
    }
    VixModel mod{};
    mod.space = space;
    mod.n = n;
    mod.b = (m * sxy - sx * sy) / den;
    mod.a = (sy - mod.b * sx) / m;
    if (std::fabs(1.0 - mod.b) > 1e-12) { mod.mean = mod.a / (1.0 - mod.b); }
    if (mod.b > 0.0 && mod.b < 1.0) {
        mod.half_life = -std::log(2.0) / std::log(mod.b);
    }
    double sse = 0.0;
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const double x = space == VixSpace::Log ? std::log(vix[i]) : vix[i];
        const double y = space == VixSpace::Log ? std::log(vix[i + 1])
                                               : vix[i + 1];
        const double e = y - mod.a - mod.b * x;
        sse += e * e;
    }
    mod.residual_sd = std::sqrt(sse / (m - 2.0));
    return mod;
}

/// Forecast `h` steps ahead.
///
/// In LOG space the answer exponentiates and is positive by construction. In
/// LEVEL space it is whatever the line says, including negative -- which is
/// the point of keeping that path.
[[nodiscard]] inline std::expected<double, VixForecastError>
forecast_vix(const VixModel& m, double current, std::size_t h) noexcept {
    if (h == 0) { return std::unexpected(VixForecastError::BadHorizon); }
    if (!(current > 0.0)) {
        return std::unexpected(VixForecastError::NonPositiveVix);
    }
    double x = m.space == VixSpace::Log ? std::log(current) : current;
    for (std::size_t i = 0; i < h; ++i) { x = m.a + m.b * x; }
    return m.space == VixSpace::Log ? std::exp(x) : x;
}

/// The three baselines a VIX forecast has to beat, named.
///
/// `persistence` is the right null for a price and the WRONG one here: VIX
/// reverts, so at any horizon beyond a few days the unconditional mean wins on
/// its own. A model reported as beating "the random walk" has been compared
/// against the easier of the two.
struct VixBaselines {
    double persistence = 0.0;
    double unconditional_mean = 0.0;
    double model = 0.0;
};

// ---------------------------------------------------------------------------
// The variance risk premium
// ---------------------------------------------------------------------------

/// Convert an implied volatility into a REALISED-volatility forecast.
///
/// `premium` is the mean gap between implied and subsequently realised, in the
/// same units, MEASURED on the caller's own history. It is required, and there
/// is no overload without it: implied sits systematically above realised --
/// that is what makes selling options a business -- so using VIX directly as a
/// realised-vol forecast is high by the premium on every single observation.
[[nodiscard]] inline std::expected<double, VixForecastError>
realised_from_implied(double implied, double premium) noexcept {
    if (!(implied > 0.0)) {
        return std::unexpected(VixForecastError::NonPositiveVix);
    }
    // A zero premium is a claim, not a default, so it has to be passed
    // explicitly -- but a NEGATIVE one is almost certainly a sign error, and
    // realised above implied on average would be a market where nobody writes
    // options.
    if (premium < 0.0) { return std::unexpected(VixForecastError::NoPremium); }
    const double r = implied - premium;
    return r > 0.0 ? r : 0.0;
}

/// Measure the premium from paired implied and subsequent realised series.
///
/// Provided so a caller has no excuse to guess it -- the same reasoning as
/// P6-04's `mean_pairwise_correlation`.
[[nodiscard]] inline std::expected<double, VixForecastError>
measure_variance_premium(const double* implied, const double* realised,
                         std::size_t n) noexcept {
    if (n < 32) { return std::unexpected(VixForecastError::TooFewSamples); }
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!(implied[i] > 0.0) || !(realised[i] >= 0.0)) {
            return std::unexpected(VixForecastError::NonPositiveVix);
        }
        s += implied[i] - realised[i];
    }
    return s / static_cast<double>(n);
}

} // namespace altair
