// analytics/vanna_volga.hpp -- the vanna-volga smile: a fair implied vol for
// every strike from three quoted ones.
//
// THE IDEA (Castagna & Mercurio, "The vanna-volga method for implied
// volatilities", Risk, January 2007). Black-76 at one flat vol (the ATM one)
// misprices the wings because it ignores what a vol move does to the option:
// vega (first order), vanna (the delta hedge moving with vol) and volga (the
// vega moving with vol). Three liquid options -- the 25-delta put, the ATM
// and the 25-delta call -- carry the market's price of those risks. A
// portfolio of the three with the same vega, vanna and volga as the target
// hedges them; the target's fair price is its flat-vol price plus what that
// hedge costs over its own flat-vol value:
//
//     C_VV(K) = C_BS(K, s2) + sum_i x_i(K) * (C_mkt(K_i) - C_BS(K_i, s2))
//
// with weights in closed form (log-strike Lagrange weights times vega ratios):
//
//     x_1 = V(K)/V(K1) * ln(K2/K) ln(K3/K) / (ln(K2/K1) ln(K3/K1))
//     x_2 = V(K)/V(K2) * ln(K/K1) ln(K3/K) / (ln(K2/K1) ln(K3/K2))
//     x_3 = V(K)/V(K3) * ln(K/K1) ln(K/K2) / (ln(K3/K1) ln(K3/K2))
//
// every vega at s2. The implied vol of C_VV has a second-order closed form
// (their eq. 16), `vv_vol`; `vv_vol_first` is the first-order one, the
// log-strike interpolation of the three vols. Both reproduce the pillars
// exactly, and a flat smile stays flat.
//
// WHAT IT IS FOR HERE. Rich/cheap: an option whose market IV sits above the
// vanna-volga IV of its own chain is dear relative to the three most liquid
// options of that expiry, and below it, cheap. It is a consistency check
// across one expiry's smile, not a forecast: it says nothing about whether
// the whole smile is too high (that is the vol risk premium's question).
//
// UNITS. Forwards and strikes in any one unit (rupees or paise, the same for
// both); vols as fractions; T in years. Prices come back in the forward's unit.

#pragma once

#include <analytics/greeks.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace altair::vanna_volga {

/// The three quoted options: K[0] < K[1] < K[2] (25-delta put, ATM, 25-delta
/// call) and their market vols.
struct Pillars {
    std::array<double, 3> k{};
    std::array<double, 3> vol{};
};

[[nodiscard]] inline bool valid(const Pillars& p) noexcept {
    return p.k[0] > 0 && p.k[0] < p.k[1] && p.k[1] < p.k[2] && p.vol[0] > 0 && p.vol[1] > 0 && p.vol[2] > 0;
}

namespace detail {
inline double d1(double F, double K, double T, double s) { return (std::log(F / K) + 0.5 * s * s * T) / (s * std::sqrt(T)); }
inline double npdf(double x) { return std::exp(-0.5 * x * x) / std::sqrt(2.0 * 3.14159265358979323846); }
/// Undiscounted Black-76 vega, per unit vol.
inline double vega(double F, double K, double T, double s) { return F * std::sqrt(T) * npdf(d1(F, K, T, s)); }
inline double price(bool call, double F, double K, double T, double s, double r) {
    return altair::detail::black76_unchecked(call ? OptionRight::Call : OptionRight::Put, F, K, T, s, r).price;
}
/// The three log-strike Lagrange weights at K.
inline std::array<double, 3> lagrange(double K, const Pillars& p) {
    const double l1 = std::log(p.k[0]), l2 = std::log(p.k[1]), l3 = std::log(p.k[2]), l = std::log(K);
    return {(l2 - l) * (l3 - l) / ((l2 - l1) * (l3 - l1)), (l - l1) * (l3 - l) / ((l2 - l1) * (l3 - l2)),
            (l - l1) * (l - l2) / ((l3 - l1) * (l3 - l2))};
}
} // namespace detail

/// The weights x_1..x_3 of the hedge (see the header).
[[nodiscard]] inline std::array<double, 3> weights(double F, double K, double T, const Pillars& p) {
    const double s = p.vol[1];
    const auto w = detail::lagrange(K, p);
    const double v = detail::vega(F, K, T, s);
    std::array<double, 3> x{};
    for (std::size_t i = 0; i < 3; ++i) x[i] = v / detail::vega(F, p.k[i], T, s) * w[i];
    return x;
}

/// The vanna-volga price of a call or put at K (discounted at r, like black76).
[[nodiscard]] inline double price(bool call, double F, double K, double T, double r, const Pillars& p) {
    const double s = p.vol[1];
    const auto x = weights(F, K, T, p);
    double c = detail::price(call, F, K, T, s, r);
    for (std::size_t i = 0; i < 3; ++i)
        c += x[i] * (detail::price(call, F, p.k[i], T, p.vol[i], r) - detail::price(call, F, p.k[i], T, s, r));
    return c;
}

/// First-order vanna-volga vol: the log-strike interpolation of the pillars.
[[nodiscard]] inline double vol_first(double K, const Pillars& p) {
    const auto w = detail::lagrange(K, p);
    return w[0] * p.vol[0] + w[1] * p.vol[1] + w[2] * p.vol[2];
}

/// Second-order vanna-volga vol (Castagna-Mercurio eq. 16). Falls back to the
/// first-order one where the square root would be of a negative number (far
/// wings, short expiries).
[[nodiscard]] inline double vol(double F, double K, double T, const Pillars& p) {
    const double s2 = p.vol[1];
    const auto w = detail::lagrange(K, p);
    const auto dd = [&](double x) {
        const double a = detail::d1(F, x, T, s2);
        return a * (a - s2 * std::sqrt(T));
    };
    const double D1 = vol_first(K, p) - s2;
    const double D2 = w[0] * dd(p.k[0]) * (p.vol[0] - s2) * (p.vol[0] - s2) + w[2] * dd(p.k[2]) * (p.vol[2] - s2) * (p.vol[2] - s2);
    const double d = dd(K);
    if (std::fabs(d) < 1e-12) return s2 + D1;   // the limit as d1 d2 -> 0
    const double disc = s2 * s2 + d * (2.0 * s2 * D1 + D2);
    if (disc < 0) return s2 + D1;
    return s2 + (-s2 + std::sqrt(disc)) / d;
}

/// One quoted strike of a smile: strike and market vol (OTM side preferred).
struct SmilePoint {
    double k = 0.0, vol = 0.0;
};

/// The pillars of a quoted smile: the strike nearest the forward, and the
/// strikes whose Black-76 delta at their own vol is nearest -0.25 (put, below
/// the forward) and +0.25 (call, above). Nullopt with fewer than three usable
/// points, or no strike on each side.
[[nodiscard]] inline std::optional<Pillars> pillars(double F, double T, const std::vector<SmilePoint>& smile, double target = 0.25) {
    if (!(F > 0) || !(T > 0)) return std::nullopt;
    const SmilePoint* atm = nullptr;
    const SmilePoint* put = nullptr;
    const SmilePoint* call = nullptr;
    double best_atm = 0, best_put = 0, best_call = 0;
    for (const auto& s : smile) {
        if (!(s.k > 0) || !(s.vol > 0) || !std::isfinite(s.vol)) continue;
        const double gap = std::fabs(std::log(s.k / F));
        if (atm == nullptr || gap < best_atm) { atm = &s; best_atm = gap; }
    }
    if (atm == nullptr) return std::nullopt;
    for (const auto& s : smile) {
        if (!(s.k > 0) || !(s.vol > 0) || !std::isfinite(s.vol) || &s == atm) continue;
        const double nd = 0.5 * std::erfc(-detail::d1(F, s.k, T, s.vol) / std::sqrt(2.0));   // call delta, undiscounted
        if (s.k < atm->k) {
            const double e = std::fabs((nd - 1.0) + target);
            if (put == nullptr || e < best_put) { put = &s; best_put = e; }
        } else if (s.k > atm->k) {
            const double e = std::fabs(nd - target);
            if (call == nullptr || e < best_call) { call = &s; best_call = e; }
        }
    }
    if (put == nullptr || call == nullptr) return std::nullopt;
    Pillars p;
    p.k = {put->k, atm->k, call->k};
    p.vol = {put->vol, atm->vol, call->vol};
    return valid(p) ? std::optional<Pillars>(p) : std::nullopt;
}

} // namespace altair::vanna_volga
