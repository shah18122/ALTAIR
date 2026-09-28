// models/dataset.hpp -- tensor assembly from features and labels.
//
// P8-02.
//
// NORMALISATION STATISTICS FITTED ON THE WHOLE DATASET LEAK THE TEST SET INTO
// EVERY TRAINING ROW.
//
// This is the card, and it is the leak that survives every other precaution.
// A team can purge, embargo, walk forward, hold out a final year -- and then
// standardise the features by computing a mean and a standard deviation over
// all of it before splitting. Each training row is now expressed in units
// derived partly from the test set. Nothing was copied, the split is still
// clean, and information crossed anyway.
//
// Measured, on a feature whose level shifts by +3.0 at the train/test boundary
// -- which is what a regime change is, and is the ordinary case rather than an
// adversarial one:
//
//     fitted on EVERYTHING   mean +0.9497   sd 1.7123
//     fitted on TRAIN only   mean +0.0376   sd 1.0018
//
// EVERY training row is now expressed in different units under the two
// scalers: mean absolute gap 0.5683, worst 1.9552. The training data itself
// has been changed by rows the model is not supposed to have seen.
//
// It is worse than it sounds because it is invisible in the split code. The
// fold boundary is identical in both cases and every review that reads it
// finds it correct. The leak is in the PREPROCESSING, which usually lives in a
// different file written at a different time.
//
// So `Scaler` has to be `fit` before it can `transform`, `fit` records the row
// range it saw, and `transform` refuses a row outside that range unless the
// caller explicitly says it is applying a training-fitted scaler to test data
// -- which is the one correct use and therefore the one that should have to be
// spelled out.
//
// A TENSOR HAS NO WAY TO SAY "ABSENT".
//
// P5-02 spent a whole card establishing that an unready feature is absent, not
// zero. A dense matrix of doubles cannot represent that, so the builder has to
// decide, and both options change the data:
//
//   DROP the row      -- changes which samples exist, and drops them
//                        non-randomly: warmup rows, gaps and illiquid symbols
//                        go first, which are exactly the hard cases.
//   MASK the value    -- keeps the row and hands the model a companion matrix
//                        saying which entries are real.
//
// There is no third option where the zero is harmless, so `MissingPolicy` has
// no default and a zero-fill is not offered at all.

#pragma once

#include <features/vector.hpp>
#include <models/labels.hpp>
#include <models/tensor.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class DatasetError : std::uint8_t {
    /// The scaler was used before it was fitted.
    NotFitted,
    /// The scaler was fitted twice.
    AlreadyFitted,
    /// A row outside the fitted range was transformed without saying so.
    OutOfFittedRange,
    /// The missing-value policy was not chosen.
    NoMissingPolicy,
    /// Shapes did not line up.
    ShapeMismatch,
    /// A feature had zero variance over the fitted rows.
    Degenerate,
    /// Fewer rows than the operation needs.
    TooFewRows
};

/// What to do with a feature that is ABSENT at a given row.
enum class MissingPolicy : std::uint8_t {
    Unspecified = 0,
    /// Drop the whole row. Honest, and non-random -- see the header.
    DropRow,
    /// Keep the row, write the fitted mean, and set the mask to 0 so the model
    /// can see which entries are real.
    MaskAndImpute
};

// ---------------------------------------------------------------------------
// The scaler
// ---------------------------------------------------------------------------

/// Per-feature standardisation, fitted on an explicit row range.
///
/// The range is recorded so `transform` can tell a training row from a test
/// row and refuse the one that would leak.
class Scaler {
public:
    /// Fit on rows [from, to). NOTHING may be standardised before this.
    [[nodiscard]] std::expected<void, DatasetError>
    fit(const Matrix& x, std::size_t from, std::size_t to) noexcept {
        if (fitted_) { return std::unexpected(DatasetError::AlreadyFitted); }
        if (!x.valid() || to <= from || to > x.rows()) {
            return std::unexpected(DatasetError::TooFewRows);
        }
        if (x.cols() > kMaxFeatures) {
            return std::unexpected(DatasetError::ShapeMismatch);
        }
        const std::size_t n = to - from;
        if (n < 2) { return std::unexpected(DatasetError::TooFewRows); }

        cols_ = x.cols();
        for (std::size_t j = 0; j < cols_; ++j) {
            double s = 0.0;
            for (std::size_t i = from; i < to; ++i) { s += x.at(i, j); }
            mean_[j] = s / static_cast<double>(n);
        }
        for (std::size_t j = 0; j < cols_; ++j) {
            double s2 = 0.0;
            for (std::size_t i = from; i < to; ++i) {
                const double d = x.at(i, j) - mean_[j];
                s2 += d * d;
            }
            sd_[j] = std::sqrt(s2 / static_cast<double>(n - 1));
            // A constant feature has no scale. Standardising it divides by
            // zero; leaving it alone hands the model a column of a single
            // value, which is a bias term it already has. Marked and reported
            // rather than silently kept.
            if (!(sd_[j] > 0.0)) { sd_[j] = 0.0; ++degenerate_; }
        }
        from_ = from;
        to_ = to;
        fitted_ = true;
        return {};
    }

    /// Standardise rows [from, to) in place.
    ///
    /// `applying_to_holdout` must be true when the range falls outside what
    /// was fitted. That is the ONE correct use -- a training-fitted scaler
    /// applied to test data -- and requiring it to be said is what makes the
    /// incorrect use, fitting across everything, impossible to reach by
    /// accident.
    [[nodiscard]] std::expected<void, DatasetError>
    transform(Matrix& x, std::size_t from, std::size_t to,
              bool applying_to_holdout) const noexcept {
        if (!fitted_) { return std::unexpected(DatasetError::NotFitted); }
        if (!x.valid() || x.cols() != cols_ || to > x.rows() || to <= from) {
            return std::unexpected(DatasetError::ShapeMismatch);
        }
        const bool inside = from >= from_ && to <= to_;
        if (!inside && !applying_to_holdout) {
            return std::unexpected(DatasetError::OutOfFittedRange);
        }
        for (std::size_t i = from; i < to; ++i) {
            for (std::size_t j = 0; j < cols_; ++j) {
                if (sd_[j] > 0.0) {
                    x.at(i, j) = (x.at(i, j) - mean_[j]) / sd_[j];
                }
            }
        }
        return {};
    }

    [[nodiscard]] bool fitted() const noexcept { return fitted_; }
    [[nodiscard]] std::size_t fitted_from() const noexcept { return from_; }
    [[nodiscard]] std::size_t fitted_to() const noexcept { return to_; }
    [[nodiscard]] std::size_t degenerate_features() const noexcept {
        return degenerate_;
    }
    [[nodiscard]] double mean(std::size_t j) const noexcept { return mean_[j]; }
    [[nodiscard]] double sd(std::size_t j) const noexcept { return sd_[j]; }

private:
    double mean_[kMaxFeatures] = {};
    double sd_[kMaxFeatures] = {};
    std::size_t cols_ = 0;
    std::size_t from_ = 0;
    std::size_t to_ = 0;
    std::size_t degenerate_ = 0;
    bool fitted_ = false;
};

/// One-dimensional target standardisation for forecasting/regression.
/// Fit only on the training fold; `inverse` restores predictions to the
/// original price/return unit. This is deliberately separate from feature
/// scaling so a caller cannot accidentally report a scaled target as a price.
class TargetScaler {
public:
    [[nodiscard]] std::expected<void, DatasetError>
    fit(const double* values, std::size_t from, std::size_t to) noexcept {
        if (fitted_) return std::unexpected(DatasetError::AlreadyFitted);
        if (values == nullptr || to <= from || to - from < 2)
            return std::unexpected(DatasetError::TooFewRows);
        double sum = 0.0;
        for (std::size_t i = from; i < to; ++i) {
            if (!std::isfinite(values[i]))
                return std::unexpected(DatasetError::ShapeMismatch);
            sum += values[i];
        }
        mean_ = sum / static_cast<double>(to - from);
        double squared = 0.0;
        for (std::size_t i = from; i < to; ++i) {
            const double d = values[i] - mean_;
            squared += d * d;
        }
        scale_ = std::sqrt(squared / static_cast<double>(to - from - 1));
        if (!(scale_ > 0.0) || !std::isfinite(scale_))
            return std::unexpected(DatasetError::Degenerate);
        from_ = from; to_ = to; fitted_ = true;
        return {};
    }

    [[nodiscard]] std::expected<double, DatasetError>
    transform(double value) const noexcept {
        if (!fitted_) return std::unexpected(DatasetError::NotFitted);
        if (!std::isfinite(value)) return std::unexpected(DatasetError::ShapeMismatch);
        return (value - mean_) / scale_;
    }

    [[nodiscard]] std::expected<double, DatasetError>
    inverse(double scaled) const noexcept {
        if (!fitted_) return std::unexpected(DatasetError::NotFitted);
        if (!std::isfinite(scaled)) return std::unexpected(DatasetError::ShapeMismatch);
        return mean_ + scaled * scale_;
    }

    [[nodiscard]] double mean() const noexcept { return mean_; }
    [[nodiscard]] double scale() const noexcept { return scale_; }
    [[nodiscard]] std::size_t fitted_from() const noexcept { return from_; }
    [[nodiscard]] std::size_t fitted_to() const noexcept { return to_; }

private:
    double mean_ = 0.0;
    double scale_ = 0.0;
    std::size_t from_ = 0;
    std::size_t to_ = 0;
    bool fitted_ = false;
};

// ---------------------------------------------------------------------------
// Assembly
// ---------------------------------------------------------------------------

/// One assembled dataset. Views over caller-owned storage.
struct Dataset {
    Matrix x{};
    /// 1.0 where the feature is real, 0.0 where it was imputed. Same shape as
    /// x. Empty under DropRow.
    Matrix mask{};
    double* y = nullptr;
    /// Sample weights, from P8-01's average uniqueness. NOT all ones: rows
    /// whose labels overlap are not separate observations.
    double* weight = nullptr;
    /// Which bar each row came from. Required, because a shuffled dataset
    /// with no index back to time cannot be purged, walked forward, or
    /// audited.
    std::size_t* bar = nullptr;
    /// Each row's label window, for P6-06.
    LabelWindow* window = nullptr;
    std::size_t rows = 0;
    std::size_t dropped_absent = 0;
    std::size_t dropped_unlabelled = 0;
};

/// Assemble features + labels into a dataset.
///
/// `feature_index` selects which registry slots become columns, in order --
/// P5-01 hashes feature ORDER for exactly this reason, and a dataset built in
/// a different order is a different dataset even with identical columns.
[[nodiscard]] inline std::expected<Dataset, DatasetError>
build_dataset(const FeatureVector* vectors, const Label* labels,
              const double* uniqueness, std::size_t n,
              const FeatureIndex* feature_index, std::size_t k,
              MissingPolicy policy, double* x_store, double* mask_store,
              double* y_store, double* w_store, std::size_t* bar_store,
              LabelWindow* win_store) noexcept {
    if (policy == MissingPolicy::Unspecified) {
        return std::unexpected(DatasetError::NoMissingPolicy);
    }
    if (n == 0 || k == 0) { return std::unexpected(DatasetError::TooFewRows); }
    if (policy == MissingPolicy::MaskAndImpute && mask_store == nullptr) {
        return std::unexpected(DatasetError::ShapeMismatch);
    }

    Dataset d{};
    std::size_t out = 0;
    for (std::size_t i = 0; i < n; ++i) {
        // An undecided label is not a training row. Truncated events at the
        // ragged right edge of the series are excluded here rather than being
        // labelled neutral upstream (P8-01).
        if (!labels[i].decided()) { ++d.dropped_unlabelled; continue; }

        bool complete = true;
        for (std::size_t j = 0; j < k; ++j) {
            if (!vectors[i].has(feature_index[j])) { complete = false; break; }
        }
        if (!complete && policy == MissingPolicy::DropRow) {
            ++d.dropped_absent;
            continue;
        }
        for (std::size_t j = 0; j < k; ++j) {
            const auto v = vectors[i].value(feature_index[j]);
            const bool present = v.has_value();
            // Imputed with ZERO here and corrected to the fitted mean by
            // `impute_with_fitted_mean` AFTER the scaler is fitted -- because
            // the mean is a training-fold statistic and computing it now,
            // over every row, would be the very leak this file is about.
            x_store[out * k + j] = present ? *v : 0.0;
            if (mask_store != nullptr) {
                mask_store[out * k + j] = present ? 1.0 : 0.0;
            }
        }
        y_store[out] = static_cast<double>(labels[i].sign);
        w_store[out] = uniqueness != nullptr ? uniqueness[i] : 1.0;
        bar_store[out] = i;
        win_store[out] = labels[i].window;
        ++out;
    }
    if (out == 0) { return std::unexpected(DatasetError::TooFewRows); }

    d.x = Matrix{x_store, out, k};
    if (mask_store != nullptr) { d.mask = Matrix{mask_store, out, k}; }
    d.y = y_store;
    d.weight = w_store;
    d.bar = bar_store;
    d.window = win_store;
    d.rows = out;
    return d;
}

/// Replace masked-out entries with the scaler's fitted mean.
///
/// Called AFTER `Scaler::fit` and BEFORE `transform`. The ordering is the
/// point: the imputation value is a training-fold statistic, so computing it
/// from every row -- which is what an impute-then-split pipeline does -- is
/// the same leak in a different coat.
[[nodiscard]] inline std::expected<std::size_t, DatasetError>
impute_with_fitted_mean(Dataset& d, const Scaler& s) noexcept {
    if (!s.fitted()) { return std::unexpected(DatasetError::NotFitted); }
    if (!d.mask.valid()) { return 0; }
    if (d.mask.rows() != d.x.rows() || d.mask.cols() != d.x.cols()) {
        return std::unexpected(DatasetError::ShapeMismatch);
    }
    std::size_t filled = 0;
    for (std::size_t i = 0; i < d.x.rows(); ++i) {
        for (std::size_t j = 0; j < d.x.cols(); ++j) {
            if (d.mask.at(i, j) == 0.0) {
                d.x.at(i, j) = s.mean(j);
                ++filled;
            }
        }
    }
    return filled;
}

/// Effective rows: the sum of the sample weights, not the row count.
///
/// The number P7-03's deflated Sharpe wants. P8-01 measured 2,000 consecutive
/// labels coming to 559 effective observations.
[[nodiscard]] inline double effective_rows(const Dataset& d) noexcept {
    double s = 0.0;
    for (std::size_t i = 0; i < d.rows; ++i) { s += d.weight[i]; }
    return s;
}

} // namespace altair
