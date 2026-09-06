// analytics/garch.hpp -- GARCH(1,1), GJR-GARCH, and the loss functions that
// decide which of them is any good.
//
// P14-01 / 02 / 03.
//
// THE MODEL
//
//     sigma^2_t = omega + alpha * eps^2_{t-1} + beta * sigma^2_{t-1}
//
// Today's variance is a constant, plus what happened yesterday, plus what we
// thought yesterday. That is the whole idea, and it is worth stating plainly
// because GARCH's reputation exceeds what the equation actually says: it is an
// EWMA (alpha, beta) with a constant bolted on (omega). RiskMetrics EWMA is
// literally the special case omega = 0, alpha + beta = 1.
//
// STATIONARITY IS REFUSED, NOT CLAMPED.
//
// alpha + beta < 1 or the unconditional variance omega / (1 - alpha - beta) is
// negative or infinite, and every forecast beyond one step diverges. Optimisers
// walk into that region constantly. Clamping to 0.999 -- which is what most
// implementations do -- returns a number from a model that does not exist, and
// the number looks like a volatility forecast. So `fit` refuses and says which
// constraint failed.
//
// WHY GJR AND NOT JUST GARCH.
//
// Symmetric GARCH cannot represent the leverage effect: a 2% fall raises
// tomorrow's expected volatility more than a 2% rise does, and eps^2 discards
// the sign that carries it. GJR adds one term:
//
//     + gamma * eps^2_{t-1} * 1[eps_{t-1} < 0]
//
// A gamma indistinguishable from zero is a real answer -- it says this series
// has no measurable asymmetry -- so the fit reports gamma's standard error
// and the test reads it rather than the point estimate.
//
// AND THE LOSS FUNCTION IS NOT A DETAIL.
//
// Volatility is never observed, so a forecast is scored against a PROXY --
// usually the squared return, which is an unbiased but extremely noisy
// estimator of variance. MSE on that proxy rewards models that predict the
// noise. QLIKE
//
//     QLIKE = log(sigma^2) + r^2 / sigma^2
//
// is robust to proxy noise and asymmetric in the direction that matters: it
// punishes UNDER-prediction of variance much harder than over-prediction,
// which is the correct asymmetry for anything that sizes positions. Both are
// reported, because a model that wins on MSE and loses on QLIKE is telling you
// something about itself.

#pragma once

#include <cmath>
#include <cstdint>
#include <expected>

namespace altair {

enum class GarchError : std::uint8_t {
    /// Fewer observations than the estimator can support.
    TooFewSamples,
    /// alpha + beta (+ gamma/2) >= 1. The variance process has no finite
    /// unconditional level.
    NotStationary,
    /// A parameter left its admissible range: omega <= 0 or a weight < 0.
    BadParameter,
    /// The series has no variation, so there is nothing to model.
    Degenerate
};

[[nodiscard]] inline const char* garch_error_text(GarchError e) noexcept {
    switch (e) {
    case GarchError::TooFewSamples: return "too few observations";
    case GarchError::NotStationary: return "alpha + beta >= 1, NOT STATIONARY";
    case GarchError::BadParameter:  return "parameter outside its range";
    case GarchError::Degenerate:    return "the series has no variation";
    }
    return "unknown";
}

struct GarchParams {
    double omega = 0.0;
    double alpha = 0.0;
    double beta = 0.0;
    /// GJR asymmetry. Zero for plain GARCH(1,1).
    double gamma = 0.0;

    /// Persistence. For GJR the negative shock fires about half the time, so
    /// gamma enters at half weight -- using the full gamma would report a
    /// model as non-stationary that is not.
    [[nodiscard]] double persistence() const noexcept {
        return alpha + beta + 0.5 * gamma;
    }
    [[nodiscard]] bool stationary() const noexcept {
        return persistence() < 1.0;
    }
    /// The level the variance reverts to. Meaningless unless stationary,
    /// which is why the caller must check first.
    [[nodiscard]] double unconditional_variance() const noexcept {
        const double p = persistence();
        return p < 1.0 ? omega / (1.0 - p) : 0.0;
    }
    /// How many steps for a shock to decay by half. The number that says
    /// whether a forecast is worth making at the horizon being traded.
    [[nodiscard]] double half_life() const noexcept {
        const double p = persistence();
        return (p > 0.0 && p < 1.0) ? std::log(0.5) / std::log(p) : 0.0;
    }
};

struct GarchFit {
    GarchParams p{};
    /// Maximised log-likelihood, for comparing nested models.
    double log_likelihood = 0.0;
    std::size_t n = 0;
    /// Standard error of gamma, from a numerical second derivative. Reported
    /// because a gamma of 0.03 means nothing without it.
    double gamma_se = 0.0;
};

namespace detail {

/// Negative log-likelihood under a conditional normal. Not because returns are
/// normal -- they are emphatically not, and P11Q-10 measured excess kurtosis
/// of 11.34 on this very series -- but because the Gaussian QMLE is CONSISTENT
/// for the variance parameters even when the distribution is wrong. That is a
/// real theorem and it is the reason this estimator is defensible here; the
/// standard errors it produces are not, which is why gamma_se is computed
/// numerically rather than read off the information matrix.
[[nodiscard]] inline double nll(const double* r, std::size_t n,
                                const GarchParams& p, double v0) noexcept {
    double v = v0;
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!(v > 1e-300)) { return 1e300; }
        acc += std::log(v) + r[i] * r[i] / v;
        const double e = r[i];
        v = p.omega + p.alpha * e * e + p.beta * v
          + (e < 0.0 ? p.gamma * e * e : 0.0);
    }
    return 0.5 * acc;
}

} // namespace detail

/// Fit by a coarse-to-fine grid search on the QMLE.
///
/// A grid rather than a gradient optimiser, deliberately. The likelihood
/// surface is flat along alpha+beta and a quasi-Newton method walks straight
/// into the non-stationary region and reports a "converged" fit at
/// alpha+beta = 0.9999. A grid that simply never evaluates outside the
/// admissible set cannot do that, and it is fast enough at this problem size.
/// It is also fully reproducible, which rule 10 needs.
[[nodiscard]] inline std::expected<GarchFit, GarchError>
fit_garch(const double* r, std::size_t n, bool asymmetric) noexcept {
    if (r == nullptr || n < 100) {
        return std::unexpected(GarchError::TooFewSamples);
    }
    double s2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) { s2 += r[i] * r[i]; }
    const double sample_var = s2 / static_cast<double>(n);
    if (!(sample_var > 0.0)) { return std::unexpected(GarchError::Degenerate); }

    GarchParams best{};
    double best_nll = 1e300;
    bool found = false;

    // Two passes: coarse, then refined around the winner.
    double a_lo = 0.01, a_hi = 0.30, b_lo = 0.50, b_hi = 0.985;
    double g_lo = 0.0, g_hi = asymmetric ? 0.30 : 0.0;
    for (int pass = 0; pass < 2; ++pass) {
        const int steps = 24;
        for (int ia = 0; ia <= steps; ++ia) {
            const double a = a_lo + (a_hi - a_lo) * ia / steps;
            for (int ib = 0; ib <= steps; ++ib) {
                const double b = b_lo + (b_hi - b_lo) * ib / steps;
                const int gsteps = asymmetric ? 8 : 0;
                for (int ig = 0; ig <= gsteps; ++ig) {
                    GarchParams p;
                    p.alpha = a;
                    p.beta = b;
                    p.gamma = gsteps > 0
                        ? g_lo + (g_hi - g_lo) * ig / gsteps : 0.0;
                    // THE CONSTRAINT IS ENFORCED BY NOT LOOKING, which is the
                    // only way it cannot be violated by a numerical accident.
                    if (!p.stationary()) { continue; }
                    // omega is pinned by targeting the sample variance as the
                    // unconditional level -- standard variance targeting, and
                    // it removes one dimension from the search.
                    p.omega = sample_var * (1.0 - p.persistence());
                    if (!(p.omega > 0.0)) { continue; }
                    const double f = detail::nll(r, n, p, sample_var);
                    if (f < best_nll) {
                        best_nll = f;
                        best = p;
                        found = true;
                    }
                }
            }
        }
        if (!found) { return std::unexpected(GarchError::NotStationary); }
        const double aw = (a_hi - a_lo) / 8.0, bw = (b_hi - b_lo) / 8.0;
        a_lo = best.alpha - aw > 0.0 ? best.alpha - aw : 1e-6;
        a_hi = best.alpha + aw;
        b_lo = best.beta - bw > 0.0 ? best.beta - bw : 1e-6;
        b_hi = best.beta + bw < 0.999 ? best.beta + bw : 0.999;
        if (asymmetric) {
            const double gw = (g_hi - g_lo) / 8.0;
            g_lo = best.gamma - gw > 0.0 ? best.gamma - gw : 0.0;
            g_hi = best.gamma + gw;
        }
    }

    GarchFit out;
    out.p = best;
    out.log_likelihood = -best_nll;
    out.n = n;

    if (asymmetric) {
        // Numerical curvature of the log-likelihood in gamma. Crude, and
        // labelled as such: it is enough to answer "is gamma distinguishable
        // from zero", which is the only question asked of it.
        const double h = 0.01;
        GarchParams up = best, dn = best;
        up.gamma += h;
        dn.gamma = best.gamma - h > 0.0 ? best.gamma - h : 0.0;
        if (up.stationary()) {
            up.omega = sample_var * (1.0 - up.persistence());
            dn.omega = sample_var * (1.0 - dn.persistence());
            const double f_up = detail::nll(r, n, up, sample_var);
            const double f_dn = detail::nll(r, n, dn, sample_var);
            const double curv = (f_up - 2.0 * best_nll + f_dn) / (h * h);
            out.gamma_se = curv > 0.0 ? 1.0 / std::sqrt(curv) : 0.0;
        }
    }
    return out;
}

/// One-step-ahead conditional variances, filtered through the sample.
/// `out[i]` is the variance FORECAST for observation i, made from data up to
/// i-1 -- so it is usable as a forecast without look-ahead (rule 7).
[[nodiscard]] inline std::expected<void, GarchError>
garch_filter(const double* r, std::size_t n, const GarchParams& p,
             double* out) noexcept {
    if (r == nullptr || out == nullptr || n == 0) {
        return std::unexpected(GarchError::TooFewSamples);
    }
    if (!p.stationary()) { return std::unexpected(GarchError::NotStationary); }
    if (!(p.omega > 0.0) || p.alpha < 0.0 || p.beta < 0.0 || p.gamma < 0.0) {
        return std::unexpected(GarchError::BadParameter);
    }
    double v = p.unconditional_variance();
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = v;
        const double e = r[i];
        v = p.omega + p.alpha * e * e + p.beta * v
          + (e < 0.0 ? p.gamma * e * e : 0.0);
    }
    return {};
}

// ---------------------------------------------------------------------------
// Loss functions
// ---------------------------------------------------------------------------

/// QLIKE, averaged. LOWER IS BETTER.
///
/// Robust to the noise in the squared-return proxy, and asymmetric in the
/// right direction: under-predicting variance costs far more than
/// over-predicting it, which is what a position size actually cares about.
[[nodiscard]] inline std::expected<double, GarchError>
qlike(const double* forecast_var, const double* realised, std::size_t n) noexcept {
    if (n == 0) { return std::unexpected(GarchError::TooFewSamples); }
    double acc = 0.0;
    std::size_t used = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = forecast_var[i];
        if (!(v > 1e-300)) { continue; }
        acc += std::log(v) + realised[i] * realised[i] / v;
        ++used;
    }
    if (used == 0) { return std::unexpected(GarchError::Degenerate); }
    return acc / static_cast<double>(used);
}

/// MSE on the squared-return proxy. Reported alongside QLIKE and NOT trusted
/// on its own: the proxy is unbiased but so noisy that MSE rewards fitting the
/// noise, which is why the horse race reads both.
[[nodiscard]] inline std::expected<double, GarchError>
proxy_mse(const double* forecast_var, const double* realised,
          std::size_t n) noexcept {
    if (n == 0) { return std::unexpected(GarchError::TooFewSamples); }
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = forecast_var[i] - realised[i] * realised[i];
        acc += d * d;
    }
    return acc / static_cast<double>(n);
}

} // namespace altair
