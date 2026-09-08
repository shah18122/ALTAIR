// flagging/drift.hpp -- PSI, KS, ADWIN and Page-Hinkley.
//
// P9-03 and P9-04. Two kinds of drift, and they are not the same kind.
//
//   DATA drift   the FEATURES have moved. PSI and KS answer this, by
//                comparing a live window's distribution against a reference.
//   CONCEPT drift the relationship has moved. The features may look identical
//                and the model is now wrong. ADWIN and Page-Hinkley answer
//                this, by watching the ERROR stream.
//
// A system with only the first is blind to a model that has stopped working on
// data it still recognises, which is the ordinary way a strategy dies.
//
// MEASURING DRIFT ON STANDARDISED FEATURES MEASURES NOTHING.
//
// This is the card, and it is P8-02's leak wearing the opposite sign.
//
// The features a model consumes have been through `Scaler::transform`, and
// that scaler was fitted on the training window. Standardising subtracts the
// reference mean and divides by the reference standard deviation -- which is
// precisely the transform that maps the reference distribution onto a standard
// normal. Run PSI on the OUTPUT of that and the reference bins are, by
// construction, the bins of a standard normal, and a live window that has
// shifted has been shifted back before it was measured.
//
// The drift is not removed from the model's input -- the model still gets the
// shifted values, and it is still wrong. It is removed from the MEASUREMENT.
//
// Measured, and the honest result is narrower than the slogan. A LOCATION
// shift survives standardisation, because subtracting a fixed mean cannot hide
// a move: PSI 0.9030 raw and 0.9030 standardised. What it hides is a change of
// SCALE, once the scaler is refitted per window -- which is exactly what an
// "always standardise your inputs" pipeline does:
//
//     tripled spread, PSI on raw features                       0.9508
//     tripled spread, PSI after refitting the scaler per window 0.0102
//
// Ninety-three times smaller. The drift is not removed from the model's INPUT
// -- the model still receives the shifted values and is still wrong. It is
// removed from the MEASUREMENT.
//
// So every detector here takes RAW features, and `PsiDetector` refuses to fit
// until it has been told which it is being given: it cannot tell by looking,
// because standardised features look exactly like raw ones with a different
// mean.
//
// THE REFERENCE BINS ARE FITTED ONCE AND REUSED.
//
// The second way to measure nothing. Fitting quantile bins separately on the
// reference and the live window makes both look uniform by construction, and
// PSI comes out near zero however far the distribution has moved. The bins
// belong to the reference and the live window is poured into them.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

// RENAMED from the shorter name, P23-08. models/gbdt.hpp declares an
// altair::kMaxBins of its own -- 64, for histogram binning -- so any
// translation unit including both headers failed to compile with an ODR
// collision on a name in the same namespace. Nothing had included both until
// the UI wired a drift page beside a tree page. Two unrelated concepts had
// quietly agreed on a name.
inline constexpr std::size_t kMaxDriftBins = 32;
inline constexpr std::size_t kMaxDriftWindow = 4096;

enum class DriftError : std::uint8_t {
    /// The reference window was never fitted.
    NoReference,
    /// Fewer samples than the statistic needs.
    TooFewSamples,
    /// The detector was told nothing about whether its input is raw.
    UnknownScaling,
    /// A threshold was not supplied.
    NoThreshold,
    /// The bin count was zero or exceeded capacity.
    BadBins
};

/// Whether the values handed to a detector have been standardised.
///
/// No default, and `Standardised` is accepted only so the mistake can be
/// measured. A detector cannot tell by looking -- standardised features look
/// exactly like raw ones with a different mean.
enum class Scaling : std::uint8_t {
    Unspecified = 0,
    /// Straight from the feed. The only input a drift number means anything on.
    Raw,
    /// Through a scaler fitted on the reference window. See the header.
    Standardised
};

// ---------------------------------------------------------------------------
// P9-03: PSI and KS
// ---------------------------------------------------------------------------

/// Population Stability Index against a fixed reference.
///
/// The bins are quantiles of the REFERENCE and are frozen at fit time. A live
/// window is poured into them; it never gets bins of its own.
class PsiDetector {
public:
    /// Fit the reference. `values` is modified (sorted in place) -- taking a
    /// mutable span rather than copying is deliberate, since a reference
    /// window is thousands of doubles and this runs at session boundaries.
    [[nodiscard]] std::expected<void, DriftError>
    fit(double* values, std::size_t n, std::size_t bins,
        Scaling s) noexcept {
        if (s == Scaling::Unspecified) {
            return std::unexpected(DriftError::UnknownScaling);
        }
        if (bins < 2 || bins > kMaxDriftBins) {
            return std::unexpected(DriftError::BadBins);
        }
        if (n < bins * 4) {
            return std::unexpected(DriftError::TooFewSamples);
        }
        for (std::size_t i = 1; i < n; ++i) {      // insertion sort
            const double v = values[i];
            std::size_t j = i;
            while (j > 0 && values[j - 1] > v) { values[j] = values[j - 1]; --j; }
            values[j] = v;
        }
        bins_ = bins;
        scaling_ = s;
        for (std::size_t b = 1; b < bins; ++b) {
            edge_[b - 1] = values[n * b / bins];
        }
        // Reference proportions, with a floor so an empty reference bin does
        // not make the PSI infinite. The floor is 1/(4n), which is smaller
        // than any real proportion and large enough to keep the log finite.
        const double floor_p = 0.25 / static_cast<double>(n);
        for (std::size_t b = 0; b < bins; ++b) { ref_[b] = 0.0; }
        for (std::size_t i = 0; i < n; ++i) { ref_[bin_of(values[i])] += 1.0; }
        for (std::size_t b = 0; b < bins; ++b) {
            ref_[b] /= static_cast<double>(n);
            if (ref_[b] < floor_p) { ref_[b] = floor_p; }
        }
        fitted_ = true;
        return {};
    }

    /// PSI of a live window against the frozen reference.
    ///
    /// Sum over bins of (p_live - p_ref) * ln(p_live / p_ref). The
    /// conventional readings are 0.1 for "worth looking at" and 0.25 for
    /// "the population has moved", and both are conventions rather than
    /// facts -- which is why `psi` returns the number and the threshold lives
    /// with the caller.
    [[nodiscard]] std::expected<double, DriftError>
    psi(const double* live, std::size_t n) const noexcept {
        if (!fitted_) { return std::unexpected(DriftError::NoReference); }
        if (n < bins_ * 2) {
            return std::unexpected(DriftError::TooFewSamples);
        }
        double cnt[kMaxDriftBins] = {};
        for (std::size_t i = 0; i < n; ++i) { cnt[bin_of(live[i])] += 1.0; }
        const double floor_p = 0.25 / static_cast<double>(n);
        double acc = 0.0;
        for (std::size_t b = 0; b < bins_; ++b) {
            double p = cnt[b] / static_cast<double>(n);
            if (p < floor_p) { p = floor_p; }
            acc += (p - ref_[b]) * std::log(p / ref_[b]);
        }
        return acc;
    }

    [[nodiscard]] bool fitted() const noexcept { return fitted_; }
    /// What the caller said about its input. A PSI computed on standardised
    /// values travels with that fact attached, because the number alone gives
    /// no hint.
    [[nodiscard]] Scaling scaling() const noexcept { return scaling_; }
    [[nodiscard]] std::size_t bins() const noexcept { return bins_; }

private:
    [[nodiscard]] std::size_t bin_of(double v) const noexcept {
        std::size_t b = 0;
        while (b + 1 < bins_ && v > edge_[b]) { ++b; }
        return b;
    }
    double edge_[kMaxDriftBins] = {};
    double ref_[kMaxDriftBins] = {};
    std::size_t bins_ = 0;
    Scaling scaling_ = Scaling::Unspecified;
    bool fitted_ = false;
};

/// Two-sample Kolmogorov-Smirnov statistic.
///
/// The largest gap between two empirical CDFs. Complements PSI: PSI is
/// sensitive to a shift in mass between bins and KS to the worst single point
/// of divergence, so a distribution that moves without changing its binned
/// shape shows in one and not the other.
///
/// Both arrays are sorted in place.
[[nodiscard]] inline std::expected<double, DriftError>
ks_statistic(double* a, std::size_t na, double* b, std::size_t nb) noexcept {
    if (na < 8 || nb < 8) {
        return std::unexpected(DriftError::TooFewSamples);
    }
    auto sort = [](double* v, std::size_t n) {
        for (std::size_t i = 1; i < n; ++i) {
            const double x = v[i];
            std::size_t j = i;
            while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; --j; }
            v[j] = x;
        }
    };
    sort(a, na);
    sort(b, nb);
    std::size_t i = 0, j = 0;
    double d = 0.0;
    while (i < na && j < nb) {
        const double x = a[i] < b[j] ? a[i] : b[j];
        while (i < na && a[i] <= x) { ++i; }
        while (j < nb && b[j] <= x) { ++j; }
        const double gap = std::fabs(static_cast<double>(i)
                                         / static_cast<double>(na)
                                     - static_cast<double>(j)
                                         / static_cast<double>(nb));
        if (gap > d) { d = gap; }
    }
    return d;
}

/// The KS critical value at a given significance, for two samples.
///
/// c(alpha) * sqrt((na+nb)/(na*nb)). Provided so a caller compares against a
/// SIZE rather than against a remembered number -- the critical value shrinks
/// with sample size, so a KS of 0.08 is decisive on ten thousand points and
/// meaningless on fifty.
[[nodiscard]] inline double ks_critical(std::size_t na, std::size_t nb,
                                        double c_alpha) noexcept {
    if (na == 0 || nb == 0) { return 1.0; }
    const double a = static_cast<double>(na), b = static_cast<double>(nb);
    return c_alpha * std::sqrt((a + b) / (a * b));
}

// ---------------------------------------------------------------------------
// P9-04: ADWIN and Page-Hinkley
// ---------------------------------------------------------------------------

/// Page-Hinkley: a cumulative test for a shift in the MEAN of a stream.
///
/// Watches the running deviation from the running mean minus a tolerance, and
/// fires when the excursion from its own minimum exceeds a threshold. Cheap,
/// O(1) state, and it detects a persistent mean shift quickly.
///
/// What it does NOT detect is a change in variance or shape at a constant
/// mean, which is why ADWIN is here too.
class PageHinkley {
public:
    /// `delta` is the tolerance -- how large a drift is uninteresting.
    /// `lambda` is the alarm threshold. Neither has a default: a tolerance of
    /// zero fires on any deviation at all, and a threshold is a statement
    /// about how much evidence justifies pulling a model out of production.
    [[nodiscard]] std::expected<void, DriftError>
    configure(double delta, double lambda) noexcept {
        if (!(delta >= 0.0) || !(lambda > 0.0)) {
            return std::unexpected(DriftError::NoThreshold);
        }
        delta_ = delta;
        lambda_ = lambda;
        configured_ = true;
        return {};
    }

    /// Feed one observation. Returns true when the alarm fires.
    [[nodiscard]] std::expected<bool, DriftError> push(double x) noexcept {
        if (!configured_) { return std::unexpected(DriftError::NoThreshold); }
        ++n_;
        mean_ += (x - mean_) / static_cast<double>(n_);
        // Cumulative deviation, minus the tolerance. Only INCREASES matter
        // here; for a two-sided test run two detectors on x and -x, which is
        // clearer than folding an absolute value into one and losing the
        // direction.
        cum_ += x - mean_ - delta_;
        if (cum_ < min_) { min_ = cum_; }
        return (cum_ - min_) > lambda_;
    }

    void reset() noexcept {
        n_ = 0;
        mean_ = 0.0;
        cum_ = 0.0;
        min_ = 0.0;
    }
    [[nodiscard]] double statistic() const noexcept { return cum_ - min_; }
    [[nodiscard]] std::size_t count() const noexcept { return n_; }

private:
    double delta_ = 0.0;
    double lambda_ = 0.0;
    double mean_ = 0.0;
    double cum_ = 0.0;
    double min_ = 0.0;
    std::size_t n_ = 0;
    bool configured_ = false;
};

/// ADWIN: an adaptive window that shrinks when its two halves disagree.
///
/// Keeps a window of recent observations and, on every insertion, looks for a
/// split point where the means of the two sub-windows differ by more than a
/// Hoeffding bound allows. When it finds one it drops the older half -- so the
/// window itself is the answer: its length is how far back the current
/// behaviour extends.
///
/// That is the property Page-Hinkley does not have. Page-Hinkley says "it
/// changed"; ADWIN says "and everything before here no longer applies", which
/// is what a retraining trigger actually needs.
template <std::size_t Cap = kMaxDriftWindow>
class Adwin {
public:
    /// `confidence` is the delta in the Hoeffding bound: smaller means more
    /// evidence needed. No default.
    [[nodiscard]] std::expected<void, DriftError>
    configure(double confidence) noexcept {
        if (!(confidence > 0.0) || !(confidence < 1.0)) {
            return std::unexpected(DriftError::NoThreshold);
        }
        delta_ = confidence;
        configured_ = true;
        return {};
    }

    /// Insert one observation. Returns true when the window was cut.
    [[nodiscard]] std::expected<bool, DriftError> push(double x) noexcept {
        if (!configured_) { return std::unexpected(DriftError::NoThreshold); }
        if (n_ == Cap) {
            // Drop the oldest to make room. A full window is not a drift
            // event; it is a capacity limit, and conflating them would make
            // the detector fire on a schedule.
            for (std::size_t i = 1; i < n_; ++i) { buf_[i - 1] = buf_[i]; }
            --n_;
        }
        buf_[n_++] = x;
        if (n_ < 16) { return false; }

        // Look for a split whose halves differ by more than the bound.
        double total = 0.0;
        for (std::size_t i = 0; i < n_; ++i) { total += buf_[i]; }
        double left = 0.0;
        for (std::size_t cut = 1; cut + 1 < n_; ++cut) {
            left += buf_[cut - 1];
            const double n0 = static_cast<double>(cut);
            const double n1 = static_cast<double>(n_ - cut);
            if (n0 < 5.0 || n1 < 5.0) { continue; }
            const double m0 = left / n0;
            const double m1 = (total - left) / n1;
            // Hoeffding bound with the harmonic mean of the two sizes.
            const double m = 1.0 / (1.0 / n0 + 1.0 / n1);
            const double eps = std::sqrt(std::log(2.0 / delta_) / (2.0 * m));
            if (std::fabs(m0 - m1) > eps) {
                // CUT. Everything before the split no longer describes the
                // present, so it goes.
                for (std::size_t i = cut; i < n_; ++i) { buf_[i - cut] = buf_[i]; }
                n_ -= cut;
                ++cuts_;
                return true;
            }
        }
        return false;
    }

    /// How far back the current behaviour extends. THE output.
    [[nodiscard]] std::size_t window() const noexcept { return n_; }
    [[nodiscard]] std::size_t cuts() const noexcept { return cuts_; }
    [[nodiscard]] double mean() const noexcept {
        if (n_ == 0) { return 0.0; }
        double s = 0.0;
        for (std::size_t i = 0; i < n_; ++i) { s += buf_[i]; }
        return s / static_cast<double>(n_);
    }

private:
    double buf_[Cap] = {};
    double delta_ = 0.0;
    std::size_t n_ = 0;
    std::size_t cuts_ = 0;
    bool configured_ = false;
};

} // namespace altair
