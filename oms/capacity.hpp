// oms/capacity.hpp -- market impact, and the capacity ceiling a size-blind
// cost model cannot express.
//
// P13-07.
//
// QUANTLAB's Q7: its Phase 2 cost model was LINEAR IN SIZE, and a linear model
// literally cannot produce a capacity bound. Cost per share stays constant as
// size grows, so expected profit grows without limit and the optimiser happily
// allocates a billion dollars to a strategy that would move the market twenty
// percent. Phase 7 replaced it and the book acquired a ceiling: roughly $68M,
// set by NSE liquidity rather than by anything about the alpha.
//
// THE SQUARE ROOT LAW.
//
//     impact = eta * sigma * sqrt(Q / V)
//
// Q the order size, V the daily volume, sigma the daily volatility. The
// square root is empirical, it holds across venues and decades, and eta is
// around 0.5 to 1 for equities. What matters is the SHAPE, not the constant:
//
//   * cost per share grows as sqrt(Q), so TOTAL cost grows as Q^1.5;
//   * revenue grows at best linearly in Q;
//   * so there is a size where the two cross, and that size is the capacity.
//
// A linear model has cost growing as Q and revenue as Q and they never cross.
// That is not a calibration error, it is the wrong functional form, and no
// amount of fitting fixes it.
//
// THE COEFFICIENT IS SWEPT, NOT FITTED.
//
// QUANTLAB rejected its own design-doc plan here as circular: calibrating an
// impact coefficient from a limit-order-book simulator whose depth profile you
// wrote gives you back the number you put in. So eta is taken from the
// literature and SWEPT across its plausible range, and the capacity is reported
// as an interval. A single capacity number implies a precision nobody has.

#pragma once

#include <core/types/units.hpp>

#include <cmath>
#include <cstdint>
#include <expected>

namespace altair {

enum class CapacityError : std::uint8_t {
    /// Volume, volatility or edge outside its admissible range.
    BadParameter,
    /// The strategy has no edge to spend on impact, so there is no capacity
    /// question -- the answer is zero at every size.
    NoEdge
};

struct ImpactParams {
    /// Daily volume in the instrument, in SHARES or contracts.
    double daily_volume = 0.0;
    /// Daily volatility, a plain fraction.
    double daily_vol = 0.0;
    /// The square-root-law coefficient. Literature: roughly 0.5-1.0 for
    /// equities. SWEPT, not fitted -- see the header.
    double eta = 0.7;

    [[nodiscard]] bool valid() const noexcept {
        return daily_volume > 0.0 && daily_vol > 0.0 && eta > 0.0;
    }
};

/// Impact as a FRACTION of price, for an order of `qty` shares.
///
/// Per share, not total: this is what each share pays, and it rises with size,
/// which is the property the linear model lacks.
[[nodiscard]] inline std::expected<double, CapacityError>
square_root_impact(double qty, const ImpactParams& p) noexcept {
    if (!p.valid() || !(qty >= 0.0)) {
        return std::unexpected(CapacityError::BadParameter);
    }
    return p.eta * p.daily_vol * std::sqrt(qty / p.daily_volume);
}

struct Capacity {
    /// Size at which impact exactly consumes the edge. Shares.
    double break_even_qty = 0.0;
    /// Size that MAXIMISES total profit -- always smaller than break-even,
    /// and it is the one that matters. Trading at break-even earns nothing.
    double optimal_qty = 0.0;
    /// Profit at the optimal size, in the same units as `edge * price * qty`.
    double max_profit_fraction = 0.0;
};

/// Where edge and impact cross.
///
/// Profit per share is `edge - impact(Q)`, so total profit is
///
///     P(Q) = Q * (edge - eta * sigma * sqrt(Q/V))
///
/// dP/dQ = 0 gives Q* = (2/3 * edge / (eta*sigma))^2 * V, and break-even is at
/// (edge/(eta*sigma))^2 * V. The optimum is FOUR NINTHS of break-even, which
/// is worth stating because the instinct is to size at break-even and that
/// earns exactly nothing.
[[nodiscard]] inline std::expected<Capacity, CapacityError>
capacity_from_edge(double edge_fraction, const ImpactParams& p) noexcept {
    if (!p.valid()) { return std::unexpected(CapacityError::BadParameter); }
    if (!(edge_fraction > 0.0)) {
        return std::unexpected(CapacityError::NoEdge);
    }
    const double k = p.eta * p.daily_vol;
    const double ratio = edge_fraction / k;
    Capacity c;
    c.break_even_qty = ratio * ratio * p.daily_volume;
    c.optimal_qty = (4.0 / 9.0) * c.break_even_qty;
    const auto imp = square_root_impact(c.optimal_qty, p);
    c.max_profit_fraction = imp
        ? c.optimal_qty * (edge_fraction - *imp) : 0.0;
    return c;
}

/// THE LINEAR MODEL, kept so the comparison can be made rather than asserted.
///
/// `cost_per_share = slope * edge_fraction` -- constant in size. Total profit
/// is then Q * (edge - constant), which is linear in Q and unbounded. Any
/// optimiser handed this allocates infinite capital.
[[nodiscard]] inline double linear_impact(double, double slope) noexcept {
    return slope;
}

} // namespace altair
