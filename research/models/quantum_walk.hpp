// research/models/quantum_walk.hpp -- Orrell's quantum walk option model.
//
// D. Orrell, "A Quantum Walk Model of Financial Options", Wilmott 2021: 62-69
// (SSRN 3512481; PDF supplied by Smit). Investors' beliefs about the future
// price are a Hadamard quantum walk, not a random walk: deterministic until
// measured, with interference, so its variance grows as n^2 instead of n and
// its distribution has two horns instead of one hump (Figure 1). Then:
//   * grid scaled so the walk's final variance is sigma^2 T, the log price is
//     the walk plus the risk-neutral drift, and an option is its discounted
//     expected payoff (eq. 1) -- the "quantum price";
//   * a seller prices with the classical binomial walk plus a 10 % margin, a
//     buyer with the quantum one less 20 %; the trade happens at Orrell's
//     propensity mean mu = (sigma_b^2 mu_s + sigma_s^2 mu_b) / sigma_t^2, with
//     the flexibilities in the margins' ratio (Appendix: sigma_t 0.15, start
//     angle 50 degrees, a 0.001 floor) -- the "market price".
// Decoherence (a coin phase drawn N(0, sigma_dec) each step) moves the walk
// back towards the classical one.
//
// The paper's testable claim is the shape. The quantum price is HIGHER at the
// money than the classical one and lower in the wings: read through Black-
// Scholes it is a FROWN (the test: 23 % at the money, 10-15 % at +-20 % for a
// 20 % walk), the opposite of NSE's smile (altair_quantum_smile). The
// compromise "market price" is a downward skew, but that comes from the
// seller's 10 % margin on deep in-the-money calls, not from the walk.

#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <random>
#include <vector>

namespace altair::qwalk {

/// Probabilities of positions -n..n (index j = position + n) after n steps.
struct WalkDist {
    std::vector<double> p;
    int n = 0;
    [[nodiscard]] double mean() const {
        double m = 0.0;
        for (std::size_t j = 0; j < p.size(); ++j) m += p[j] * (static_cast<double>(j) - n);
        return m;
    }
    [[nodiscard]] double variance() const {
        const double m = mean();
        double v = 0.0;
        for (std::size_t j = 0; j < p.size(); ++j) { const double x = static_cast<double>(j) - n - m; v += p[j] * x * x; }
        return v;
    }
};

/// The classical walk: binomial, variance n.
[[nodiscard]] inline WalkDist classical_walk(int n) {
    WalkDist d;
    d.n = n;
    d.p.assign(static_cast<std::size_t>(2 * n + 1), 0.0);
    for (int k = 0; k <= n; ++k)
        d.p[static_cast<std::size_t>(2 * k)] = std::exp(std::lgamma(n + 1.0) - std::lgamma(k + 1.0) - std::lgamma(n - k + 1.0) - n * std::numbers::ln2);
    return d;
}

/// The Hadamard walk from coin state sin(theta)|up> + i cos(theta)|down>
/// (theta = 45 degrees is balanced; above it leans up, as the paper's 50). With sigma_dec > 0 each step's coin
/// carries a random phase (the paper's imperfect Hadamard), averaged over
/// `runs` walks.
[[nodiscard]] inline WalkDist quantum_walk(int n, double theta = std::numbers::pi / 4.0, double sigma_dec = 0.0, int runs = 1,
                                           unsigned seed = 1) {
    using C = std::complex<double>;
    const std::size_t w = static_cast<std::size_t>(2 * n + 1);
    WalkDist d;
    d.n = n;
    d.p.assign(w, 0.0);
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> z(0.0, sigma_dec > 0.0 ? sigma_dec : 1.0);
    const double h = 1.0 / std::numbers::sqrt2;
    const int reps = sigma_dec > 0.0 ? std::max(runs, 1) : 1;
    for (int r = 0; r < reps; ++r) {
        std::vector<C> a(w), b(w), a2(w), b2(w);
        a[static_cast<std::size_t>(n)] = std::sin(theta);
        b[static_cast<std::size_t>(n)] = C(0.0, std::cos(theta));
        for (int s = 0; s < n; ++s) {
            const C e = sigma_dec > 0.0 ? std::polar(1.0, z(rng)) : C(1.0, 0.0);
            std::fill(a2.begin(), a2.end(), C{});
            std::fill(b2.begin(), b2.end(), C{});
            for (std::size_t j = 0; j < w; ++j) {
                if (a[j] == C{} && b[j] == C{}) continue;
                // coin: R = 1/sqrt2 [[1, e], [conj(e), -1]]; then up moves +1, down -1
                const C up = h * (a[j] + e * b[j]);
                const C dn = h * (std::conj(e) * a[j] - b[j]);
                if (j + 1 < w) a2[j + 1] += up;
                if (j >= 1) b2[j - 1] += dn;
            }
            a.swap(a2);
            b.swap(b2);
        }
        for (std::size_t j = 0; j < w; ++j) d.p[j] += (std::norm(a[j]) + std::norm(b[j])) / reps;
    }
    return d;
}

/// Eq. 1: a European call on spot 1 (strike `k` as a fraction of spot) from a
/// walk's final distribution, its grid scaled to variance sigma^2 T about the
/// risk-neutral drift, discounted at `r`.
[[nodiscard]] inline double call_from_walk(const WalkDist& d, double k, double sigma, double t, double r) {
    const double m = d.mean(), v = d.variance();
    if (!(v > 0.0) || !(t > 0.0)) return std::nan("");
    const double dx = sigma * std::sqrt(t / v);
    double ev = 0.0;
    for (std::size_t j = 0; j < d.p.size(); ++j) {
        const double x = (static_cast<double>(j) - d.n - m) * dx + (r - 0.5 * sigma * sigma) * t;
        ev += d.p[j] * std::max(std::exp(x) - k, 0.0);
    }
    return std::exp(-r * t) * ev;
}

/// The Appendix's compromise between a seller on the classical price and a
/// buyer on the quantum one.
struct MarketParams {
    double seller_margin = 0.10, buyer_margin = 0.20;   ///< as fractions of each side's model price
    double sigma_t = 0.15;                              ///< total price flexibility
    double floor = 0.001;
};

[[nodiscard]] inline double market_price(double classical, double quantum, const MarketParams& mp = {}) {
    const double mu_s = classical * (1.0 + mp.seller_margin) + mp.floor;
    const double mu_b = quantum * (1.0 - mp.buyer_margin) + mp.floor;
    // flexibility in the margins' ratio: sigma_s / sigma_b = d_s / d_b
    const double ratio = mp.buyer_margin > 0.0 ? mp.seller_margin / mp.buyer_margin : 1.0;
    const double ss = mp.sigma_t * ratio / std::sqrt(1.0 + ratio * ratio), sb = mp.sigma_t / std::sqrt(1.0 + ratio * ratio);
    return (sb * sb * mu_s + ss * ss * mu_b) / (mp.sigma_t * mp.sigma_t);
}

/// Black-Scholes call on spot 1, and its implied vol by bisection.
[[nodiscard]] inline double bs_call(double k, double sigma, double t, double r) {
    const double sd = sigma * std::sqrt(t);
    const double d1 = (-std::log(k) + (r + 0.5 * sigma * sigma) * t) / sd, d2 = d1 - sd;
    const auto N = [](double x) { return 0.5 * std::erfc(-x / std::numbers::sqrt2); };
    return N(d1) - k * std::exp(-r * t) * N(d2);
}

[[nodiscard]] inline double bs_implied(double price, double k, double t, double r) {
    double lo = 1e-4, hi = 5.0;
    if (!(price > bs_call(k, lo, t, r)) || !(price < bs_call(k, hi, t, r))) return std::nan("");
    for (int i = 0; i < 100; ++i) {
        const double mid = 0.5 * (lo + hi);
        (bs_call(k, mid, t, r) < price ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

} // namespace altair::qwalk
