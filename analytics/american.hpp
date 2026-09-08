// analytics/american.hpp -- Bjerksund-Stensland American option approximation.
//
// P3-03.
//
// SCOPE NOTE, READ THIS FIRST. Nothing on an NSE path needs this today. NIFTY
// and BANKNIFTY index options have always been European, and NSE moved SINGLE
// STOCK options to European exercise years ago; MCX commodity options are
// European on futures. So as far as Altair currently trades, every option it
// will ever see is European and `black76` / `black_scholes` price all of them
// exactly. This header is here because ROADMAP section 674 lists it and
// because the premise is a market-structure fact rather than a mathematical
// one -- if Altair ever reaches a venue with American exercise, this is the
// right tool. Verify the exercise style against a current exchange circular
// before relying on either statement.
//
// WHAT IT IS. Bjerksund-Stensland (1993) prices an American option by assuming
// exercise at a FLAT boundary -- a single trigger price held constant to
// expiry -- rather than the true curved one. That assumption makes the value
// closed-form instead of a lattice.
//
// IT IS THEREFORE A LOWER BOUND, AND THAT MATTERS COMMERCIALLY.
//
// A sub-optimal exercise rule can only be worth less than the optimal one, so
// this ALWAYS under-prices. Measured against a 3000-step binomial:
//
//     S=42  K=40  T=.75  r=.08  b=-.04  v=.35     5.18026  vs  5.21786
//     S=100 K=100 T=.5   r=.10  b=-.05  v=.25     5.76949  vs  5.84035
//     S=110 K=100 T=.5   r=.10  b=-.05  v=.35    14.24709  vs  14.33620
//
// 0.2% to 1.2% low, never high. That direction is safe when BUYING and
// dangerous when SELLING: quoting this as the fair value of a short American
// option is quoting below what the option is worth, every time. The bound is
// asserted in the tests rather than merely described, so it cannot silently
// stop holding.
//
// PARAMETERISATION: spot with a COST OF CARRY b, matching `black_scholes`.
//   b = r        non-dividend-paying stock
//   b = r - q    continuous dividend yield q
//   b = 0        a future (Black-76 in this parameterisation)
// The carry is what drives early exercise, so it is an explicit argument
// rather than something inferred.
//
// A CALL WITH b >= r IS NEVER EXERCISED EARLY. Exactly, not approximately --
// the time value of the strike always exceeds the carry given up. So the
// American call on a non-dividend-paying stock IS the European call, and this
// header returns exactly that rather than an approximation of it.

#pragma once

#include <analytics/greeks.hpp>

#include <cmath>
#include <cstdint>
#include <expected>

namespace altair {

/// Price and the early-exercise premium, together.
///
/// The premium is returned because it is the number worth looking at: it says
/// how much of the value is optionality about WHEN to exercise rather than
/// about where the price goes. A premium of zero means the American and
/// European values coincide, which is a fact about the carry, not a rounding.
struct AmericanValue {
    /// American value. UNIT: paise.
    double price = 0.0;
    /// The corresponding European value. UNIT: paise.
    double european = 0.0;
    /// price - european, always >= 0. UNIT: paise.
    double early_exercise_premium = 0.0;
    /// The flat exercise trigger the approximation assumed, IN THE OPTION'S
    /// OWN COORDINATES. UNIT: paise.
    ///
    /// For a call it is a level the spot rises to; for a put, one it falls to.
    /// The put is priced through a spot/strike-swapped call (see
    /// `american_value`) and this field is transformed BACK out of those
    /// coordinates -- it was not, until P26-03, and a put reported a boundary
    /// above the money.
    ///
    /// Equal to 0 when early exercise is never optimal.
    double exercise_boundary = 0.0;
    /// True when the spot is already at or past the boundary, so the
    /// approximation returns intrinsic value directly.
    bool immediate_exercise = false;
};

namespace detail {

/// European price under the cost-of-carry parameterisation. UNIT: paise.
///
/// The same model as `black_scholes`, written in terms of b rather than q so
/// that the American formulas below -- which are all expressed in b -- do not
/// have to convert back and forth and get a sign wrong doing it.
[[nodiscard]] inline double
euro_carry(OptionRight right, double S, double K, double T, double v,
           double r, double b) noexcept {
    const double sq = std::sqrt(T);
    const double d1 = (std::log(S / K) + (b + 0.5 * v * v) * T) / (v * sq);
    const double d2 = d1 - v * sq;
    const double dq = std::exp((b - r) * T);
    const double dr = std::exp(-r * T);
    if (right == OptionRight::Call) {
        return S * dq * norm_cdf(d1) - K * dr * norm_cdf(d2);
    }
    return K * dr * norm_cdf(-d2) - S * dq * norm_cdf(-d1);
}

/// The phi function of Bjerksund-Stensland (1993).
[[nodiscard]] inline double
bs_phi(double S, double T, double gamma, double H, double X, double r,
       double b, double v) noexcept {
    const double v2 = v * v;
    const double sq = std::sqrt(T);
    const double lambda = (-r + gamma * b + 0.5 * gamma * (gamma - 1.0) * v2) * T;
    const double d = -(std::log(S / H) + (b + (gamma - 0.5) * v2) * T) / (v * sq);
    const double kappa = 2.0 * b / v2 + (2.0 * gamma - 1.0);
    return std::exp(lambda) * std::pow(S, gamma)
         * (norm_cdf(d)
            - std::pow(X / S, kappa)
              * norm_cdf(d - 2.0 * std::log(X / S) / (v * sq)));
}

/// American CALL by Bjerksund-Stensland. Everything in doubles, validated.
///
/// The put is not a separate formula -- see `american_value`.
[[nodiscard]] inline AmericanValue
bs_american_call(double S, double K, double T, double v, double r,
                 double b) noexcept {
    AmericanValue out{};
    out.european = euro_carry(OptionRight::Call, S, K, T, v, r, b);

    // b >= r: the carry given up by exercising always exceeds what is gained,
    // so early exercise is never optimal and the two values coincide EXACTLY.
    if (b >= r) {
        out.price = out.european;
        out.early_exercise_premium = 0.0;
        return out;
    }

    const double v2 = v * v;
    const double beta = (0.5 - b / v2)
                      + std::sqrt((b / v2 - 0.5) * (b / v2 - 0.5) + 2.0 * r / v2);
    const double b_inf = beta / (beta - 1.0) * K;
    const double b_zero = (K > r / (r - b) * K) ? K : r / (r - b) * K;
    const double h = -(b * T + 2.0 * v * std::sqrt(T))
                   * (b_zero / (b_inf - b_zero));
    const double X = b_zero + (b_inf - b_zero) * (1.0 - std::exp(h));
    out.exercise_boundary = X;

    if (S >= X) {
        // Already past the trigger: the approximation exercises immediately.
        out.immediate_exercise = true;
        out.price = S - K;
        out.early_exercise_premium = out.price - out.european;
        if (out.early_exercise_premium < 0.0) {
            // Cannot happen for a well-posed input, but if it ever did the
            // European value is the better answer and the bound must hold.
            out.price = out.european;
            out.early_exercise_premium = 0.0;
        }
        return out;
    }

    const double alpha = (X - K) * std::pow(X, -beta);
    out.price = alpha * std::pow(S, beta)
              - alpha * bs_phi(S, T, beta, X, X, r, b, v)
              + bs_phi(S, T, 1.0, X, X, r, b, v)
              - bs_phi(S, T, 1.0, K, X, r, b, v)
              - K * bs_phi(S, T, 0.0, X, X, r, b, v)
              + K * bs_phi(S, T, 0.0, K, X, r, b, v);

    // The approximation is a lower bound on the AMERICAN value, but it is not
    // guaranteed to beat the EUROPEAN one in every corner of the parameter
    // space -- and an American option is worth at least its European
    // counterpart by a no-arbitrage argument that owes nothing to any model.
    // So the floor is enforced rather than assumed.
    if (out.price < out.european) { out.price = out.european; }
    out.early_exercise_premium = out.price - out.european;
    return out;
}

} // namespace detail

/// American option value by Bjerksund-Stensland (1993).
///
/// `spot` and `strike` in paise; `t` in years; `vol` annualised; `rate` and
/// `carry` continuously compounded as plain fractions. See the header for what
/// `carry` means and why it is explicit.
///
/// Same refusals as `black76`: T <= 0 and sigma <= 0 are errors, not limits.
[[nodiscard]] inline std::expected<AmericanValue, GreekError>
american_value(OptionRight right, Price spot, Price strike, Years t, Vol vol,
               double rate, double carry) noexcept {
    const double S = static_cast<double>(spot.raw());
    const double K = static_cast<double>(strike.raw());
    const double T = t.raw();
    const double v = vol.raw();

    if (S <= 0.0 || K <= 0.0) {
        return std::unexpected(GreekError::NonPositivePrice);
    }
    if (!(T > 0.0)) { return std::unexpected(GreekError::Expired); }
    if (!(v > 0.0)) { return std::unexpected(GreekError::NonPositiveVol); }
    if (!detail::finite(S) || !detail::finite(K) || !detail::finite(T)
        || !detail::finite(v) || !detail::finite(rate)
        || !detail::finite(carry)) {
        return std::unexpected(GreekError::NotFinite);
    }

    AmericanValue out{};
    if (right == OptionRight::Call) {
        out = detail::bs_american_call(S, K, T, v, rate, carry);
    } else {
        // THE PUT IS THE CALL, TRANSFORMED. An American put on (S, K, r, b) is
        // worth exactly an American call on (K, S, r-b, -b): swap spot and
        // strike, and the roles of the two carry rates with them. This is an
        // exact identity, not an approximation, so the put inherits whatever
        // accuracy the call has and there is only ONE boundary formula in this
        // file to get wrong.
        const AmericanValue c =
            detail::bs_american_call(K, S, T, v, rate - carry, -carry);
        out.price = c.price;
        out.european = detail::euro_carry(OptionRight::Put, S, K, T, v, rate,
                                          carry);
        // TRANSFORM THE BOUNDARY BACK. P26-03.
        //
        // `c.exercise_boundary` is in the AUXILIARY call's coordinates, where
        // the "spot" is this put's STRIKE. Published unchanged it is a number
        // with a different meaning under the same field name -- and it looks
        // plausible, which is worse: for a 24,000 spot it came out at 25,900,
        // above the money, for a PUT. A reader comparing it against spot to
        // see how far from exercise they are gets a confident wrong answer.
        //
        // The auxiliary call exercises when its spot (this put's K) reaches X,
        // and X is proportional to its strike (this put's S): X = c*S. So
        // K >= c*S is the same condition as S <= K/c = K*S/X, which is the
        // put's own trigger in its own coordinates.
        //
        // `immediate_exercise` needs no transform: the auxiliary condition
        // K >= X already IS "this put should be exercised now".
        out.exercise_boundary = c.exercise_boundary > 0.0
            ? (S * K) / c.exercise_boundary : 0.0;
        out.immediate_exercise = c.immediate_exercise;
        if (out.price < out.european) { out.price = out.european; }
        out.early_exercise_premium = out.price - out.european;
    }

    if (!detail::finite(out.price) || !detail::finite(out.european)) {
        return std::unexpected(GreekError::NotFinite);
    }
    return out;
}

/// Is early exercise ever optimal for this contract?
///
/// A call with carry >= rate is never exercised early, exactly. Cheap enough
/// to check before pricing, and the answer is a fact about the contract rather
/// than about the numbers -- worth surfacing so a caller can skip the
/// approximation entirely and use the exact European price.
[[nodiscard]] constexpr bool
early_exercise_possible(OptionRight right, double rate, double carry) noexcept {
    if (right == OptionRight::Call) { return carry < rate; }
    // A put is the transformed call: its condition is -carry < rate - carry,
    // i.e. rate > 0. A put on a zero-rate asset is never exercised early
    // either, which is the same statement seen from the other side.
    return rate > 0.0;
}

} // namespace altair
