// research/models/gamma_scalping.hpp -- the delta-hedged option P&L, and what
// discrete hedging and costs do to it.
//
// Background: N. Bawa, "Gamma Scalping and the Volatility Risk Premium"
// (Medium, 2026-04-01). The article itself could not be fetched; its core
// formula, as quoted to us, is (1) below written both ways,
//     P&L ~ 1/2 Gamma dS^2 - Theta dt = 1/2 Gamma S^2 dt (sigma_r^2 - sigma_i^2),
// profitable only when sigma_r > sigma_i, and path dependent because each
// interval is weighted by the gamma held then. The test checks both points.
// The rest are the standard results that go with it:
//
//   * One hedge interval of a delta-hedged option, priced at implied vol
//     sigma_i, earns (theta-gamma balance, Black-Scholes)
//         dPnL = 1/2 Gamma (dF^2 - F^2 sigma_i^2 dt)                (1)
//     so long gamma wins when the market moves more than implied, and short
//     gamma -- the variance-premium seller of strategies/vol_premium.hpp --
//     wins when it moves less.
//   * Summed to expiry, for a near-the-money option,
//         E[PnL] ~ vega (sigma_r - sigma_i)                         (2)
//   * Hedging N times instead of continuously leaves a hedging error with
//     standard deviation (Kamal & Derman 1999, "When you cannot hedge
//     continuously", Risk 12(1))
//         sd ~ sqrt(pi/4) vega sigma / sqrt(N)                      (3)
//   * Proportional costs k (round trip, as a fraction of the price) act like
//     a change of volatility (Leland 1985, J. Finance 40(5)):
//         sigma_L^2 = sigma^2 (1 +- sqrt(2/pi) k / (sigma sqrt(dt)))  (4)
//     + for a long option's hedger (pays more to hedge: needs more realised
//     vol to break even), - for a short one.
//
// The test checks (2) and (3) by Monte Carlo against a Black-76 hedge.
//
// ON THE HEADLINE. As reported, SEBI's interim order on Jane Street (July
// 2025) is about alleged manipulation of BANKNIFTY on expiry days: buying the index's
// stocks and futures in the morning while holding large option positions,
// and selling them later in the day. That is not gamma scalping. It is not
// something to build, and nothing here does.

#pragma once

#include <cmath>
#include <numbers>

namespace altair {

/// (1): one interval's P&L for an option of the given gamma, priced at
/// sigma_i, when the forward moved by dF from F over dt years. Long gamma
/// sign; negate for a short option.
[[nodiscard]] inline double gamma_scalp_step(double gamma, double F, double dF,
                                             double sigma_i, double dt) noexcept {
    return 0.5 * gamma * (dF * dF - F * F * sigma_i * sigma_i * dt);
}

/// (2): expected P&L of a long, delta-hedged near-the-money option held to
/// expiry when sigma_r is realised against sigma_i paid.
[[nodiscard]] inline double gamma_scalp_expected(double vega, double sigma_r,
                                                 double sigma_i) noexcept {
    return vega * (sigma_r - sigma_i);
}

/// (3): standard deviation of the hedging error from n equally spaced hedges.
[[nodiscard]] inline double gamma_scalp_hedge_error(double vega, double sigma,
                                                    double n) noexcept {
    return n > 0.0 ? std::sqrt(std::numbers::pi / 4.0) * vega * sigma / std::sqrt(n) : 0.0;
}

/// (4): Leland's cost-adjusted volatility for round-trip cost k and hedge
/// interval dt (years). For a short option the adjustment lowers sigma; it is
/// floored at zero, where hedging costs exceed the whole premium.
[[nodiscard]] inline double gamma_scalp_leland_vol(double sigma, double k, double dt,
                                                   bool long_option) noexcept {
    if (!(sigma > 0.0) || !(dt > 0.0)) { return sigma; }
    const double a = std::sqrt(2.0 / std::numbers::pi) * k / (sigma * std::sqrt(dt));
    const double v = sigma * sigma * (1.0 + (long_option ? a : -a));
    return v > 0.0 ? std::sqrt(v) : 0.0;
}

} // namespace altair
