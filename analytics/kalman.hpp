// analytics/kalman.hpp -- the Kalman filter, in the form that survives
// contact with floating point.
//
// P14-04.
//
// THE JOSEPH FORM IS NOT AN OPTIMISATION. IT IS THE POINT.
//
// The textbook covariance update is
//
//     P = (I - K H) P
//
// which is algebraically correct and numerically radioactive. It is not
// symmetric in its own arithmetic, so rounding pushes P off the symmetric
// manifold; once P has a negative eigenvalue the filter is claiming NEGATIVE
// VARIANCE, the gain goes wrong, and the state diverges. It does this
// silently, after thousands of updates, in production, on the day the input
// gets noisy. The Joseph form
//
//     P = (I - K H) P (I - K H)' + K R K'
//
// is a sum of two quadratic forms, so it is symmetric and positive
// semi-definite BY CONSTRUCTION for any K -- including a K that is slightly
// wrong. It costs a few more multiplies and it removes an entire class of
// failure. That trade is not close.
//
// THE INNOVATION IS THE DIAGNOSTIC, AND ALMOST NOBODY READS IT.
//
// A Kalman filter always produces an answer. Whether that answer means
// anything is decided by the INNOVATION sequence -- the one-step prediction
// errors. If the model is right they are white, zero-mean, and have exactly
// the variance the filter predicted (S). If they are autocorrelated, the state
// model is wrong. If their variance is larger than S, the filter is
// over-confident and every downstream interval is too tight.
//
// So `Kalman1D` accumulates the NORMALISED innovation squared,
//
//     NIS = v^2 / S
//
// whose mean is 1.0 under a correct model. That number is the whole
// credibility of the filter and it is one division per update, so there is no
// excuse for not having it.
//
// A ONE-DIMENSIONAL STATE, DELIBERATELY.
//
// The general n-dimensional filter needs matrix inversion and an allocation
// strategy, and the two things this project actually wants a Kalman for -- a
// time-varying hedge ratio and a slowly-drifting level -- are both scalar. A
// scalar filter has no matrix inverse, cannot allocate, and fits in
// ALTAIR_HOT. When a genuine multivariate need appears it gets its own card
// rather than a template parameter added here in anticipation.

#pragma once

#include <cmath>
#include <cstdint>
#include <expected>

namespace altair {

enum class KalmanError : std::uint8_t {
    /// A variance was given as zero or negative.
    BadVariance,
    /// The innovation variance came out non-positive, which means the filter
    /// has already lost positive-definiteness.
    Degenerate,
    /// Nothing has been observed yet.
    NoObservations
};

/// Scalar state, scalar observation:
///
///     x_t = x_{t-1} + w,     w ~ N(0, q)      the state drifts
///     y_t = h_t * x_t + v,   v ~ N(0, r)      what we see
///
/// `h_t` is supplied per observation, which is what lets the same filter be a
/// level tracker (h = 1) and a regression on a moving regressor (h = the
/// regressor) without a second class.
class Kalman1D {
public:
    /// `q` is process variance: how fast the state is allowed to move. `r` is
    /// observation variance: how noisy the measurement is. Their RATIO is the
    /// only thing that matters to the steady-state gain -- doubling both
    /// changes nothing -- which is worth knowing before tuning either.
    [[nodiscard]] static std::expected<Kalman1D, KalmanError>
    make(double x0, double p0, double q, double r) noexcept {
        if (!(p0 > 0.0) || !(q > 0.0) || !(r > 0.0)) {
            return std::unexpected(KalmanError::BadVariance);
        }
        Kalman1D k;
        k.x_ = x0;
        k.p_ = p0;
        k.q_ = q;
        k.r_ = r;
        return k;
    }

    /// One predict-update cycle against observation `y` with regressor `h`.
    ///
    /// PREDICT FIRST, ALWAYS. The prediction uses only what was known before
    /// `y` arrived, which is what makes the innovation an out-of-sample error
    /// and keeps rule 7: a filter that updated before predicting would report
    /// a one-step error it had already seen the answer to.
    [[nodiscard]] std::expected<void, KalmanError>
    update(double y, double h) noexcept {
        // Predict. The state model is a random walk, so the mean is unchanged
        // and only the uncertainty grows.
        const double p_pred = p_ + q_;

        // Innovation and its variance.
        const double v = y - h * x_;
        const double s = h * h * p_pred + r_;
        if (!(s > 0.0)) { return std::unexpected(KalmanError::Degenerate); }

        const double k = p_pred * h / s;
        x_ = x_ + k * v;

        // JOSEPH FORM. See the header -- this is why the filter does not
        // quietly die after ten thousand updates.
        const double ikh = 1.0 - k * h;
        p_ = ikh * p_pred * ikh + k * r_ * k;

        last_v_ = v;
        last_s_ = s;
        nis_sum_ += v * v / s;
        ++n_;
        return {};
    }

    [[nodiscard]] double state() const noexcept { return x_; }
    [[nodiscard]] double variance() const noexcept { return p_; }
    [[nodiscard]] double innovation() const noexcept { return last_v_; }
    [[nodiscard]] double innovation_variance() const noexcept { return last_s_; }
    [[nodiscard]] std::size_t observations() const noexcept { return n_; }

    /// Mean normalised innovation squared. ONE under a correctly specified
    /// model. Materially above one means the filter is over-confident -- its
    /// error bars are too tight and anything sized off them is oversized.
    /// Materially below one means it is over-cautious and is ignoring data it
    /// could be using.
    [[nodiscard]] std::expected<double, KalmanError> nis() const noexcept {
        if (n_ == 0) { return std::unexpected(KalmanError::NoObservations); }
        return nis_sum_ / static_cast<double>(n_);
    }

private:
    double x_ = 0.0;
    double p_ = 1.0;
    double q_ = 1.0;
    double r_ = 1.0;
    double last_v_ = 0.0;
    double last_s_ = 0.0;
    double nis_sum_ = 0.0;
    std::size_t n_ = 0;
};

} // namespace altair
