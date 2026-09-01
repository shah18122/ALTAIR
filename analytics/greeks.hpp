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

/// Which variable delta and gamma differentiate against.
///
/// Black-76 delta is dV/dF; Black-Scholes delta is dV/dS. They differ by the
/// factor e^((r-q)T) -- about 0.44% on a 30-day NIFTY option. Small enough to
/// read as rounding, large enough to leave a book systematically under-hedged
/// in one direction every day it is not caught.
///
/// Ordinal 0 is Unspecified ON PURPOSE. A default-constructed Greeks must not
/// read as a valid basis: a zeroed struct that satisfies its own checker is
/// the single most-repeated defect class in this repo.
enum class GreekBasis : std::uint8_t { Unspecified = 0, Forward, Spot };

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
    /// Which variable `delta` and `gamma` differentiate against. Set by every
    /// pricing function; never Unspecified on a returned value.
    GreekBasis basis = GreekBasis::Unspecified;
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

/// Black-76 with validation already performed and every argument in double.
///
/// Extracted so `analytics/iv.hpp` can iterate on the EXACT formula the pricer
/// uses, without round-tripping the forward through integer paise on every
/// step -- and, more importantly, so there is exactly ONE Black-76 in the
/// codebase. A solver that carries its own copy of the pricer is a solver that
/// inverts a different model from the one that quotes, and the two drift apart
/// the first time either is edited.
///
/// Not part of the public surface: rule 2 keeps `double` money off module
/// boundaries, and `detail::` is not a boundary. Callers outside this header
/// go through `black76`, which takes `Price`.
[[nodiscard]] inline Greeks black76_unchecked(OptionRight right, double F,
                                              double K, double T, double s,
                                              double rate) noexcept {
    const double sqrtT = std::sqrt(T);
    const double vsqrtT = s * sqrtT;
    // log(F/K) rather than log(F) - log(K): one rounding instead of two, and
    // the difference of two large logs is exactly where near-the-money
    // precision goes.
    const double d1 = (std::log(F / K) + 0.5 * s * s * T) / vsqrtT;
    const double d2 = d1 - vsqrtT;
    const double df = std::exp(-rate * T);          // discount factor

    const double nd1 = norm_cdf(d1);
    const double nd2 = norm_cdf(d2);
    const double pdf1 = norm_pdf(d1);

    Greeks g{};
    g.basis = GreekBasis::Forward;

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
        const double nmd1 = norm_cdf(-d1);
        const double nmd2 = norm_cdf(-d2);
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
    return g;
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

    const Greeks g = detail::black76_unchecked(right, F, K, T, s, rate);

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


// ---------------------------------------------------------------------------
// Black-Scholes on SPOT.
//
// P3-04 addition. This is NOT a second model -- it is the SAME model in a
// different parameterisation. Substituting F = S*e^((r-q)T) into Black-76
// reproduces Black-Scholes exactly, and the test asserts the two prices agree
// to 1e-10 paise on identical inputs.
//
// It exists because the greeks are NOT the same numbers, and mixing the two
// conventions is a live way to mis-hedge:
//
//   * delta_spot = delta_fwd * e^((r-q)T). On a 30-day NIFTY option at
//     r = 6.5%, q = 1.2%, that factor is 1.004366 -- a 0.44% error. Small
//     enough to read as rounding, large enough to leave a book systematically
//     under-hedged in one direction every single day.
//   * rho is not even the same SHAPE. Under Black-76 the rate enters only the
//     discount factor, so rho = -T*V and is NEGATIVE for a long call. Under
//     Black-Scholes the rate ALSO drifts the forward, so rho = K*T*e^(-rT)*N(d2)
//     and is POSITIVE for a long call. On the reference case that is
//     -Rs 38.19 versus +Rs 1043.44 -- opposite sign, 27x magnitude.
//
// Which to use is a question about the INPUT, not a preference:
//
//   * A listed future quotes the forward directly. Use black76 -- it is one
//     fewer estimated parameter, and the market's own forward already contains
//     whatever the dividend and repo actually are.
//   * A cash index or a stock quotes spot. Use black_scholes, and own the
//     dividend-yield assumption explicitly rather than smuggling it into a
//     forward somebody guessed.
// ---------------------------------------------------------------------------

/// Forward implied by spot under cost-of-carry: F = S * e^((r - q) T).
///
/// Rounded to whole paise, because Price is integer paise (rule 3). That
/// rounding is why `black_scholes` does NOT route through this function: it
/// carries the forward in double internally. This helper is for display and
/// for reconciling a computed forward against a listed future, where a
/// half-paise is noise; inside a pricer it is a source of drift.
[[nodiscard]] inline std::expected<Price, GreekError>
forward_from_spot(Price spot, Years t, double rate,
                  double dividend_yield) noexcept {
    const double S = static_cast<double>(spot.raw());
    const double T = t.raw();
    if (S <= 0.0) {
        return std::unexpected(GreekError::NonPositivePrice);
    }
    if (!detail::finite(S) || !detail::finite(T) || !detail::finite(rate)
        || !detail::finite(dividend_yield)) {
        return std::unexpected(GreekError::NotFinite);
    }
    const double F = S * std::exp((rate - dividend_yield) * T);
    if (!detail::finite(F) || F <= 0.0) {
        return std::unexpected(GreekError::NotFinite);
    }
    // Round half away from zero. F is positive here, so this is a plain +0.5.
    return Price{static_cast<std::int64_t>(F + 0.5)};
}

/// Black-Scholes price and first-order greeks, on SPOT, with a continuous
/// dividend yield.
///
/// `spot` and `strike` in paise; `t` in years; `vol` annualised; `rate` and
/// `dividend_yield` continuously compounded, as plain fractions.
///
/// Delta and gamma are with respect to SPOT (`basis == GreekBasis::Spot`).
/// Vega and theta are in the same units as `black76`. Rho is the full
/// Black-Scholes rho, which is NOT -T*V -- see the note above.
///
/// Same refusals as `black76` at T <= 0 and sigma <= 0 (D4).
[[nodiscard]] ALTAIR_HOT inline std::expected<Greeks, GreekError>
black_scholes(OptionRight right, Price spot, Price strike, Years t, Vol vol,
              double rate, double dividend_yield) noexcept {
    const double S = static_cast<double>(spot.raw());
    const double K = static_cast<double>(strike.raw());
    const double T = t.raw();
    const double s = vol.raw();

    if (S <= 0.0 || K <= 0.0) {
        return std::unexpected(GreekError::NonPositivePrice);
    }
    if (!(T > 0.0)) {
        return std::unexpected(GreekError::Expired);
    }
    if (!(s > 0.0)) {
        return std::unexpected(GreekError::NonPositiveVol);
    }
    if (!detail::finite(S) || !detail::finite(K) || !detail::finite(T)
        || !detail::finite(s) || !detail::finite(rate)
        || !detail::finite(dividend_yield)) {
        return std::unexpected(GreekError::NotFinite);
    }

    const double sqrtT = std::sqrt(T);
    const double vsqrtT = s * sqrtT;
    const double d1 = (std::log(S / K) + (rate - dividend_yield + 0.5 * s * s) * T)
                    / vsqrtT;
    const double d2 = d1 - vsqrtT;

    const double dq = std::exp(-dividend_yield * T);   // dividend discount
    const double dr = std::exp(-rate * T);             // rate discount
    const double pdf1 = detail::norm_pdf(d1);

    Greeks g{};
    g.basis = GreekBasis::Spot;

    // The decay term is right-independent -- a call and a put on one strike
    // differ by a forward, which has no gamma and no vega, and whose only
    // theta is carry. Computing it once keeps that identity unbreakable.
    const double decay = -(S * dq * pdf1 * s) / (2.0 * sqrtT);

    if (right == OptionRight::Call) {
        const double nd1 = detail::norm_cdf(d1);
        const double nd2 = detail::norm_cdf(d2);
        g.price = S * dq * nd1 - K * dr * nd2;
        g.delta = dq * nd1;
        g.rho   = K * T * dr * nd2;
        g.theta = decay + dividend_yield * S * dq * nd1 - rate * K * dr * nd2;
    } else {
        const double nmd1 = detail::norm_cdf(-d1);
        const double nmd2 = detail::norm_cdf(-d2);
        g.price = K * dr * nmd2 - S * dq * nmd1;
        g.delta = -dq * nmd1;
        g.rho   = -K * T * dr * nmd2;
        g.theta = decay - dividend_yield * S * dq * nmd1 + rate * K * dr * nmd2;
    }

    g.gamma = dq * pdf1 / (S * vsqrtT);
    g.vega  = S * dq * pdf1 * sqrtT;

    if (!detail::finite(g.price) || !detail::finite(g.delta)
        || !detail::finite(g.gamma) || !detail::finite(g.vega)
        || !detail::finite(g.theta) || !detail::finite(g.rho)) {
        return std::unexpected(GreekError::NotFinite);
    }
    return g;
}

/// Convert a Black-76 forward delta to a Black-Scholes spot delta.
///
/// delta_spot = delta_fwd * dF/dS = delta_fwd * e^((r-q)T).
///
/// Named rather than inlined at call sites so that the conversion is one
/// reviewable place, and so a search for it finds every book that mixes bases.
[[nodiscard]] inline double forward_delta_to_spot(double delta_fwd, Years t,
                                                  double rate,
                                                  double dividend_yield) noexcept {
    return delta_fwd * std::exp((rate - dividend_yield) * t.raw());
}

} // namespace altair
