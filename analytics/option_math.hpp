// analytics/option_math.hpp -- the "phenomena" of Nations' Options Math for Traders.
//
// Five numbers a seller checks before and after selling, each one line of
// arithmetic in the book:
//
// VOLATILITY SLOPE. Index IV moves against spot, and further on falls than on
// rises (Nations Table 9.1: S&P -1..-2% days lift VIX 7%, +1..+2% days cut it
// 5%). `vol_slope` measures that from daily pairs as two through-origin
// betas, vol points per 1% fall and per 1% rise, so a stress test can move IV
// with spot on the measured, asymmetric slope instead of one guessed number.
//
// PROBABILITY OF LOSS AT EXPIRY. A short straddle loses at expiry only outside
// strike +/- credit. Under the pricing model that probability is two normal
// tails (Nations Ch. 10/14: "the delta of a hypothetical breakeven-strike
// option"). Logged with each trade, it says what the seller signed up for.
//
// SKEW PREMIUM. What a strike's own IV adds over the ATM IV, in rupees
// (Nations Table 6.2, "option price error"). The highest-IV strike is NOT the
// one that pays most: vega falls away from the money.
//
// ANNUALISED YIELD. (1 + y)^N - 1, not y * N (Nations appendix).
//
// Units: prices and levels in rupees; vol as a fraction (0.15); time in years;
// vol slopes in VOL POINTS per 1% spot move. NaN in, NaN out; nothing throws.

#pragma once

#include <analytics/greeks.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace altair {

namespace option_math_detail {
inline constexpr double kOmNaN = std::numeric_limits<double>::quiet_NaN();
}

/// IV's response to spot, split by direction. Both numbers are positive on an
/// index whose IV rises on falls and eases on rallies.
struct VolSlope {
    double per_pct_fall = option_math_detail::kOmNaN;   ///< vol points UP per 1% spot fall
    double per_pct_rise = option_math_detail::kOmNaN;   ///< vol points DOWN per 1% spot rise
    std::size_t n_fall = 0, n_rise = 0;
};

/// Regress daily IV changes (vol points) on daily spot returns (percent),
/// through the origin, separately on down days and up days. Pairs with a NaN
/// and unchanged days are skipped. A side with fewer than `min_each` days
/// stays NaN; so does every side when the spans differ in length.
[[nodiscard]] inline VolSlope vol_slope(std::span<const double> spot_ret_pct, std::span<const double> d_vol_pts,
                                        std::size_t min_each = 20) noexcept {
    VolSlope out;
    if (spot_ret_pct.size() != d_vol_pts.size()) { return out; }
    double fxy = 0.0, fxx = 0.0, rxy = 0.0, rxx = 0.0;
    for (std::size_t i = 0; i < spot_ret_pct.size(); ++i) {
        const double x = spot_ret_pct[i], y = d_vol_pts[i];
        if (!std::isfinite(x) || !std::isfinite(y) || x == 0.0) { continue; }
        if (x < 0.0) { fxy += x * y; fxx += x * x; ++out.n_fall; }
        else { rxy += x * y; rxx += x * x; ++out.n_rise; }
    }
    // Slope dVol/dRet is negative on both sides for an index; report magnitudes
    // in the direction named, so a positive number means "the usual way".
    if (out.n_fall >= min_each && fxx > 0.0) { out.per_pct_fall = -fxy / fxx; }
    if (out.n_rise >= min_each && rxx > 0.0) { out.per_pct_rise = -rxy / rxx; }
    return out;
}

/// The IV shift (vol points) that goes with a spot move of `spot_pct` percent
/// on a measured slope: up `per_pct_fall` per 1% fall, down `per_pct_rise`
/// per 1% rise. A NaN side gives NaN for moves that way.
[[nodiscard]] inline double linked_vol_points_asym(double spot_pct, const VolSlope& s) noexcept {
    if (!std::isfinite(spot_pct)) { return option_math_detail::kOmNaN; }
    if (spot_pct < 0.0) { return -s.per_pct_fall * spot_pct; }
    if (spot_pct > 0.0) { return -s.per_pct_rise * spot_pct; }
    return 0.0;
}

/// P(F_T <= level) for a driftless lognormal forward: the model's chance the
/// forward ends at or below `level` (Black-76's N(-d2)).
[[nodiscard]] inline double prob_below(double fwd, double level, double years, double iv) noexcept {
    if (!(fwd > 0.0) || !(years > 0.0) || !(iv > 0.0) || !std::isfinite(level)) {
        return option_math_detail::kOmNaN;
    }
    if (!(level > 0.0)) { return 0.0; }
    const double sd = iv * std::sqrt(years);
    return detail::norm_cdf((std::log(level / fwd) + 0.5 * sd * sd) / sd);
}

/// The model's probability that a short straddle at `strike`, sold for
/// `credit` (rupees per unit), is a loser if held to expiry unhedged: the
/// forward ends outside strike +/- credit.
[[nodiscard]] inline double straddle_loss_probability(double fwd, double strike, double years, double iv,
                                                      double credit) noexcept {
    if (!(credit > 0.0) || !(strike > 0.0)) { return option_math_detail::kOmNaN; }
    const double below = prob_below(fwd, strike - credit, years, iv);
    const double below_up = prob_below(fwd, strike + credit, years, iv);
    if (!std::isfinite(below) || !std::isfinite(below_up)) { return option_math_detail::kOmNaN; }
    return below + (1.0 - below_up);
}

/// Rupees a strike's own IV adds to (or takes from) its price over the ATM
/// IV: price(iv_strike) - price(iv_atm), Black-76, per unit.
[[nodiscard]] inline double skew_premium(bool call, double fwd, double strike, double years, double iv_strike,
                                         double iv_atm, double rate = 0.0) noexcept {
    if (!(fwd > 0.0) || !(strike > 0.0)) { return option_math_detail::kOmNaN; }
    const auto px = [&](double v) {
        const auto g = black76(call ? OptionRight::Call : OptionRight::Put,
                               Price{static_cast<std::int64_t>(std::llround(fwd * 100.0))},
                               Price{static_cast<std::int64_t>(std::llround(strike * 100.0))}, Years{years}, Vol{v},
                               rate);
        return g ? g->price / 100.0 : option_math_detail::kOmNaN;
    };
    return px(iv_strike) - px(iv_atm);
}

/// Compounded annual yield of a per-period yield earned `periods_per_year`
/// times: (1 + y)^N - 1. A 5.2% monthly overwrite is 83.7% a year, not 62.4%.
[[nodiscard]] inline double annualised_yield(double period_yield, double periods_per_year) noexcept {
    if (!(period_yield > -1.0) || !(periods_per_year > 0.0)) { return option_math_detail::kOmNaN; }
    return std::pow(1.0 + period_yield, periods_per_year) - 1.0;
}

} // namespace altair
