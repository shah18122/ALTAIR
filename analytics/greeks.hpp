// analytics/greeks.hpp — Black-76 price and first-order greeks.
//
// P3-01. Kite exposes no greeks, no IV and no vol -- verified against the whole
// gokiteconnect v4.4.2 surface, where the richest payload is price, quantity,
// OI, OHLC and five levels of depth. So they are computed here.
//
// They would be computed here even if Kite offered them:
//
//   * Rule 6 -- backtest and live share ONE code path. Fetched greeks live and
//     computed greeks in backtest would diverge, and the backtest would be a
//     lie. Nobody stores historical greeks per tick.
//   * Rule 10 -- a fetched greek carries an unknown IV model, an unknown rate
//     and an unknown dividend assumption. It cannot enter the reproducibility
//     tuple because nobody knows what produced it.
//   * Latency -- retail APIs cap at 10-50 ms round trips (CLAUDE.md's own
//     reality check). Portfolio greeks re-evaluate the WHOLE chain per tick.
//   * A hypothetical position has no API. "What is my delta if I sell this
//     strike" is a question only a model answers.
//
// BLACK-76 ON A FORWARD, not Black-Scholes on spot.
//
// NIFTY and BANKNIFTY options are European and cash-settled on the index. The
// forward is what actually prices them, and taking it as an INPUT avoids
// embedding a dividend-yield model this card has no business owning: the caller
// supplies the future's price, or a synthetic forward from put-call parity.
// One fewer estimated parameter is one fewer place to be quietly wrong.
//
// Decisions D1..D7 are fixed in prompts/P3-01_greeks.md.

#pragma once

#include <core/types/units.hpp>

#include <cmath>
#include <cstdint>
#include <expected>

namespace altair {

enum class OptionRight : std::uint8_t { Call, Put };

enum class GreekError : std::uint8_t {
    /// T <= 0. At or past expiry the option is an exercise decision, not a
    /// derivative: gamma at the money is genuinely INFINITE, and returning a
    /// large number would size a position on infinity (D4, rule 9).
    Expired,
    /// sigma <= 0. Same degeneracy: with no vol there is no distribution.
    NonPositiveVol,
    /// Forward or strike <= 0. Black-76 takes logs of both.
    NonPositivePrice,
    NotFinite
};

/// First-order greeks, in their MATHEMATICAL units.
///
/// D3 — these are raw partial derivatives, NOT the desk conventions. Vega is
/// per unit of vol (sigma = 1.0, i.e. 100 vol points), and theta is per YEAR.
/// The conventional "vega per 1% vol" and "theta per day" are scalings, and
/// scalings applied twice or not at all are where greek bugs hide. Convert
/// explicitly with the helpers below, at the point of display or sizing.
struct Greeks {
    /// Option value in the same units as forward and strike. UNIT: paise.
    double price = 0.0;
    /// dV/dF. UNIT: dimensionless.
    double delta = 0.0;
    /// d2V/dF2. UNIT: 1/paise.
    double gamma = 0.0;
    /// dV/dsigma, per UNIT vol. UNIT: paise per 1.0 of vol.
    double vega = 0.0;
    /// dV/dt, per YEAR. Negative for a long option. UNIT: paise per year.
    double theta = 0.0;
    /// dV/dr, per UNIT rate. UNIT: paise per 1.0 of rate.
    double rho = 0.0;
};

/// Desk conventions, named so nobody applies them twice.
[[nodiscard]] constexpr double vega_per_vol_point(double vega) noexcept {
    return vega * 0.01;                 // one vol POINT is 0.01 of sigma
}

/// Calendar days, not trading days. A weekend decays an option.
[[nodiscard]] constexpr double theta_per_day(double theta) noexcept {
    return theta / 365.0;
}

namespace detail {

/// Standard normal CDF.
///
/// Built on erfc, NOT erf. For large negative x, erf(x) approaches -1 and the
/// textbook 0.5 * (1 + erf(x/sqrt2)) loses every significant digit to
/// cancellation -- exactly where deep out-of-the-money options live, and
/// exactly where a delta of 1e-9 versus 0 decides whether a hedge exists.
/// erfc is defined to stay accurate in that tail.
[[nodiscard]] inline double norm_cdf(double x) noexcept {
    return 0.5 * std::erfc(-x * 0.70710678118654752440084436210485);
}

/// Standard normal PDF.
[[nodiscard]] inline double norm_pdf(double x) noexcept {
    constexpr double kInvSqrt2Pi = 0.39894228040143267793994605993438;
    return kInvSqrt2Pi * std::exp(-0.5 * x * x);
}

[[nodiscard]] inline bool finite(double x) noexcept {
    return std::isfinite(x);
}

} // namespace detail

/// Black-76 price and first-order greeks.
///
/// `forward` and `strike` in paise; `t` in years; `vol` annualised; `rate`
/// the continuously-compounded risk-free rate as a plain fraction.
///
/// Returns an error rather than a limit value at T <= 0 or sigma <= 0 (D4).
[[nodiscard]] ALTAIR_HOT inline std::expected<Greeks, GreekError>
black76(OptionRight right, Price forward, Price strike, Years t, Vol vol,
        double rate) noexcept {
    const double F = static_cast<double>(forward.raw());
    const double K = static_cast<double>(strike.raw());
    const double T = t.raw();
    const double s = vol.raw();

    if (F <= 0.0 || K <= 0.0) {
        return std::unexpected(GreekError::NonPositivePrice);
    }
    if (!(T > 0.0)) {
        return std::unexpected(GreekError::Expired);
    }
    if (!(s > 0.0)) {
        return std::unexpected(GreekError::NonPositiveVol);
    }
    if (!detail::finite(F) || !detail::finite(K) || !detail::finite(T)
        || !detail::finite(s) || !detail::finite(rate)) {
        return std::unexpected(GreekError::NotFinite);
    }

    const double sqrtT = std::sqrt(T);
    const double vsqrtT = s * sqrtT;
    // log(F/K) rather than log(F) - log(K): one rounding instead of two, and
    // the difference of two large logs is exactly where near-the-money
    // precision goes.
    const double d1 = (std::log(F / K) + 0.5 * s * s * T) / vsqrtT;
    const double d2 = d1 - vsqrtT;
    const double df = std::exp(-rate * T);          // discount factor

    const double nd1 = detail::norm_cdf(d1);
    const double nd2 = detail::norm_cdf(d2);
    const double pdf1 = detail::norm_pdf(d1);

    Greeks g{};
    if (right == OptionRight::Call) {
        g.price = df * (F * nd1 - K * nd2);
        g.delta = df * nd1;
        // rho is dV/dr. Under Black-76 the ONLY r-dependence is the discount
        // factor, so rho = -T * V. Not the Black-Scholes-on-spot form, where
        // r also drifts the forward -- using that here would double-count.
        g.rho = -T * g.price;
        g.theta = -(F * df * pdf1 * s) / (2.0 * sqrtT)
                + rate * df * (F * nd1 - K * nd2);
    } else {
        const double nmd1 = detail::norm_cdf(-d1);
        const double nmd2 = detail::norm_cdf(-d2);
        g.price = df * (K * nmd2 - F * nmd1);
        g.delta = -df * nmd1;
        g.rho = -T * g.price;
        g.theta = -(F * df * pdf1 * s) / (2.0 * sqrtT)
                + rate * df * (K * nmd2 - F * nmd1);
    }

    // Gamma and vega are RIGHT-INDEPENDENT: a call and a put on the same
    // strike differ by a forward contract, which has neither convexity nor
    // vol sensitivity. Computing them once rather than per branch makes that
    // identity impossible to break by editing one side.
    g.gamma = df * pdf1 / (F * vsqrtT);
    g.vega  = df * F * pdf1 * sqrtT;

    if (!detail::finite(g.price) || !detail::finite(g.delta)
        || !detail::finite(g.gamma) || !detail::finite(g.vega)
        || !detail::finite(g.theta) || !detail::finite(g.rho)) {
        return std::unexpected(GreekError::NotFinite);
    }
    return g;
}

/// Intrinsic value at expiry. UNIT: paise.
///
/// Separate from `black76` on purpose: at T = 0 an option is an exercise
/// decision, not a derivative, and pretending otherwise is how a position gets
/// sized on an infinite gamma.
[[nodiscard]] constexpr Price intrinsic(OptionRight right, Price forward,
                                        Price strike) noexcept {
    const std::int64_t d = (right == OptionRight::Call)
                         ? forward.raw() - strike.raw()
                         : strike.raw() - forward.raw();
    return Price{d > 0 ? d : 0};
}

/// Put-call parity residual: C - P - df*(F - K). UNIT: paise.
///
/// Zero for a consistent pair. Exposed rather than kept in a test because it
/// is a runtime invariant worth checking on a live surface: a residual that
/// drifts means the forward being used is not the one the market is pricing.
[[nodiscard]] inline double parity_residual(double call, double put,
                                            Price forward, Price strike,
                                            Years t, double rate) noexcept {
    const double df = std::exp(-rate * t.raw());
    return call - put
         - df * (static_cast<double>(forward.raw())
                 - static_cast<double>(strike.raw()));
}

} // namespace altair
