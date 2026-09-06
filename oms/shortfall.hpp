// oms/shortfall.hpp -- Almgren-Chriss, and implementation shortfall.
//
// P13-05 / P13-06.
//
// THE TRADE-OFF ALMGREN-CHRISS MAKES EXPLICIT.
//
// Trade fast and you pay impact. Trade slowly and you carry the risk that the
// price moves against you while you wait. Every execution algorithm chooses a
// point on that curve; most choose it by accident, through a slice count
// somebody picked. Almgren-Chriss makes the choice a parameter -- risk
// aversion, lambda -- and solves for the trajectory that minimises
//
//     E[cost] + lambda * Var[cost]
//
// The closed form for the linear-impact, arithmetic-Brownian case is a
// hyperbolic-sine trajectory:
//
//     x(t) = X * sinh(kappa * (T - t)) / sinh(kappa * T)
//
// where x is the quantity REMAINING at time t, and
//
//     kappa ~ sqrt(lambda * sigma^2 / eta)
//
// eta being the temporary-impact coefficient. Two limits are worth holding on
// to because they are the sanity checks:
//
//   * lambda -> 0 (risk-neutral): kappa -> 0, the sinh ratio becomes linear,
//     and the trajectory IS TWAP. A risk-neutral trader has no reason to
//     hurry, so the optimal schedule is the uniform one. This is not a
//     coincidence and it is asserted in the test.
//   * lambda large: kappa large, the trajectory collapses toward the front --
//     get out now and pay for it.
//
// LAMBDA IS SMIT'S NUMBER, NOT A DEFAULT.
//
// It is a statement about how much money he is willing to pay to reduce
// variance, denominated in his own utility. There is no defensible default and
// this file does not supply one: `AcParams` has no initialiser for it and the
// solver refuses a non-positive value. CLAUDE.md's card-sizing rule says a
// card requiring a design decision is a card that should not have been
// written -- so the decision is surfaced rather than buried in a constant.
//
// AND THE ATTRIBUTION MUST SUM.
//
// Implementation shortfall decomposes the gap between the decision price and
// the realised average into delay, execution, and fees. The parts are
// individually interesting and collectively load-bearing: if they do not sum
// to the total, one of them is wrong and the plausible-looking three are
// hiding it. QUANTLAB asserted this and so does `attribute()`.

#pragma once

#include <core/types/units.hpp>

#include <cmath>
#include <cstdint>
#include <expected>

namespace altair {

enum class AcError : std::uint8_t {
    BadQuantity,
    /// Horizon is zero or negative.
    BadHorizon,
    /// Volatility, impact, or risk aversion outside its admissible range.
    BadParameter,
    /// More steps than the trajectory can hold.
    TooManySteps
};

struct AcParams {
    /// Annualised volatility of the price, as a fraction. Measured, and it
    /// carries error -- ROADMAP section 3 -- so the trajectory it produces is
    /// an estimate too.
    double sigma = 0.0;
    /// Temporary impact coefficient: price concession per unit of trading
    /// RATE. UNIT: paise per (unit per year).
    double eta = 0.0;
    /// Risk aversion. NO DEFAULT, deliberately -- see the header.
    double lambda = 0.0;

    [[nodiscard]] bool valid() const noexcept {
        return sigma > 0.0 && eta > 0.0 && lambda > 0.0;
    }
};

/// Quantity REMAINING at each step, including both endpoints: `remaining[0]`
/// is the full parent and `remaining[steps]` is zero.
class AcTrajectory {
public:
    static constexpr std::size_t kMaxSteps = 256;

    [[nodiscard]] std::size_t steps() const noexcept { return n_; }
    [[nodiscard]] std::int64_t remaining(std::size_t i) const noexcept {
        return rem_[i <= n_ ? i : n_];
    }
    /// Quantity to TRADE in step i, one-based on the interval (i-1, i].
    [[nodiscard]] std::int64_t trade(std::size_t i) const noexcept {
        if (i == 0 || i > n_) { return 0; }
        return rem_[i - 1] - rem_[i];
    }
    /// Every child summed. Must equal the parent exactly -- the same
    /// conservation check `Schedule::total()` carries in execution.hpp.
    [[nodiscard]] std::int64_t total_traded() const noexcept {
        return rem_[0] - rem_[n_];
    }
    [[nodiscard]] double kappa() const noexcept { return kappa_; }
    /// kappa * T -- THE regime parameter, and the one to read.
    ///
    /// kappa has units of 1/time, so on its own it says nothing: kappa = 6.3
    /// is deeply risk-averse over a year and indistinguishable from TWAP over
    /// a day. The first draft of the test picked a lambda by looking at kappa
    /// and got a "risk-averse" trajectory that was still uniform, because
    /// kappa*T was 0.017. Below about 0.1 the sinh is linear and this IS TWAP;
    /// above about 1 the front-loading is visible.
    [[nodiscard]] double kappa_t() const noexcept { return kappa_ * horizon_; }

    friend std::expected<AcTrajectory, AcError>
    almgren_chriss(Qty, double, const AcParams&, std::size_t) noexcept;

private:
    std::int64_t rem_[kMaxSteps + 1]{};
    std::size_t n_ = 0;
    double kappa_ = 0.0;
    double horizon_ = 0.0;
};

/// The closed-form trajectory. `horizon_years` is the execution window.
[[nodiscard]] inline std::expected<AcTrajectory, AcError>
almgren_chriss(Qty total, double horizon_years, const AcParams& p,
               std::size_t steps) noexcept {
    if (total.raw() <= 0) { return std::unexpected(AcError::BadQuantity); }
    if (!(horizon_years > 0.0)) { return std::unexpected(AcError::BadHorizon); }
    if (!p.valid()) { return std::unexpected(AcError::BadParameter); }
    if (steps == 0 || steps > AcTrajectory::kMaxSteps) {
        return std::unexpected(AcError::TooManySteps);
    }

    AcTrajectory t;
    t.n_ = steps;
    t.kappa_ = std::sqrt(p.lambda * p.sigma * p.sigma / p.eta);
    t.horizon_ = horizon_years;

    const double X = static_cast<double>(total.raw());
    const double T = horizon_years;
    const double k = t.kappa_;
    const double kT = k * T;

    // sinh(kT) overflows for large kT and underflows the ratio for small kT.
    // Both are handled explicitly rather than left to the library: the small
    // case is the RISK-NEUTRAL LIMIT and it must come out as TWAP, not as
    // 0/0.
    const bool linear = kT < 1e-8;
    const double denom = linear ? 0.0 : std::sinh(kT);

    t.rem_[0] = total.raw();
    for (std::size_t i = 1; i <= steps; ++i) {
        const double tau = T * static_cast<double>(i)
                         / static_cast<double>(steps);
        const double frac = linear
            ? (1.0 - tau / T)                       // TWAP
            : std::sinh(k * (T - tau)) / denom;
        // Round to the nearest unit; the endpoint is forced below rather than
        // trusted to arrive at zero, because a trajectory that leaves one unit
        // unexecuted is a position nobody meant to hold.
        t.rem_[i] = static_cast<std::int64_t>(X * frac + 0.5);
    }
    t.rem_[steps] = 0;

    // Monotone by construction in exact arithmetic; enforced after rounding so
    // no step can report a NEGATIVE trade, which would read as buying during a
    // sell programme.
    for (std::size_t i = 1; i <= steps; ++i) {
        if (t.rem_[i] > t.rem_[i - 1]) { t.rem_[i] = t.rem_[i - 1]; }
    }
    return t;
}

// ---------------------------------------------------------------------------
// P13-06 — implementation shortfall
// ---------------------------------------------------------------------------

/// Where the money went between deciding and being done.
///
/// Signs are stated once and hold throughout: every component is a COST when
/// positive, in paise, for the parent as a whole. A negative delay component
/// means the market moved in our favour while we waited -- which happens, and
/// reporting it as a negative cost rather than as "no delay cost" is what
/// makes the average over many parents meaningful.
struct Shortfall {
    /// Decision price to arrival price: the market moved before we started.
    std::int64_t delay_paise = 0;
    /// Arrival price to realised average: what the execution itself cost.
    std::int64_t execution_paise = 0;
    /// Brokerage, STT, exchange, GST and the rest. Always a debit.
    std::int64_t fees_paise = 0;
    /// Quantity that never got done, valued at the close against the decision
    /// price. THE COMPONENT EVERYONE FORGETS: an algorithm that finishes 60%
    /// of the parent and reports a wonderful average price has not done well,
    /// it has done less.
    std::int64_t opportunity_paise = 0;

    [[nodiscard]] std::int64_t total() const noexcept {
        return delay_paise + execution_paise + fees_paise + opportunity_paise;
    }
};

/// Decompose the shortfall for one parent.
///
/// `side` is +1 for a buy and -1 for a sell, and it is what makes "the price
/// went up" a cost on one side and a gain on the other. Getting this wrong is
/// the classic sign error that makes every short look profitable -- the same
/// one `core/invariant/conservation.hpp` warns about for costs.
[[nodiscard]] inline Shortfall
attribute(int side, Price decision, Price arrival, Price realised_avg,
          Qty ordered, Qty filled, Price close, Notional fees) noexcept {
    Shortfall s;
    const std::int64_t sgn = side >= 0 ? 1 : -1;
    const std::int64_t q = filled.raw();
    const std::int64_t unfilled = ordered.raw() - q;

    s.delay_paise = sgn * (arrival.raw() - decision.raw()) * q;
    s.execution_paise = sgn * (realised_avg.raw() - arrival.raw()) * q;
    s.fees_paise = fees.raw();
    s.opportunity_paise =
        unfilled > 0 ? sgn * (close.raw() - decision.raw()) * unfilled : 0;
    return s;
}

/// Do the parts sum to the whole?
///
/// The total is computed INDEPENDENTLY here -- from the decision price, the
/// realised average, the close and the fees -- and compared against the sum of
/// the four components. They must agree to the paisa. If they do not, one
/// component is wrong and the other three look fine, which is precisely the
/// failure an attribution is supposed to prevent.
[[nodiscard]] inline bool
attribution_sums(int side, Price decision, Price realised_avg, Qty ordered,
                 Qty filled, Price close, Notional fees,
                 const Shortfall& s) noexcept {
    const std::int64_t sgn = side >= 0 ? 1 : -1;
    const std::int64_t q = filled.raw();
    const std::int64_t unfilled = ordered.raw() - q;
    const std::int64_t independent =
        sgn * (realised_avg.raw() - decision.raw()) * q
        + (unfilled > 0 ? sgn * (close.raw() - decision.raw()) * unfilled : 0)
        + fees.raw();
    return independent == s.total();
}

} // namespace altair
