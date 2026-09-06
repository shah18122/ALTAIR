// risk/covariance.hpp -- covariance estimation, and why the sample one is
// usually the wrong answer.
//
// P15-03.
//
// THE SAMPLE COVARIANCE IS UNUSABLE EXACTLY WHERE IT IS MOST NEEDED.
//
// With p assets and n observations it has p(p+1)/2 parameters estimated from
// pn numbers. At p = 50 and n = 250 -- one year of daily data on a modest
// universe -- that is 1,275 parameters from 12,500 observations, and the
// estimate is dominated by noise. At p >= n it is SINGULAR: it has exact zero
// eigenvalues, cannot be inverted, and every optimiser that needs an inverse
// either fails or, worse, succeeds on a pseudo-inverse and returns enormous
// offsetting positions in the directions the data says have no risk.
//
// That failure has a signature and it is worth recognising: a mean-variance
// optimiser handed a near-singular covariance produces weights of +400% and
// -350%, and they look like conviction. They are division by almost zero.
//
// SHRINKAGE, AND WHY IT IS NOT A FUDGE.
//
// Ledoit-Wolf shrinks the sample estimate toward a structured target:
//
//     Sigma_hat = delta * F + (1 - delta) * S
//
// where F is the target (here: constant variance, zero correlation) and delta
// is chosen to minimise expected squared error. The point is not that F is
// true -- it plainly is not -- but that S is so noisy that a biased estimate
// with far less variance beats it on the only metric that matters, which is
// distance from the truth. That is a bias-variance trade made deliberately
// rather than a parameter tuned until the backtest looked better.
//
// THE CONDITION NUMBER IS THE DIAGNOSTIC.
//
// Ratio of largest to smallest eigenvalue. Above about 1e4 the inverse is
// numerically meaningless and any optimiser using it is reporting noise with
// great confidence. It is computed here and returned, because "the optimiser
// converged" is not evidence that its input was invertible.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <vector>

namespace altair {

enum class CovError : std::uint8_t {
    /// Fewer observations than assets + 2, so even the sample estimate has no
    /// degrees of freedom left.
    TooFewSamples,
    /// Zero assets, or a ragged input.
    BadShape,
    /// An asset had no variation at all.
    Degenerate
};

/// A p x p symmetric matrix, dense, row-major. Small by construction: this is
/// a portfolio covariance, not a sparse graph.
struct CovMatrix {
    std::size_t p = 0;
    std::vector<double> a;          ///< p*p, row-major

    [[nodiscard]] double& at(std::size_t i, std::size_t j) noexcept {
        return a[i * p + j];
    }
    [[nodiscard]] double at(std::size_t i, std::size_t j) const noexcept {
        return a[i * p + j];
    }

    /// Largest / smallest eigenvalue, by symmetric power iteration on Sigma
    /// and on (cI - Sigma). Crude and adequate: the question is whether the
    /// ratio is 10 or 10^8, not what it is to four figures.
    [[nodiscard]] double condition_number() const {
        if (p == 0) { return 0.0; }
        auto power = [this](bool shifted, double shift) {
            std::vector<double> v(p, 1.0 / std::sqrt(static_cast<double>(p)));
            double lam = 0.0;
            for (int it = 0; it < 300; ++it) {
                std::vector<double> w(p, 0.0);
                for (std::size_t i = 0; i < p; ++i) {
                    for (std::size_t j = 0; j < p; ++j) {
                        const double m = shifted
                            ? (i == j ? shift - at(i, j) : -at(i, j))
                            : at(i, j);
                        w[i] += m * v[j];
                    }
                }
                double norm = 0.0;
                for (const double x : w) { norm += x * x; }
                norm = std::sqrt(norm);
                if (!(norm > 0.0)) { return 0.0; }
                for (std::size_t i = 0; i < p; ++i) { v[i] = w[i] / norm; }
                lam = norm;
            }
            return lam;
        };
        const double hi = power(false, 0.0);
        // Smallest eigenvalue via the shifted operator: lambda_min = hi - the
        // dominant eigenvalue of (hi*I - Sigma).
        const double lo = hi - power(true, hi);
        if (!(lo > 1e-300)) { return 1e18; }     // singular to working precision
        return hi / lo;
    }
};

/// Plain sample covariance. Unbiased (n-1 denominator), and noisy.
[[nodiscard]] inline std::expected<CovMatrix, CovError>
sample_covariance(const std::vector<std::vector<double>>& r) {
    const std::size_t p = r.size();
    if (p == 0) { return std::unexpected(CovError::BadShape); }
    const std::size_t n = r[0].size();
    for (const auto& s : r) {
        if (s.size() != n) { return std::unexpected(CovError::BadShape); }
    }
    if (n < 3) { return std::unexpected(CovError::TooFewSamples); }

    std::vector<double> mu(p, 0.0);
    for (std::size_t i = 0; i < p; ++i) {
        for (const double x : r[i]) { mu[i] += x; }
        mu[i] /= static_cast<double>(n);
    }
    CovMatrix c;
    c.p = p;
    c.a.assign(p * p, 0.0);
    for (std::size_t i = 0; i < p; ++i) {
        for (std::size_t j = i; j < p; ++j) {
            double acc = 0.0;
            for (std::size_t k = 0; k < n; ++k) {
                acc += (r[i][k] - mu[i]) * (r[j][k] - mu[j]);
            }
            const double v = acc / static_cast<double>(n - 1);
            c.at(i, j) = v;
            c.at(j, i) = v;                 // symmetric BY CONSTRUCTION, not
                                            // by hoping the loop is symmetric
        }
    }
    return c;
}

struct ShrunkCovariance {
    CovMatrix sigma{};
    /// The shrinkage intensity actually used, in [0, 1]. Reported because it
    /// says how much the estimator distrusted the data: a delta near 1 means
    /// the sample covariance was contributing almost nothing.
    double delta = 0.0;
};

/// Ledoit-Wolf shrinkage toward a constant-variance, zero-correlation target.
///
/// The optimal intensity is (roughly) the ratio of the estimator's variance to
/// its distance from the target. Implemented here in the standard
/// finite-sample form: delta = min(1, (sum of asset-wise estimator variances)
/// / (squared Frobenius distance from S to F)).
[[nodiscard]] inline std::expected<ShrunkCovariance, CovError>
ledoit_wolf(const std::vector<std::vector<double>>& r) {
    const auto s = sample_covariance(r);
    if (!s) { return std::unexpected(s.error()); }
    const std::size_t p = s->p;
    const std::size_t n = r[0].size();

    // Target F: the average variance on the diagonal, zero off it. Chosen
    // because it has ONE parameter, so it cannot itself be overfitted -- the
    // whole point of a shrinkage target.
    double avg_var = 0.0;
    for (std::size_t i = 0; i < p; ++i) { avg_var += s->at(i, i); }
    avg_var /= static_cast<double>(p);
    if (!(avg_var > 0.0)) { return std::unexpected(CovError::Degenerate); }

    CovMatrix f;
    f.p = p;
    f.a.assign(p * p, 0.0);
    for (std::size_t i = 0; i < p; ++i) { f.at(i, i) = avg_var; }

    // Squared Frobenius distance between S and F.
    double gamma = 0.0;
    for (std::size_t i = 0; i < p; ++i) {
        for (std::size_t j = 0; j < p; ++j) {
            const double d = s->at(i, j) - f.at(i, j);
            gamma += d * d;
        }
    }

    // pi-hat: the summed variance of the entries of S.
    std::vector<double> mu(p, 0.0);
    for (std::size_t i = 0; i < p; ++i) {
        for (const double x : r[i]) { mu[i] += x; }
        mu[i] /= static_cast<double>(n);
    }
    double pihat = 0.0;
    for (std::size_t i = 0; i < p; ++i) {
        for (std::size_t j = 0; j < p; ++j) {
            double acc = 0.0;
            for (std::size_t k = 0; k < n; ++k) {
                const double e = (r[i][k] - mu[i]) * (r[j][k] - mu[j])
                               - s->at(i, j);
                acc += e * e;
            }
            pihat += acc / static_cast<double>(n);
        }
    }

    ShrunkCovariance out;
    out.delta = gamma > 0.0
        ? std::min(1.0, std::max(0.0, (pihat / static_cast<double>(n)) / gamma))
        : 1.0;
    out.sigma.p = p;
    out.sigma.a.assign(p * p, 0.0);
    for (std::size_t i = 0; i < p; ++i) {
        for (std::size_t j = 0; j < p; ++j) {
            out.sigma.at(i, j) =
                out.delta * f.at(i, j) + (1.0 - out.delta) * s->at(i, j);
        }
    }
    return out;
}

} // namespace altair
