// oms/alpha_execution.hpp -- Almgren-Chriss with a short-term alpha signal.
//
// Lai Jien Weng, "Optimal Execution with Alpha Signals" (Monash, 2026-03-01,
// SSRN 6323159).
//
// THE MODEL (paper section 3).
//
// A buyer must acquire X over [0, T]; x is the quantity still to buy and
// v = -dx/dt the buying rate. The price drifts with a predictive signal f,
//     dS = (f + gamma v) dt + sigma dW,     df = -phi f dt + nu dW^f,
// and the trader minimises
//     J = E[ int_t^T (eta v^2 + lambda sigma^2 x^2 + f x) ds ]       (eq. 3)
// eta v^2 is temporary impact, lambda sigma^2 x^2 the risk of the unbought
// remainder, f x the drift paid on it. Permanent impact adds gamma X^2 / 2
// whatever the schedule, so it drops out of the control.
//
// THE SOLUTION (eq. 7-11). W = A x^2 + B x f + C f^2 + D solves the HJB, with
//     A' - A^2/eta + lambda sigma^2 = 0,     A(T) -> infinity       (eq. 8)
//     B' - (A/eta + phi) B + 1 = 0,          B(T) = 0               (eq. 9)
// so A = eta kappa coth(kappa (T - t)), kappa = sqrt(lambda sigma^2 / eta),
// and the optimal rate is affine in inventory and signal:
//     v* = kappa coth(kappa tau) x  +  B(tau) f / (2 eta),   tau = T - t
//          '--- Almgren-Chriss ---'    '- signal overlay -'          (eq. 11)
// The buy-side mandate (never sell to chase alpha) clamps it at zero (eq. 12).
//
// ONE CORRECTION TO THE PAPER.
//
// Eq. 10 prints B(t) = int_t^T exp(-phi (s-t)) sinh(kappa(T-t)) /
// sinh(kappa(T-s)) ds. That ratio is upside down: as s -> T the integrand
// goes like 1/(T-s) and the integral DIVERGES, so B would be infinite at
// every t. Integrating eq. 9 with the integrating factor
// exp(-int (A/eta + phi)) = exp(-phi(s-t)) sinh(kappa(T-s)) / sinh(kappa(T-t))
// gives the ratio the other way up,
//     B(tau) = int_0^tau exp(-phi (tau - u)) sinh(kappa u) / sinh(kappa tau) du,
// which is finite, vanishes at tau = 0 as eq. 9 requires, and is what
// alpha_exec_b() returns in closed form. The test checks it against a
// direct numerical solution of eq. 9.
//
// WHAT IT IS NOT.
//
// A research rule, like almgren_chriss() beside it. It needs a calibrated
// signal (phi, nu, and a live f -- order-book imbalance, say) and an impact
// coefficient eta for the instrument. Nothing in oms/ routes a live parent
// through it.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace altair {

struct AlphaExecParams {
    /// Temporary impact: cost eta v^2 per unit time. Positive.
    double eta = 0.0;
    /// Risk aversion. Zero is allowed (the risk-neutral limit, TWAP).
    double lambda = 0.0;
    /// Price volatility, in price units per sqrt(time).
    double sigma = 0.0;
    /// Mean-reversion speed of the signal. Zero is allowed (a persistent
    /// drift).
    double phi = 0.0;

    [[nodiscard]] bool valid() const noexcept {
        return eta > 0.0 && lambda >= 0.0 && sigma >= 0.0 && phi >= 0.0;
    }
    [[nodiscard]] double kappa() const noexcept {
        return std::sqrt(lambda * sigma * sigma / eta);
    }
};

namespace alpha_exec_detail {

/// (1 - exp(-a t)) / a, continuous through a = 0 (where it is t).
[[nodiscard]] inline double one_minus_exp_over(double a, double t) noexcept {
    if (std::fabs(a * t) < 1e-12) { return t; }
    return -std::expm1(-a * t) / a;
}

} // namespace alpha_exec_detail

/// kappa coth(kappa tau): the Almgren-Chriss urgency, the rate per unit of
/// inventory. 1/tau in the risk-neutral limit, so x/tau is TWAP.
[[nodiscard]] inline double alpha_exec_urgency(const AlphaExecParams& p,
                                               double tau) noexcept {
    if (!(tau > 0.0)) { return INFINITY; }
    const double k = p.kappa();
    const double kt = k * tau;
    if (kt < 1e-8) { return 1.0 / tau; }
    return k / std::tanh(kt);
}

/// B(tau), the signal coefficient, from eq. 9 (see the correction above).
/// Written in decaying exponentials so a large kappa tau cannot overflow:
///   B = [ (1 - e^{-(phi+k)tau})/(phi+k) - e^{-2k tau}(1 - e^{-(phi-k)tau})/(phi-k) ]
///       / (1 - e^{-2k tau})
[[nodiscard]] inline double alpha_exec_b(const AlphaExecParams& p,
                                         double tau) noexcept {
    using alpha_exec_detail::one_minus_exp_over;
    if (!(tau > 0.0)) { return 0.0; }
    const double k = p.kappa(), ph = p.phi;
    if (k * tau < 1e-8) {
        // kappa -> 0: sinh(k u)/sinh(k tau) -> u/tau, and
        // int_0^tau e^{-phi(tau-u)} u du / tau = (tau - (1-e^{-phi tau})/phi)/(phi tau).
        if (ph * tau < 1e-8) { return 0.5 * tau; }
        return (tau - one_minus_exp_over(ph, tau)) / (ph * tau);
    }
    const double num = one_minus_exp_over(ph + k, tau)
                     - std::exp(-2.0 * k * tau) * one_minus_exp_over(ph - k, tau);
    return num / (-std::expm1(-2.0 * k * tau));
}

/// The optimal buying rate (eq. 11), or its buy-only clamp (eq. 12).
/// `x` is the quantity still to buy, `f` the current signal (positive: the
/// price is expected to rise, so buy sooner).
[[nodiscard]] inline double alpha_exec_rate(const AlphaExecParams& p,
                                            double tau, double x, double f,
                                            bool buy_only = true) noexcept {
    const double v = alpha_exec_urgency(p, tau) * x
                   + alpha_exec_b(p, tau) * f / (2.0 * p.eta);
    return buy_only ? std::max(0.0, v) : v;
}

} // namespace altair
