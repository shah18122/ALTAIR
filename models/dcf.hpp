// models/dcf.hpp -- three-stage discounted cash flow, FCFF and FCFE.
//
// P10-02.
//
// FCFF IS DISCOUNTED AT WACC. FCFE IS DISCOUNTED AT THE COST OF EQUITY.
// MIXING THEM IS THE CLASSIC ERROR AND ITS SIZE IS THE LEVERAGE.
//
// This is the card. The two cash flows answer different questions and the two
// rates belong to different claimants:
//
//   FCFF  cash available to EVERYONE -- debt and equity. Discount at WACC,
//         which blends both costs. The result is ENTERPRISE value, and equity
//         is what remains after subtracting net debt.
//   FCFE  cash available to SHAREHOLDERS, after interest and debt repayment.
//         Discount at the cost of equity. The result is EQUITY value directly.
//
// Discount FCFF at the cost of equity and the answer is too low by roughly the
// tax shield; discount FCFE at WACC and it is too high, and then subtracting
// net debt double-counts the debt that FCFE already paid. Both produce a
// number with the right units, the right order of magnitude, and a confident
// air.
//
// Measured on a company 27% funded by debt, WACC 0.1083 against a cost of
// equity of 0.1250: discounting FCFF at the cost of equity gives an equity
// value of Rs 1,750,111,131 against a correct Rs 2,313,023,839. The mistake is
// worth 24.3% of the answer.
//
// So the two are separate functions with separate return types,
// `EnterpriseValue` and `EquityValue`, and neither takes a bare "discount
// rate" -- each takes the rate it belongs with, named, so a caller reaching for
// the wrong one has to type the wrong word to get it in.
//
// THE TERMINAL VALUE IS MOST OF THE ANSWER.
//
// The second thing, and it is what a DCF actually is. Under any ordinary
// parameterisation the terminal value -- everything from year 11 onward -- is
// 60 to 80 percent of the present value. A DCF is therefore mostly an
// assumption about a perpetuity, dressed as a ten-year forecast, and the ten
// years of detailed modelling move the answer less than a 50 basis point
// change in the terminal growth rate.
//
// Measured on the same company: the ten explicit years contribute
// Rs 990,974,975 and the perpetuity Rs 1,522,048,863 -- a TERMINAL SHARE of
// 60.6%. And 50 basis points on the terminal growth rate moves the whole
// valuation by 6.1%, which is more than the ten years of detailed modelling
// that precede it.
//
// `DcfResult` reports the terminal share for exactly that reason. A valuation
// quoted without it invites the reader to believe the forecast did the work.
//
// AND g MUST BE BELOW r, OR THE PERPETUITY IS NOT A NUMBER.
//
// The Gordon terminal value is FCF*(1+g)/(r-g). At g = r it is infinite; above
// it, negative. A model that lets a user type a terminal growth of 12% against
// a 10% discount rate returns a large negative enterprise value and looks like
// a bug somewhere else entirely.

#pragma once

#include <core/types/units.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

inline constexpr std::size_t kMaxDcfYears = 30;

enum class DcfError : std::uint8_t {
    /// Terminal growth is at or above the discount rate.
    GrowthExceedsDiscount,
    /// A rate was non-positive, or a stage length was zero.
    BadParameter,
    /// More projection years than the fixed capacity.
    TooManyYears,
    /// The share count is zero, so there is no per-share value.
    NoShares
};

/// A three-stage growth profile: explicit, fading, then perpetual.
///
/// Three stages rather than two because a single fade from a high growth rate
/// straight to perpetuity implies a discontinuity no business has -- and
/// because the middle stage is where an analyst's actual view lives, so making
/// it explicit is what allows it to be argued with.
struct GrowthProfile {
    /// Stage 1: explicit years at `high_growth`.
    std::size_t high_years = 0;
    double high_growth = 0.0;
    /// Stage 2: years fading LINEARLY from high_growth to terminal_growth.
    std::size_t fade_years = 0;
    /// Stage 3: forever.
    double terminal_growth = 0.0;

    [[nodiscard]] bool valid() const noexcept {
        return high_years > 0 && fade_years > 0
            && high_years + fade_years <= kMaxDcfYears;
    }
    /// Growth in year `t`, one-based.
    [[nodiscard]] double growth_at(std::size_t t) const noexcept {
        if (t <= high_years) { return high_growth; }
        const std::size_t k = t - high_years;
        if (k > fade_years) { return terminal_growth; }
        // Linear fade. At k = fade_years the rate IS terminal_growth, so the
        // perpetuity starts from the rate the fade ended at rather than
        // stepping down again.
        const double f = static_cast<double>(k)
                       / static_cast<double>(fade_years);
        return high_growth + (terminal_growth - high_growth) * f;
    }
};

struct DcfResult {
    /// Present value of the explicit and fading years.
    double pv_explicit = 0.0;
    /// Present value of the perpetuity.
    double pv_terminal = 0.0;
    double total = 0.0;
    /// pv_terminal / total. THE number that says how much of this valuation is
    /// a forecast and how much is an assumption about forever.
    double terminal_share = 0.0;
    std::size_t years = 0;
};

/// Enterprise value from FCFF, discounted at WACC.
///
/// Strongly typed so it cannot be handed to a per-share calculation without
/// net debt being subtracted first.
struct EnterpriseValue {
    DcfResult dcf{};
    [[nodiscard]] double value() const noexcept { return dcf.total; }
};

/// Equity value from FCFE, discounted at the cost of equity.
struct EquityValue {
    DcfResult dcf{};
    [[nodiscard]] double value() const noexcept { return dcf.total; }
};

namespace detail {

[[nodiscard]] inline std::expected<DcfResult, DcfError>
discount(double base_cf, const GrowthProfile& g, double rate) noexcept {
    if (!g.valid()) { return std::unexpected(DcfError::BadParameter); }
    if (!(rate > 0.0)) { return std::unexpected(DcfError::BadParameter); }
    // g < r, checked BEFORE anything is computed. At g = r the perpetuity is
    // infinite and above it negative, and a large negative enterprise value
    // looks like a bug somewhere else entirely.
    if (!(g.terminal_growth < rate)) {
        return std::unexpected(DcfError::GrowthExceedsDiscount);
    }

    DcfResult r{};
    r.years = g.high_years + g.fade_years;
    double cf = base_cf;
    double df = 1.0;
    for (std::size_t t = 1; t <= r.years; ++t) {
        cf *= 1.0 + g.growth_at(t);
        df /= 1.0 + rate;
        r.pv_explicit += cf * df;
    }
    // Gordon, applied to the LAST projected cash flow and discounted back over
    // the whole explicit period. Forgetting the second discounting is the
    // other common slip and it inflates the answer by (1+r)^N.
    const double terminal = cf * (1.0 + g.terminal_growth)
                          / (rate - g.terminal_growth);
    r.pv_terminal = terminal * df;
    r.total = r.pv_explicit + r.pv_terminal;
    r.terminal_share = std::fabs(r.total) > 0.0
                     ? r.pv_terminal / r.total : 0.0;
    return r;
}

} // namespace detail

/// FCFF -> enterprise value, at WACC.
///
/// The parameter is named `wacc` and not `discount_rate`. That is the whole
/// defence against the card's headline error: a caller reaching for the cost
/// of equity has to type the wrong word to get it in.
[[nodiscard]] inline std::expected<EnterpriseValue, DcfError>
dcf_fcff(double fcff, const GrowthProfile& g, double wacc) noexcept {
    const auto r = detail::discount(fcff, g, wacc);
    if (!r) { return std::unexpected(r.error()); }
    return EnterpriseValue{*r};
}

/// FCFE -> equity value, at the cost of equity.
[[nodiscard]] inline std::expected<EquityValue, DcfError>
dcf_fcfe(double fcfe, const GrowthProfile& g, double cost_of_equity) noexcept {
    const auto r = detail::discount(fcfe, g, cost_of_equity);
    if (!r) { return std::unexpected(r.error()); }
    return EquityValue{*r};
}

/// Equity value from an ENTERPRISE value: subtract net debt.
///
/// Only defined on `EnterpriseValue`. An `EquityValue` has already netted the
/// debt -- FCFE is after interest and repayment -- so there is deliberately no
/// overload, and double-subtracting is a compile error rather than a
/// valuation that is low by the debt.
[[nodiscard]] inline double equity_from_enterprise(const EnterpriseValue& ev,
                                                   Notional total_debt,
                                                   Notional cash) noexcept {
    return ev.value() - static_cast<double>(total_debt.raw())
                      + static_cast<double>(cash.raw());
}

/// WACC = E/V * Re + D/V * Rd * (1 - tax).
///
/// The tax shield is on the DEBT leg only. Applying it to the whole thing is
/// the third of the three errors this file exists around, and it understates
/// the discount rate by the equity share of the shield.
[[nodiscard]] inline std::expected<double, DcfError>
wacc(double equity_value, double debt_value, double cost_of_equity,
     double cost_of_debt, double tax_rate) noexcept {
    const double v = equity_value + debt_value;
    if (!(v > 0.0) || !(cost_of_equity > 0.0) || tax_rate < 0.0
        || tax_rate >= 1.0) {
        return std::unexpected(DcfError::BadParameter);
    }
    return equity_value / v * cost_of_equity
         + debt_value / v * cost_of_debt * (1.0 - tax_rate);
}

/// How sensitive the answer is to the terminal growth rate.
///
/// Returns the fractional change in total value per 1 basis point of terminal
/// growth. Reported alongside a valuation because a DCF whose answer moves 4%
/// on 50bp of an unobservable perpetual growth rate is a different kind of
/// object from one that moves 0.2%.
[[nodiscard]] inline std::expected<double, DcfError>
terminal_growth_sensitivity(double base_cf, const GrowthProfile& g,
                            double rate) noexcept {
    const auto base = detail::discount(base_cf, g, rate);
    if (!base) { return std::unexpected(base.error()); }
    GrowthProfile bumped = g;
    bumped.terminal_growth += 0.0001;
    const auto up = detail::discount(base_cf, bumped, rate);
    if (!up) { return std::unexpected(up.error()); }
    if (!(std::fabs(base->total) > 0.0)) {
        return std::unexpected(DcfError::BadParameter);
    }
    return (up->total - base->total) / base->total;
}

// ---------------------------------------------------------------------------
// P10-02b — the two things that make a DCF falsifiable
// ---------------------------------------------------------------------------

/// A two-way sensitivity table over WACC and terminal growth.
///
/// A DCF QUOTED AS ONE NUMBER IS DISHONEST, and not by a little. The two
/// inputs that move it most are the two nobody can observe: a discount rate
/// assembled from a beta and an equity risk premium that are themselves
/// estimates, and a growth rate that runs to forever. Moving each by half a
/// percent -- well inside any analyst's uncertainty -- routinely swings the
/// answer by more than the discount to market price that the whole exercise
/// was meant to establish.
///
/// So the deliverable is the GRID, and `spread()` below is the number to read
/// first: if the range is wider than the mispricing being claimed, the
/// valuation has not answered the question it was asked.
struct SensitivityGrid {
    static constexpr std::size_t kMax = 9;
    std::size_t n_rate = 0;      ///< rows: discount rate
    std::size_t n_growth = 0;    ///< cols: terminal growth
    double rate[kMax] = {};
    double growth[kMax] = {};
    /// value[i * n_growth + j]. Zero where the cell was refused (g >= r).
    double value[kMax * kMax] = {};
    bool priced[kMax * kMax] = {};

    /// Largest finite value divided by the smallest. A ratio, not a
    /// difference, so it is readable without knowing the currency scale.
    [[nodiscard]] double spread() const noexcept {
        double lo = 0.0, hi = 0.0;
        bool any = false;
        for (std::size_t k = 0; k < n_rate * n_growth; ++k) {
            if (!priced[k]) { continue; }
            if (!any) { lo = hi = value[k]; any = true; continue; }
            if (value[k] < lo) { lo = value[k]; }
            if (value[k] > hi) { hi = value[k]; }
        }
        return (any && lo > 0.0) ? hi / lo : 0.0;
    }

    /// How many cells could not be priced. NOT zero-filled silently: a cell
    /// where terminal growth meets the discount rate is not a low valuation,
    /// it is an undefined one, and averaging a zero into a range would drag
    /// the answer down while looking like arithmetic.
    [[nodiscard]] std::size_t refused() const noexcept {
        std::size_t r = 0;
        for (std::size_t k = 0; k < n_rate * n_growth; ++k) {
            if (!priced[k]) { ++r; }
        }
        return r;
    }
};

[[nodiscard]] inline std::expected<SensitivityGrid, DcfError>
sensitivity_grid(double base_cf, const GrowthProfile& g, double centre_rate,
                 double rate_step, double growth_step,
                 std::size_t half = 2) noexcept {
    if (!(base_cf > 0.0) || !(centre_rate > 0.0) || !(rate_step > 0.0)
        || !(growth_step > 0.0) || half == 0
        || 2 * half + 1 > SensitivityGrid::kMax) {
        return std::unexpected(DcfError::BadParameter);
    }
    SensitivityGrid out;
    out.n_rate = 2 * half + 1;
    out.n_growth = 2 * half + 1;
    for (std::size_t i = 0; i < out.n_rate; ++i) {
        out.rate[i] = centre_rate
                    + (static_cast<double>(i) - static_cast<double>(half))
                          * rate_step;
    }
    for (std::size_t j = 0; j < out.n_growth; ++j) {
        out.growth[j] = g.terminal_growth
                      + (static_cast<double>(j) - static_cast<double>(half))
                            * growth_step;
    }
    for (std::size_t i = 0; i < out.n_rate; ++i) {
        for (std::size_t j = 0; j < out.n_growth; ++j) {
            GrowthProfile p = g;
            p.terminal_growth = out.growth[j];
            const auto d = detail::discount(base_cf, p, out.rate[i]);
            const std::size_t k = i * out.n_growth + j;
            if (d) {
                out.value[k] = d->total;
                out.priced[k] = true;
            }
        }
    }
    return out;
}

/// REVERSE DCF: the terminal growth the market's own price implies.
///
/// This is the more useful direction and it is the one that is almost never
/// run. A forward DCF asks "what is it worth", answers with a number built on
/// two unobservables, and cannot be wrong in any way that shows up before the
/// position is closed. A reverse DCF asks "what would have to be true for
/// today's price to be right" -- and THAT is a claim about the world, which
/// can be compared against the company's actual history, its industry, and
/// arithmetic. India's nominal GDP growth bounds it from above: a perpetual
/// growth rate above the economy's means the firm eventually becomes the
/// economy.
///
/// Bisection rather than Newton: value is monotone in terminal growth over the
/// admissible range and bisection cannot leave it, where a Newton step near
/// g -> r can jump across the singularity and converge to a root on the wrong
/// side of it.
[[nodiscard]] inline std::expected<double, DcfError>
implied_terminal_growth(double base_cf, const GrowthProfile& g, double rate,
                        double target_value) noexcept {
    if (!(base_cf > 0.0) || !(rate > 0.0) || !(target_value > 0.0)) {
        return std::unexpected(DcfError::BadParameter);
    }
    // The upper bound stops strictly short of the discount rate: AT g == rate
    // the perpetuity is infinite, so any target is reachable and the answer
    // would be meaningless rather than large.
    double lo = -0.10;
    double hi = rate - 1e-6;
    GrowthProfile p = g;

    p.terminal_growth = lo;
    const auto v_lo = detail::discount(base_cf, p, rate);
    if (!v_lo) { return std::unexpected(v_lo.error()); }
    if (v_lo->total > target_value) {
        // Even at -10% perpetual decline the model is worth more than the
        // market pays. Refused rather than clamped: the honest reading is
        // that the cash-flow input or the discount rate is wrong, and
        // returning -0.10 would present a bound as a measurement.
        return std::unexpected(DcfError::BadParameter);
    }
    for (int it = 0; it < 200; ++it) {
        const double mid = 0.5 * (lo + hi);
        p.terminal_growth = mid;
        const auto v = detail::discount(base_cf, p, rate);
        if (!v) { hi = mid; continue; }
        if (v->total < target_value) { lo = mid; } else { hi = mid; }
    }
    return 0.5 * (lo + hi);
}

} // namespace altair
