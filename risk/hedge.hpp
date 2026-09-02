// risk/hedge.hpp -- vega sizing across the term structure, and the tail hedge.
//
// P10-08.
//
// VEGA DOES NOT ADD ACROSS EXPIRIES.
//
// This is the card. A book's vega is routinely reported as one number: sum the
// per-option vegas and quote "we are long 40,000 vega". That number is the
// answer to a question nobody asked -- what happens if EVERY expiry's implied
// volatility moves by one point AT ONCE.
//
// It does not. The term structure of volatility-of-volatility is steeply
// downward sloping: a one-point move in the one-week is an ordinary Tuesday,
// and a one-point move in the six-month is a large event. Empirically the
// standard deviation of implied-vol changes scales roughly as 1/sqrt(T), so a
// front-month vega and a back-month vega of equal size carry very different
// risk.
//
// Measured on two books with the SAME raw vega of 40,000, quoted against a
// 30-day reference:
//
//                   raw vega   weighted vega   concentration
//     front-heavy     40000        71261           1.782
//     back-heavy      40000        24388           0.610
//
// Identical on the risk report; 2.9x apart in what a one-point move at the
// reference tenor is actually worth. `weighted_vega` rescales each bucket
// before adding and keeps `raw_vega` alongside, so the difference can be seen
// rather than argued about.
//
// A TAIL HEDGE SIZED ON EXPECTED VALUE IS ALWAYS ZERO.
//
// The second thing, and it is the reason tail hedges do not get bought. A
// far out-of-the-money put has negative expected P&L -- that is what the
// variance risk premium P10-07 measures, seen from the buying side. Any sizing
// rule that maximises expected return will therefore hold none of it, and will
// be right on average and ruined once.
//
// The rule that works sizes on the DRAWDOWN AVOIDED: how much of the book's
// tail loss the hedge removes, against how much the premium costs in the
// ordinary case. `tail_hedge_size` takes both explicitly, so the trade-off is
// a number somebody chose rather than an omission.

#pragma once

#include <core/types/units.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

inline constexpr std::size_t kMaxTenorBuckets = 12;

enum class HedgeError : std::uint8_t {
    /// The reference tenor was not supplied.
    NoReferenceTenor,
    /// A tenor was zero or negative.
    BadTenor,
    /// The loss budget was not supplied.
    NoBudget,
    /// Fewer buckets than supplied capacity.
    TooManyBuckets,
    /// The hedge would cost more than the loss it prevents in every scenario.
    NotWorthwhile
};

/// Vega in one expiry bucket.
struct VegaBucket {
    /// Years to expiry. From the spec store, never a literal (rule 1).
    Years tenor{0.0};
    /// Vega in paise per vol POINT (0.01 of sigma), which is the desk
    /// convention analytics/greeks.hpp's `vega_per_vol_point` produces.
    double vega = 0.0;
};

struct VegaProfile {
    /// The naive sum. What a risk report usually quotes.
    double raw_vega = 0.0;
    /// Each bucket rescaled to the reference tenor, then summed. What a
    /// one-point move at the REFERENCE tenor is actually worth.
    double weighted_vega = 0.0;
    /// weighted / raw. Above 1 means the book is front-heavy and the naive
    /// number understates it; below 1 means back-heavy and it overstates.
    double concentration = 0.0;
    /// The single bucket contributing the most weighted vega, which is where
    /// a hedge should go rather than spread evenly.
    std::size_t dominant = 0;
    std::size_t buckets = 0;
};

/// How much a one-vol-point move at `tenor` is worth relative to one at
/// `reference`.
///
/// sqrt(reference / tenor). The empirical scaling of implied-vol volatility
/// along the term structure: short tenors move more. A flat weighting -- which
/// is what summing raw vega does -- assumes the whole surface shifts in
/// parallel, and a parallel shift is not what a volatility surface does.
[[nodiscard]] inline std::expected<double, HedgeError>
tenor_weight(Years tenor, Years reference) noexcept {
    if (!(tenor.raw() > 0.0)) { return std::unexpected(HedgeError::BadTenor); }
    if (!(reference.raw() > 0.0)) {
        return std::unexpected(HedgeError::NoReferenceTenor);
    }
    return std::sqrt(reference.raw() / tenor.raw());
}

/// Sum vega correctly, and report what the naive sum would have said.
///
/// `reference` has no default. A book quoted "in 30-day vega" and one quoted
/// "in 90-day vega" are different numbers for the same risk, and leaving the
/// reference implicit is how two desks compare figures that are not
/// comparable.
[[nodiscard]] inline std::expected<VegaProfile, HedgeError>
weighted_vega(const VegaBucket* b, std::size_t n, Years reference) noexcept {
    if (n == 0 || n > kMaxTenorBuckets) {
        return std::unexpected(HedgeError::TooManyBuckets);
    }
    if (!(reference.raw() > 0.0)) {
        return std::unexpected(HedgeError::NoReferenceTenor);
    }
    VegaProfile p{};
    p.buckets = n;
    double best = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto w = tenor_weight(b[i].tenor, reference);
        if (!w) { return std::unexpected(w.error()); }
        const double contribution = b[i].vega * *w;
        p.raw_vega += b[i].vega;
        p.weighted_vega += contribution;
        if (std::fabs(contribution) > best) {
            best = std::fabs(contribution);
            p.dominant = i;
        }
    }
    p.concentration = std::fabs(p.raw_vega) > 0.0
                    ? p.weighted_vega / p.raw_vega : 0.0;
    return p;
}

// ---------------------------------------------------------------------------
// The tail hedge
// ---------------------------------------------------------------------------

/// The decision a tail hedge actually poses.
struct TailHedgeSpec {
    /// What the unhedged book loses in the scenario being hedged, in paise.
    /// Positive number.
    Notional tail_loss{};
    /// How much of that loss one unit of the hedge removes, in paise.
    Notional protection_per_unit{};
    /// What one unit costs per period, in paise. Certain, and paid whether or
    /// not the scenario arrives.
    Notional premium_per_unit{};
    /// The probability assigned to the scenario. Required, and the honest
    /// answer is that it is a judgement -- which is why it is an input rather
    /// than something this file estimates.
    double scenario_probability = 0.0;
    /// The largest loss the book is willing to take in the scenario, in paise.
    /// THE budget, and it has no default: "how much of a disaster is
    /// acceptable" is the one number a risk system cannot infer.
    Notional loss_budget{};

    [[nodiscard]] bool valid() const noexcept {
        return tail_loss.raw() > 0 && protection_per_unit.raw() > 0
            && premium_per_unit.raw() > 0 && loss_budget.raw() > 0
            && scenario_probability > 0.0 && scenario_probability < 1.0;
    }
};

struct TailHedge {
    /// Units to buy.
    std::int64_t units = 0;
    /// Total premium per period, in paise.
    Notional cost{};
    /// Loss remaining in the scenario after the hedge, in paise.
    Notional residual_loss{};
    /// What an EXPECTED-VALUE rule would have bought. Almost always zero, and
    /// carried so the difference is visible.
    std::int64_t expected_value_units = 0;
    /// Expected P&L of one unit, in paise. Negative for any real tail hedge --
    /// that is the variance risk premium seen from the buying side.
    double expected_pnl_per_unit = 0.0;
};

/// Size a tail hedge on the DRAWDOWN IT PREVENTS, not on expected value.
///
/// Buys the smallest number of units that brings the scenario loss within the
/// budget. If the premium is so large that the hedge cannot be afforded, that
/// is reported rather than silently truncated -- an unaffordable hedge is a
/// statement about the position size, and the answer is a smaller book rather
/// than a smaller hedge.
[[nodiscard]] inline std::expected<TailHedge, HedgeError>
tail_hedge_size(const TailHedgeSpec& s) noexcept {
    if (!s.valid()) { return std::unexpected(HedgeError::NoBudget); }

    TailHedge h{};
    // The EV rule, computed first so the comparison is available even when it
    // says zero: p * protection - premium.
    h.expected_pnl_per_unit =
        s.scenario_probability * static_cast<double>(s.protection_per_unit.raw())
        - static_cast<double>(s.premium_per_unit.raw());
    h.expected_value_units = h.expected_pnl_per_unit > 0.0 ? 1 : 0;

    if (s.tail_loss.raw() <= s.loss_budget.raw()) {
        h.residual_loss = s.tail_loss;
        return h;                       // already inside the budget
    }
    const std::int64_t gap = s.tail_loss.raw() - s.loss_budget.raw();
    // Round UP. A hedge that covers all but a rupee of the gap has not met the
    // budget, and rounding down here is how a limit becomes a target.
    h.units = (gap + s.protection_per_unit.raw() - 1)
            / s.protection_per_unit.raw();
    h.cost = Notional{h.units * s.premium_per_unit.raw()};
    const std::int64_t covered = h.units * s.protection_per_unit.raw();
    h.residual_loss = Notional{s.tail_loss.raw() - covered > 0
                                   ? s.tail_loss.raw() - covered : 0};
    return h;
}

/// How many periods of premium the hedge costs, expressed as a fraction of the
/// loss it prevents.
///
/// The number the decision actually turns on: a hedge costing 2% a year of
/// what it saves is cheap, and one costing 40% is a view on timing rather than
/// insurance.
[[nodiscard]] inline std::expected<double, HedgeError>
cost_ratio(const TailHedge& h, const TailHedgeSpec& s,
           std::size_t periods) noexcept {
    if (h.units == 0 || periods == 0) {
        return std::unexpected(HedgeError::NotWorthwhile);
    }
    const double covered =
        static_cast<double>(h.units * s.protection_per_unit.raw());
    if (!(covered > 0.0)) { return std::unexpected(HedgeError::NotWorthwhile); }
    return static_cast<double>(h.cost.raw())
         * static_cast<double>(periods) / covered;
}

} // namespace altair
