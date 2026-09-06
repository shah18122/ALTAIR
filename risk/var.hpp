// risk/var.hpp -- Value at Risk three ways, and Expected Shortfall.
//
// P15-01 / P15-02.
//
// THE THREE METHODS DISAGREE, AND THE DISAGREEMENT IS THE DELIVERABLE.
//
// Historical VaR reads the empirical quantile: it makes no distributional
// assumption and cannot see a loss larger than the worst day in its window.
// Parametric VaR assumes a normal and reads off a z-score: it can extrapolate
// past the sample, and it is wrong in a specific direction. Monte Carlo VaR
// simulates: it is as good as the distribution it samples from, which is
// usually the normal again, in which case it is a slower parametric VaR with
// error bars.
//
// P11Q-10 measured excess kurtosis of 11.34 on daily NIFTY -- 136 of 8,755
// days beyond three sigma against a normal's expected 24. So the parametric
// number is not approximately right here. It is systematically too small at
// the tail, and too small is the direction that sizes you bigger than the
// market allows.
//
// A RISK SYSTEM REPORTING ONE VaR HAS CHOSEN AN ASSUMPTION AND HIDDEN IT.
// Reporting all three makes the assumption the reader's problem, which is
// where it belongs.
//
// VaR IS NOT COHERENT. EXPECTED SHORTFALL IS.
//
// VaR fails SUBADDITIVITY: there exist portfolios A and B with
//
//     VaR(A + B) > VaR(A) + VaR(B)
//
// meaning the model says diversification INCREASED risk. That is not a
// numerical artefact, it is a property of quantiles, and it is why Basel moved
// the trading book from VaR to Expected Shortfall after 2008. A concrete
// counterexample is an acceptance test in this card rather than a footnote,
// because the failure is easy to state and almost never demonstrated.
//
// ES answers the question VaR only appears to:
//
//     VaR:  "I lose no more than X on 99 days in 100."
//     ES:   "On the 1 day in 100 where I do, I lose X on average."
//
// VaR says nothing whatsoever about the size of the loss when it is breached,
// and the breach is the entire reason anyone asked.
//
// SIGN CONVENTION, STATED ONCE.
//
// Both are returned as POSITIVE LOSSES. A VaR of 250 means "a loss of 250".
// Returning a negative number for a loss is defensible and every second caller
// gets the sign wrong, so it is not done here.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <vector>

namespace altair {

enum class VarError : std::uint8_t {
    /// Fewer observations than the quantile can be read from.
    TooFewSamples,
    /// Confidence outside (0, 1), or so extreme the sample cannot support it.
    BadConfidence,
    /// The series has no variation.
    Degenerate
};

struct VarResult {
    /// Loss at the confidence level, POSITIVE. Same units as the input.
    double var = 0.0;
    /// Mean loss GIVEN the VaR is breached. Positive, and always >= var.
    double expected_shortfall = 0.0;
    /// How many observations sat in the tail. Reported because an ES computed
    /// from three observations is a number, not an estimate -- and at 99% on
    /// 250 days that is exactly what you get.
    std::size_t tail_n = 0;
};

namespace detail {

/// Inverse standard normal, Acklam's rational approximation. Accurate to
/// about 1.15e-9 over the whole range, which is far tighter than any input to
/// a VaR calculation deserves.
[[nodiscard]] inline double inv_norm_cdf(double p) noexcept {
    static const double a[6] = {-3.969683028665376e+01, 2.209460984245205e+02,
                                -2.759285104469687e+02, 1.383577518672690e+02,
                                -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[5] = {-5.447609879822406e+01, 1.615858368580409e+02,
                                -1.556989798598866e+02, 6.680131188771972e+01,
                                -1.328068155288572e+01};
    static const double c[6] = {-7.784894002430293e-03, -3.223964580411365e-01,
                                -2.400758277161838e+00, -2.549732539343734e+00,
                                4.374664141464968e+00, 2.938163982698783e+00};
    static const double d[4] = {7.784695709041462e-03, 3.224671290700398e-01,
                                2.445134137142996e+00, 3.754408661907416e+00};
    const double pl = 0.02425;
    if (p <= 0.0 || p >= 1.0) { return 0.0; }
    if (p < pl) {
        const double q = std::sqrt(-2.0 * std::log(p));
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q
                + c[5])
             / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    if (p > 1.0 - pl) {
        const double q = std::sqrt(-2.0 * std::log(1.0 - p));
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q
                 + c[5])
             / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    const double q = p - 0.5;
    const double r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5])
         * q
         / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
}

} // namespace detail

/// HISTORICAL VaR and ES. No distributional assumption.
///
/// The empirical quantile, and the mean of everything beyond it. Its blind
/// spot is stated rather than buried: it CANNOT produce a loss larger than the
/// worst observation in the window, so on 250 days of calm it will report that
/// 2008 is impossible.
[[nodiscard]] inline std::expected<VarResult, VarError>
historical_var(std::vector<double> returns, double confidence) {
    if (returns.size() < 30) {
        return std::unexpected(VarError::TooFewSamples);
    }
    if (!(confidence > 0.5) || !(confidence < 1.0)) {
        return std::unexpected(VarError::BadConfidence);
    }
    std::sort(returns.begin(), returns.end());       // worst first
    const double alpha = 1.0 - confidence;
    // Floor at one so the tail is never empty; the caller sees tail_n and can
    // judge whether one observation is an estimate.
    auto k = static_cast<std::size_t>(alpha * static_cast<double>(returns.size()));
    if (k < 1) { k = 1; }

    VarResult out;
    out.var = -returns[k - 1];                       // positive loss
    double acc = 0.0;
    for (std::size_t i = 0; i < k; ++i) { acc -= returns[i]; }
    out.expected_shortfall = acc / static_cast<double>(k);
    out.tail_n = k;
    return out;
}

/// PARAMETRIC (Gaussian) VaR and ES.
///
/// Closed form, extrapolates past the sample, and assumes the one thing daily
/// equity returns are most famously not. Kept because it is what most desks
/// quote and because the GAP between it and the historical number is the
/// single most informative output of this file.
///
/// ES for a normal has a closed form: sigma * phi(z) / alpha - mu.
[[nodiscard]] inline std::expected<VarResult, VarError>
parametric_var(const double* r, std::size_t n, double confidence) noexcept {
    if (r == nullptr || n < 30) {
        return std::unexpected(VarError::TooFewSamples);
    }
    if (!(confidence > 0.5) || !(confidence < 1.0)) {
        return std::unexpected(VarError::BadConfidence);
    }
    double mu = 0.0;
    for (std::size_t i = 0; i < n; ++i) { mu += r[i]; }
    mu /= static_cast<double>(n);
    double s2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = r[i] - mu;
        s2 += d * d;
    }
    const double sd = std::sqrt(s2 / static_cast<double>(n - 1));
    if (!(sd > 0.0)) { return std::unexpected(VarError::Degenerate); }

    const double alpha = 1.0 - confidence;
    const double z = detail::inv_norm_cdf(alpha);       // negative
    VarResult out;
    out.var = -(mu + z * sd);
    const double phi = std::exp(-0.5 * z * z) / std::sqrt(6.283185307179586);
    out.expected_shortfall = sd * phi / alpha - mu;
    out.tail_n = n;                                    // the whole sample fits it
    return out;
}

/// MONTE CARLO VaR, resampling the EMPIRICAL distribution.
///
/// Bootstrapping the observed returns rather than drawing from a fitted
/// normal, deliberately. Simulating from a normal would make this a slower
/// parametric VaR wearing a costume -- it would inherit exactly the tail
/// assumption the historical number exists to challenge. Resampling keeps the
/// real tail and adds what MC is actually for: an error bar on the quantile,
/// which neither of the other two produces.
[[nodiscard]] inline std::expected<VarResult, VarError>
monte_carlo_var(const std::vector<double>& returns, double confidence,
                std::size_t paths, std::size_t horizon, std::uint64_t seed) {
    if (returns.size() < 30) {
        return std::unexpected(VarError::TooFewSamples);
    }
    if (!(confidence > 0.5) || !(confidence < 1.0)) {
        return std::unexpected(VarError::BadConfidence);
    }
    if (paths < 100 || horizon == 0) {
        return std::unexpected(VarError::TooFewSamples);
    }
    std::uint64_t s = seed != 0 ? seed : 0x9E3779B97F4A7C15ull;
    auto next = [&s](std::size_t m) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return static_cast<std::size_t>(s % m);
    };
    std::vector<double> sim;
    sim.reserve(paths);
    for (std::size_t p = 0; p < paths; ++p) {
        double acc = 0.0;
        for (std::size_t h = 0; h < horizon; ++h) {
            acc += returns[next(returns.size())];
        }
        sim.push_back(acc);
    }
    return historical_var(std::move(sim), confidence);
}

} // namespace altair
