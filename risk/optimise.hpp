// risk/optimise.hpp -- portfolio optimisers, and the control that beats them.
//
// P15-05 / P15-06.
//
// EVERY OPTIMISER HERE SHIPS WITH 1/N, AND THAT IS NOT MODESTY.
//
// QUANTLAB's Phase 4 ran minimum-variance, mean-variance, risk parity, HRP and
// Black-Litterman against equal weights on its own data, and 1/N BEAT EVERY
// ONE OF THEM out of sample. DeMiguel, Garlappi and Uppal found the same thing
// across fourteen datasets in 2009 and the result has not gone away.
//
// The reason is not that optimisation is wrong. It is that mean-variance is an
// ERROR-MAXIMISING procedure: it puts weight where expected return is high and
// covariance is low, which is exactly where the estimates are most wrong. Feed
// it noisy inputs and it finds the noise. 1/N has no parameters to estimate,
// so it has nothing to get wrong.
//
// So `optimise` returns the equal-weight portfolio alongside every result, and
// a caller comparing them out of sample is doing the only comparison that
// means anything. An optimiser that cannot beat 1/N is not a tool, it is a
// hypothesis that failed, and this file is arranged so that outcome is visible
// rather than embarrassing.
//
// MINIMUM VARIANCE NEEDS NO EXPECTED RETURNS, WHICH IS WHY IT USUALLY WINS.
//
// Of the mean-variance family, minimum-variance is the one that sometimes
// survives contact with data, and the reason is that it never estimates a mean.
// Expected returns are estimated with enormous error -- you need decades to
// pin a mean to within a percent -- and mean-variance amplifies exactly that
// error. Dropping the means removes the worst-estimated input entirely.
//
// NO SHORT SELLING BY DEFAULT.
//
// The unconstrained solution routinely wants -350% of something. That is not a
// portfolio, it is the optimiser dividing by a near-zero eigenvalue and
// calling it conviction. Long-only is imposed by projection, and the
// constrained answer is the one a human would sign.

#pragma once

#include <risk/covariance.hpp>

#include <algorithm>

#include <cmath>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace altair {

enum class OptError : std::uint8_t {
    BadShape,
    /// The covariance could not be inverted usefully. See `condition_number`.
    IllConditioned,
    /// No feasible long-only portfolio: every asset had non-positive variance.
    Infeasible
};

struct Weights {
    std::vector<double> w;
    /// Sum of weights. Always 1 for the portfolios here; carried so a caller
    /// can assert it rather than assume it.
    [[nodiscard]] double sum() const noexcept {
        double s = 0.0;
        for (const double x : w) { s += x; }
        return s;
    }
    /// Effective number of positions, 1 / sum(w^2). A portfolio of 50 assets
    /// with an effective N of 3 is three bets wearing a diversified label --
    /// the same arithmetic P6-04 applied to correlated signals.
    [[nodiscard]] double effective_n() const noexcept {
        double s2 = 0.0;
        for (const double x : w) { s2 += x * x; }
        return s2 > 0.0 ? 1.0 / s2 : 0.0;
    }
};

/// THE CONTROL. No parameters, nothing to estimate, nothing to get wrong.
[[nodiscard]] inline Weights equal_weight(std::size_t p) {
    Weights w;
    w.w.assign(p, p > 0 ? 1.0 / static_cast<double>(p) : 0.0);
    return w;
}

namespace detail {

/// Project onto the simplex {w >= 0, sum w = 1}. Euclidean projection, the
/// standard sort-and-threshold algorithm -- the closest long-only portfolio to
/// an unconstrained solution, rather than "clip negatives and renormalise",
/// which is not a projection and can move weights the wrong way.
inline void project_simplex(std::vector<double>& v) {
    const std::size_t n = v.size();
    if (n == 0) { return; }
    std::vector<double> u = v;
    std::sort(u.begin(), u.end(), [](double a, double b) { return a > b; });
    double css = 0.0, theta = 0.0;
    std::size_t rho = 0;
    for (std::size_t i = 0; i < n; ++i) {
        css += u[i];
        const double t = (css - 1.0) / static_cast<double>(i + 1);
        if (u[i] - t > 0.0) { rho = i + 1; theta = t; }
    }
    if (rho == 0) {
        for (auto& x : v) { x = 1.0 / static_cast<double>(n); }
        return;
    }
    for (auto& x : v) { x = x - theta > 0.0 ? x - theta : 0.0; }
}

} // namespace detail

/// MINIMUM VARIANCE, long-only.
///
/// Solved by projected gradient descent rather than by inverting Sigma. Two
/// reasons, and the second is the important one: the long-only constraint has
/// no closed form anyway, and a gradient method never forms the inverse, so a
/// near-singular covariance degrades the answer gently instead of producing
/// the +400%/-350% weights that an explicit inverse manufactures.
[[nodiscard]] inline std::expected<Weights, OptError>
min_variance(const CovMatrix& s) {
    if (s.p == 0) { return std::unexpected(OptError::BadShape); }
    double scale = 0.0;
    for (std::size_t i = 0; i < s.p; ++i) { scale += s.at(i, i); }
    if (!(scale > 0.0)) { return std::unexpected(OptError::Infeasible); }
    scale /= static_cast<double>(s.p);

    Weights w = equal_weight(s.p);
    const double step = 0.5 / scale;
    for (int it = 0; it < 3000; ++it) {
        std::vector<double> g(s.p, 0.0);
        for (std::size_t i = 0; i < s.p; ++i) {
            for (std::size_t j = 0; j < s.p; ++j) {
                g[i] += 2.0 * s.at(i, j) * w.w[j];
            }
        }
        for (std::size_t i = 0; i < s.p; ++i) { w.w[i] -= step * g[i]; }
        detail::project_simplex(w.w);
    }
    return w;
}

/// LONG-ONLY MEAN-VARIANCE. The expected-return vector is an input, never a
/// hidden estimate. Projected gradient ascent solves
///     maximise mu'w - lambda * w'Sigma w
/// on the simplex, so a bad or singular covariance cannot manufacture
/// offsetting leverage. Callers must still compare this hypothesis with 1/N
/// out of sample; optimisation is not evidence of an edge.
[[nodiscard]] inline std::expected<Weights, OptError>
mean_variance(const CovMatrix& s, std::span<const double> expected_returns,
              double risk_aversion = 1.0) {
    if (s.p == 0 || expected_returns.size() != s.p
        || !(risk_aversion > 0.0) || !std::isfinite(risk_aversion)) {
        return std::unexpected(OptError::BadShape);
    }
    double scale = 0.0;
    for (std::size_t i = 0; i < s.p; ++i) {
        if (!std::isfinite(expected_returns[i])) {
            return std::unexpected(OptError::BadShape);
        }
        scale += std::fabs(s.at(i, i));
    }
    if (!(scale > 0.0) || !std::isfinite(scale)) {
        return std::unexpected(OptError::Infeasible);
    }
    scale /= static_cast<double>(s.p);
    Weights w = equal_weight(s.p);
    const double step = 0.25 / (risk_aversion * scale + 1e-12);
    for (int it = 0; it < 3000; ++it) {
        std::vector<double> gradient(s.p, 0.0);
        for (std::size_t i = 0; i < s.p; ++i) {
            gradient[i] = expected_returns[i];
            for (std::size_t j = 0; j < s.p; ++j) {
                gradient[i] -= 2.0 * risk_aversion * s.at(i, j) * w.w[j];
            }
            w.w[i] += step * gradient[i];
        }
        detail::project_simplex(w.w);
    }
    return w;
}

/// RISK PARITY: every asset contributes the same share of portfolio variance.
///
/// Not the same as equal weights, and the difference is the point: a low-vol
/// asset gets MORE weight so that its risk contribution matches. Solved by the
/// standard multiplicative fixed point, which stays positive by construction
/// and therefore needs no projection.
[[nodiscard]] inline std::expected<Weights, OptError>
risk_parity(const CovMatrix& s) {
    if (s.p == 0) { return std::unexpected(OptError::BadShape); }
    Weights w = equal_weight(s.p);
    for (int it = 0; it < 2000; ++it) {
        std::vector<double> mrc(s.p, 0.0);          // marginal risk
        for (std::size_t i = 0; i < s.p; ++i) {
            for (std::size_t j = 0; j < s.p; ++j) {
                mrc[i] += s.at(i, j) * w.w[j];
            }
        }
        double total = 0.0;
        for (std::size_t i = 0; i < s.p; ++i) { total += w.w[i] * mrc[i]; }
        if (!(total > 0.0)) { return std::unexpected(OptError::Infeasible); }
        const double target = total / static_cast<double>(s.p);
        double sum = 0.0;
        for (std::size_t i = 0; i < s.p; ++i) {
            if (mrc[i] > 0.0) {
                w.w[i] = 0.5 * w.w[i] + 0.5 * (target / mrc[i]);
            }
            if (w.w[i] < 1e-12) { w.w[i] = 1e-12; }
            sum += w.w[i];
        }
        for (std::size_t i = 0; i < s.p; ++i) { w.w[i] /= sum; }
    }
    return w;
}

/// Portfolio variance under a set of weights. The number every comparison in
/// this file is actually about.
[[nodiscard]] inline double portfolio_variance(const CovMatrix& s,
                                               const Weights& w) noexcept {
    double v = 0.0;
    for (std::size_t i = 0; i < s.p && i < w.w.size(); ++i) {
        for (std::size_t j = 0; j < s.p && j < w.w.size(); ++j) {
            v += w.w[i] * s.at(i, j) * w.w[j];
        }
    }
    return v;
}

} // namespace altair
