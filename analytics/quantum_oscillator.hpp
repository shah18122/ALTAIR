// analytics/quantum_oscillator.hpp -- Orrell's quantum oscillator model.
//
// D. Orrell, "A Quantum Oscillator Model of Stock Markets" (Systems
// Forecasting; SSRN 3941518). The price is a quantum harmonic oscillator held
// by the buyers' and sellers' entropic forces; its energy level n is read as
// the number of transactions in a step and is Poisson with mean lambda
// (default 1/4: the half-spread carries all the uncertainty). What follows
// from that, with the paper's equation numbers:
//
//   * level n has volatility sigma_n = sigma sqrt(2n + 1), so the RMS
//     volatility is sigma_a = sigma sqrt(2 lambda + 1)                     (1)
//   * price changes are a Poisson mix of normals (the paper's Figure 1);
//     its kurtosis is 3 (4 lambda^2 + 8 lambda + 1) / (2 lambda + 1)^2,
//     4.33 at lambda = 1/4
//   * a net order Q against daily volume V moves the price by
//         x = sqrt(2 / (2 lambda + 1)) sigma_d sqrt(Q / V)                 (5)
//     -- the square-root law, with Y = 1 at lambda = 1/2 and 1.15 at 1/4
//   * the variance of impact over an order lasting T days is
//         Var(T) = sigma^2 T + sigma^2 Q / (V T)                           (9)
//   * volatility given a move x over T is a smile,
//         sigma_x^2 = sigma^2 + x^2 / (2 T)                                (8)
//     with its floor at sigma = sigma_a / sqrt(2 lambda + 1).
//
// The paper has no forecast and no trading rule. Two of its claims can be
// checked on NIFTY: the test reads dataset/'s daily closes (Figure 1's
// distribution, and (8) against realised vol), and altair_quantum_smile tests
// (8) as an option smile on the NSE bhavcopy. (5) is the square-root law
// oms/capacity.hpp already prices; this gives its constant a derivation.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace altair::qosc {

/// P(n) for a Poisson energy level with mean `lambda`.
[[nodiscard]] inline double level_prob(std::size_t n, double lambda) noexcept {
    if (!(lambda >= 0.0)) { return std::nan(""); }
    if (lambda == 0.0) { return n == 0 ? 1.0 : 0.0; }
    return std::exp(static_cast<double>(n) * std::log(lambda) - lambda - std::lgamma(static_cast<double>(n) + 1.0));
}

/// Eq. 1: the RMS volatility of ground-state volatility `sigma` at mean level `lambda`.
[[nodiscard]] inline double rms_vol(double sigma, double lambda) noexcept { return sigma * std::sqrt(2.0 * lambda + 1.0); }

/// The ground-state volatility that gives RMS volatility `sigma_a`.
[[nodiscard]] inline double ground_vol(double sigma_a, double lambda) noexcept { return sigma_a / std::sqrt(2.0 * lambda + 1.0); }

/// The density of a price change `x` under the Poisson mix of normals.
/// Levels are summed until their weight is below 1e-15.
[[nodiscard]] inline double density(double x, double sigma, double lambda) noexcept {
    if (!(sigma > 0.0) || !(lambda >= 0.0)) { return std::nan(""); }
    double f = 0.0;
    for (std::size_t n = 0; n < 200; ++n) {
        const double p = level_prob(n, lambda);
        const double v = sigma * sigma * (2.0 * static_cast<double>(n) + 1.0);
        f += p * std::exp(-x * x / (2.0 * v)) / std::sqrt(2.0 * std::numbers::pi * v);
        if (static_cast<double>(n) > lambda && p < 1e-15) { break; }
    }
    return f;
}

/// The mix's kurtosis (3 for a normal): 3 E[(2n+1)^2] / E[2n+1]^2.
[[nodiscard]] inline double kurtosis(double lambda) noexcept {
    const double m = 2.0 * lambda + 1.0;
    return 3.0 * (4.0 * lambda * lambda + 8.0 * lambda + 1.0) / (m * m);
}

/// Eq. 5's constant Y in x = Y sigma_d sqrt(Q / V).
[[nodiscard]] inline double impact_constant(double lambda) noexcept { return std::sqrt(2.0 / (2.0 * lambda + 1.0)); }

/// Eq. 5: the price change, as a fraction, of a net order `q` against daily
/// volume `v` at daily volatility `sigma_d`. Signed like `q`.
[[nodiscard]] inline double impact(double q, double v, double sigma_d, double lambda) noexcept {
    if (!(v > 0.0) || !(sigma_d >= 0.0)) { return std::nan(""); }
    return std::copysign(impact_constant(lambda) * sigma_d * std::sqrt(std::fabs(q) / v), q);
}

/// Eq. 9: the variance of impact for an order of size `q` over `t_days`.
[[nodiscard]] inline double impact_variance(double sigma_d, double t_days, double q, double v) noexcept {
    if (!(t_days > 0.0) || !(v > 0.0)) { return std::nan(""); }
    return sigma_d * sigma_d * (t_days + std::fabs(q) / (v * t_days));
}

/// Eq. 8: volatility given a log move `x` over `t` (same time unit as `sigma`).
[[nodiscard]] inline double smile_vol(double sigma, double x, double t) noexcept {
    if (!(t > 0.0)) { return std::nan(""); }
    return std::sqrt(sigma * sigma + x * x / (2.0 * t));
}

/// One expiry's smile fitted two ways, both in variance: Orrell's fixed slope,
/// sigma^2 + k^2 / (2T), with sigma the only parameter; and a free slope,
/// a + b k^2, to see whether the data want b = 1/2. k = ln(K / F).
struct SmileFit {
    std::size_t points = 0;
    double sigma = 0.0;                        ///< Orrell: the smile's floor
    double a = 0.0, b = 0.0;                   ///< free slope; Orrell says b = 1 / (2T)
    double rmse_flat = 0.0, rmse_orrell = 0.0, rmse_free = 0.0;   ///< in vol, against the market
};

[[nodiscard]] inline SmileFit fit_smile(const std::vector<double>& k, const std::vector<double>& iv, double t) {
    SmileFit f;
    const std::size_t n = std::min(k.size(), iv.size());
    f.points = n;
    if (n < 3 || !(t > 0.0)) { return f; }
    double s_y = 0, s_q = 0, s_qq = 0, s_qy = 0, s_iv = 0;
    double s_orr = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double q = k[i] * k[i], y = iv[i] * iv[i];
        s_y += y; s_q += q; s_qq += q * q; s_qy += q * y; s_iv += iv[i];
        s_orr += y - q / (2.0 * t);
    }
    const double dn = static_cast<double>(n);
    const double s2 = s_orr / dn;
    f.sigma = std::sqrt(std::max(s2, 0.0));
    const double den = dn * s_qq - s_q * s_q;
    f.b = den != 0.0 ? (dn * s_qy - s_q * s_y) / den : 0.0;
    f.a = (s_y - f.b * s_q) / dn;
    const double flat = s_iv / dn;
    double e_flat = 0, e_orr = 0, e_free = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double q = k[i] * k[i];
        const double orr = std::sqrt(std::max(s2 + q / (2.0 * t), 0.0));
        const double fr = std::sqrt(std::max(f.a + f.b * q, 0.0));
        e_flat += (iv[i] - flat) * (iv[i] - flat);
        e_orr += (iv[i] - orr) * (iv[i] - orr);
        e_free += (iv[i] - fr) * (iv[i] - fr);
    }
    f.rmse_flat = std::sqrt(e_flat / dn);
    f.rmse_orrell = std::sqrt(e_orr / dn);
    f.rmse_free = std::sqrt(e_free / dn);
    return f;
}

} // namespace altair::qosc
