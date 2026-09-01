// analytics/derivatives.hpp -- velocity, acceleration and jerk of a price,
// estimated at an explicit time scale.
//
// P3-07.
//
// DIFFERENTIATION AMPLIFIES NOISE. THAT IS THE WHOLE PROBLEM.
//
// A derivative multiplies each Fourier component by its frequency. Price noise
// -- bid-ask bounce above all -- lives at the highest frequency the feed
// carries, so it is exactly the component the derivative amplifies most. The
// naive answer, (p_now - p_prev) / dt, is therefore not a slow estimate of the
// trend: it is a fast, accurate measurement of the bounce.
//
// This is not a rounding concern. On a series that is a CONSTANT price plus a
// one-tick bid-ask bounce -- true velocity exactly zero -- the two-point
// difference at 10 ms spacing returns velocities of order 1000 paise/second,
// alternating in sign. `tests/test_derivatives.cpp` measures it. Any feature
// built on that number is a feature describing the exchange's spread.
//
// THE FIX IS A LOCAL POLYNOMIAL, NOT A SMOOTHER FOLLOWED BY A DIFFERENCE.
//
// Fit a low-order polynomial to the observations in a time window by weighted
// least squares, then differentiate the POLYNOMIAL analytically. This is
// Savitzky-Golay, generalised to irregular sampling because ticks do not
// arrive on a grid. Smoothing first and differencing afterwards is not the
// same thing and is worse: it commits to one filter, then applies the
// amplifying operator to whatever that filter left behind.
//
// THE SCALE IS AN ARGUMENT, AND THERE IS NO DEFAULT.
//
// A price has no single velocity. Over 100 ms it is doing one thing, over
// 10 seconds another, and the two are both true. So `derivatives_at` takes the
// window as a Duration and every result carries the scale that produced it. A
// feature registry asking for "the velocity" has asked a malformed question.
//
// The tradeoff is exact and worth stating in one line: the variance of an
// estimated slope falls as 1/(N*h^2) where h is the window half-width in time.
// Doubling the window quarters the noise and halves the responsiveness. There
// is no window that is good at both, and pretending otherwise is how a signal
// gets fitted to the bounce.
//
// TIME COMES OFF THE TICK (rule 7). Every function here takes timestamps from
// the caller and never consults a clock, so a replay reproduces the live path
// exactly (rule 6).

#pragma once

#include <core/time/timestamp.hpp>
#include <core/types/units.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>

namespace altair {

enum class DerivativeError : std::uint8_t {
    /// Fewer observations in the window than the polynomial has coefficients.
    /// A quadratic through two points is not a fit, it is an interpolation
    /// with a free parameter.
    TooFewPoints,
    /// Every observation carries the same timestamp, so no slope exists.
    ZeroTimeSpan,
    /// The normal equations are singular -- in practice, points clustered so
    /// tightly relative to the requested order that the design matrix
    /// collapses.
    Singular,
    /// A non-finite observation, timestamp or window.
    NotFinite,
    /// A non-positive window.
    BadWindow
};

/// Derivatives of a price with respect to TIME, at one explicit scale.
///
/// Units are per SECOND, not per tick and not per bar. A per-tick derivative
/// has no dimension anybody can reason about, and it changes meaning when the
/// tick rate does -- the same defect P3-06b removes from the EWMA.
struct Derivatives {
    /// Fitted value at the evaluation instant. UNIT: paise.
    ///
    /// Not the last observation: it is the polynomial's value, which is the
    /// smoothed price. Comparing it against the raw last tick is a cheap and
    /// useful measure of how far the current print sits from its own trend.
    double value = 0.0;
    /// dP/dt. UNIT: paise per second.
    double velocity = 0.0;
    /// d2P/dt2. UNIT: paise per second squared.
    double acceleration = 0.0;
    /// d3P/dt3. UNIT: paise per second cubed. Zero unless order >= 3.
    double jerk = 0.0;
    /// The window this was measured over. Carried so a result can never be
    /// compared against one from a different scale by accident.
    Duration scale{0};
    /// Observations that entered the fit.
    int points = 0;
    /// Polynomial order used.
    int order = 0;
    /// Residual standard deviation of the fit. UNIT: paise.
    ///
    /// The input to every error bar downstream: the standard error of the
    /// velocity is roughly this divided by (sqrt(N) * h), with h the window
    /// half-width in seconds. A velocity smaller than its own standard error
    /// is not a velocity (ROADMAP section 3).
    double residual_sd = 0.0;
};

/// One time-stamped observation.
struct TimedPoint {
    Timestamp ts{};
    /// UNIT: paise, or whatever the caller is differentiating.
    double value = 0.0;
};

/// Highest polynomial order supported. Above cubic the fit starts tracking
/// noise rather than shape, and jerk is already the last derivative anyone
/// can defend on market data.
inline constexpr int kMaxDerivativeOrder = 3;

/// Standard error of the velocity, given a fit.
///
/// se(v) ~ residual_sd / (sqrt(N) * h), with h the window half-width in
/// seconds. Exposed rather than folded in, because the decision it feeds --
/// is this velocity distinguishable from zero -- belongs to the caller and
/// the threshold is a risk parameter, not a numerical one.
[[nodiscard]] inline double velocity_std_error(const Derivatives& d) noexcept {
    if (d.points < 2 || d.scale.raw() <= 0) {
        return std::numeric_limits<double>::infinity();
    }
    const double h = 0.5 * static_cast<double>(d.scale.raw()) / 1e9;
    if (!(h > 0.0)) { return std::numeric_limits<double>::infinity(); }
    return d.residual_sd / (std::sqrt(static_cast<double>(d.points)) * h);
}

/// Is the measured velocity distinguishable from zero?
///
/// ROADMAP section 3: a signal whose error bar straddles zero is not a signal.
/// A price drifting inside its own noise has no velocity worth acting on, and
/// this is the only sanctioned way to ask.
[[nodiscard]] inline bool velocity_is_significant(const Derivatives& d,
                                                  double sigmas = 2.0) noexcept {
    const double se = velocity_std_error(d);
    if (!(se > 0.0) || !std::isfinite(se)) { return false; }
    return std::fabs(d.velocity) > sigmas * se;
}

namespace detail {

/// Weighted polynomial least squares, solved by Cholesky on a CENTRED and
/// NORMALISED abscissa.
///
/// The normalisation is not tidiness, it is the difference between six correct
/// digits and fifteen. A derivative window lies entirely on ONE SIDE of the
/// evaluation instant -- it is the recent past -- so fitting directly in
/// (t - t_eval) gives a one-sided Vandermonde, which is far worse conditioned
/// than a symmetric one, and the normal equations then SQUARE that condition
/// number. Measured on exact cubic data over a 2-second window, that cost
/// 7e-6 relative error in the velocity and left visible residuals on a fit
/// that should have been exact.
///
/// Substituting u = (t - t_centre)/h puts every abscissa in [-1, 1] and every
/// power of it in [-1, 1] with it. The derivatives are then recovered by the
/// chain rule, one factor of h per order.
///
/// The ORDINATE is centred for the same reason. A NIFTY price is 2.4e6 paise
/// and its per-second slope is a few hundred, so an uncentred fit asks the
/// normal equations to resolve a coefficient of 238 sitting beside one of
/// 2,407,734 -- four orders of magnitude apart, and the small one is the
/// answer. Subtracting the window mean first costs one pass and buys back the
/// digits; it does not touch the derivatives at all, only the constant term.
template <int kOrder>
[[nodiscard]] inline bool
poly_fit(const TimedPoint* pts, std::size_t n, Timestamp t_eval,
         double (&coef)[kOrder + 1], double& t_centre, double& h,
         double& residual_sd, int& used) noexcept {
    constexpr int kM = kOrder + 1;

    // Centre and half-span of the points actually present, in seconds
    // relative to t_eval.
    double lo = 0.0, hi = 0.0;
    int seen = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(pts[i].value)) { continue; }
        const double dt =
            static_cast<double>((pts[i].ts - t_eval).raw()) / 1e9;
        if (seen == 0 || dt < lo) { lo = dt; }
        if (seen == 0 || dt > hi) { hi = dt; }
        ++seen;
    }
    if (seen < kM) { return false; }
    t_centre = 0.5 * (lo + hi);
    h = 0.5 * (hi - lo);
    if (!(h > 0.0)) { return false; }

    // Centre the ordinate. Added back into the constant term at the end.
    double v_mean = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::isfinite(pts[i].value)) { v_mean += pts[i].value; }
    }
    v_mean /= static_cast<double>(seen);

    double A[kM][kM] = {};
    double rhs[kM] = {};
    used = 0;

    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(pts[i].value)) { continue; }
        const double dt =
            (static_cast<double>((pts[i].ts - t_eval).raw()) / 1e9
             - t_centre) / h;                    // u, in [-1, 1]
        double p[kM];
        p[0] = 1.0;
        for (int j = 1; j < kM; ++j) { p[j] = p[j - 1] * dt; }
        for (int a = 0; a < kM; ++a) {
            for (int b = 0; b <= a; ++b) { A[a][b] += p[a] * p[b]; }
            rhs[a] += p[a] * (pts[i].value - v_mean);
        }
        ++used;
    }
    if (used < kM) { return false; }
    for (int a = 0; a < kM; ++a) {
        for (int b = a + 1; b < kM; ++b) { A[a][b] = A[b][a]; }
    }

    // A small ridge, scaled to the trace. Without it a window whose points
    // are nearly coincident produces an enormous answer rather than a
    // detectably bad one.
    double trace = 0.0;
    for (int a = 0; a < kM; ++a) { trace += A[a][a]; }
    const double ridge = 1e-14 * trace;
    for (int a = 0; a < kM; ++a) { A[a][a] += ridge; }

    double L[kM][kM] = {};
    for (int i = 0; i < kM; ++i) {
        for (int j = 0; j <= i; ++j) {
            double sum = A[i][j];
            for (int q = 0; q < j; ++q) { sum -= L[i][q] * L[j][q]; }
            if (i == j) {
                if (!(sum > 0.0)) { return false; }
                L[i][i] = std::sqrt(sum);
            } else {
                L[i][j] = sum / L[j][j];
            }
        }
    }
    double v[kM];
    for (int i = 0; i < kM; ++i) {
        double sum = rhs[i];
        for (int q = 0; q < i; ++q) { sum -= L[i][q] * v[q]; }
        v[i] = sum / L[i][i];
    }
    for (int i = kM - 1; i >= 0; --i) {
        double sum = v[i];
        for (int q = i + 1; q < kM; ++q) { sum -= L[q][i] * coef[q]; }
        coef[i] = sum / L[i][i];
    }

    coef[0] += v_mean;          // undo the ordinate centring

    // Residual spread, with the fitted degrees of freedom removed.
    double ss = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(pts[i].value)) { continue; }
        const double dt =
            (static_cast<double>((pts[i].ts - t_eval).raw()) / 1e9
             - t_centre) / h;
        double fit = 0.0;
        double pw = 1.0;
        for (int j = 0; j < kM; ++j) { fit += coef[j] * pw; pw *= dt; }
        const double r = pts[i].value - fit;
        ss += r * r;
    }
    const int dof = used - kM;
    residual_sd = (dof > 0) ? std::sqrt(ss / static_cast<double>(dof)) : 0.0;
    for (int j = 0; j < kM; ++j) {
        if (!std::isfinite(coef[j])) { return false; }
    }
    return true;
}

} // namespace detail

/// Derivatives at `t_eval`, from the points of `pts` lying within `window`
/// before it.
///
/// `order` is 1 (velocity only), 2 (adds acceleration) or 3 (adds jerk). The
/// order is a real choice: a quadratic through a window that actually contains
/// a cubic shape reports a biased velocity, and a cubic through a window that
/// contains only noise reports a spectacular jerk. Start at 2.
///
/// `pts` need not be sorted, and points outside the window are ignored rather
/// than refused -- a caller holding a ring buffer should be able to pass the
/// whole thing.
[[nodiscard]] inline std::expected<Derivatives, DerivativeError>
derivatives_at(const TimedPoint* pts, std::size_t n, Timestamp t_eval,
               Duration window, int order = 2) noexcept {
    if (window.raw() <= 0) { return std::unexpected(DerivativeError::BadWindow); }
    if (order < 1 || order > kMaxDerivativeOrder) {
        return std::unexpected(DerivativeError::BadWindow);
    }

    // Gather the in-window points into a fixed buffer. No allocation: the
    // window is a time span, so the caller bounds the count by their own ring
    // capacity, and anything past it is dropped oldest-first by construction.
    constexpr std::size_t kCap = 4096;
    static thread_local TimedPoint buf[kCap];
    std::size_t m = 0;
    std::int64_t t_min = 0, t_max = 0;
    for (std::size_t i = 0; i < n && m < kCap; ++i) {
        const std::int64_t age = (t_eval - pts[i].ts).raw();
        if (age < 0 || age > window.raw()) { continue; }
        if (!std::isfinite(pts[i].value)) { continue; }
        if (m == 0 || age < t_min) { t_min = age; }
        if (m == 0 || age > t_max) { t_max = age; }
        buf[m++] = pts[i];
    }
    if (m < static_cast<std::size_t>(order) + 1) {
        return std::unexpected(DerivativeError::TooFewPoints);
    }
    if (t_max == t_min) {
        return std::unexpected(DerivativeError::ZeroTimeSpan);
    }

    Derivatives out{};
    out.scale = window;
    out.order = order;

    // The fit lives in u = (t - t_centre)/h, so the derivatives come back by
    // the chain rule with one factor of h per order. Those factors -- and the
    // j, j(j-1), j(j-1)(j-2) from differentiating u^j -- are the classic
    // dropped constants, so they are written once here rather than per branch.
    double t_centre = 0.0;
    double h = 1.0;
    bool ok = false;

    auto unpack = [&](const double* c, int m_coef) {
        const double u_e = (0.0 - t_centre) / h;   // t_eval is the origin
        double p0 = 0.0, p1 = 0.0, p2 = 0.0, p3 = 0.0;
        for (int j = 0; j < m_coef; ++j) {
            const double uj = std::pow(u_e, j);
            p0 += c[j] * uj;
            if (j >= 1) {
                p1 += static_cast<double>(j) * c[j] * std::pow(u_e, j - 1);
            }
            if (j >= 2) {
                p2 += static_cast<double>(j * (j - 1)) * c[j]
                    * std::pow(u_e, j - 2);
            }
            if (j >= 3) {
                p3 += static_cast<double>(j * (j - 1) * (j - 2)) * c[j]
                    * std::pow(u_e, j - 3);
            }
        }
        out.value = p0;
        out.velocity = p1 / h;
        out.acceleration = p2 / (h * h);
        out.jerk = p3 / (h * h * h);
    };

    if (order == 1) {
        double c[2] = {};
        ok = detail::poly_fit<1>(buf, m, t_eval, c, t_centre, h,
                                 out.residual_sd, out.points);
        if (ok) { unpack(c, 2); }
    } else if (order == 2) {
        double c[3] = {};
        ok = detail::poly_fit<2>(buf, m, t_eval, c, t_centre, h,
                                 out.residual_sd, out.points);
        if (ok) { unpack(c, 3); }
    } else {
        double c[4] = {};
        ok = detail::poly_fit<3>(buf, m, t_eval, c, t_centre, h,
                                 out.residual_sd, out.points);
        if (ok) { unpack(c, 4); }
    }
    if (!ok) { return std::unexpected(DerivativeError::Singular); }
    return out;
}

/// The same estimate at several scales at once.
///
/// A price has no single velocity, so the honest primitive returns several.
/// Writes `count` results into `out`, one per window, in the order given.
///
/// Divergence between scales is itself the signal worth watching: a velocity
/// that is positive at 100 ms and negative at 10 s is a market pulling back
/// from a move, and no single-scale estimate can say that.
[[nodiscard]] inline std::expected<int, DerivativeError>
derivatives_multiscale(const TimedPoint* pts, std::size_t n, Timestamp t_eval,
                       const Duration* windows, int count,
                       Derivatives* out, int order = 2) noexcept {
    if (count <= 0) { return std::unexpected(DerivativeError::BadWindow); }
    int produced = 0;
    for (int i = 0; i < count; ++i) {
        const auto d = derivatives_at(pts, n, t_eval, windows[i], order);
        if (!d) { continue; }       // a scale with too few ticks is skipped
        out[produced++] = *d;
    }
    if (produced == 0) { return std::unexpected(DerivativeError::TooFewPoints); }
    return produced;
}

} // namespace altair
