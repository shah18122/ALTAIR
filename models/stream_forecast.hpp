// Cached forward-return model. Fitting is cold; inference has fixed storage.
//
// WHY THE ONE-HOUR FORECAST USED TO FAIL (fixed 2026-11-03). Every training
// window needed 21 closes AND the target bars back to back inside one
// session. An NSE session has 25 fifteen-minute bars and 6 complete sixty-
// minute bars, so a 15m bar with a four-bar (one hour) target needed all 25
// bars of a day (one window a day at best, under the 500-window minimum from
// the 12,000-bar history) and a 60m bar could never get one at all ("Fit
// refused"). Live, the same rule asked for 20 fresh bars after every session
// break, so a forecast appeared at 14:15 on 15m and never on 60m.
//
// THE RULE NOW. Features are the last 20 SAME-SESSION bar returns: a return is
// counted only between two bars exactly one interval apart, so the overnight
// gap and a missing bar are never an input, but the window may reach back
// into earlier sessions (up to kMaxLookbackNs). The target alone must stay in
// one session. Training and live inference build the window with the same
// class (ReturnWindow), so they cannot drift apart.
//
// LEAKAGE RULES, all checked by models/tests/test_stream_forecast_leak.cpp:
//   * a row's features use bars ending at or before its anchor time, its
//     label uses the anchor close and the close `horizon_bars` later;
//   * the 20-return volatility scale uses only that row's own past returns
//     (no scaler is fitted on the whole series), and the GBDT bin edges are
//     fitted on the training rows only (models/gbdt.hpp);
//   * splits are chronological 60/20/20 (fit / tree-depth selection /
//     untouched calibration), PURGED by time (a row whose label ends after
//     the next partition's first anchor is dropped) and EMBARGOED (the next
//     partition starts `horizon_bars` intervals after the boundary);
//   * depth features for a bar use only minutes inside that bar
//     (models/depth_features.hpp).
#pragma once
#include <models/depth_features.hpp>
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

inline constexpr std::size_t kStreamReturnLags = 20;
inline constexpr std::size_t kStreamReturnFeatures = 8;
inline constexpr std::int64_t kStreamDayNs = 86'400'000'000'000LL;
/// The oldest of the 20 returns may be at most this old. 20 sixty-minute
/// returns span about four sessions; 14 days covers a long holiday.
inline constexpr std::int64_t kMaxLookbackNs = 14 * kStreamDayNs;

struct ForwardEstimate {
    double current = 0.0, predicted = 0.0, low = 0.0, high = 0.0, move_bps = 0.0;
    // target_ns is 0 for a daily bar: next trading-session close needs an
    // exchange calendar, which this model deliberately does not guess.
    std::int64_t issued_ns = 0, target_ns = 0;
};

/// The last 20 same-session bar returns (bps), oldest first. Fed completed
/// bars in time order by both the fit and the live page.
class ReturnWindow {
public:
    ReturnWindow() = default;
    explicit ReturnWindow(std::int64_t interval_ns) noexcept : interval_(interval_ns) {}

    [[nodiscard]] bool daily() const noexcept { return interval_ >= kStreamDayNs; }

    /// A completed bar. False (and nothing changes) for a bad price or a
    /// stamp not after the previous bar's.
    bool push(double close, std::int64_t end_ns) noexcept {
        if (!(close > 0.0) || !std::isfinite(close) || interval_ <= 0 || end_ns <= 0 ||
            (last_end_ != 0 && end_ns <= last_end_))
            return false;
        if (have_last_ && (daily() || end_ns - last_end_ == interval_)) {
            const double r = 10000.0 * (std::log(close) - std::log(last_close_));
            if (!std::isfinite(r)) return false;
            ret_[head_] = r;
            end_[head_] = end_ns;
            head_ = (head_ + 1) % kStreamReturnLags;
            if (count_ < kStreamReturnLags) ++count_;
        }
        last_close_ = close;
        last_end_ = end_ns;
        have_last_ = true;
        return true;
    }

    /// The next bar's return is not counted (a stream gap made this bar's
    /// close untrustworthy, or the bar was skipped).
    void break_chain() noexcept { have_last_ = false; }

    [[nodiscard]] bool chained() const noexcept { return have_last_; }
    [[nodiscard]] double last_close() const noexcept { return last_close_; }
    [[nodiscard]] std::int64_t last_end() const noexcept { return last_end_; }
    [[nodiscard]] std::size_t count() const noexcept { return count_; }
    [[nodiscard]] std::int64_t interval_ns() const noexcept { return interval_; }

    /// End time of the oldest return held (0 when empty).
    [[nodiscard]] std::int64_t oldest_end() const noexcept {
        if (count_ == 0) return 0;
        return end_[(head_ + kStreamReturnLags - count_) % kStreamReturnLags];
    }

    /// 20 returns, none older than kMaxLookbackNs at `at_ns` (intraday).
    [[nodiscard]] bool ready(std::int64_t at_ns) const noexcept {
        return count_ == kStreamReturnLags && (daily() || at_ns - oldest_end() <= kMaxLookbackNs);
    }

    /// Oldest first.
    void copy(std::array<double, kStreamReturnLags>& out) const noexcept {
        for (std::size_t i = 0; i < kStreamReturnLags; ++i)
            out[i] = ret_[(head_ + kStreamReturnLags - count_ + i) % kStreamReturnLags];
    }

    /// The window as it would be if the bar in progress (ending at
    /// `bar_end_ns`) closed at `price` now: the oldest return drops and the
    /// provisional one joins, when that bar follows the last completed one in
    /// the same session. Otherwise the completed returns alone (the bar opens
    /// a session or follows a gap, which is what a training row anchored on
    /// such a bar sees). True when the provisional return was used.
    bool observe(double price, std::int64_t bar_end_ns, std::array<double, kStreamReturnLags>& out) const noexcept {
        copy(out);
        if (!have_last_ || !(price > 0.0) || !std::isfinite(price) ||
            (!daily() && bar_end_ns - last_end_ != interval_))
            return false;
        std::move(out.begin() + 1, out.end(), out.begin());
        out.back() = 10000.0 * (std::log(price) - std::log(last_close_));
        return true;
    }

private:
    std::int64_t interval_ = 0;
    std::array<double, kStreamReturnLags> ret_{};
    std::array<std::int64_t, kStreamReturnLags> end_{};
    std::size_t head_ = 0, count_ = 0;
    double last_close_ = 0.0;
    std::int64_t last_end_ = 0;
    bool have_last_ = false;
};

/// One training row: features from bars ending at or before anchor_ns, the
/// label from anchor_ns to label_end_ns.
struct StreamRow {
    std::size_t bar = 0;   ///< index of the anchor bar
    std::int64_t anchor_ns = 0, label_end_ns = 0;
    std::array<double, kStreamReturnLags> returns{};
    double label_bps = 0.0;
};

struct StreamRows {
    std::vector<StreamRow> rows;
    std::size_t skipped_windows = 0;   ///< target crosses a session break or a gap, or too few returns
};

/// Every usable row, in time order. Stamps are completed-bar END times.
[[nodiscard]] inline std::expected<StreamRows, StreamForecastError>
build_stream_rows(std::span<const double> closes, std::span<const std::int64_t> ends,
                  std::int64_t interval_ns, std::size_t horizon_bars) {
    const bool daily = interval_ns >= kStreamDayNs;
    if (closes.size() != ends.size() || interval_ns <= 0 || horizon_bars == 0 || horizon_bars > 120 ||
        (daily && horizon_bars != 1))
        return std::unexpected(StreamForecastError::BadInput);
    for (std::size_t i = 0; i < closes.size(); ++i)
        if (!std::isfinite(closes[i]) || closes[i] <= 0.0 || ends[i] <= 0 || (i && ends[i] <= ends[i - 1]))
            return std::unexpected(StreamForecastError::BadInput);
    StreamRows out;
    ReturnWindow window(interval_ns);
    for (std::size_t i = 0; i < closes.size(); ++i) {
        if (!window.push(closes[i], ends[i])) return std::unexpected(StreamForecastError::BadInput);
        if (i + horizon_bars >= closes.size()) continue;   // no label yet: the future, not a skip
        bool same_session = window.ready(ends[i]);
        for (std::size_t k = 1; same_session && k <= horizon_bars; ++k)
            same_session = daily || ends[i + k] - ends[i + k - 1] == interval_ns;
        if (!same_session) { ++out.skipped_windows; continue; }
        StreamRow r;
        r.bar = i;
        r.anchor_ns = ends[i];
        r.label_end_ns = ends[i + horizon_bars];
        window.copy(r.returns);
        r.label_bps = 10000.0 * (std::log(closes[i + horizon_bars]) - std::log(closes[i]));
        out.rows.push_back(r);
    }
    return out;
}

/// Returns are in basis points. Scaling uses only the 20 observed returns.
struct StreamForecastModel {
    Gbdt model;
    std::int64_t interval_ns = 0, observed_through_ns = 0;
    /// Bars ahead the label spans (4 at 15m = the next hour). Always 1 for daily.
    std::size_t horizon_bars = 1;
    /// 0, or depth::kDepthFeatures when five-level depth is an input.
    std::size_t depth_features = 0;
    double residual90_bps = 0.0, rmse_bps = 0.0, naive_rmse_bps = 0.0;
    /// With depth: the returns-only model's RMSE on the very same calibration
    /// rows (NaN without depth). Depth earns its place only below this.
    double returns_only_rmse_bps = std::numeric_limits<double>::quiet_NaN();
    std::size_t training_rows = 0, validation_rows = 0, scored_rows = 0;
    std::size_t skipped_windows = 0, chosen_depth = 0;
    std::size_t purged_rows = 0, embargoed_rows = 0, depth_missing_rows = 0;
    std::int64_t first_row_ns = 0, last_row_ns = 0;

    [[nodiscard]] std::size_t feature_count() const noexcept { return kStreamReturnFeatures + depth_features; }

    /// Price units are the caller's analytics units; no monetary ledger write.
    /// `returns` are the 20 same-session returns ending at issue time (see
    /// ReturnWindow::observe), `current` the price at issue time, `depth` this
    /// bar's depth row when the model uses depth (else empty).
    /// Live provisional-bar use must be labelled separately from held-out scores.
    [[nodiscard]] std::expected<ForwardEstimate, StreamForecastError>
    predict(std::span<const double, kStreamReturnLags> returns, double current, std::int64_t issued_ns,
            std::span<const double> depth = {}) const noexcept {
        const bool daily = interval_ns >= kStreamDayNs;
        if (interval_ns <= 0 || issued_ns < observed_through_ns || model.binner.edge.size() != feature_count() ||
            depth.size() != depth_features || !(current > 0.0) || !std::isfinite(current) ||
            horizon_bars == 0 || (daily && horizon_bars != 1) ||
            (!daily && interval_ns > (std::numeric_limits<std::int64_t>::max() - issued_ns) /
                                         static_cast<std::int64_t>(horizon_bars)))
            return std::unexpected(StreamForecastError::BadInput);
        std::array<double, kStreamReturnFeatures + depth::kDepthFeatures> features{};
        double scale = 0.0;
        if (!make_features(returns, std::span<double, kStreamReturnFeatures>(features.data(), kStreamReturnFeatures), scale))
            return std::unexpected(StreamForecastError::BadInput);
        for (std::size_t c = 0; c < depth.size(); ++c) {
            if (!std::isfinite(depth[c])) return std::unexpected(StreamForecastError::BadInput);
            features[kStreamReturnFeatures + c] = depth[c];
        }
        std::array<std::uint8_t, kStreamReturnFeatures + depth::kDepthFeatures> bins{};
        for (std::size_t c = 0; c < feature_count(); ++c)
            bins[c] = model.binner.bin_of(features[c], c);
        ForwardEstimate out;
        out.current = current;
        out.move_bps = model.predict_binned(bins.data()) * scale;
        out.predicted = out.current * std::exp(out.move_bps / 10000.0);
        out.low = out.current * std::exp((out.move_bps - residual90_bps) / 10000.0);
        out.high = out.current * std::exp((out.move_bps + residual90_bps) / 10000.0);
        out.issued_ns = issued_ns;
        out.target_ns = daily ? 0 : issued_ns + interval_ns * static_cast<std::int64_t>(horizon_bars);
        if (!std::isfinite(out.predicted) || !std::isfinite(out.low) ||
            !std::isfinite(out.high) || out.low <= 0.0 || out.low > out.predicted || out.high < out.predicted)
            return std::unexpected(StreamForecastError::NonFinite);
        return out;
    }

    /// The last 8 returns, and the scale (sample std of all 20, floored at 1 bp).
    static bool make_features(std::span<const double, kStreamReturnLags> returns,
                              std::span<double, kStreamReturnFeatures> features, double& scale) noexcept {
        double mean = 0.0;
        for (double r : returns) {
            if (!std::isfinite(r)) return false;
            mean += r;
        }
        mean /= static_cast<double>(kStreamReturnLags);
        double variance = 0.0;
        for (double r : returns) variance += (r - mean) * (r - mean);
        scale = std::sqrt(variance / static_cast<double>(kStreamReturnLags - 1));
        // A defined one-basis-point floor avoids division by zero on flat input.
        if (scale < 1.0) scale = 1.0;
        for (std::size_t i = 0; i < features.size(); ++i)
            features[i] = returns[kStreamReturnLags - kStreamReturnFeatures + i];
        return std::isfinite(scale);
    }
};

/// Row indices of the three chronological partitions after purge and embargo.
struct StreamSplit {
    std::vector<std::size_t> train, valid, test;
    std::size_t purged = 0, embargoed = 0;
};

/// 60/20/20 by row count, then: a row whose label ends after the next
/// partition's first anchor is PURGED; a row of the next partition anchored
/// less than `embargo_ns` after that boundary is EMBARGOED.
[[nodiscard]] inline StreamSplit split_stream_rows(std::span<const StreamRow> rows, std::int64_t embargo_ns) {
    StreamSplit s;
    const std::size_t n = rows.size();
    if (n < 5) return s;
    const std::size_t c1 = n * 3 / 5, c2 = n * 4 / 5;
    const std::int64_t b1 = rows[c1].anchor_ns, b2 = rows[c2].anchor_ns;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& r = rows[i];
        if (i < c1) {
            if (r.label_end_ns > b1) ++s.purged; else s.train.push_back(i);
        } else if (i < c2) {
            if (r.anchor_ns < b1 + embargo_ns) ++s.embargoed;
            else if (r.label_end_ns > b2) ++s.purged;
            else s.valid.push_back(i);
        } else {
            if (r.anchor_ns < b2 + embargo_ns) ++s.embargoed; else s.test.push_back(i);
        }
    }
    return s;
}

/// Chronological fit / tree-depth selection / untouched calibration.
/// Select tree depth 2..5 by validation RMSE; calibration never selects.
/// `depth` is empty, or one depth::DepthBar per bar (NaN = no reading; such
/// rows are dropped and counted). With depth, a returns-only model is fitted
/// on the SAME rows and scored on the same calibration rows for comparison.
[[nodiscard]] inline std::expected<StreamForecastModel, StreamForecastError>
fit_stream_forecast(std::span<const double> closes, std::span<const std::int64_t> ends,
                    std::int64_t interval_ns, std::size_t horizon_bars = 1,
                    std::span<const depth::DepthBar> depth = {}) {
    const bool daily = interval_ns >= kStreamDayNs;
    if (!depth.empty() && (depth.size() != closes.size() || daily))
        return std::unexpected(StreamForecastError::BadInput);
    auto built = build_stream_rows(closes, ends, interval_ns, horizon_bars);
    if (!built) return std::unexpected(built.error());
    if (closes.size() < 600) return std::unexpected(StreamForecastError::InsufficientHistory);
    StreamForecastModel out;
    out.interval_ns = interval_ns;
    out.horizon_bars = horizon_bars;
    out.observed_through_ns = ends.back();
    out.skipped_windows = built->skipped_windows;
    out.depth_features = depth.empty() ? 0 : depth::kDepthFeatures;
    std::vector<StreamRow> rows;
    rows.reserve(built->rows.size());
    for (const auto& r : built->rows) {
        if (!depth.empty()) {
            bool ok = true;
            for (double v : depth[r.bar]) ok = ok && std::isfinite(v);
            if (!ok) { ++out.depth_missing_rows; continue; }
        }
        rows.push_back(r);
    }
    if (rows.size() < 500) return std::unexpected(StreamForecastError::InsufficientHistory);
    const std::int64_t embargo = static_cast<std::int64_t>(horizon_bars) * (daily ? kStreamDayNs : interval_ns);
    const auto split = split_stream_rows(rows, embargo);
    if (split.train.size() < 100 || split.valid.size() < 20 || split.test.size() < 20)
        return std::unexpected(StreamForecastError::InsufficientHistory);
    out.purged_rows = split.purged;
    out.embargoed_rows = split.embargoed;
    out.first_row_ns = rows.front().anchor_ns;
    out.last_row_ns = rows.back().anchor_ns;

    // The feature matrix and the per-row scale. Each row's scale comes from
    // its own 20 past returns only.
    const auto matrix = [&](std::size_t p, Frame& all, std::vector<double>& scales) -> bool {
        all = Frame{};
        all.p = p;
        all.rows = rows.size();
        all.x.assign(rows.size() * p, 0.0);
        scales.assign(rows.size(), 0.0);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            double* x = all.x.data() + i * p;
            if (!StreamForecastModel::make_features(rows[i].returns,
                                                    std::span<double, kStreamReturnFeatures>(x, kStreamReturnFeatures),
                                                    scales[i]))
                return false;
            for (std::size_t c = kStreamReturnFeatures; c < p; ++c) x[c] = depth[rows[i].bar][c - kStreamReturnFeatures];
        }
        return true;
    };
    struct Selected { Gbdt model; std::size_t depth = 0; };
    const auto select = [&](const Frame& all, const std::vector<double>& scales)
        -> std::expected<Selected, StreamForecastError> {
        Frame train;
        train.p = all.p;
        train.rows = split.train.size();
        std::vector<double> y;
        y.reserve(split.train.size());
        for (std::size_t i : split.train) {
            train.x.insert(train.x.end(), all.x.begin() + static_cast<std::ptrdiff_t>(i * all.p),
                           all.x.begin() + static_cast<std::ptrdiff_t>((i + 1) * all.p));
            y.push_back(rows[i].label_bps / scales[i]);
        }
        double best = std::numeric_limits<double>::infinity();
        Selected chosen;
        for (std::size_t tree_depth : {2u, 3u, 4u, 5u}) {
            GbdtParams params;
            params.trees = 64;
            params.max_depth = tree_depth;
            params.learning_rate = 0.05;
            params.seed = 0xB0057u;
            auto fitted = fit_gbdt(train, y, params);
            if (!fitted) continue;
            double loss = 0.0;
            for (std::size_t i : split.valid) {
                const double error = fitted->predict_row(all.x.data() + i * all.p) * scales[i] - rows[i].label_bps;
                loss += error * error;
            }
            if (std::isfinite(loss) && loss < best) {
                best = loss;
                chosen.model = std::move(*fitted);
                chosen.depth = tree_depth;
            }
        }
        if (!std::isfinite(best)) return std::unexpected(StreamForecastError::FitFailed);
        return chosen;
    };
    const auto score = [&](const Gbdt& m, const Frame& all, const std::vector<double>& scales,
                           std::vector<double>* abs_errors) -> double {
        double squared = 0.0;
        for (std::size_t i : split.test) {
            const double error = m.predict_row(all.x.data() + i * all.p) * scales[i] - rows[i].label_bps;
            if (!std::isfinite(error)) return std::numeric_limits<double>::quiet_NaN();
            if (abs_errors) abs_errors->push_back(std::fabs(error));
            squared += error * error;
        }
        return std::sqrt(squared / static_cast<double>(split.test.size()));
    };

    Frame all;
    std::vector<double> scales;
    if (!matrix(out.feature_count(), all, scales)) return std::unexpected(StreamForecastError::BadInput);
    auto chosen = select(all, scales);
    if (!chosen) return std::unexpected(chosen.error());
    out.model = std::move(chosen->model);
    out.chosen_depth = chosen->depth;
    std::vector<double> errors;
    out.rmse_bps = score(out.model, all, scales, &errors);
    if (!std::isfinite(out.rmse_bps)) return std::unexpected(StreamForecastError::NonFinite);
    double naive = 0.0;
    for (std::size_t i : split.test) naive += rows[i].label_bps * rows[i].label_bps;
    out.naive_rmse_bps = std::sqrt(naive / static_cast<double>(split.test.size()));
    if (out.depth_features > 0) {
        Frame plain;
        std::vector<double> plain_scales;
        if (!matrix(kStreamReturnFeatures, plain, plain_scales)) return std::unexpected(StreamForecastError::BadInput);
        const auto baseline = select(plain, plain_scales);
        if (baseline) out.returns_only_rmse_bps = score(baseline->model, plain, plain_scales, nullptr);
    }
    std::sort(errors.begin(), errors.end());
    out.scored_rows = errors.size();
    out.training_rows = split.train.size();
    out.validation_rows = split.valid.size();
    // Empirical 90th percentile, no distributional or tick-level coverage claim.
    out.residual90_bps = errors[(errors.size() - 1) * 9 / 10];
    return out;
}
} // namespace altair
