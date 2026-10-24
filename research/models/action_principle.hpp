// research/models/action_principle.hpp -- Manhire's "physical laws" price bound.
//
// J.T. Manhire, "Predicting Stock Market Prices with Physical Laws" (also
// "The Action Principle in Market Mechanics"), Texas A&M, 2017; the version
// accepted for the 13th Annual Econophysics Colloquium, University of Warsaw,
// July 2017 (checked against the PDF). Equation numbers are that version's.
//
// THE MODEL.
//
// Over a period of t sessions the price displacement x is driven by the net
// of buying and selling, x ~ alpha - beta, and treated as a simple harmonic
// oscillator, m x'' = -kappa x. So x = |A| cos(phi) is BOUNDED by +-|A|, and
// with p the share of buying, x = |A| (2p - 1). From history:
//     |A| = |x-bar| / (2 p-bar - 1)                                    (6)
// With kinetic energy taken as zero, L = -m x^2 / 2t^2, the action is
//     S = m x^2 / 2t                                                   (11)
// and Postulate III sets phi^2 = S, so phi = x sqrt(m / 2t). The inertial
// coefficient is
//     m = 2t (pi / |A|)^2                                              (12)
// which makes phi = pi x / |A|, and the probabilities are
//     Pr(|x| <  X) = erf(X sqrt(m/2t))^2                 (13, unnumbered form)
//     Pr(|x| >= X) = erfc(X sqrt(m/2t))^2                              (17)
//     Pr(|x| =  X) = (m / 2 pi t) exp(-m X^2 / t)                      (16)
// and the paper reports (16) within 0.01 and (17) within 0.02 of SPY's and
// DIA's historical frequencies.
// The paper's check: SPY weekly 2002-08, |x-bar| = 1.76, p-bar = 0.5398,
// |A| = 22.11 points, m = 0.2019; the October 2008 crash week was -21.84,
// inside the bound. DIA: |A| = 20.25, m = 0.2407, crash week -19.45.
//
// WHAT THE PAPER LEAVES OPEN, AND WHAT IS DONE HERE.
//
//   * p is "buyer activity / (buyer + seller activity)" and the paper never
//     says how it is measured. Here p-bar is the share of periods that closed
//     up, and |x-bar| the mean absolute displacement -- the reading under
//     which the SPY numbers are plausible for weekly moves.
//   * (6) divides by 2 p-bar - 1. With p-bar near one half -- which is what a
//     market close to a random walk gives -- the bound is very sensitive:
//     p-bar 0.53 instead of 0.54 moves |A| by a third, and p-bar <= 0.5 gives
//     no bound at all. amplitude() refuses p-bar <= 0.5 rather than return a
//     negative or infinite "limit".
//   * (17) does not add up: erf^2 + erfc^2 = 1 - 2 erf erfc, which is less
//     than 1 everywhere except the ends (0.50 at phi = 0.48), although the
//     paper says Pr(>=) = 1 - Pr(<). Both are returned exactly as published;
//     the test asserts the shortfall so nobody reads them as a distribution.
//     A caller wanting one tail should take 1 - within().
//   * (16) is not normalised either: it integrates to sqrt(m / 4 pi t), about
//     0.06 for SPY.
//   * The closing suggestion that the best option strike width is sqrt(m)
//     (0.45 for SPY, 0.49 for DIA, against 0.50 listed) is not implemented:
//     m scales with 1/price^2, so sqrt(m) changes with the units the price is
//     quoted in. For NIFTY it comes out near 0.01 points against 50-point
//     strikes.
//
// ON NIFTY. The test re-runs the paper's two checks on weekly NIFTY moves in
// three windows (fit 2002-08 as the paper did, 2013-19 and 2016-23). The
// bound is broken after every fit, the October 2008 crash week included, and
// it is broken inside the 2002-08 fit itself (|A| 331 points, largest week
// 998). erfc^2 misses the share of big weeks by 0.11 to 0.22.
//
// WHAT IT IS NOT. A forecast of direction or timing (the paper says so), or
// a limit to size or stop against: on NIFTY it is not a limit at all.

#pragma once

#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <span>

namespace altair {

struct ActionBound {
    double amplitude = 0.0;   ///< |A|, price units: the largest displacement
    double inertia = 0.0;     ///< m, eq. 12
    double period = 0.0;      ///< t, sessions per displacement

    /// phi for a displacement of size X: X sqrt(m / 2t) = pi X / |A|.
    [[nodiscard]] double phase(double x) const noexcept {
        return std::fabs(x) * std::sqrt(inertia / (2.0 * period));
    }
    /// Eq. 17, as published: Pr(|x| < X) = erf(phi)^2.
    [[nodiscard]] double within(double x) const noexcept {
        const double e = std::erf(phase(x));
        return e * e;
    }
    /// Eq. 17, as published: Pr(|x| >= X) = erfc(phi)^2. NOT 1 - within().
    [[nodiscard]] double beyond(double x) const noexcept {
        const double e = std::erfc(phase(x));
        return e * e;
    }
    /// Eq. 16: the density the paper gives at |x| = X.
    [[nodiscard]] double density(double x) const noexcept {
        return inertia / (2.0 * std::numbers::pi * period)
             * std::exp(-inertia * x * x / period);
    }
    /// Is a displacement inside the oscillator's limit?
    [[nodiscard]] bool inside(double x) const noexcept {
        return std::fabs(x) <= amplitude;
    }
};

/// Eq. 6 and 12 from the two historical averages. Empty when p-bar <= 0.5,
/// where (6) gives no bound.
[[nodiscard]] inline std::optional<ActionBound>
action_bound(double mean_abs_move, double p_up, double period) noexcept {
    if (!(p_up > 0.5) || !(mean_abs_move > 0.0) || !(period > 0.0)) {
        return std::nullopt;
    }
    ActionBound b;
    b.period = period;
    b.amplitude = mean_abs_move / (2.0 * p_up - 1.0);
    const double r = std::numbers::pi / b.amplitude;
    b.inertia = 2.0 * period * r * r;
    return b;
}

/// The same from closes: non-overlapping displacements of `period` sessions,
/// x-bar their mean absolute size, p-bar the share that went up.
[[nodiscard]] inline std::optional<ActionBound>
action_bound_from_closes(std::span<const double> closes,
                         std::size_t period) noexcept {
    if (period == 0 || closes.size() <= period) { return std::nullopt; }
    double sum_abs = 0.0;
    std::size_t n = 0, up = 0;
    for (std::size_t i = period; i < closes.size(); i += period) {
        const double x = closes[i] - closes[i - period];
        sum_abs += std::fabs(x);
        up += x > 0.0 ? 1 : 0;
        ++n;
    }
    if (n == 0) { return std::nullopt; }
    return action_bound(sum_abs / static_cast<double>(n),
                        static_cast<double>(up) / static_cast<double>(n),
                        static_cast<double>(period));
}

} // namespace altair
