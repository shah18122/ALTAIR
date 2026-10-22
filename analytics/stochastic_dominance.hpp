// analytics/stochastic_dominance.hpp -- conditional stochastic dominance
// between two return streams, by kernel weighting and a multiplier bootstrap.
//
// J. Olmo & M. Sanso-Navarro, "Forecasting the Performance of Hedge Fund
// Styles", March 2012 (SSRN 1932225); the test builds on Gonzalo & Olmo
// (2011) and Linton, Maasoumi & Whang (2005). Equation numbers are the paper's.
//
// THE QUESTION. Not "which strategy has the higher mean" but "would every
// investor of a given kind prefer A to B, given today's conditions x":
//   order 1  every investor who prefers more to less      (A's cdf below B's)
//   order 2  every risk-averse one                        (integrated cdf)
//   order 3  every one whose risk aversion falls with wealth (prefers skew)
// A dominates B at order s, given x, when for every threshold y
//     E[d_t,s(y) | I_{t-1} = x] <= 0,
//     d_t,s(y) = (y - Y_A)^{s-1} 1(Y_A <= y) - (y - Y_B)^{s-1} 1(Y_B <= y)   (7)
// estimated by kernel-weighting the days by how close their conditioning
// vector was to x (eq. 9), and tested with
//     T = sqrt(n_eff) sup_y D(y),
// whose p-value comes from the paper's algorithm: draw v_t iid N(0,1),
// recompute the sup with d_t v_t, J times, and count exceedances (eq. 10).
// A small p REJECTS "A dominates B".
//
// TWO CHOICES DIFFERENT FROM THE PAPER.
//   * The paper scales by (n h_1...h_q)^{1/2}; with a product Gaussian kernel
//     the effective sample size n_eff = (sum w)^2 / sum w^2 is used instead,
//     which is the same order and needs no bandwidth bookkeeping by the caller.
//   * The paper picks bandwidths by Hall-Racine-Li least-squares cross
//     validation. Here the caller passes them (or none, for the unconditional
//     test); sd_silverman_bandwidth() gives the usual rule of thumb.
//
// USE IN ALTAIR. Two strategies' daily P&L, or two instruments' returns,
// conditioned on something known the day before (realised vol, VIX, trend).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

namespace altair {

struct SdResult {
    double stat = 0.0;      ///< sqrt(n_eff) sup_y D(y); <= 0 means no violation seen
    double p_value = 1.0;   ///< small rejects "A dominates B"
    double n_eff = 0.0;     ///< effective number of days behind the estimate
};

/// Rule-of-thumb bandwidth 1.06 sd n^{-1/5} for one conditioning variable.
[[nodiscard]] inline double sd_silverman_bandwidth(std::span<const double> x) noexcept {
    const std::size_t n = x.size();
    if (n < 2) { return 1.0; }
    double s1 = 0.0, s2 = 0.0;
    for (double v : x) { s1 += v; s2 += v * v; }
    const double m = s1 / static_cast<double>(n);
    const double sd = std::sqrt(std::max(0.0, s2 / static_cast<double>(n) - m * m));
    return 1.06 * sd * std::pow(static_cast<double>(n), -0.2);
}

/// Kernel weights W_h(I_{t-1}; x) for each day: product Gaussian over the
/// conditioning variables. `cond[t]` is day t's conditioning vector (known
/// before day t's returns); `x` the conditions to test at; `h` the bandwidths.
[[nodiscard]] inline std::vector<double>
sd_kernel_weights(const std::vector<std::vector<double>>& cond,
                  const std::vector<double>& x, const std::vector<double>& h) {
    std::vector<double> w(cond.size(), 0.0);
    for (std::size_t t = 0; t < cond.size(); ++t) {
        double e = 0.0;
        for (std::size_t s = 0; s < x.size() && s < cond[t].size() && s < h.size(); ++s) {
            const double z = (cond[t][s] - x[s]) / h[s];
            e += z * z;
        }
        w[t] = std::exp(-0.5 * e);
    }
    return w;
}

/// Does A dominate B at `order` (1, 2 or 3)? `weights` empty means every day
/// counts equally (the unconditional test). `grid` points span the pooled
/// returns; `draws` is J.
[[nodiscard]] inline SdResult
stochastic_dominance(std::span<const double> a, std::span<const double> b, int order,
                     std::span<const double> weights = {}, std::size_t grid = 100,
                     std::size_t draws = 500, std::uint64_t seed = 2012) {
    SdResult r;
    const std::size_t n = std::min(a.size(), b.size());
    if (n < 2 || order < 1 || order > 3 || grid < 2) { return r; }
    std::vector<double> w(n, 1.0);
    if (!weights.empty()) {
        for (std::size_t t = 0; t < n && t < weights.size(); ++t) { w[t] = weights[t]; }
    }
    double sw = 0.0, sw2 = 0.0;
    for (double v : w) { sw += v; sw2 += v * v; }
    if (!(sw > 0.0)) { return r; }
    r.n_eff = sw * sw / sw2;

    double lo = a[0], hi = a[0];
    for (std::size_t t = 0; t < n; ++t) {
        lo = std::min({lo, a[t], b[t]});
        hi = std::max({hi, a[t], b[t]});
    }
    std::vector<double> ys(grid);
    for (std::size_t i = 0; i < grid; ++i) {
        ys[i] = lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(grid - 1);
    }
    const auto term = [order](double y, double v) {
        if (v > y) { return 0.0; }
        return order == 1 ? 1.0 : (order == 2 ? y - v : (y - v) * (y - v));
    };
    // d[i * n + t] = d_t,s(y_i)
    std::vector<double> d(grid * n);
    double sup = -1e300;
    for (std::size_t i = 0; i < grid; ++i) {
        double D = 0.0;
        for (std::size_t t = 0; t < n; ++t) {
            const double v = term(ys[i], a[t]) - term(ys[i], b[t]);
            d[i * n + t] = v;
            D += w[t] * v;
        }
        sup = std::max(sup, D / sw);
    }
    const double scale = std::sqrt(r.n_eff);
    r.stat = scale * sup;

    std::mt19937_64 rng(seed);
    std::normal_distribution<double> N;
    std::vector<double> v(n);
    std::size_t exceed = 0;
    for (std::size_t j = 0; j < draws; ++j) {
        for (double& x : v) { x = N(rng); }
        double s = -1e300;
        for (std::size_t i = 0; i < grid; ++i) {
            double D = 0.0;
            for (std::size_t t = 0; t < n; ++t) { D += w[t] * d[i * n + t] * v[t]; }
            s = std::max(s, D / sw);
        }
        exceed += scale * s > r.stat ? 1 : 0;
    }
    r.p_value = draws > 0 ? static_cast<double>(exceed) / static_cast<double>(draws) : 1.0;
    return r;
}

} // namespace altair
