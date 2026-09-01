// risk/sizing.hpp -- how many lots, and which constraint decided.
//
// P4-01. Fixed-fractional, fractional Kelly and volatility targeting, all
// lot-size aware, all answering in whole contracts.
//
// THE MINIMUM BINDS, NEVER THE AVERAGE.
//
// Each method answers "how much can I hold" from a different premise: how much
// I am willing to lose on this trade, how much my edge justifies, how much
// volatility I am targeting. Averaging them produces a number no premise
// supports. The size is the SMALLEST of them, and `SizeDecision::binding`
// records which one it was -- so a position that looks too small has a named
// reason rather than an opinion.
//
// EVERYTHING IS ROUNDED DOWN.
//
// Down to whole lots, never to nearest. A NIFTY lot at 24,000 is about
// Rs 18 lakh of notional, so rounding to nearest can exceed the risk budget by
// most of a lakh on a single name. Rounding down can only ever leave capacity
// unused, and unused capacity does not blow up.
//
// ZERO IS A DECISION. When the budget does not reach one lot, the answer is
// zero lots with the binding constraint named -- not an error. "Do not trade
// this" is the most common correct answer a sizer gives, and making it an
// error tempts a caller into treating it as an exception to route around.
//
// QUARTER KELLY, AND THE FACTOR OF FOUR IS THE WHOLE POINT.
//
// Kelly maximises expected log growth ONLY IF the edge estimate is exact. It
// never is. With expected log growth g(f) = f*mu - f^2*sigma^2/2:
//
//     bet at            true edge mu        edge overestimated 2x
//     ---------------------------------------------------------------
//     full Kelly        0.500 (optimal)     0.000  -- ALL growth gone
//     half Kelly        0.375               0.375
//     quarter Kelly     0.219               0.375
//                                        (units of mu^2/sigma^2)
//
// Full Kelly tolerates a 2x overestimate before expected growth reaches ZERO.
// Quarter Kelly tolerates 8x. Giving up half the theoretical growth rate buys
// a four-fold increase in how wrong the edge estimate may be, and edge
// estimates on 10-minute forecasts are wrong by more than 2x routinely
// (CLAUDE.md: "anything claiming 70% is overfit"). The test computes that
// table rather than quoting it.
//
// THE EDGE MUST ALREADY BE NET OF COST (rule 5). Nothing here subtracts
// brokerage, STT or slippage: P3-09 and P3-10 do that, and a sizer that also
// deducted would double-count. `edge_bps` is what is left AFTER them.
//
// AND IT IS TAKEN AT ITS LOWER CONFIDENCE BOUND (ROADMAP section 3). Sizing on
// a point estimate assumes a precision no fitted edge has. An edge whose error
// bar straddles zero sizes to zero lots, by construction rather than by a
// caller remembering to check.

#pragma once

#include <core/types/units.hpp>

#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>

namespace altair {

enum class SizingError : std::uint8_t {
    /// Capital <= 0. Nothing to size against.
    NonPositiveCapital,
    /// Price <= 0, so a notional cannot be turned into units.
    NonPositivePrice,
    /// Lot size <= 0. Never a literal -- this comes from the spec store, and a
    /// zero here means the store was queried before it was loaded.
    BadLotSize,
    /// Volatility <= 0. A zero-vol asset would size to infinity.
    NonPositiveVol,
    /// A risk fraction outside (0, 1], or a stop distance <= 0.
    BadParameter,
    /// int64 overflow forming a notional or a quantity.
    Overflow,
    NotFinite
};

/// Which constraint produced the final size.
///
/// Ordinal 0 is `Unset` so a default-constructed decision cannot read as a
/// real one -- the same rule as GreekBasis, for the same reason.
enum class SizingBound : std::uint8_t {
    Unset = 0,
    FixedFractional,
    Kelly,
    VolTarget,
    FreezeQuantity,
    /// The edge's lower bound was not positive, so there is no position.
    NoEdge
};

/// The answer, and the full working that produced it.
///
/// Every method's own answer is kept, not just the winner. A size that
/// surprises someone is then a question with an answer already attached, and
/// rule 10 wants the decision reproducible from what was recorded.
struct SizeDecision {
    Lots lots{0};
    Qty qty{0};
    Notional notional{0};
    SizingBound binding = SizingBound::Unset;

    /// What each method allowed on its own, in whole lots.
    Lots by_fixed_fractional{0};
    Lots by_kelly{0};
    Lots by_vol_target{0};
    Lots by_freeze{0};

    /// The Kelly fraction actually used, after the multiple and the lower
    /// bound. Dimensionless fraction of capital.
    double kelly_fraction = 0.0;
    /// The lower confidence bound of the edge that Kelly was given. UNIT: bps.
    double edge_lower_bps = 0.0;
};

/// What the caller must supply. All of it, deliberately: a sizer with
/// defaults is a sizer that sizes on a default nobody chose.
///
/// The five policy numbers below are ZERO, and every one of them is refused as
/// zero by `size_position`. That is not an oversight -- it is the point. An
/// earlier version of this file shipped 1% risk, quarter Kelly, a 10% vol
/// target and 1x leverage as defaults, which are CAPITAL DECISIONS and belong
/// to Smit, not here (CLAUDE.md role table). A struct that compiles with
/// plausible-looking risk parameters nobody chose is how those numbers end up
/// in production unexamined.
struct SizingInputs {
    /// Account equity. UNIT: paise.
    Notional capital{0};
    /// Price of ONE unit. UNIT: paise.
    Price price{0};
    /// From the spec store. Never a literal (rule 1).
    LotSize lot_size{0};
    /// Exchange maximum single-order quantity, from the spec store. Zero means
    /// the exchange publishes none and no cap applies.
    Qty freeze_qty{0};

    /// Expected edge per trade, NET OF ALL COST. UNIT: bps of notional.
    Bps edge_bps{0.0};
    /// Standard error of that edge. UNIT: bps.
    Bps edge_std_error_bps{0.0};
    /// How many standard errors to subtract before sizing. 2.0 is the usual.
    double edge_sigmas = 0.0;

    /// Volatility of the instrument over the holding period, as a fraction.
    Vol period_vol{0.0};

    /// Fraction of capital risked if the stop is hit. 0.01 is 1%.
    double risk_fraction = 0.0;
    /// Distance from entry to stop. UNIT: paise per unit.
    Price stop_distance{0};

    /// Kelly denominator. 4.0 is quarter Kelly. 1.0 is full Kelly and is
    /// almost never right -- see the header table.
    double kelly_divisor = 0.0;

    /// Target portfolio volatility for the vol-targeting leg, as a fraction
    /// over the same period as `period_vol`.
    double target_vol = 0.0;
    /// Hard ceiling on notional / capital, whatever the methods say.
    double max_leverage = 0.0;
};

/// Whole lots that fit in a notional budget at this price. Rounds DOWN.
///
/// The only place a notional becomes contracts, so the rounding direction is
/// decided once. Returns 0 lots when the budget does not reach one.
[[nodiscard]] inline std::expected<Lots, SizingError>
lots_within(Notional budget, Price price, LotSize lot_size) noexcept {
    if (price.raw() <= 0) { return std::unexpected(SizingError::NonPositivePrice); }
    if (lot_size.raw() <= 0) { return std::unexpected(SizingError::BadLotSize); }
    if (budget.raw() <= 0) { return Lots{0}; }
    // Notional per lot = price * lot_size. Checked, because a currency
    // contract with a 1e7 price scale and a large lot is not far from int64.
    const auto per_lot = notional_of(price, Qty{lot_size.raw()});
    if (!per_lot) { return std::unexpected(SizingError::Overflow); }
    if (per_lot->raw() <= 0) { return std::unexpected(SizingError::Overflow); }
    return Lots{budget.raw() / per_lot->raw()};      // integer division: floor
}

/// Fixed-fractional: risk `risk_fraction` of capital if the stop is hit.
///
/// The budget is a LOSS budget, not a position budget: units = (capital *
/// fraction) / stop_distance. Confusing the two is how a 1% risk rule becomes
/// a 1% position and then a 20x under-sized book.
[[nodiscard]] inline std::expected<Lots, SizingError>
size_fixed_fractional(const SizingInputs& in) noexcept {
    if (in.capital.raw() <= 0) {
        return std::unexpected(SizingError::NonPositiveCapital);
    }
    if (!(in.risk_fraction > 0.0) || in.risk_fraction > 1.0) {
        return std::unexpected(SizingError::BadParameter);
    }
    if (in.stop_distance.raw() <= 0) {
        return std::unexpected(SizingError::BadParameter);
    }
    if (in.lot_size.raw() <= 0) { return std::unexpected(SizingError::BadLotSize); }

    const double loss_budget =
        static_cast<double>(in.capital.raw()) * in.risk_fraction;
    const double units = loss_budget / static_cast<double>(in.stop_distance.raw());
    const double lots = units / static_cast<double>(in.lot_size.raw());
    if (!std::isfinite(lots)) { return std::unexpected(SizingError::NotFinite); }
    if (lots <= 0.0) { return Lots{0}; }
    if (lots > 9.0e15) { return std::unexpected(SizingError::Overflow); }
    return Lots{static_cast<std::int64_t>(lots)};    // floor
}

/// The Kelly fraction of capital, on the LOWER bound of the edge.
///
/// f = (edge_lower / variance) / divisor, with edge and vol over the same
/// period. Returns 0 when the lower bound is not positive: an edge that could
/// be zero is not an edge to bet on.
[[nodiscard]] inline std::expected<double, SizingError>
kelly_fraction(const SizingInputs& in) noexcept {
    if (!(in.period_vol.raw() > 0.0)) {
        return std::unexpected(SizingError::NonPositiveVol);
    }
    if (!(in.kelly_divisor > 0.0)) {
        return std::unexpected(SizingError::BadParameter);
    }
    // bps -> fraction. 1 bp = 1e-4.
    const double edge_lower =
        (in.edge_bps.raw() - in.edge_sigmas * in.edge_std_error_bps.raw())
        * 1.0e-4;
    if (!std::isfinite(edge_lower)) {
        return std::unexpected(SizingError::NotFinite);
    }
    if (edge_lower <= 0.0) { return 0.0; }
    const double var = in.period_vol.raw() * in.period_vol.raw();
    const double f = edge_lower / var / in.kelly_divisor;
    if (!std::isfinite(f)) { return std::unexpected(SizingError::NotFinite); }
    return f > 0.0 ? f : 0.0;
}

/// Expected log growth rate of betting fraction `f` when the TRUE edge is
/// `mu` and volatility is `sigma`: g = f*mu - f^2*sigma^2/2.
///
/// Exposed because it is the function that justifies the Kelly divisor, and a
/// justification that cannot be evaluated is a slogan. The test uses it to
/// build the overbetting table in the header.
[[nodiscard]] constexpr double
log_growth(double f, double mu, double sigma) noexcept {
    return f * mu - 0.5 * f * f * sigma * sigma;
}

/// Volatility targeting: hold notional such that position vol meets a target.
///
/// notional = capital * target_vol / period_vol, capped by max_leverage. The
/// cap is not optional: as realised vol falls toward zero the unlevered answer
/// goes to infinity, and a quiet market is exactly when that is most tempting.
[[nodiscard]] inline std::expected<Lots, SizingError>
size_vol_target(const SizingInputs& in) noexcept {
    if (in.capital.raw() <= 0) {
        return std::unexpected(SizingError::NonPositiveCapital);
    }
    if (!(in.period_vol.raw() > 0.0)) {
        return std::unexpected(SizingError::NonPositiveVol);
    }
    if (!(in.target_vol > 0.0) || !(in.max_leverage > 0.0)) {
        return std::unexpected(SizingError::BadParameter);
    }
    double lev = in.target_vol / in.period_vol.raw();
    if (lev > in.max_leverage) { lev = in.max_leverage; }
    const double budget = static_cast<double>(in.capital.raw()) * lev;
    if (!std::isfinite(budget)) { return std::unexpected(SizingError::NotFinite); }
    if (budget > 9.0e18) { return std::unexpected(SizingError::Overflow); }
    return lots_within(Notional{static_cast<std::int64_t>(budget)}, in.price,
                       in.lot_size);
}

/// The whole decision: every method, the minimum, and what bound it.
[[nodiscard]] inline std::expected<SizeDecision, SizingError>
size_position(const SizingInputs& in) noexcept {
    if (in.capital.raw() <= 0) {
        return std::unexpected(SizingError::NonPositiveCapital);
    }
    if (in.price.raw() <= 0) {
        return std::unexpected(SizingError::NonPositivePrice);
    }
    if (in.lot_size.raw() <= 0) { return std::unexpected(SizingError::BadLotSize); }

    SizeDecision d{};
    d.edge_lower_bps =
        in.edge_bps.raw() - in.edge_sigmas * in.edge_std_error_bps.raw();

    const auto ff = size_fixed_fractional(in);
    if (!ff) { return std::unexpected(ff.error()); }
    d.by_fixed_fractional = *ff;

    const auto kf = kelly_fraction(in);
    if (!kf) { return std::unexpected(kf.error()); }
    d.kelly_fraction = *kf;
    double kelly_budget = static_cast<double>(in.capital.raw()) * *kf;
    const double cap = static_cast<double>(in.capital.raw()) * in.max_leverage;
    if (kelly_budget > cap) { kelly_budget = cap; }
    if (!std::isfinite(kelly_budget)) {
        return std::unexpected(SizingError::NotFinite);
    }
    const auto kl = lots_within(
        Notional{static_cast<std::int64_t>(kelly_budget)}, in.price,
        in.lot_size);
    if (!kl) { return std::unexpected(kl.error()); }
    d.by_kelly = *kl;

    const auto vt = size_vol_target(in);
    if (!vt) { return std::unexpected(vt.error()); }
    d.by_vol_target = *vt;

    // The exchange freeze quantity is a hard cap on ONE order, in units.
    // Zero means none is published. Floor to whole lots like everything else.
    d.by_freeze = Lots{std::numeric_limits<std::int64_t>::max()};
    if (in.freeze_qty.raw() > 0) {
        d.by_freeze = Lots{in.freeze_qty.raw() / in.lot_size.raw()};
    }

    // The minimum binds. Order matters only for reporting ties, and the order
    // chosen names the most informative cause first: no edge at all, then the
    // risk budget, then the model, then vol, then the exchange.
    Lots best = d.by_fixed_fractional;
    SizingBound who = SizingBound::FixedFractional;
    if (d.by_kelly.raw() < best.raw()) {
        best = d.by_kelly;
        who = (d.edge_lower_bps <= 0.0) ? SizingBound::NoEdge : SizingBound::Kelly;
    }
    if (d.by_vol_target.raw() < best.raw()) {
        best = d.by_vol_target;
        who = SizingBound::VolTarget;
    }
    if (d.by_freeze.raw() < best.raw()) {
        best = d.by_freeze;
        who = SizingBound::FreezeQuantity;
    }
    if (best.raw() < 0) { best = Lots{0}; }

    d.lots = best;
    d.binding = who;
    const auto q = qty_of(d.lots, in.lot_size);
    if (!q) { return std::unexpected(SizingError::Overflow); }
    d.qty = *q;
    const auto n = notional_of(in.price, d.qty);
    if (!n) { return std::unexpected(SizingError::Overflow); }
    d.notional = *n;
    return d;
}

} // namespace altair
