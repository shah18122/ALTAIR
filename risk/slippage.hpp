// risk/slippage.hpp -- execution cost learned from fill history.
//
// P3-10. Distinct from `book/flow.hpp`'s KyleLambda, which measures price
// impact from ORDER FLOW observed in the book. This measures what Altair's own
// orders actually cost, from Altair's own fills, which is the only number that
// belongs in rule 5's "net of full cost before it exists".
//
// IMPACT IS NOT LINEAR IN SIZE.
//
// The empirically robust law across every market anyone has measured is the
// SQUARE ROOT:
//
//     impact_bps  =  eta * daily_vol_bps * sqrt(participation)
//
// with participation = order size / average daily volume. A linear model
// fitted to square-root reality is wrong in both tails at once -- it
// underestimates the small orders that make up most of the count and
// overestimates the large ones that make up most of the cost. Both errors push
// a sizing rule the wrong way, and `tests/test_slippage.cpp` measures them.
//
// FITTED THROUGH THE ORIGIN. No intercept, for the same reason KyleLambda has
// none: an order of zero size moves nothing. An intercept would let the model
// charge a fixed cost that has nothing to do with the order -- and that cost
// already has a name, the spread, and a home, P3-09. Two models charging for
// the same thing is how a backtest ends up double-counting its way to
// profitability.
//
// THE SELECTION BIAS IS THE HARD PART, AND IT IS NOT SMALL.
//
// Fills are not a random sample of orders. A passive order fills when the
// market comes to you and misses when it runs away -- so the fills you HAVE
// are systematically the cheap ones, and an eta fitted to them alone is biased
// low. The orders that got away are the expensive ones and they are invisible
// in fill statistics by construction.
//
// The test builds exactly that experiment: generate orders from a known eta,
// let adverse ones miss, fit on the survivors, and watch eta come back well
// under the truth. `SlippageModel::add` therefore takes the WHOLE order,
// filled or not, and prices the unfilled remainder as opportunity cost. An
// unfilled order is not a free one.
//
// SIZE ON THE PESSIMISTIC BOUND. ROADMAP section 3 says to size on the lower
// confidence bound of EDGE; the same principle applied to COST means the upper
// bound. `predict_upper` exists so that a caller cannot accidentally size on
// the point estimate, and eta's standard error is carried for exactly that.

#pragma once

#include <risk/cost.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>

namespace altair {

enum class SlippageError : std::uint8_t {
    /// Fewer observations than the fit needs. Two points through the origin
    /// give a slope with no residual degrees of freedom, hence no error bar,
    /// which is the most dangerous thing this could report.
    TooFewFills,
    /// Every observation carries zero explanatory weight -- no size, or no
    /// volatility, so the regressor is identically zero.
    NoVariation,
    /// A non-finite or non-positive price, size or volatility.
    BadObservation
};

/// One order and what became of it.
///
/// The whole ORDER, not the fill. `ordered` and `filled` are separate because
/// the difference is the selection bias, and a record type that cannot express
/// a miss will quietly produce a model that has never seen one.
struct OrderOutcome {
    /// Mid at the instant the decision was taken. UNIT: paise.
    Price decision_price{0};
    /// Mid when the order reached the book. UNIT: paise.
    ///
    /// Between decision and arrival is DELAY COST, which belongs to the
    /// signal's latency and not to its impact. Separating them is why the
    /// shortfall decomposition below exists.
    Price arrival_price{0};
    /// Volume-weighted price achieved on the filled quantity. UNIT: paise.
    Price fill_price{0};
    /// Mid at the moment the order was cancelled or expired, used to price
    /// the unfilled remainder. UNIT: paise.
    Price final_price{0};
    /// Quantity sent, and quantity actually done.
    Qty ordered{0};
    Qty filled{0};
    Side side = Side::Buy;
    /// Order size as a fraction of average daily volume. Dimensionless.
    double participation = 0.0;
    /// Daily volatility at the time, as a fraction (0.015 is 1.5% a day).
    double daily_vol = 0.0;
};

/// Perold's implementation shortfall, decomposed. UNIT: basis points, signed
/// so that POSITIVE IS ADVERSE for both sides.
///
/// The decomposition matters because the three terms have different owners: a
/// large delay cost is a latency problem, a large impact cost is a sizing
/// problem, and a large opportunity cost is a limit-price problem. A single
/// blended "slippage" number cannot tell anyone which of the three to fix.
struct SlippageShortfall {
    /// decision -> arrival, on the whole order.
    double delay_bps = 0.0;
    /// arrival -> fill, on the filled quantity only.
    double impact_bps = 0.0;
    /// decision -> final, on the UNFILLED quantity. The cost of the order
    /// that got away, which is invisible in fill statistics.
    double opportunity_bps = 0.0;
    /// The sum. What the decision actually cost.
    double total_bps = 0.0;
    /// Fraction of the order that never traded.
    double unfilled_ratio = 0.0;
};

/// Sign convention: +1 when a rising price hurts (a buy), -1 for a sell.
[[nodiscard]] constexpr double adverse_sign(Side s) noexcept {
    return (s == Side::Buy) ? 1.0 : -1.0;
}

/// Decompose one order outcome.
///
/// Every term is weighted by the quantity it actually applies to, so the three
/// add to the total without double counting: delay applies to the whole order,
/// impact only to what filled, opportunity only to what did not.
[[nodiscard]] inline std::expected<SlippageShortfall, SlippageError>
implementation_shortfall(const OrderOutcome& o) noexcept {
    const double dp = static_cast<double>(o.decision_price.raw());
    const double ap = static_cast<double>(o.arrival_price.raw());
    const double fp = static_cast<double>(o.fill_price.raw());
    const double xp = static_cast<double>(o.final_price.raw());
    const double q = static_cast<double>(o.ordered.raw());
    const double f = static_cast<double>(o.filled.raw());

    if (!(dp > 0.0) || !(q > 0.0) || f < 0.0 || f > q
        || !std::isfinite(ap) || !std::isfinite(fp) || !std::isfinite(xp)) {
        return std::unexpected(SlippageError::BadObservation);
    }

    const double sgn = adverse_sign(o.side);
    const double to_bps = 10'000.0 / dp;

    SlippageShortfall s{};
    s.unfilled_ratio = (q - f) / q;
    s.delay_bps = sgn * (ap - dp) * to_bps;
    // Impact and opportunity are each scaled by the share of the order they
    // apply to. Reporting either on its own notional would make the three
    // terms not add up, which is exactly how a decomposition stops being one.
    s.impact_bps = sgn * (fp - ap) * to_bps * (f / q);
    s.opportunity_bps = sgn * (xp - dp) * to_bps * s.unfilled_ratio;
    s.total_bps = s.delay_bps + s.impact_bps + s.opportunity_bps;
    if (!std::isfinite(s.total_bps)) {
        return std::unexpected(SlippageError::BadObservation);
    }
    return s;
}

/// Realised impact of one order, in bps, against its arrival price.
///
/// This is the quantity the square-root law predicts, so it is the one the
/// model regresses. Positive is adverse.
[[nodiscard]] inline std::expected<double, SlippageError>
realised_impact_bps(const OrderOutcome& o) noexcept {
    const double ap = static_cast<double>(o.arrival_price.raw());
    const double fp = static_cast<double>(o.fill_price.raw());
    if (!(ap > 0.0) || !std::isfinite(fp) || o.filled.raw() <= 0) {
        return std::unexpected(SlippageError::BadObservation);
    }
    return adverse_sign(o.side) * (fp - ap) * 10'000.0 / ap;
}

/// The square-root impact model, fitted online from order outcomes.
///
/// One free parameter, eta, and it is dimensionless. Published estimates
/// cluster near 1 across equity markets; a fitted eta far from that is a
/// reason to look at the data rather than to believe the number.
class SlippageModel {
public:
    /// Fold in one order outcome.
    ///
    /// Takes the WHOLE order. An order that missed entirely still contributes
    /// its opportunity cost, which is what stops the model being fitted to
    /// the cheap half of the sample.
    ALTAIR_HOT std::expected<void, SlippageError>
    add(const OrderOutcome& o) noexcept {
        if (!(o.participation > 0.0) || !(o.daily_vol > 0.0)
            || !std::isfinite(o.participation)
            || !std::isfinite(o.daily_vol)) {
            return std::unexpected(SlippageError::BadObservation);
        }
        const auto sf = implementation_shortfall(o);
        if (!sf) { return std::unexpected(sf.error()); }

        // The regressor: vol in bps times the square root of participation.
        const double x = o.daily_vol * 10'000.0
                       * std::sqrt(o.participation);
        // The response: the FULL shortfall against arrival, including what the
        // unfilled part cost. Regressing realised_impact_bps instead would fit
        // only the fills, which is the bias this class exists to avoid.
        const double y = sf->impact_bps + sf->opportunity_bps;
        if (!std::isfinite(x) || !std::isfinite(y) || !(x > 0.0)) {
            return std::unexpected(SlippageError::BadObservation);
        }

        sxy_ += x * y;
        sxx_ += x * x;
        syy_ += y * y;
        ++n_;
        if (o.filled.raw() <= 0) { ++misses_; }
        unfilled_ += sf->unfilled_ratio;
        return {};
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return n_; }
    /// Orders that filled nothing at all. Reported because a model fitted to a
    /// sample with no misses in it has not seen the expensive half of reality.
    [[nodiscard]] std::uint64_t misses() const noexcept { return misses_; }
    /// Mean unfilled fraction across every order seen.
    [[nodiscard]] double mean_unfilled() const noexcept {
        return (n_ > 0) ? unfilled_ / static_cast<double>(n_) : 0.0;
    }

    /// The fitted impact coefficient. Dimensionless.
    [[nodiscard]] std::expected<double, SlippageError> eta() const noexcept {
        if (n_ < 3) { return std::unexpected(SlippageError::TooFewFills); }
        if (!(sxx_ > 0.0)) { return std::unexpected(SlippageError::NoVariation); }
        return sxy_ / sxx_;
    }

    /// Standard error of eta, from the residual spread about the fitted line.
    ///
    /// n-1 degrees of freedom, not n-2: the fit has ONE parameter because it
    /// passes through the origin. Using n-2 here would overstate the error by
    /// a few percent on a small sample and understate the confidence a caller
    /// is entitled to.
    [[nodiscard]] std::expected<double, SlippageError>
    eta_std_error() const noexcept {
        const auto e = eta();
        if (!e) { return std::unexpected(e.error()); }
        const double dof = static_cast<double>(n_) - 1.0;
        if (!(dof > 0.0)) { return std::unexpected(SlippageError::TooFewFills); }
        // Residual sum of squares for a through-origin fit.
        const double rss = syy_ - *e * sxy_;
        const double s2 = (rss > 0.0) ? rss / dof : 0.0;
        return std::sqrt(s2 / sxx_);
    }

    /// Expected impact, in bps, for an order of this size in this volatility.
    [[nodiscard]] std::expected<double, SlippageError>
    predict(double daily_vol, double participation) const noexcept {
        if (!(daily_vol > 0.0) || !(participation > 0.0)) {
            return std::unexpected(SlippageError::BadObservation);
        }
        const auto e = eta();
        if (!e) { return std::unexpected(e.error()); }
        return *e * daily_vol * 10'000.0 * std::sqrt(participation);
    }

    /// The pessimistic bound: expected impact plus `sigmas` standard errors.
    ///
    /// THE ONE TO SIZE ON. Sizing on `predict` assumes the fitted eta is the
    /// true one, and on a hundred fills it is not -- the error bar is the
    /// honest statement of how little is known. Rule 5 says a signal is priced
    /// net of FULL cost before it exists, and a cost estimate that ignores its
    /// own uncertainty is not the full cost.
    [[nodiscard]] std::expected<double, SlippageError>
    predict_upper(double daily_vol, double participation,
                  double sigmas = 2.0) const noexcept {
        const auto mid = predict(daily_vol, participation);
        if (!mid) { return std::unexpected(mid.error()); }
        const auto se = eta_std_error();
        if (!se) { return std::unexpected(se.error()); }
        return *mid + sigmas * *se * daily_vol * 10'000.0
                    * std::sqrt(participation);
    }

    /// The largest participation whose expected cost stays within `budget_bps`.
    ///
    /// Inverts the square-root law, which is the direction a sizing rule
    /// actually needs: not "what does this order cost" but "how much can I
    /// trade for this cost". Uses the PESSIMISTIC bound by default, so the
    /// answer is a size that is affordable even if eta is understated.
    [[nodiscard]] std::expected<double, SlippageError>
    max_participation(double daily_vol, double budget_bps,
                      double sigmas = 2.0) const noexcept {
        if (!(daily_vol > 0.0) || !(budget_bps > 0.0)) {
            return std::unexpected(SlippageError::BadObservation);
        }
        const auto e = eta();
        if (!e) { return std::unexpected(e.error()); }
        const auto se = eta_std_error();
        if (!se) { return std::unexpected(se.error()); }
        const double eff = *e + sigmas * *se;
        if (!(eff > 0.0)) { return std::unexpected(SlippageError::NoVariation); }
        // budget = eff * vol_bps * sqrt(p)  =>  p = (budget / (eff*vol_bps))^2.
        // The SQUARE is the point: halving the cost budget quarters the size a
        // linear model would have allowed.
        const double r = budget_bps / (eff * daily_vol * 10'000.0);
        return r * r;
    }

    void reset() noexcept { *this = SlippageModel{}; }

private:
    double sxy_ = 0.0;
    double sxx_ = 0.0;
    double syy_ = 0.0;
    double unfilled_ = 0.0;
    std::uint64_t n_ = 0;
    std::uint64_t misses_ = 0;
};

} // namespace altair
