// models/time_series.hpp -- bounded classical time-series reference models.
// M04/M05/M06/M08. Offline fitting may allocate; forecast and OU stepping do not.
#pragma once

#include <models/classical.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <vector>

namespace altair {

enum class TimeSeriesError : std::uint8_t {
    TooFewSamples,
    BadParameter,
    NonFinite,
    Singular,
    NonStationary,
    NonInvertible
};

struct LagModel {
    double intercept = 0.0;
    std::vector<std::size_t> ar_lags;
    std::vector<std::size_t> ma_lags;
    std::vector<double> ar;
    std::vector<double> ma;
    std::vector<double> residuals;
    double residual_variance = 0.0;
    bool stationary = false;
    bool invertible = false;

    [[nodiscard]] std::expected<double, TimeSeriesError>
    forecast(std::span<const double> history) const noexcept {
        std::size_t need = 0;
        for (const auto lag : ar_lags) need = std::max(need, lag);
        if (history.size() < need || residuals.empty())
            return std::unexpected(TimeSeriesError::TooFewSamples);
        double out = intercept;
        for (std::size_t i = 0; i < ar.size(); ++i)
            out += ar[i] * history[history.size() - ar_lags[i]];
        for (std::size_t i = 0; i < ma.size(); ++i) {
            const auto lag = ma_lags[i];
            if (residuals.size() < lag)
                return std::unexpected(TimeSeriesError::TooFewSamples);
            out += ma[i] * residuals[residuals.size() - lag];
        }
        return std::isfinite(out)
            ? std::expected<double, TimeSeriesError>{out}
            : std::unexpected(TimeSeriesError::NonFinite);
    }
};

namespace time_series_detail {

[[nodiscard]] inline bool all_finite(std::span<const double> values) noexcept {
    for (const double v : values) if (!std::isfinite(v)) return false;
    return true;
}

[[nodiscard]] inline std::expected<std::vector<double>, TimeSeriesError>
difference(std::span<const double> values, std::size_t lag) {
    if (lag == 0 || values.size() <= lag)
        return std::unexpected(TimeSeriesError::TooFewSamples);
    std::vector<double> out(values.size() - lag);
    for (std::size_t i = lag; i < values.size(); ++i)
        out[i - lag] = values[i] - values[i - lag];
    return out;
}

[[nodiscard]] inline std::expected<LagModel, TimeSeriesError>
fit_lags(std::span<const double> values, std::vector<std::size_t> ar_lags,
         std::vector<std::size_t> ma_lags, std::size_t iterations,
         double ridge) {
    if (!all_finite(values) || iterations == 0 || !(ridge >= 0.0)
        || !std::isfinite(ridge))
        return std::unexpected(TimeSeriesError::BadParameter);
    std::sort(ar_lags.begin(), ar_lags.end());
    ar_lags.erase(std::unique(ar_lags.begin(), ar_lags.end()), ar_lags.end());
    std::sort(ma_lags.begin(), ma_lags.end());
    ma_lags.erase(std::unique(ma_lags.begin(), ma_lags.end()), ma_lags.end());
    std::size_t max_lag = 0;
    for (const auto lag : ar_lags) {
        if (lag == 0) return std::unexpected(TimeSeriesError::BadParameter);
        max_lag = std::max(max_lag, lag);
    }
    for (const auto lag : ma_lags) {
        if (lag == 0) return std::unexpected(TimeSeriesError::BadParameter);
        max_lag = std::max(max_lag, lag);
    }
    const std::size_t parameters = 1 + ar_lags.size() + ma_lags.size();
    if (values.size() <= max_lag + parameters + 4)
        return std::unexpected(TimeSeriesError::TooFewSamples);

    LagModel out;
    out.ar_lags = std::move(ar_lags);
    out.ma_lags = std::move(ma_lags);
    out.ar.assign(out.ar_lags.size(), 0.0);
    out.ma.assign(out.ma_lags.size(), 0.0);
    out.residuals.assign(values.size(), 0.0);
    std::vector<double> coefficient(parameters, 0.0);

    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        std::vector<double> gram(parameters * parameters, 0.0);
        std::vector<double> rhs(parameters, 0.0);
        std::vector<double> row(parameters, 1.0);
        for (std::size_t t = max_lag; t < values.size(); ++t) {
            std::size_t column = 1;
            for (const auto lag : out.ar_lags) row[column++] = values[t - lag];
            for (const auto lag : out.ma_lags)
                row[column++] = out.residuals[t - lag];
            for (std::size_t a = 0; a < parameters; ++a) {
                rhs[a] += row[a] * values[t];
                for (std::size_t b = 0; b < parameters; ++b)
                    gram[a * parameters + b] += row[a] * row[b];
            }
        }
        for (std::size_t i = 1; i < parameters; ++i)
            gram[i * parameters + i] += ridge;
        if (!classical_detail::solve(gram, rhs, parameters))
            return std::unexpected(TimeSeriesError::Singular);
        coefficient = std::move(rhs);
        for (std::size_t t = max_lag; t < values.size(); ++t) {
            double fitted = coefficient[0];
            std::size_t column = 1;
            for (const auto lag : out.ar_lags)
                fitted += coefficient[column++] * values[t - lag];
            for (const auto lag : out.ma_lags)
                fitted += coefficient[column++] * out.residuals[t - lag];
            out.residuals[t] = values[t] - fitted;
        }
    }

    out.intercept = coefficient[0];
    std::size_t column = 1;
    for (double& value : out.ar) value = coefficient[column++];
    for (double& value : out.ma) value = coefficient[column++];
    double sum_sq = 0.0;
    for (std::size_t t = max_lag; t < values.size(); ++t)
        sum_sq += out.residuals[t] * out.residuals[t];
    out.residual_variance = sum_sq
        / static_cast<double>(values.size() - max_lag - parameters);

    // Conservative sufficient checks. They may refuse some stable high-order
    // polynomials, but never label an explosive fit stationary/invertible.
    double ar_norm = 0.0, ma_norm = 0.0;
    for (const double v : out.ar) ar_norm += std::fabs(v);
    for (const double v : out.ma) ma_norm += std::fabs(v);
    out.stationary = ar_norm < 1.0 - 1e-9;
    out.invertible = ma_norm < 1.0 - 1e-9;
    if (!out.stationary) return std::unexpected(TimeSeriesError::NonStationary);
    if (!out.invertible) return std::unexpected(TimeSeriesError::NonInvertible);
    return out;
}

} // namespace time_series_detail

[[nodiscard]] inline std::expected<LagModel, TimeSeriesError>
fit_arma(std::span<const double> values, std::size_t p, std::size_t q,
         std::size_t iterations = 8, double ridge = 1e-8) {
    if (p > 16 || q > 16 || p + q == 0)
        return std::unexpected(TimeSeriesError::BadParameter);
    std::vector<std::size_t> ar(p), ma(q);
    for (std::size_t i = 0; i < p; ++i) ar[i] = i + 1;
    for (std::size_t i = 0; i < q; ++i) ma[i] = i + 1;
    return time_series_detail::fit_lags(values, std::move(ar), std::move(ma),
                                        iterations, ridge);
}

struct IntegratedModel {
    LagModel model;
    std::vector<double> original_tail;
    std::vector<double> seasonal_tail;
    std::vector<double> transformed;
    std::size_t d = 0, seasonal_d = 0, season = 0;

    [[nodiscard]] std::expected<double, TimeSeriesError> forecast_one() const {
        const auto base = model.forecast(transformed);
        if (!base) return std::unexpected(base.error());
        double value = *base;
        if (d == 1) value += seasonal_tail.back();
        else if (d == 2)
            value += 2.0 * seasonal_tail.back()
                   - seasonal_tail[seasonal_tail.size() - 2];
        if (seasonal_d == 1) value += original_tail[original_tail.size() - season];
        else if (seasonal_d == 2)
            value += 2.0 * original_tail[original_tail.size() - season]
                   - original_tail[original_tail.size() - 2 * season];
        return value;
    }
};

[[nodiscard]] inline std::expected<IntegratedModel, TimeSeriesError>
fit_arima(std::span<const double> values, std::size_t p, std::size_t d,
          std::size_t q) {
    if (d > 2 || values.size() < 12 || !time_series_detail::all_finite(values))
        return std::unexpected(TimeSeriesError::BadParameter);
    IntegratedModel out;
    out.d = d;
    out.original_tail.assign(values.begin(), values.end());
    out.seasonal_tail = out.original_tail;
    out.transformed = out.seasonal_tail;
    for (std::size_t i = 0; i < d; ++i) {
        const auto next = time_series_detail::difference(out.transformed, 1);
        if (!next) return std::unexpected(next.error());
        out.transformed = *next;
    }
    const auto fitted = fit_arma(out.transformed, p, q);
    if (!fitted) return std::unexpected(fitted.error());
    out.model = *fitted;
    return out;
}

[[nodiscard]] inline std::expected<IntegratedModel, TimeSeriesError>
fit_sarima(std::span<const double> values, std::size_t p, std::size_t d,
           std::size_t q, std::size_t seasonal_p, std::size_t seasonal_d,
           std::size_t seasonal_q, std::size_t season) {
    if (d > 2 || seasonal_d > 2 || season < 2
        || p > 8 || q > 8 || seasonal_p > 4 || seasonal_q > 4
        || !time_series_detail::all_finite(values))
        return std::unexpected(TimeSeriesError::BadParameter);
    const std::size_t minimum = (seasonal_d + seasonal_p + seasonal_q + 2) * season;
    if (values.size() <= minimum)
        return std::unexpected(TimeSeriesError::TooFewSamples);
    IntegratedModel out;
    out.d = d; out.seasonal_d = seasonal_d; out.season = season;
    out.original_tail.assign(values.begin(), values.end());
    out.seasonal_tail = out.original_tail;
    for (std::size_t i = 0; i < seasonal_d; ++i) {
        const auto next = time_series_detail::difference(out.seasonal_tail, season);
        if (!next) return std::unexpected(next.error());
        out.seasonal_tail = *next;
    }
    out.transformed = out.seasonal_tail;
    for (std::size_t i = 0; i < d; ++i) {
        const auto next = time_series_detail::difference(out.transformed, 1);
        if (!next) return std::unexpected(next.error());
        out.transformed = *next;
    }
    std::vector<std::size_t> ar, ma;
    for (std::size_t i = 1; i <= p; ++i) ar.push_back(i);
    for (std::size_t i = 1; i <= seasonal_p; ++i) ar.push_back(i * season);
    for (std::size_t i = 1; i <= q; ++i) ma.push_back(i);
    for (std::size_t i = 1; i <= seasonal_q; ++i) ma.push_back(i * season);
    if (ar.empty() && ma.empty()) return std::unexpected(TimeSeriesError::BadParameter);
    const auto fitted = time_series_detail::fit_lags(
        out.transformed, std::move(ar), std::move(ma), 10, 1e-8);
    if (!fitted) return std::unexpected(fitted.error());
    out.model = *fitted;
    return out;
}

struct OrnsteinUhlenbeck {
    double theta = 0.0;       // mean-reversion rate, inverse time
    double mean = 0.0;        // long-run level, input units
    double sigma = 0.0;       // diffusion scale, input units/sqrt(time)
    double dt = 0.0;
    double theta_se = 0.0;
    double mean_se = 0.0;

    [[nodiscard]] double half_life() const noexcept {
        return std::log(2.0) / theta;
    }
    [[nodiscard]] double expected_next(double current) const noexcept {
        const double decay = std::exp(-theta * dt);
        return mean + decay * (current - mean);
    }
    [[nodiscard]] double step(double current, double normal) const noexcept {
        const double decay = std::exp(-theta * dt);
        const double variance = sigma * sigma * (1.0 - decay * decay)
                              / (2.0 * theta);
        return mean + decay * (current - mean)
             + std::sqrt(std::max(0.0, variance)) * normal;
    }
};

[[nodiscard]] inline std::expected<OrnsteinUhlenbeck, TimeSeriesError>
fit_ou(std::span<const double> values, double dt) {
    if (values.size() < 20 || !(dt > 0.0) || !std::isfinite(dt)
        || !time_series_detail::all_finite(values))
        return std::unexpected(TimeSeriesError::BadParameter);
    const std::size_t n = values.size() - 1;
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = values[i], y = values[i + 1];
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    const double den = static_cast<double>(n) * sxx - sx * sx;
    if (!(std::fabs(den) > 1e-15))
        return std::unexpected(TimeSeriesError::Singular);
    const double b = (static_cast<double>(n) * sxy - sx * sy) / den;
    const double a = (sy - b * sx) / static_cast<double>(n);
    if (!(b > 0.0 && b < 1.0))
        return std::unexpected(TimeSeriesError::NonStationary);
    OrnsteinUhlenbeck out;
    out.dt = dt;
    out.theta = -std::log(b) / dt;
    out.mean = a / (1.0 - b);
    double rss = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double e = values[i + 1] - a - b * values[i];
        rss += e * e;
    }
    const double innovation_variance = rss / static_cast<double>(n - 2);
    out.sigma = std::sqrt(innovation_variance * 2.0 * out.theta
                        / (1.0 - b * b));
    const double x_mean = sx / static_cast<double>(n);
    double centred = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = values[i] - x_mean;
        centred += d * d;
    }
    const double b_se = std::sqrt(innovation_variance / centred);
    out.theta_se = b_se / (b * dt);
    out.mean_se = std::sqrt(innovation_variance
                           * (1.0 / static_cast<double>(n)
                              + x_mean * x_mean / centred)) / (1.0 - b);
    if (!std::isfinite(out.theta) || !std::isfinite(out.mean)
        || !std::isfinite(out.sigma))
        return std::unexpected(TimeSeriesError::NonFinite);
    return out;
}

} // namespace altair
