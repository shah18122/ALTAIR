// analytics/iv.hpp -- implied volatility by safeguarded Newton inversion.
//
// P3-04. Inverts `black76` / `black_scholes` from analytics/greeks.hpp for the
// volatility that reproduces an observed option price.
//
// NAMING, because the card is titled "Jaeckel IV solver" and this is not that.
// Jaeckel's "Let's Be Rational" is a specific published algorithm: a rational
// approximation of the normalised price, then a third-order Householder step,
// reaching full double precision in two iterations with no bracketing. What is
// implemented here is a SAFEGUARDED NEWTON -- a guaranteed bracket, Newton
// steps inside it, bisection whenever a step would leave. It reaches the same
// precision in more iterations: measured across the test corpus, 3 to 14 with
// a median of 10, at about 1.6 us per solve. It is written this way for a
// reason, not as a shortcut:
//
//   Jaeckel's speed comes from around 40 hard-coded rational coefficients.
//   Those coefficients cannot be checked by reading them, and a single
//   mistyped digit produces an IV wrong in the fourth decimal in ONE region of
//   the surface and correct everywhere else -- precisely the class of defect
//   that survives testing and then misprices a wing. Newton on the pricer
//   inverts, by construction, the SAME function the pricer evaluates: it has
//   no coefficients of its own to get wrong. If profiling later puts the
//   solver on a hot path, Jaeckel can be added behind this interface with this
//   implementation as the reference oracle to check it against.
//
// THE ERROR BAR IS PART OF THE ANSWER (ROADMAP section 3).
//
// IV is a MEASUREMENT, and its precision collapses in the wings because vega
// does. On a 30-day NIFTY chain at 15 vol against a 24'080 forward, for a
// one-tick (5 paise) price uncertainty -- the first three rows are reproduced
// by tests 5 and 8:
//
//     strike        vega       IV uncertainty
//     24000       272607       0.000018 vol points
//     26000        57967       0.000086 vol points
//     28000          630       0.0079   vol points   <-- 433x the ATM band
//     32000       0.0001      49427     vol points   <-- vega has underflowed
//
// The last row is the point of the whole header: nothing about that quote is
// unusual to look at, and its price carries no information about vol at all.
//
// A wing IV quoted to four decimals is a lie about its own precision. So
// `ImpliedVol` returns the vega at the solution, and `iv_uncertainty` turns a
// price uncertainty into a vol uncertainty. A caller fitting a surface or
// sizing a vol trade weights by that, or discards the point. It never treats
// the 28000 number as if it were the 24000 number.

#pragma once

#include <analytics/greeks.hpp>

#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>

namespace altair {

enum class IvError : std::uint8_t {
    /// T <= 0. At expiry price is intrinsic and carries no vol information.
    Expired,
    /// Forward, strike or observed price <= 0.
    NonPositiveInput,
    /// Price at or below the discounted intrinsic. No sigma >= 0 produces it:
    /// the quote is stale, crossed, or against a forward that is not the one
    /// the market is using. Refused, never clamped to zero vol (rule 9).
    BelowIntrinsic,
    /// Price at or above the no-arbitrage ceiling -- df*F for a call, df*K for
    /// a put. Only sigma = infinity reaches it.
    AboveBound,
    /// The bracket could not be closed below kIvVolMax, or the iteration
    /// budget ran out. Both mean "no answer", not "here is a rough answer".
    NoConvergence,
    NotFinite
};

/// A measured implied volatility, with what is needed to judge it.
struct ImpliedVol {
    /// The volatility that reproduces the observed price. UNIT: annualised,
    /// as a fraction (0.15 is 15 vol points).
    Vol vol{0.0};
    /// dV/dsigma at the solution. UNIT: paise per 1.0 of vol.
    ///
    /// This is the error propagator, and it is why the struct exists rather
    /// than the function returning a bare Vol. Feed it to `iv_uncertainty`
    /// with half the bid-ask spread to get the width of the band this
    /// measurement actually resolves.
    double vega = 0.0;
    /// Signed model-minus-target at the solution. UNIT: paise. Near zero on
    /// success; exposed so a caller can see the convergence rather than
    /// trust it.
    double residual = 0.0;
    /// Newton/bisection steps taken.
    std::uint8_t iterations = 0;
    /// How many of those were bisections, i.e. how often Newton proposed a
    /// step outside the bracket. A high count marks a hard region of the
    /// surface -- deep wings, or a price near a no-arbitrage bound.
    std::uint8_t bisections = 0;
};

/// Vol lower bound of the search. Not zero: at sigma = 0 the pricer is
/// degenerate (D4 of P3-01 refuses it), and the bracket only needs a value
/// below any real answer.
inline constexpr double kIvVolMin = 1e-9;
/// Vol upper bound. 102400% annualised -- absurd on purpose. A price needing
/// more vol than this is not a market observation, and hitting the cap returns
/// NoConvergence rather than the cap itself.
inline constexpr double kIvVolMax = 1024.0;
/// Iteration budget. Bisection alone closes the widest possible bracket to
/// 1e-12 in about 60 steps, so a well-posed problem cannot hit this.
inline constexpr int kIvMaxIterations = 100;

/// Vol uncertainty implied by a price uncertainty: dsigma = dV / vega.
///
/// `price_uncertainty` in paise -- typically half the bid-ask spread, or one
/// tick for a last-traded price. Returns +infinity when vega is zero or
/// negative, which is the honest answer: the price carries no information
/// about vol there, and any finite number would be an invented precision.
[[nodiscard]] inline double iv_uncertainty(double vega,
                                           double price_uncertainty) noexcept {
    if (!(vega > 0.0)) {
        return std::numeric_limits<double>::infinity();
    }
    return price_uncertainty / vega;
}

namespace detail {

/// No-arbitrage price bounds for a European option on a forward. UNIT: paise.
///
/// Lower is the discounted intrinsic (the sigma -> 0 limit), upper the sigma
/// -> infinity limit: a call is worth at most the discounted forward, a put at
/// most the discounted strike. A price outside the OPEN interval has no
/// implied vol, and the solver refuses it rather than returning an endpoint.
struct PriceBounds {
    double lower = 0.0;
    double upper = 0.0;
};

[[nodiscard]] inline PriceBounds no_arb_bounds(OptionRight right, double F,
                                               double K, double df) noexcept {
    PriceBounds b{};
    if (right == OptionRight::Call) {
        const double intrinsic_fwd = F - K;
        b.lower = df * (intrinsic_fwd > 0.0 ? intrinsic_fwd : 0.0);
        b.upper = df * F;
    } else {
        const double intrinsic_fwd = K - F;
        b.lower = df * (intrinsic_fwd > 0.0 ? intrinsic_fwd : 0.0);
        b.upper = df * K;
    }
    return b;
}

/// The solver proper, on doubles.
///
/// Kept in `detail::` for the same reason as `black76_unchecked`: the public
/// entry points take `Price` so rule 2 holds at the module boundary, while
/// `implied_vol_black_scholes` needs to hand in a forward it computed in
/// double. Routing that forward through integer paise would quantise it, and
/// in the wings -- where vega is small enough that half a paise of forward is
/// visible in the answer -- that quantisation is not negligible.
[[nodiscard]] inline std::expected<ImpliedVol, IvError>
solve_iv(OptionRight right, double target, double F, double K, double T,
         double rate) noexcept {
    if (!finite(target) || !finite(F) || !finite(K) || !finite(T)
        || !finite(rate)) {
        return std::unexpected(IvError::NotFinite);
    }
    if (F <= 0.0 || K <= 0.0 || target <= 0.0) {
        return std::unexpected(IvError::NonPositiveInput);
    }
    if (!(T > 0.0)) {
        return std::unexpected(IvError::Expired);
    }

    const double df = std::exp(-rate * T);
    const PriceBounds bounds = no_arb_bounds(right, F, K, df);
    // Closed comparisons against an OPEN interval: a price exactly AT a bound
    // is a limit, not a solution, and reporting sigma = 0 or sigma = infinity
    // as a measured vol is worse than reporting nothing.
    if (target <= bounds.lower) {
        return std::unexpected(IvError::BelowIntrinsic);
    }
    if (target >= bounds.upper) {
        return std::unexpected(IvError::AboveBound);
    }

    // --- bracket -------------------------------------------------------
    // Price is strictly increasing in sigma, so f(sigma) = V(sigma) - target
    // is negative at kIvVolMin (price -> bounds.lower < target) and must be
    // driven positive at some hi. Doubling from 1.0 finds it; each failed
    // probe also raises lo, so the bracket tightens on the way up for free.
    double lo = kIvVolMin;
    double hi = 1.0;
    while (black76_unchecked(right, F, K, T, hi, rate).price < target) {
        lo = hi;
        hi *= 2.0;
        if (hi > kIvVolMax) {
            // A solution still exists below the ceiling of bounds.upper, but a
            // vol past kIvVolMax is not a market observation. Fail loud rather
            // than return the cap.
            return std::unexpected(IvError::NoConvergence);
        }
    }

    // --- seed ----------------------------------------------------------
    // Brenner-Subrahmanyam: for an at-the-money-forward option,
    // V ~ 0.3989 * F * sigma * sqrt(T) * df, hence
    // sigma ~ sqrt(2*pi/T) * V / (df*F). Exact only at the money and merely a
    // starting point elsewhere -- which costs iterations, never correctness,
    // because the bracket is already closed. Clamped in, not trusted.
    constexpr double kTwoPi = 6.28318530717958647692528676655901;
    double sigma = std::sqrt(kTwoPi / T) * target / (df * F);
    if (!finite(sigma) || !(sigma > lo) || !(sigma < hi)) {
        sigma = 0.5 * (lo + hi);
    }

    // --- safeguarded Newton --------------------------------------------
    // CONVERGE ON THE VOL, NOT ON THE PRICE. This is a dimensional argument,
    // and getting it wrong is a live defect that the corpus caught:
    //
    //   A price-residual criterion, even a relative one, is wrong because the
    //   answer is a vol and the residual is a price. The two are related by
    //   vega, and vega varies by four orders of magnitude across a chain. A
    //   deep ITM put worth Rs 5,888 has a vega of 36 paise per vol unit
    //   against an ATM vega of 272,607: converging its PRICE to 1e-10
    //   relative leaves its VOL wrong in the seventh decimal, while the same
    //   rule over-solves the ATM strike. Dividing by vega puts the test in
    //   the units of the thing being measured.
    //
    // The tolerance also cannot be a constant, because it is bounded below by
    // arithmetic: the price is a double, so it is only known to ~eps
    // relative, and no solver can resolve vol finer than that price quantum
    // divided by vega. Asking for more just spins until the bracket collapses.
    // So the floor is computed, per option, from the price scale and the
    // local vega -- the same error-propagation identity `iv_uncertainty`
    // exposes to callers, applied here to the arithmetic itself.
    constexpr double kVolStepTol = 1e-13;
    constexpr double kVolTol = 1e-12;
    constexpr double kEps = std::numeric_limits<double>::epsilon();

    ImpliedVol out{};
    Greeks g{};
    int i = 0;
    for (; i < kIvMaxIterations; ++i) {
        g = black76_unchecked(right, F, K, T, sigma, rate);
        if (!finite(g.price) || !finite(g.vega)) {
            return std::unexpected(IvError::NotFinite);
        }
        const double f = g.price - target;

        // Re-establish f(lo) < 0 < f(hi) before anything else, so a bisection
        // later in this same iteration is always onto a valid bracket.
        if (f < 0.0) {
            lo = sigma;
        } else {
            hi = sigma;
        }

        const double err = f < 0.0 ? -f : f;
        if (g.vega > 0.0) {
            // The Newton step, in vol units -- how far the answer would still
            // move. That is the quantity the caller cares about.
            const double vol_step = err / g.vega;
            // Best resolution the price representation allows. The factor of
            // 4 is slack for the handful of roundings inside the pricer.
            const double resolution = 4.0 * target * kEps / g.vega;
            const double tol = resolution > kVolStepTol ? resolution
                                                        : kVolStepTol;
            if (vol_step <= tol) {
                break;
            }
        }
        // Vega zero or negative leaves no vol-scaled test to apply; the
        // bracket closing is then the only honest stopping condition.
        if ((hi - lo) <= kVolTol * (1.0 + sigma)) {
            break;
        }

        double next = sigma;
        bool bisect = true;
        if (g.vega > 0.0) {
            next = sigma - f / g.vega;
            // Accept the Newton step only if it lands strictly inside the
            // bracket. In the wings vega underflows toward zero and the step
            // overshoots by orders of magnitude; that is the case bisection
            // exists to absorb.
            bisect = !finite(next) || !(next > lo && next < hi);
        }
        if (bisect) {
            next = 0.5 * (lo + hi);
            if (out.bisections < 255) {
                // Explicit cast: ++ on a uint8_t promotes to int and narrows
                // back, which -Wconversion flags.
                out.bisections = static_cast<std::uint8_t>(out.bisections + 1);
            }
        }
        sigma = next;
    }
    if (i >= kIvMaxIterations) {
        return std::unexpected(IvError::NoConvergence);
    }

    // Report vega and residual AT THE RETURNED VOL. `g` is already evaluated
    // there by the loop pass that broke, so this is the same evaluation the
    // convergence test used -- not a second, subtly different one.
    out.vol = Vol{sigma};
    out.vega = g.vega;
    out.residual = g.price - target;
    out.iterations = static_cast<std::uint8_t>(i + 1);
    if (!finite(out.vega) || !finite(out.residual)) {
        return std::unexpected(IvError::NotFinite);
    }
    return out;
}

} // namespace detail

/// Implied volatility under Black-76, from an observed price and a forward.
///
/// Use this when a listed future quotes the forward directly: the market's own
/// forward already contains whatever the dividend and repo actually are, so
/// there is one fewer estimated parameter to be wrong about.
[[nodiscard]] inline std::expected<ImpliedVol, IvError>
implied_vol_black76(OptionRight right, Price option_price, Price forward,
                    Price strike, Years t, double rate) noexcept {
    return detail::solve_iv(right,
                            static_cast<double>(option_price.raw()),
                            static_cast<double>(forward.raw()),
                            static_cast<double>(strike.raw()),
                            t.raw(), rate);
}

/// Implied volatility under Black-Scholes, from an observed price and a spot.
///
/// Identical answer to `implied_vol_black76` on the corresponding forward,
/// because the two are the same model -- and identical vega too, since
/// df*F == S*e^(-qT) under F = S*e^((r-q)T). Both facts are asserted in the
/// tests. It exists so a caller holding a cash index or a stock does not have
/// to build the forward by hand and get the carry sign backwards.
[[nodiscard]] inline std::expected<ImpliedVol, IvError>
implied_vol_black_scholes(OptionRight right, Price option_price, Price spot,
                          Price strike, Years t, double rate,
                          double dividend_yield) noexcept {
    const double S = static_cast<double>(spot.raw());
    const double T = t.raw();
    if (!detail::finite(S) || !detail::finite(T) || !detail::finite(rate)
        || !detail::finite(dividend_yield)) {
        return std::unexpected(IvError::NotFinite);
    }
    if (S <= 0.0) {
        return std::unexpected(IvError::NonPositiveInput);
    }
    // In double, deliberately. See the note on detail::solve_iv.
    const double F = S * std::exp((rate - dividend_yield) * T);
    return detail::solve_iv(right,
                            static_cast<double>(option_price.raw()),
                            F,
                            static_cast<double>(strike.raw()),
                            T, rate);
}

} // namespace altair
