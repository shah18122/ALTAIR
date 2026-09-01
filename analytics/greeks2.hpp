// analytics/greeks2.hpp -- second- and third-order Black-76 greeks.
//
// P3-02. Vanna, volga, charm, veta, speed, zomma, colour, ultima. Separate
// header from greeks.hpp because that one is already at the size limit, not
// because these are a different model -- they are the same Black-76, further
// differentiated.
//
// WHY THESE EXIST, in one line each:
//
//   vanna   how the DELTA HEDGE breaks when vol moves. The cross term. A book
//           hedged delta-neutral at 15 vol is not delta-neutral at 18.
//   volga   the convexity of a vol position. Vega is not linear in vol, so a
//           vega-neutral book is not vol-neutral across a move of any size.
//   charm   delta decay. The overnight re-hedge nobody budgets for: a
//           position that is flat at the close is not flat at the open, with
//           nothing having happened.
//   veta    vega decay. Why a long-vol position bleeds even when vol holds.
//   speed   how gamma changes with the forward -- gamma is not constant, so a
//           gamma limit set at the money is not the limit that binds.
//   zomma   how gamma changes with vol.
//   colour  how gamma changes with time.
//   ultima  third-order vol convexity; matters only on a large vol book.
//
// TWO CONVENTIONS FIXED HERE, both easy to get wrong twice:
//
//   * charm, veta and colour are derivatives with respect to CALENDAR TIME t,
//     not to time-to-expiry T. They are therefore the NEGATIVE of the dT
//     derivatives, and they are per YEAR, like theta in P3-01. Convert at the
//     point of display with `per_day`.
//   * everything is a RAW derivative. Vanna per unit vol, not per vol point.
//
// ONE ASYMMETRY WORTH KNOWING. Vanna, volga, speed, zomma, colour and ultima
// are RIGHT-INDEPENDENT: a call and a put on one strike differ by a forward,
// which has no convexity and no vol sensitivity. CHARM IS NOT. Delta differs
// between the two by the discount factor, and the discount factor moves with
// time, so charm_call - charm_put = r*df exactly. That difference is asserted
// in the tests, because a "charm is right-independent" simplification is
// exactly the kind that looks harmless and silently breaks a put book.

#pragma once

#include <analytics/greeks.hpp>

#include <cmath>
#include <cstdint>
#include <expected>

namespace altair {

/// Second- and third-order greeks, in raw mathematical units.
struct HigherGreeks {
    /// d2V/dF dsigma. Equivalently d(vega)/dF or d(delta)/dsigma -- the same
    /// number, and the test computes it both ways. UNIT: paise per 1.0 vol,
    /// per paise of forward.
    double vanna = 0.0;
    /// d2V/dsigma2, also called vomma. UNIT: paise per 1.0 vol squared.
    double volga = 0.0;
    /// d(delta)/dt, per YEAR. Positive means delta rises as time passes.
    /// UNIT: dimensionless per year.
    double charm = 0.0;
    /// d(vega)/dt, per YEAR. UNIT: paise per 1.0 vol per year.
    double veta = 0.0;
    /// d3V/dF3, also d(gamma)/dF. UNIT: 1/paise^2.
    double speed = 0.0;
    /// d(gamma)/dsigma. UNIT: 1/paise per 1.0 vol.
    double zomma = 0.0;
    /// d(gamma)/dt, per YEAR. UNIT: 1/paise per year.
    double colour = 0.0;
    /// d3V/dsigma3. UNIT: paise per 1.0 vol cubed.
    double ultima = 0.0;
    /// Which variable the F-derivatives differentiate against. Forward for
    /// `black76_higher`; ordinal 0 stays Unspecified so a zeroed struct
    /// cannot pass for a real one.
    GreekBasis basis = GreekBasis::Unspecified;
};

/// Per-day scaling for the time-derivative greeks. Calendar days: a weekend
/// decays an option, and it decays its delta too.
///
/// Named the same way as `theta_per_day` so the two cannot be applied by
/// different conventions on the same screen.
[[nodiscard]] constexpr double per_day(double per_year) noexcept {
    return per_year / 365.0;
}

namespace detail {

/// Higher greeks with validation done and everything in double.
///
/// Extracted for the same two reasons as `black76_unchecked`: there is exactly
/// ONE copy of each formula, and a caller working on a surface can evaluate at
/// a forward it computed rather than one quantised to whole paise. That
/// quantisation is not always negligible -- at the strike where d2 = 0, volga
/// is -6.9e-11 on the exact forward and -0.155 on the same forward rounded to
/// paise, because a fifth of a paise on 2.4 million moves d2 off zero by
/// 2e-6 and volga is proportional to it.
[[nodiscard]] inline HigherGreeks
black76_higher_unchecked(OptionRight right, double F, double K, double T,
                         double s, double rate) noexcept {
    const double sqrtT = std::sqrt(T);
    const double v = s * sqrtT;                 // total vol, sigma*sqrt(T)
    const double logFK = std::log(F / K);
    const double d1 = (logFK + 0.5 * s * s * T) / v;
    const double d2 = d1 - v;
    const double df = std::exp(-rate * T);
    const double pdf1 = norm_pdf(d1);

    // The first-order quantities these are built on, computed here rather
    // than re-derived: gamma and vega are right-independent, so one copy.
    const double vega = df * F * pdf1 * sqrtT;
    const double gamma = df * pdf1 / (F * v);

    HigherGreeks g{};
    g.basis = GreekBasis::Forward;

    g.vanna = -df * pdf1 * d2 / s;
    g.volga = vega * d1 * d2 / s;
    g.speed = -gamma / F * (1.0 + d1 / v);
    g.zomma = gamma * (d1 * d2 - 1.0) / s;
    g.ultima = -vega / (s * s)
             * (d1 * d2 * (1.0 - d1 * d2) + d1 * d1 + d2 * d2);

    // dd1/dT, from d1 = ln(F/K)/(sigma*sqrt(T)) + sigma*sqrt(T)/2.
    const double dd1_dT = -logFK / (2.0 * s * T * sqrtT) + s / (4.0 * sqrtT);

    // Time derivatives. Each is computed as d/dT and then NEGATED, because
    // the quoted convention is per calendar time. Doing the negation once,
    // here, is why `charm` can be read directly as "delta per year".
    //
    // vega = df*F*phi(d1)*sqrt(T), and phi'(x) = -x*phi(x).
    const double dvega_dT = df * F * pdf1
        * (-rate * sqrtT - d1 * dd1_dT * sqrtT + 1.0 / (2.0 * sqrtT));
    g.veta = -dvega_dT;

    // gamma = df*phi(d1)/(F*sigma*sqrt(T)).
    const double dgamma_dT = df / (F * s)
        * (-rate * pdf1 / sqrtT - d1 * dd1_dT * pdf1 / sqrtT
           - 0.5 * pdf1 / (T * sqrtT));
    g.colour = -dgamma_dT;

    // CHARM IS THE ONE THAT DEPENDS ON THE RIGHT. Call delta is df*N(d1) and
    // put delta is df*(N(d1) - 1); the two differ by df, which itself moves
    // with time. Hence charm_call - charm_put = r*df, not zero.
    const double nd1 = norm_cdf(d1);
    const double delta_cdf = (right == OptionRight::Call) ? nd1 : (nd1 - 1.0);
    const double ddelta_dT = -rate * df * delta_cdf + df * pdf1 * dd1_dT;
    g.charm = -ddelta_dT;

    return g;
}

} // namespace detail

/// Second- and third-order Black-76 greeks on a forward.
///
/// Same arguments, same units and the same refusals as `black76`: T <= 0 and
/// sigma <= 0 return an error rather than a limit value, because the
/// higher-order greeks diverge there even faster than gamma does.
[[nodiscard]] ALTAIR_HOT inline std::expected<HigherGreeks, GreekError>
black76_higher(OptionRight right, Price forward, Price strike, Years t,
               Vol vol, double rate) noexcept {
    const double F = static_cast<double>(forward.raw());
    const double K = static_cast<double>(strike.raw());
    const double T = t.raw();
    const double s = vol.raw();

    if (F <= 0.0 || K <= 0.0) {
        return std::unexpected(GreekError::NonPositivePrice);
    }
    if (!(T > 0.0)) { return std::unexpected(GreekError::Expired); }
    if (!(s > 0.0)) { return std::unexpected(GreekError::NonPositiveVol); }
    if (!detail::finite(F) || !detail::finite(K) || !detail::finite(T)
        || !detail::finite(s) || !detail::finite(rate)) {
        return std::unexpected(GreekError::NotFinite);
    }

    const HigherGreeks g =
        detail::black76_higher_unchecked(right, F, K, T, s, rate);

    if (!detail::finite(g.vanna) || !detail::finite(g.volga)
        || !detail::finite(g.charm) || !detail::finite(g.veta)
        || !detail::finite(g.speed) || !detail::finite(g.zomma)
        || !detail::finite(g.colour) || !detail::finite(g.ultima)) {
        return std::unexpected(GreekError::NotFinite);
    }
    return g;
}

/// Vanna-volga correction to a delta hedge for a vol move of `dvol`.
///
/// delta(sigma + dvol) ~ delta(sigma) + vanna*dvol. Exposed as a named
/// function because the correction is routinely omitted -- a book re-hedged
/// on price alone drifts every time the surface moves, and the drift is
/// systematic rather than noise.
[[nodiscard]] constexpr double
delta_after_vol_move(double delta, double vanna, double dvol) noexcept {
    return delta + vanna * dvol;
}

/// Vega after a vol move, to second order: vega + volga*dvol.
///
/// The reason a vega-neutral book is not vol-neutral. Over a 5-vol move the
/// linear term is simply wrong, and volga says by how much.
[[nodiscard]] constexpr double
vega_after_vol_move(double vega, double volga, double dvol) noexcept {
    return vega + volga * dvol;
}

} // namespace altair
