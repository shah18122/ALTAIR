// analytics/har_rv.hpp -- realised variance, and the HAR model that forecasts it.
//
// WHAT A VOLATILITY DESK FORECASTS. Not direction: how much the underlying
// will move between now and an option's expiry. The forecast is set against
// the option's implied volatility, and the gap is the trade (the variance
// risk premium: Carr & Wu 2009; research/papers/index.md).
//
// REALISED VARIANCE of a session is the sum of its squared intraday returns
// plus the squared overnight gap -- a close-to-close day measured from the
// inside (Andersen & Bollerslev 1998). It is per day, not annualised.
//
// HAR (Corsi 2009, "A Simple Approximate Long-Memory Model of Realized
// Volatility"): the variance over the next h days is a regression on the
// last day's, the last week's and the last month's average variance. Three
// horizons of traders, three regressors; it beats GARCH on realised-variance
// targets in most of the literature and is a few lines of least squares.
//
// FITTED IN LOGS. log(target) on log(day), log(week), log(month): variance is
// right-skewed and a level regression can forecast a negative one. The
// forecast is exp(fit + s2 / 2), s2 the residual variance -- the lognormal
// mean, so the forecast is of the variance, not of its median.
//
// NO LOOK-AHEAD, BY CONSTRUCTION. A forecast made at the close of day t uses
// only rows i whose target days i+1 .. i+h are all <= t: the target must be
// fully known on the day the model is fitted. The test checks it.

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace altair {

inline constexpr std::size_t kHarWeek = 5;
inline constexpr std::size_t kHarMonth = 22;

enum class HarError : std::uint8_t { TooLittleHistory, BadInput, Singular };

[[nodiscard]] inline const char* har_error_text(HarError e) noexcept {
    switch (e) {
    case HarError::TooLittleHistory: return "not enough days before the forecast to fit the regression";
    case HarError::BadInput: return "realised variance must be positive and finite, and the horizon positive";
    case HarError::Singular: return "the regressors are collinear";
    }
    return "?";
}

struct HarForecast {
    double variance = 0.0;   ///< mean daily variance over the next h days
    double vol = 0.0;        ///< sqrt(variance x 252): annualised
    std::array<double, 4> beta{};   ///< intercept, day, week, month (log space)
    double resid_var = 0.0;
    std::size_t rows = 0;    ///< regression rows used
};

namespace har_detail {

[[nodiscard]] inline double mean_log(std::span<const double> rv, std::size_t end_inclusive, std::size_t n) noexcept {
    double s = 0.0;
    for (std::size_t k = end_inclusive + 1 - n; k <= end_inclusive; ++k) { s += rv[k]; }
    return std::log(s / static_cast<double>(n));
}

/// Solve the 4x4 system a x = b by Gaussian elimination with partial pivoting.
[[nodiscard]] inline bool solve4(std::array<std::array<double, 4>, 4> a, std::array<double, 4> b,
                                 std::array<double, 4>& x) noexcept {
    for (std::size_t c = 0; c < 4; ++c) {
        std::size_t piv = c;
        for (std::size_t r = c + 1; r < 4; ++r) {
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) { piv = r; }
        }
        if (!(std::fabs(a[piv][c]) > 1e-12)) { return false; }
        std::swap(a[c], a[piv]);
        std::swap(b[c], b[piv]);
        for (std::size_t r = c + 1; r < 4; ++r) {
            const double f = a[r][c] / a[c][c];
            for (std::size_t k = c; k < 4; ++k) { a[r][k] -= f * a[c][k]; }
            b[r] -= f * b[c];
        }
    }
    for (std::size_t c = 4; c-- > 0;) {
        double s = b[c];
        for (std::size_t k = c + 1; k < 4; ++k) { s -= a[c][k] * x[k]; }
        x[c] = s / a[c][c];
    }
    return true;
}

} // namespace har_detail

/// Forecast at the close of day `t` the mean daily realised variance over
/// days t+1 .. t+h, fitted on at most `window` rows of history before it.
[[nodiscard]] inline std::expected<HarForecast, HarError>
har_forecast(std::span<const double> rv, std::size_t t, std::size_t h, std::size_t window = 1000) {
    if (h == 0 || t >= rv.size()) { return std::unexpected(HarError::BadInput); }
    for (std::size_t k = 0; k <= t; ++k) {
        if (!(rv[k] > 0.0) || !std::isfinite(rv[k])) { return std::unexpected(HarError::BadInput); }
    }
    // Row i: regressors end at day i, target is days i+1 .. i+h; usable while i + h <= t.
    if (t < kHarMonth + h) { return std::unexpected(HarError::TooLittleHistory); }
    const std::size_t last = t - h;
    const std::size_t first = last + 1 > window + (kHarMonth - 1) ? last + 1 - window : kHarMonth - 1;
    if (last < first || last - first + 1 < 30) { return std::unexpected(HarError::TooLittleHistory); }

    std::array<std::array<double, 4>, 4> xtx{};
    std::array<double, 4> xty{};
    std::vector<std::array<double, 5>> rows;
    rows.reserve(last - first + 1);
    for (std::size_t i = first; i <= last; ++i) {
        double y = 0.0;
        for (std::size_t k = i + 1; k <= i + h; ++k) { y += rv[k]; }
        y = std::log(y / static_cast<double>(h));
        const std::array<double, 4> x{1.0, std::log(rv[i]), har_detail::mean_log(rv, i, kHarWeek),
                                      har_detail::mean_log(rv, i, kHarMonth)};
        for (std::size_t a = 0; a < 4; ++a) {
            for (std::size_t b = 0; b < 4; ++b) { xtx[a][b] += x[a] * x[b]; }
            xty[a] += x[a] * y;
        }
        rows.push_back({x[0], x[1], x[2], x[3], y});
    }
    HarForecast f;
    if (!har_detail::solve4(xtx, xty, f.beta)) { return std::unexpected(HarError::Singular); }
    double ss = 0.0;
    for (const auto& r : rows) {
        const double e = r[4] - (f.beta[0] * r[0] + f.beta[1] * r[1] + f.beta[2] * r[2] + f.beta[3] * r[3]);
        ss += e * e;
    }
    f.rows = rows.size();
    f.resid_var = ss / static_cast<double>(rows.size() - 4);
    const double fit = f.beta[0] + f.beta[1] * std::log(rv[t]) + f.beta[2] * har_detail::mean_log(rv, t, kHarWeek)
                     + f.beta[3] * har_detail::mean_log(rv, t, kHarMonth);
    f.variance = std::exp(fit + 0.5 * f.resid_var);
    f.vol = std::sqrt(f.variance * 252.0);
    return f;
}

/// One session's realised variance from its bars' closes, the first bar's
/// open, and the previous session's last close (0 for none: no gap term).
[[nodiscard]] inline double session_rv(std::span<const double> closes, double first_open,
                                       double prev_close) noexcept {
    double s = 0.0;
    double last = first_open;
    for (const double c : closes) {
        if (last > 0.0 && c > 0.0) {
            const double r = std::log(c / last);
            s += r * r;
        }
        last = c;
    }
    if (prev_close > 0.0 && first_open > 0.0) {
        const double g = std::log(first_open / prev_close);
        s += g * g;
    }
    return s;
}

} // namespace altair
