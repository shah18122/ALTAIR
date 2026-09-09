// models/conformal.hpp -- a band that covers what it claims to cover.
//
// P35-01. Implements Algorithm 1 of "Taming Tail Risk in Financial Markets:
// Conformal Risk Control for Nonstationary Portfolio VaR" (Schmitt), which is
// in research/papers/inbox/.
//
// WHY THIS FILE EXISTS AT ALL.
//
// forecast_scorecard.hpp already measures whether a published band covers what
// it says: a band of one out-of-sample RMSE claims 68.27% under a normal error,
// and `ForecastScore::coverage` reports what it ACTUALLY covered. That check
// exists because a band that covers 40% is not a confidence interval, it is a
// decoration, and every position size taken from it is wrong in the direction
// that costs money.
//
// This file is the other half: having measured the miss, correct it. The paper's
// finding is stark and is the reason the method is worth having -- an
// uncalibrated gradient-boosted quantile forecaster exceeded its own 99% VaR on
// 5.31% of days, five times its stated rate, and a conformal wrapper brought
// that to 1.09% (their Table 1). The wrapper does not make the forecast better.
// It makes the band HONEST, which is a different and more useful thing.
//
// This is exactly what ROADMAP §3 asks for. "Size on the lower confidence bound
// of edge, never the point estimate" is only sound if the lower bound is real.
// A normal approximation on financial residuals is not real; heavy tails and
// volatility clustering are the two things it gets wrong, and they are the two
// things markets always do.
//
// THE METHOD, IN ONE PARAGRAPH.
//
// Keep the recent history of how wrong the forecaster was: the conformity score
// s = actual - forecast. To publish a bound now, take a high quantile of those
// past scores and add it to the current forecast. The quantile is WEIGHTED, so
// that recent errors and errors made in similar market conditions count for
// more. That is the whole idea; the care is all in the weighting and in what
// happens when the weights concentrate.
//
// WHY THE DEFAULT IS TIME-WEIGHTED AND NOT REGIME-WEIGHTED.
//
// The paper's title method is RWC (recency x regime similarity), but its own
// numbers do not support shipping RWC by default, and its stated conclusion is
// "TWC as a robust default, with RWC as a targeted option". Against a 1% target
// with a gradient-boosted base: TWC 1.09%, ACI 1.14%, RWC 1.14%. Regime
// weighting only pulled ahead with a WEAK base model in stress -- 2.86% against
// 3.71% in the top volatility quintile with historical simulation underneath.
//
// The reason is a bias-variance trade the paper makes explicit: regime
// similarity reduces bias by localising to comparable history, and raises
// variance by shrinking the effective sample the quantile is taken from. When
// the base model is already adaptive, time decay tracks the drift by itself and
// the extra localisation buys noise. Altair's base IS a GBDT, so TimeWeighted
// is the default here and RegimeWeighted is a mode you select and then have to
// justify with a measured exceedance rate.
//
// AND THE DECAY RATE TURNED OUT TO MATTER MORE THAN EITHER. Measured on a
// clustering-volatility series, shortening the memory from 250 bars to 25 cut
// the per-regime calibration error from 4.50 pp to 0.77 pp -- a bigger gain
// than regime weighting ever produced, and enough that regime weighting then
// made things WORSE. See `ConformalSpec::lambda` for the table. The mode is the
// parameter everybody discusses and the memory is the one that moves the
// number.
//
// CAUSALITY -- HARD RULE 7.
//
// Every score in the buffer comes from a bar whose outcome was already known
// when it was appended. `calibrate()` reads the buffer and returns a bound;
// `observe()` puts the realised score in afterwards. The two are separate calls
// so that the ordering is a property of the interface rather than of a comment,
// and a caller cannot accidentally calibrate on the point being predicted.
//
// The regime embedding is standardised by `RegimeScaler`, which is fitted on
// training data ONLY. The paper flags this explicitly, and it is the same
// train-only-fitted rule `models/dataset.hpp::Scaler` already enforces: a
// standardiser fitted over the test period leaks the test period's variance
// into every weight computed from it.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace altair {

enum class ConformalError : std::uint8_t {
    /// A spec that cannot produce a bound.
    BadParameter,
    /// The calibration buffer is empty, so there is no history to take a
    /// quantile of. The caller must publish the base forecast's own band and
    /// say it is uncalibrated -- NOT silently a zero buffer, which would read
    /// as "the model is perfectly calibrated".
    NoScores,
    /// The regime embedding has more dimensions than kMaxRegimeDim.
    EmbeddingTooWide,
    /// The embedding handed to calibrate() is a different width from the ones
    /// in the buffer. Comparing them would be comparing different quantities.
    EmbeddingWidthChanged
};

/// Maximum width of a regime embedding.
///
/// RULE 11: this bound REFUSES. A caller handing over nine regime features gets
/// EmbeddingTooWide, not the first eight silently. Truncating here would be the
/// worst of the three permitted behaviours: the weights would still be
/// computable, the quantile would still come out, and the number would be wrong
/// in a way nothing downstream could detect.
///
/// Eight is not arbitrary. The paper's own experiments use interpretable
/// low-dimensional embeddings (volatility level and trend), and the Gaussian
/// kernel is a distance in this space -- every added dimension dilutes the
/// similarity signal and shrinks the effective sample.
inline constexpr std::size_t kMaxRegimeDim = 8;

enum class ConformalMode : std::uint8_t {
    /// Unweighted quantile over the last m scores. The paper's SWC baseline.
    Sliding,
    /// Recency only: w = exp(-lambda * age). The paper's TWC, and the default.
    TimeWeighted,
    /// Recency x regime similarity. The paper's RWC.
    RegimeWeighted
};

struct ConformalSpec {
    /// Target miscoverage. 0.01 means a 99% one-sided bound, so roughly one
    /// exceedance in a hundred bars.
    double alpha = 0.05;

    /// Calibration buffer cap.
    ///
    /// RULE 11: this bound TRUNCATES, and that is the intent rather than a
    /// limitation -- the paper's I_t is explicitly the last m scores, because
    /// on a nonstationary series older errors describe a market that no longer
    /// exists. What matters is that the truncation is visible, so `dropped()`
    /// counts every score the buffer has evicted.
    std::size_t m = 500;

    /// Recency decay per bar. Larger forgets faster.
    ///
    /// The useful way to choose this is the effective memory it implies, which
    /// `ConformalStep::effective_memory` reports back: 1/lambda bars, roughly.
    ///
    /// THE DECAY MATTERS MORE THAN THE MODE, AND THAT WAS NOT OBVIOUS. This
    /// defaulted to 0.004 -- about 250 bars, one trading year -- on the
    /// reasoning that a year of errors is a natural calibration window. The
    /// measurement in models/tests/test_calibration.cpp says otherwise. On a
    /// series with clustering volatility, against a 10% target:
    ///
    ///     fixed band              reg-MAE 4.87 pp
    ///     SWC, 500 unweighted     reg-MAE 5.22 pp   (worse than no wrapper)
    ///     TWC, 250-bar memory     reg-MAE 4.50 pp
    ///     RWC, 250-bar memory     reg-MAE 2.69 pp
    ///     TWC, 25-bar memory      reg-MAE 0.77 pp
    ///     RWC, 25-bar memory      reg-MAE 1.71 pp
    ///
    /// Two things fall out. Shortening the memory beats adding the regime
    /// kernel, by a wide margin. And once the memory IS short the kernel makes
    /// things worse, because it shrinks an already-small effective sample for
    /// bias it has no room left to remove -- the localisation-variance trade
    /// of the paper's Theorem 5.4, landing on the variance side.
    ///
    /// 0.02 is about 50 bars. It is a defensible default and it is NOT a
    /// substitute for sweeping this on the actual series, which is why
    /// `CalibrationReport` carries n_eff and effective memory.
    double lambda = 0.02;

    /// Gaussian kernel bandwidth, in STANDARDISED units -- so 1.0 means "one
    /// standard deviation of the training regime distribution". Only read in
    /// RegimeWeighted mode.
    double bandwidth = 1.0;

    /// Effective-sample floor. Below this the regime kernel is dropped for
    /// that step and the weights fall back to recency only.
    ///
    /// RULE 11 AND IT IS THE INTERESTING ONE. This bound cannot refuse -- a
    /// risk system that declines to publish a bound because the market looks
    /// unfamiliar has failed at the moment it was most needed -- and it cannot
    /// prove itself unreachable, because concentrated weights are exactly what
    /// a novel regime produces. So it degrades, and the degradation is toward
    /// the SAFE side: time-only weights use MORE history, which widens the
    /// bound rather than narrowing it. Every occurrence is counted in
    /// `fallbacks()` and carried on the step in `fell_back`, because a
    /// RegimeWeighted run that silently spent half its life in TimeWeighted
    /// mode is not the method the caller thinks they measured.
    double n_min = 30.0;

    ConformalMode mode = ConformalMode::TimeWeighted;

    /// Use the paper's inflated level (its equation 5) instead of 1 - alpha.
    ///
    /// This is the finite-sample correction analogous to the usual (m+1)
    /// conformal adjustment. The paper notes the difference is negligible once
    /// the total calibration weight is large, and uses 1 - alpha in its own
    /// experiments; it is off by default here for the same reason and available
    /// because "negligible" is a claim about a sample size, not a fact.
    bool finite_sample = false;

    [[nodiscard]] bool valid() const noexcept {
        return alpha > 0.0 && alpha < 1.0 && m >= 2 && lambda >= 0.0
               && bandwidth > 0.0 && n_min >= 1.0;
    }
};

/// What one calibration step did, and how much history it really used.
///
/// These are the paper's Table 4 diagnostics, and they are returned rather than
/// logged because the localisation-variance trade is the whole argument for
/// choosing one mode over another. A buffer number with no n_eff beside it
/// cannot be reasoned about.
struct ConformalStep {
    /// The safety buffer to add to the base forecast. Same units as the
    /// conformity scores the caller supplied.
    double buffer = 0.0;
    /// Effective weighted sample size, 1 / sum of squared normalised weights.
    /// Equals the count when weights are uniform, and falls toward 1 as they
    /// concentrate on a single observation.
    double n_eff = 0.0;
    /// Weighted mean age of the scores used, in bars. The paper's "effective
    /// memory".
    double effective_memory = 0.0;
    /// Scores available to this step.
    std::size_t used = 0;
    /// The regime kernel was dropped for this step because n_eff fell below
    /// n_min. Only ever true in RegimeWeighted mode.
    bool fell_back = false;
    /// The level actually used, after any finite-sample inflation.
    double level = 0.0;
};

/// The weighted quantile the whole method rests on.
///
/// "Smallest value whose cumulative normalised weight reaches `level`" -- the
/// standard weighted-quantile definition, and the one the paper specifies.
///
/// Exposed as a free function because it is the piece most likely to be wrong
/// and the piece most easily tested against hand-computed cases. `values` and
/// `weights` must be the same length; weights must be non-negative and not all
/// zero. Returns the largest value when the level is not reached, which happens
/// only through floating-point shortfall at level 1.
[[nodiscard]] inline std::expected<double, ConformalError>
weighted_quantile(std::span<const double> values,
                  std::span<const double> weights, double level) {
    if (values.empty() || values.size() != weights.size()) {
        return std::unexpected(ConformalError::BadParameter);
    }
    if (!(level > 0.0) || level > 1.0) {
        return std::unexpected(ConformalError::BadParameter);
    }
    double total = 0.0;
    for (double w : weights) {
        if (!(w >= 0.0)) { return std::unexpected(ConformalError::BadParameter); }
        total += w;
    }
    if (!(total > 0.0)) { return std::unexpected(ConformalError::NoScores); }

    // Sort by VALUE, carrying the weight. Indices rather than pairs so the
    // caller's arrays are never modified.
    std::vector<std::size_t> order(values.size());
    for (std::size_t i = 0; i < order.size(); ++i) { order[i] = i; }
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return values[a] < values[b]; });

    const double want = level * total;
    double cum = 0.0;
    for (std::size_t i : order) {
        cum += weights[i];
        if (cum >= want) { return values[i]; }
    }
    return values[order.back()];
}

/// Standardises a regime embedding using TRAINING-PERIOD statistics only.
///
/// The kernel is a Euclidean distance, so a coordinate measured in basis points
/// and a coordinate measured in units would make the second invisible. Fitted
/// once, on data that precedes everything it is then applied to -- the same
/// contract as models/dataset.hpp::Scaler, for the same reason.
class RegimeScaler {
public:
    /// `rows` is a flat row-major matrix of `n` rows and `dim` columns.
    [[nodiscard]] std::expected<void, ConformalError>
    fit(std::span<const double> rows, std::size_t dim) {
        if (dim == 0 || dim > kMaxRegimeDim) {
            return std::unexpected(ConformalError::EmbeddingTooWide);
        }
        if (rows.empty() || rows.size() % dim != 0) {
            return std::unexpected(ConformalError::BadParameter);
        }
        const std::size_t n = rows.size() / dim;
        dim_ = dim;
        mean_.assign(dim, 0.0);
        sd_.assign(dim, 1.0);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t d = 0; d < dim; ++d) { mean_[d] += rows[i * dim + d]; }
        }
        for (double& m : mean_) { m /= static_cast<double>(n); }
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t d = 0; d < dim; ++d) {
                const double e = rows[i * dim + d] - mean_[d];
                sd_[d] += e * e;
            }
        }
        for (std::size_t d = 0; d < dim; ++d) {
            // The -1.0 undoes the 1.0 the vector was seeded with; seeding then
            // correcting keeps the "a constant column scales by 1, not by 0"
            // guarantee below in one place.
            const double var = (sd_[d] - 1.0) / (n > 1 ? (n - 1.0) : 1.0);
            // A CONSTANT COLUMN IS NOT AN ERROR AND MUST NOT DIVIDE BY ZERO.
            // It carries no information, so it is scaled by 1 and contributes
            // a constant zero to every distance -- which is exactly the
            // influence a feature that never varies should have.
            sd_[d] = var > 1e-12 ? std::sqrt(var) : 1.0;
        }
        fitted_ = true;
        return {};
    }

    [[nodiscard]] bool fitted() const noexcept { return fitted_; }
    [[nodiscard]] std::size_t dim() const noexcept { return dim_; }

    /// Standardise one embedding in place into `out`, which must be `dim()`
    /// long. An unfitted scaler passes the values through unchanged, so a
    /// caller who forgot to fit gets raw units rather than zeros.
    void transform(std::span<const double> z, std::span<double> out) const {
        for (std::size_t d = 0; d < z.size() && d < out.size(); ++d) {
            out[d] = fitted_ && d < dim_ ? (z[d] - mean_[d]) / sd_[d] : z[d];
        }
    }

private:
    std::vector<double> mean_;
    std::vector<double> sd_;
    std::size_t dim_ = 0;
    bool fitted_ = false;
};

/// Sequential conformal calibration. Algorithm 1 of the paper.
///
/// Usage is strictly alternating, and that is the causality guarantee:
///
///     auto step = c.calibrate(z_now);     // bound for the bar not yet seen
///     ... publish forecast + step->buffer ...
///     c.observe(actual - forecast, z_now);   // only once the bar has closed
class Conformal {
public:
    explicit Conformal(ConformalSpec spec) : spec_(spec) {}

    [[nodiscard]] const ConformalSpec& spec() const noexcept { return spec_; }
    [[nodiscard]] std::size_t size() const noexcept { return scores_.size(); }
    /// Scores evicted by the buffer cap. Rule 11: the truncation is counted.
    [[nodiscard]] std::size_t dropped() const noexcept { return dropped_; }
    /// Steps that fell back from regime weights to time-only weights.
    [[nodiscard]] std::size_t fallbacks() const noexcept { return fallbacks_; }

    /// The safety buffer for the bar that has NOT happened yet.
    ///
    /// `z_now` is the current regime embedding, already standardised by the
    /// caller's RegimeScaler. It is ignored in Sliding and TimeWeighted modes
    /// and may be empty there.
    [[nodiscard]] std::expected<ConformalStep, ConformalError>
    calibrate(std::span<const double> z_now) {
        if (!spec_.valid()) { return std::unexpected(ConformalError::BadParameter); }
        if (scores_.empty()) { return std::unexpected(ConformalError::NoScores); }
        if (spec_.mode == ConformalMode::RegimeWeighted) {
            if (z_now.size() > kMaxRegimeDim) {
                return std::unexpected(ConformalError::EmbeddingTooWide);
            }
            if (dim_ != 0 && z_now.size() != dim_) {
                return std::unexpected(ConformalError::EmbeddingWidthChanged);
            }
        }

        const std::size_t n = scores_.size();
        std::vector<double> w(n, 0.0);

        // Age in bars: the newest score is 1 away from the bar being predicted,
        // matching the paper's exp(-lambda * (t - i)) with i = t-1 newest.
        // Scores are held oldest-first, so age counts down.
        const auto recency = [&](std::size_t i) {
            const double age = static_cast<double>(n - i);
            return std::exp(-spec_.lambda * age);
        };

        bool fell_back = false;
        if (spec_.mode == ConformalMode::Sliding) {
            std::fill(w.begin(), w.end(), 1.0);
        } else if (spec_.mode == ConformalMode::TimeWeighted) {
            for (std::size_t i = 0; i < n; ++i) { w[i] = recency(i); }
        } else {
            const double h2 = 2.0 * spec_.bandwidth * spec_.bandwidth;
            for (std::size_t i = 0; i < n; ++i) {
                double d2 = 0.0;
                for (std::size_t d = 0; d < dim_; ++d) {
                    const double e = embeddings_[i * dim_ + d] - z_now[d];
                    d2 += e * e;
                }
                w[i] = recency(i) * std::exp(-d2 / h2);
            }
            // THE KERNEL CAN ANNIHILATE EVERY WEIGHT. A regime unlike anything
            // in the buffer underflows every exp() to zero, and the normalised
            // weights would be 0/0. That is the n_eff floor's job, and it is
            // also why the total is checked before dividing rather than after.
            double total = 0.0;
            for (double v : w) { total += v; }
            double ess = 0.0;
            if (total > 0.0) {
                double sq = 0.0;
                for (double v : w) { const double p = v / total; sq += p * p; }
                ess = sq > 0.0 ? 1.0 / sq : 0.0;
            }
            if (!(total > 0.0) || ess < spec_.n_min) {
                for (std::size_t i = 0; i < n; ++i) { w[i] = recency(i); }
                fell_back = true;
                ++fallbacks_;
            }
        }

        double total = 0.0;
        for (double v : w) { total += v; }
        if (!(total > 0.0)) {
            // Only reachable if lambda is so large that even the newest weight
            // underflows. Uniform is the safe answer: more history, wider bound.
            std::fill(w.begin(), w.end(), 1.0);
            total = static_cast<double>(n);
        }

        ConformalStep step{};
        step.used = n;
        step.fell_back = fell_back;

        double sq = 0.0, age_w = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double p = w[i] / total;
            sq += p * p;
            age_w += p * static_cast<double>(n - i);
        }
        step.n_eff = sq > 0.0 ? 1.0 / sq : 0.0;
        step.effective_memory = age_w;

        // The paper's equation (5). The current point's own weight is 1 in its
        // construction, so the inflation is (1 + 1/W) with W the total weight.
        double level = 1.0 - spec_.alpha;
        if (spec_.finite_sample) {
            level = std::min(1.0, level * (1.0 + 1.0 / total));
        }
        step.level = level;

        const auto q = weighted_quantile(scores_, w, level);
        if (!q) { return std::unexpected(q.error()); }
        step.buffer = *q;
        return step;
    }

    /// Record what actually happened. `score` is the conformity score, which
    /// for a one-sided upper bound is `actual - forecast`.
    [[nodiscard]] std::expected<void, ConformalError>
    observe(double score, std::span<const double> z) {
        if (spec_.mode == ConformalMode::RegimeWeighted) {
            if (z.size() > kMaxRegimeDim) {
                return std::unexpected(ConformalError::EmbeddingTooWide);
            }
            if (z.empty()) { return std::unexpected(ConformalError::BadParameter); }
            if (dim_ == 0) { dim_ = z.size(); }
            if (z.size() != dim_) {
                return std::unexpected(ConformalError::EmbeddingWidthChanged);
            }
        }
        scores_.push_back(score);
        if (spec_.mode == ConformalMode::RegimeWeighted) {
            embeddings_.insert(embeddings_.end(), z.begin(), z.end());
        }
        // Evict oldest-first past the cap, and COUNT it.
        while (scores_.size() > spec_.m) {
            scores_.erase(scores_.begin());
            if (dim_ > 0 && embeddings_.size() >= dim_) {
                embeddings_.erase(embeddings_.begin(),
                                  embeddings_.begin()
                                      + static_cast<std::ptrdiff_t>(dim_));
            }
            ++dropped_;
        }
        return {};
    }

private:
    ConformalSpec spec_;
    std::vector<double> scores_;      // oldest first
    std::vector<double> embeddings_;  // row-major, dim_ wide, parallel to scores_
    std::size_t dim_ = 0;
    std::size_t dropped_ = 0;
    std::size_t fallbacks_ = 0;
};

}  // namespace altair
