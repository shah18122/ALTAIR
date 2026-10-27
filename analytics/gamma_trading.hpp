// analytics/gamma_trading.hpp -- the short-gamma desk's arithmetic.
//
// From Simon Gleadall's Volcube guides (Option Gamma Trading; Trading Implied
// Volatility; Option Market Making). Each function is one line of algebra a
// vol desk does in its head; they are written down so the straddle seller can
// put the numbers in its trade reasons instead of only an IV-RV gap.
//
// GAMMA P&L IS QUADRATIC. A delta-hedged book carried through a move dS before
// re-hedging locks in about 1/2 * Gamma * dS^2. Double the move, four times the
// P&L. So the move at which ONE re-hedge pays (or costs) exactly a day's theta
// is sqrt(2 * theta / Gamma): the breakeven move. A seller who re-hedges at
// half that distance pays a quarter of the day's theta per hedge, and can
// stand four such hedges before the day is lost. That is the hedge band.
//
// THE VOL YOU LOCK IN DEPENDS ON HOW YOU HEDGE, not on a realised-vol number
// measured afterwards. Read it back from the P&L: a short book of vega V that
// made P has done as well as buying the options back P/V vol points below
// where it sold them. `locked_in_vol` is that number.
//
// VEGA DOES NOT ADD ACROSS EXPIRIES. Front-month IV moves more than back-month
// IV, so a rupee of weekly vega is more risk than a rupee of monthly vega.
// `time_weighted_vega` scales by sqrt(T_bench / T), the desk convention; when
// the recorder has enough chain history, `vega_beta` replaces the sqrt rule
// with the measured ratio of IV moves.
//
// A QUOTE'S WIDTH IS A COST IN VOL POINTS. (ask - bid) / vega says how many
// vols wide the market is; selling at the bid and buying back at the ask gives
// that up once. An IV-RV edge smaller than it is not an edge.
//
// Units: spot, moves and premiums in rupees per unit of underlying; vol as a
// fraction (0.15 = 15 vol points); vega in rupees per ONE vol point; theta in
// rupees per day (positive = collected, for a short book); gamma per rupee.
// NaN in, NaN out; nothing here throws.

#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

namespace altair {

namespace gamma_detail {
inline constexpr double kGammaNaN = std::numeric_limits<double>::quiet_NaN();
}

/// The one-day move the options price in: spot * IV / sqrt(sessions per year).
[[nodiscard]] inline double implied_daily_move(double spot, double iv, double sessions_per_year = 252.0) noexcept {
    if (!(spot > 0.0) || !(iv >= 0.0) || !(sessions_per_year > 0.0)) { return gamma_detail::kGammaNaN; }
    return spot * iv / std::sqrt(sessions_per_year);
}

/// P&L locked in by one re-hedge after a move `move`: 1/2 * gamma * move^2.
/// Signed by gamma: a short-gamma book (gamma < 0) loses it.
[[nodiscard]] inline double gamma_hedge_pnl(double gamma, double move) noexcept {
    return 0.5 * gamma * move * move;
}

/// The move at which one re-hedge equals a day's theta: sqrt(2 * |theta| / |gamma|).
[[nodiscard]] inline double gamma_breakeven_move(double theta_per_day, double gamma) noexcept {
    const double th = std::fabs(theta_per_day);
    const double g = std::fabs(gamma);
    if (!(g > 0.0) || !std::isfinite(th)) { return gamma_detail::kGammaNaN; }
    return std::sqrt(2.0 * th / g);
}

/// Re-hedge distances, in rupees of spot, either side of the last hedge.
struct HedgeBands {
    double down = gamma_detail::kGammaNaN;   ///< re-hedge once spot is this far BELOW the last hedge
    double up = gamma_detail::kGammaNaN;     ///< ... or this far above it
};

/// A short-gamma book's bands: `frac_up` and `frac_down` of the breakeven move.
/// 0.5 is the textbook choice (four hedges per day of theta). Make `frac_down`
/// smaller than `frac_up` when IV rises on falls, as it does on NIFTY: the
/// short-vega loss on a fall argues for hedging it sooner.
[[nodiscard]] inline HedgeBands short_gamma_bands(double theta_per_day, double gamma, double frac_up = 0.5,
                                                  double frac_down = 0.5) noexcept {
    const double b = gamma_breakeven_move(theta_per_day, gamma);
    if (!std::isfinite(b) || !(frac_up > 0.0) || !(frac_down > 0.0)) { return {}; }
    return {frac_down * b, frac_up * b};
}

/// True when `move` (spot now minus spot at the last hedge) has left the band.
/// Invalid bands always say re-hedge: never sit unhedged on a number that failed.
[[nodiscard]] inline bool outside_band(const HedgeBands& b, double move) noexcept {
    if (!std::isfinite(b.down) || !std::isfinite(b.up) || !std::isfinite(move)) { return true; }
    return move <= -b.down || move >= b.up;
}

/// |gamma| per rupee of daily |theta|. A seller wants it LOW (much theta for
/// little gamma); a buyer wants it high. Compare structures with it.
[[nodiscard]] inline double gamma_theta_ratio(double gamma, double theta_per_day) noexcept {
    const double th = std::fabs(theta_per_day);
    if (!(th > 0.0)) { return gamma_detail::kGammaNaN; }
    return std::fabs(gamma) / th;
}

/// The IV a trade effectively closed at: IV sold minus P&L over vega.
/// `vega_per_point` is the position's |vega| for one vol point (rupees);
/// `pnl` the trade's P&L (rupees); the result is a fraction like `iv_sold`.
/// For a short book that made money, the locked-in vol is below `iv_sold`.
[[nodiscard]] inline double locked_in_vol(double iv_sold, double pnl, double vega_per_point) noexcept {
    const double v = std::fabs(vega_per_point);
    if (!(v > 0.0) || !std::isfinite(iv_sold) || !std::isfinite(pnl)) { return gamma_detail::kGammaNaN; }
    return iv_sold - pnl / v / 100.0;
}

/// Vega restated in benchmark-expiry terms: vega * sqrt(T_bench / T).
[[nodiscard]] inline double time_weighted_vega(double vega, double years, double bench_years) noexcept {
    if (!(years > 0.0) || !(bench_years > 0.0)) { return gamma_detail::kGammaNaN; }
    return vega * std::sqrt(bench_years / years);
}

/// The measured alternative to the sqrt rule: regression slope of one expiry's
/// daily IV changes on the benchmark's, through the origin. Pairs with a NaN on
/// either side are skipped; fewer than `min_pairs` usable pairs gives NaN.
[[nodiscard]] inline double vega_beta(std::span<const double> d_iv, std::span<const double> d_iv_bench,
                                      std::size_t min_pairs = 20) noexcept {
    if (d_iv.size() != d_iv_bench.size()) { return gamma_detail::kGammaNaN; }
    double sxy = 0.0, sxx = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < d_iv.size(); ++i) {
        if (!std::isfinite(d_iv[i]) || !std::isfinite(d_iv_bench[i])) { continue; }
        sxy += d_iv[i] * d_iv_bench[i];
        sxx += d_iv_bench[i] * d_iv_bench[i];
        ++n;
    }
    if (n < min_pairs || !(sxx > 0.0)) { return gamma_detail::kGammaNaN; }
    return sxy / sxx;
}

/// How many vol points wide a quote is: (ask - bid) / vega per point.
[[nodiscard]] inline double spread_in_vols(double bid, double ask, double vega_per_point) noexcept {
    const double v = std::fabs(vega_per_point);
    if (!(v > 0.0) || !std::isfinite(bid) || !std::isfinite(ask) || ask < bid) { return gamma_detail::kGammaNaN; }
    return (ask - bid) / v;
}

/// A premium amount (slippage, charges) restated in vol points of the
/// position, as a FRACTION like an IV (0.004 = 0.4 vol points).
[[nodiscard]] inline double premium_in_vol(double premium, double vega_per_point) noexcept {
    const double v = std::fabs(vega_per_point);
    if (!(v > 0.0) || !std::isfinite(premium)) { return gamma_detail::kGammaNaN; }
    return premium / v / 100.0;
}

/// IV shift that goes with a spot move on an index whose IV rises on falls:
/// `vol_pts_per_pct` vol points per 1% FALL (and the same down per 1% rise).
/// Index options are rarely stressed with spot alone; this couples the two.
[[nodiscard]] inline double linked_vol_points(double spot_pct, double vol_pts_per_pct) noexcept {
    return -vol_pts_per_pct * spot_pct;
}

} // namespace altair
