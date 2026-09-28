// Cached forward-return model. Fitting is cold; inference has fixed storage.
#pragma once
#include <models/gbdt.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <vector>

namespace altair {
enum class StreamForecastError { BadInput, InsufficientHistory, FitFailed, NonFinite };
struct ForwardEstimate {
    double current = 0.0, predicted = 0.0, low = 0.0, high = 0.0, move_bps = 0.0;
    // target_ns is 0 for a daily bar: next trading-session close needs an
    // exchange calendar, which this model deliberately does not guess.
    std::int64_t issued_ns = 0, target_ns = 0;
};

/// Returns are in basis points. Scaling uses only the 20 observed returns.
struct StreamForecastModel {
    Gbdt model;
    std::int64_t interval_ns = 0, observed_through_ns = 0;
    double residual90_bps = 0.0, rmse_bps = 0.0, naive_rmse_bps = 0.0;
    std::size_t training_rows = 0, validation_rows = 0, scored_rows = 0;
    std::size_t skipped_windows = 0, chosen_depth = 0;

    /// Price units are the caller's analytics units; no monetary ledger write.
    /// `closes` contains 21 ordered observations ending at issue time.
    /// Live provisional-bar use must be labelled separately from held-out scores.
    [[nodiscard]] std::expected<ForwardEstimate, StreamForecastError>
    predict(std::span<const double, 21> closes, std::int64_t issued_ns) const noexcept {
        const bool daily = interval_ns >= 86'400'000'000'000LL;
        if (interval_ns <= 0 || issued_ns < observed_through_ns || model.binner.edge.size() != 8 ||
            (!daily && issued_ns > std::numeric_limits<std::int64_t>::max() - interval_ns))
            return std::unexpected(StreamForecastError::BadInput);
        std::array<double, 8> features{};
        double scale = 0.0;
        if (!make_features(closes, features, scale))
            return std::unexpected(StreamForecastError::BadInput);
        std::array<std::uint8_t, 8> bins{};
        for (std::size_t c = 0; c < bins.size(); ++c)
            bins[c] = model.binner.bin_of(features[c], c);
        ForwardEstimate out;
        out.current = closes.back();
        out.move_bps = model.predict_binned(bins.data()) * scale;
        out.predicted = out.current * std::exp(out.move_bps / 10000.0);
        out.low = out.current * std::exp((out.move_bps - residual90_bps) / 10000.0);
        out.high = out.current * std::exp((out.move_bps + residual90_bps) / 10000.0);
        out.issued_ns = issued_ns;
        out.target_ns = daily ? 0 : issued_ns + interval_ns;
        if (!std::isfinite(out.predicted) || !std::isfinite(out.low) ||
            !std::isfinite(out.high) || out.low <= 0.0 || out.low > out.predicted || out.high < out.predicted)
            return std::unexpected(StreamForecastError::NonFinite);
        return out;
    }

    /// Fixed-size causal features. Reject nonpositive or nonfinite prices.
    static bool make_features(std::span<const double, 21> closes,
                              std::array<double, 8>& features, double& scale) noexcept {
        std::array<double, 20> returns{};
        for (double p : closes) if (!(p > 0.0) || !std::isfinite(p)) return false;
        double mean = 0.0;
        for (std::size_t i = 0; i < returns.size(); ++i) {
            returns[i] = 10000.0 * (std::log(closes[i + 1]) - std::log(closes[i]));
            mean += returns[i];
        }
        mean /= 20.0;
        double variance = 0.0;
        for (double r : returns) variance += (r - mean) * (r - mean);
        scale = std::sqrt(variance / 19.0);
        // A defined one-basis-point floor avoids division by zero on flat input.
        if (scale < 1.0) scale = 1.0;
        for (std::size_t i = 0; i < features.size(); ++i) features[i] = returns[i + 12];
        return std::isfinite(scale);
    }
};

/// Chronological 60/20/20 fit, model selection, then untouched calibration.
/// Select depth 2 or 3 by validation RMSE; test/calibration never selects depth.
/// Stamps are completed-bar END times. Input order is strict, gaps skip windows.
[[nodiscard]] inline std::expected<StreamForecastModel, StreamForecastError>
fit_stream_forecast(std::span<const double> closes, std::span<const std::int64_t> ends,
                    std::int64_t interval_ns) {
    if (closes.size() != ends.size() || interval_ns <= 0)
        return std::unexpected(StreamForecastError::BadInput);
    if (closes.size() < 600) return std::unexpected(StreamForecastError::InsufficientHistory);
    for (std::size_t i = 0; i < closes.size(); ++i) {
        if (!std::isfinite(closes[i]) || closes[i] <= 0.0 || ends[i] <= 0 ||
            (i && ends[i] <= ends[i - 1])) return std::unexpected(StreamForecastError::BadInput);
    }
    Frame all;
    all.p = 8;
    std::vector<double> labels, scales;
    StreamForecastModel out;
    out.interval_ns = interval_ns;
    out.observed_through_ns = ends.back();
    for (std::size_t i = 20; i + 1 < closes.size(); ++i) {
        // Intraday features and target must each represent one matching bar.
        // Never turn an overnight or missing-bar move into a one-minute input.
        bool continuous = interval_ns >= 86'400'000'000'000LL;
        if (!continuous) {
            continuous = ends[i + 1] - ends[i] == interval_ns;
            for (std::size_t j = i - 19; continuous && j <= i; ++j)
                continuous = ends[j] - ends[j - 1] == interval_ns;
        }
        if (!continuous) { ++out.skipped_windows; continue; }
        std::array<double, 8> x{};
        double scale = 0.0;
        if (!StreamForecastModel::make_features(std::span<const double, 21>(closes.data() + i - 20, 21), x, scale))
            return std::unexpected(StreamForecastError::BadInput);
        all.x.insert(all.x.end(), x.begin(), x.end());
        labels.push_back(10000.0 * (std::log(closes[i + 1]) - std::log(closes[i])));
        scales.push_back(scale);
        ++all.rows;
    }
    if (all.rows < 500) return std::unexpected(StreamForecastError::InsufficientHistory);
    const std::size_t train_end = all.rows * 3 / 5, valid_end = all.rows * 4 / 5;
    Frame train;
    train.p = 8;
    train.rows = train_end;
    train.x.assign(all.x.begin(), all.x.begin() + static_cast<std::ptrdiff_t>(train_end * 8));
    std::vector<double> y(train_end);
    for (std::size_t i = 0; i < train_end; ++i) y[i] = labels[i] / scales[i];
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t depth : {2u, 3u}) {
        GbdtParams params;
        params.trees = 64;
        params.max_depth = depth;
        params.learning_rate = 0.05;
        params.seed = 0xB0057u;
        auto fitted = fit_gbdt(train, y, params);
        if (!fitted) continue;
        double loss = 0.0;
        // One purged label at each partition boundary.
        for (std::size_t i = train_end + 1; i < valid_end; ++i) {
            const double error = fitted->predict_row(all.x.data() + i * 8) * scales[i] - labels[i];
            loss += error * error;
        }
        if (std::isfinite(loss) && loss < best) {
            best = loss;
            out.model = std::move(*fitted);
            out.chosen_depth = depth;
        }
    }
    if (!std::isfinite(best)) return std::unexpected(StreamForecastError::FitFailed);
    std::vector<double> errors;
    double squared = 0.0, naive = 0.0;
    for (std::size_t i = valid_end + 1; i < all.rows; ++i) {
        const double error = out.model.predict_row(all.x.data() + i * 8) * scales[i] - labels[i];
        if (!std::isfinite(error)) return std::unexpected(StreamForecastError::NonFinite);
        errors.push_back(std::fabs(error));
        squared += error * error;
        naive += labels[i] * labels[i];
    }
    std::sort(errors.begin(), errors.end());
    out.scored_rows = errors.size();
    out.training_rows = train_end;
    out.validation_rows = valid_end - train_end - 1;
    out.rmse_bps = std::sqrt(squared / static_cast<double>(errors.size()));
    out.naive_rmse_bps = std::sqrt(naive / static_cast<double>(errors.size()));
    // Empirical 90th percentile, no distributional or tick-level coverage claim.
    out.residual90_bps = errors[(errors.size() - 1) * 9 / 10];
    return out;
}
} // namespace altair
