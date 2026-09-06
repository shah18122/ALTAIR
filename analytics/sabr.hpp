// analytics/sabr.hpp -- SABR implied vol, and Dupire local vol.
//
// P17-02 / P17-03.
//
// SABR IS AN APPROXIMATION, AND IT IS WRONG IN A KNOWN PLACE.
//
// Hagan's formula is an ASYMPTOTIC EXPANSION in small time-to-expiry and small
// vol-of-vol. It is the market standard because it is fast, has four
// interpretable parameters, and fits a smile well in the region anyone trades.
// It is also known to admit ARBITRAGE at low strikes and long expiries: the
// implied density it induces goes NEGATIVE, which is not a rounding artefact
// but the expansion being used outside its domain.
//
// So `sabr_vol` computes the standard formula and `sabr_density_positive`
// checks the butterfly condition on the resulting smile. A surface that fails
// that check is not a surface to price from, and the check is a function
// rather than a footnote because the failure is silent: every individual
// implied vol looks perfectly reasonable.
//
//   alpha  overall level of vol
//   beta   the backbone: 1 is lognormal, 0 is normal, 0.5 is CIR-like.
//          FIXED BY CONVENTION, not fitted -- alpha and beta are nearly
//          unidentifiable together, and a fitter allowed both will trade one
//          against the other and report a confident nonsense.
//   rho    correlation between spot and vol; this is the SKEW
//   nu     vol of vol; this is the SMILE CURVATURE
//
// DUPIRE IS EXACT, AND THAT IS THE PROBLEM.
//
// Local volatility is the unique deterministic sigma(K, T) reproducing every
// observed European price. It fits perfectly by construction -- which means it
// tells you nothing about whether the model is right, and its DYNAMICS are
// famously wrong: it predicts the smile flattens as spot moves, and the market
// does the opposite. It is the correct tool for pricing an exotic consistently
// with today's vanillas, and the wrong tool for anything about tomorrow.
//
// The formula needs second derivatives of price in strike, and those are
// numerically brutal: a central difference on a noisy surface amplifies the
// noise by 1/h^2. That is why this takes a SMOOTH parametric surface (SVI,
// already in analytics/svi.hpp) rather than raw quotes, and why the
// denominator is guarded rather than trusted.

#pragma once

#include <analytics/greeks.hpp>

#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>

namespace altair {

enum class SabrError : std::uint8_t {
    /// Forward or strike non-positive, or expiry non-positive.
    BadParameter,
    /// alpha or nu negative, or |rho| >= 1.
    BadCalibration,
    /// The local-vol denominator vanished: the surface is not arbitrage-free
    /// at this point and dividing would produce a confident wrong number.
    NoLocalVol
};

struct SabrParams {
    double alpha = 0.0;    ///< vol level
    double beta = 0.5;     ///< backbone. FIXED, not fitted -- see the header.
    double rho = 0.0;      ///< spot/vol correlation: the skew
    double nu = 0.0;       ///< vol of vol: the curvature

    [[nodiscard]] bool valid() const noexcept {
        return alpha > 0.0 && beta >= 0.0 && beta <= 1.0
            && rho > -1.0 && rho < 1.0 && nu >= 0.0;
    }
};

/// Hagan's implied Black vol.
///
/// The at-the-money branch is handled separately, not because the general
/// formula is slow there but because it is 0/0: z / x(z) has a removable
/// singularity at K = F and a naive evaluation returns NaN for the single most
/// traded strike on the board.
[[nodiscard]] inline std::expected<double, SabrError>
sabr_vol(double forward, double strike, double t, const SabrParams& p) noexcept {
    if (!(forward > 0.0) || !(strike > 0.0) || !(t > 0.0)) {
        return std::unexpected(SabrError::BadParameter);
    }
    if (!p.valid()) { return std::unexpected(SabrError::BadCalibration); }

    const double F = forward, K = strike;
    const double one_b = 1.0 - p.beta;
    const double fk = std::pow(F * K, 0.5 * one_b);
    const double log_fk = std::log(F / K);

    // The three correction terms, common to both branches.
    const double t1 = one_b * one_b / 24.0 * p.alpha * p.alpha / (fk * fk);
    const double t2 = 0.25 * p.rho * p.beta * p.nu * p.alpha / fk;
    const double t3 = (2.0 - 3.0 * p.rho * p.rho) / 24.0 * p.nu * p.nu;
    const double corr = 1.0 + (t1 + t2 + t3) * t;

    if (std::fabs(log_fk) < 1e-9) {
        // ATM: the removable singularity. Returning the limit rather than
        // evaluating 0/0.
        return p.alpha / std::pow(F, one_b) * corr;
    }

    const double z = p.nu / p.alpha * fk * log_fk;
    const double sqrt_arg = 1.0 - 2.0 * p.rho * z + z * z;
    if (!(sqrt_arg > 0.0)) { return std::unexpected(SabrError::BadCalibration); }
    const double xz = std::log((std::sqrt(sqrt_arg) + z - p.rho)
                               / (1.0 - p.rho));
    if (std::fabs(xz) < 1e-300) {
        return std::unexpected(SabrError::BadCalibration);
    }

    const double denom = fk
        * (1.0 + one_b * one_b / 24.0 * log_fk * log_fk
             + std::pow(one_b, 4) / 1920.0 * std::pow(log_fk, 4));
    return p.alpha / denom * (z / xz) * corr;
}

/// BUTTERFLY ARBITRAGE CHECK on a SABR smile.
///
/// The risk-neutral density is proportional to the second derivative of call
/// price in strike. Negative density means a butterfly with a negative price:
/// free money, and a sign the expansion has been pushed outside its domain.
///
/// Checked by finite difference on PRICES, not on vols -- a convex vol smile
/// does not imply a convex price surface, and checking the wrong one passes
/// exactly the cases that matter.
///
/// Returns the MINIMUM second difference of price across the range, scaled by
/// h^2 so it is comparable to a density. Negative means arbitrage.
///
/// A value rather than a bool, because "is it arbitrage-free" is the wrong
/// granularity: a smile that clears zero by 1e-15 is one recalibration away
/// from not clearing it, and only a number shows that.
[[nodiscard]] inline double
sabr_min_density(double forward, double t, const SabrParams& p,
                 double k_lo, double k_hi, std::size_t n) noexcept {
    // NaN for 'could not compute', never a negative number. A sentinel of
    // -1.0 is indistinguishable from a genuine density of -1.0, and the whole
    // point of this function is to report negative densities -- a caller
    // reading -1.0 cannot tell 'the smile is arbitrageable' from 'sabr_vol
    // refused'. Check with std::isfinite.
    const double kNoAnswer = std::numeric_limits<double>::quiet_NaN();
    if (n < 3 || !(k_lo > 0.0) || !(k_hi > k_lo)) { return kNoAnswer; }
    const double h = (k_hi - k_lo) / static_cast<double>(n - 1);
    double prev = 0.0, cur = 0.0, worst = 1e300;
    for (std::size_t i = 0; i < n; ++i) {
        const double K = k_lo + h * static_cast<double>(i);
        const auto v = sabr_vol(forward, K, t, p);
        if (!v) { return kNoAnswer; }
        const auto g = black76(OptionRight::Call,
                               Price{static_cast<std::int64_t>(forward)},
                               Price{static_cast<std::int64_t>(K)},
                               Years{t}, Vol{*v}, 0.0);
        if (!g) { return kNoAnswer; }
        const double px = g->price;
        if (i >= 2) {
            worst = std::min(worst, (px - 2.0 * cur + prev) / (h * h));
        }
        prev = cur;
        cur = px;
    }
    return worst < 1e299 ? worst : kNoAnswer;
}

[[nodiscard]] inline bool
sabr_density_positive(double forward, double t, const SabrParams& p,
                      double k_lo, double k_hi, std::size_t n) noexcept {
    const double d = sabr_min_density(forward, t, p, k_lo, k_hi, n);
    // A surface that could not be evaluated is NOT arbitrage-free. Returning
    // true on NaN would let an uncomputable smile pass the safety check.
    return std::isfinite(d) && d >= 0.0;
}

// ---------------------------------------------------------------------------
// P17-03 — Dupire local volatility
// ---------------------------------------------------------------------------

/// Local vol from an implied-vol surface, by finite difference.
///
/// `iv(K, T)` must be a SMOOTH parametric surface. Handing this raw market
/// quotes produces noise amplified by 1/h^2 in the second derivative and a
/// local-vol surface that oscillates violently between strikes -- the classic
/// failure, and the reason SVI exists.
///
/// Dupire in implied-vol form, with w = sigma^2 * T the total variance and
/// y = log(K/F):
///
///     sigma_loc^2 = dw/dT / [1 - y/w dw/dy + 1/4(-1/4 - 1/w + y^2/w^2)(dw/dy)^2
///                            + 1/2 d2w/dy2]
///
/// The DENOMINATOR IS THE ARBITRAGE CHECK. It is positive exactly when the
/// surface is free of butterfly arbitrage, so a non-positive value is not a
/// numerical inconvenience to clamp -- it is the surface telling you it cannot
/// be priced from.
template <typename IvSurface>
[[nodiscard]] std::expected<double, SabrError>
dupire_local_vol(const IvSurface& iv, double forward, double strike, double t,
                 double dk_rel = 0.01, double dt = 1.0 / 365.0) {
    if (!(forward > 0.0) || !(strike > 0.0) || !(t > dt)) {
        return std::unexpected(SabrError::BadParameter);
    }
    auto w = [&](double K, double T) -> std::expected<double, SabrError> {
        const auto s = iv(K, T);
        if (!s || !(*s > 0.0)) {
            return std::unexpected(SabrError::BadParameter);
        }
        return *s * *s * T;
    };
    const double y = std::log(strike / forward);
    const double dy = dk_rel;
    const double k_up = forward * std::exp(y + dy);
    const double k_dn = forward * std::exp(y - dy);

    const auto w0 = w(strike, t);
    const auto wu = w(k_up, t);
    const auto wd = w(k_dn, t);
    const auto wt_up = w(strike, t + dt);
    const auto wt_dn = w(strike, t - dt);
    if (!w0 || !wu || !wd || !wt_up || !wt_dn) {
        return std::unexpected(SabrError::BadParameter);
    }

    const double dwdy = (*wu - *wd) / (2.0 * dy);
    const double d2wdy2 = (*wu - 2.0 * *w0 + *wd) / (dy * dy);
    const double dwdt = (*wt_up - *wt_dn) / (2.0 * dt);
    if (!(*w0 > 0.0)) { return std::unexpected(SabrError::NoLocalVol); }

    const double denom = 1.0 - y / *w0 * dwdy
        + 0.25 * (-0.25 - 1.0 / *w0 + y * y / (*w0 * *w0)) * dwdy * dwdy
        + 0.5 * d2wdy2;
    if (!(denom > 1e-8) || !(dwdt > 0.0)) {
        // dwdt <= 0 is CALENDAR arbitrage: total variance must increase with
        // maturity. denom <= 0 is BUTTERFLY arbitrage. Both are refused
        // rather than clamped, because a clamped local vol prices an exotic
        // with great confidence off a surface that admits free money.
        return std::unexpected(SabrError::NoLocalVol);
    }
    return std::sqrt(dwdt / denom);
}

} // namespace altair
