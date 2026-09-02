// strategies/fundamentals.hpp -- point-in-time fundamentals, relative
// multiples and quality screens.
//
// P10-01 and P10-03.
//
// A FUNDAMENTAL DATUM HAS THREE DATES AND ONLY ONE OF THEM IS USABLE.
//
// This is the card, and it is the largest look-ahead in the whole tree because
// the gap is measured in weeks rather than bars.
//
//   PERIOD END    the quarter the number describes. 30 September.
//   FILED         when it was published. Mid-November, six to eight weeks
//                 later under SEBI's 45-day rule, and later still for the
//                 annual.
//   RESTATED      when it was changed. Sometimes years later.
//
// A database keyed on PERIOD END hands you September's earnings on 30
// September. Nobody had them: measured on an ordinary filing, the lag is 45
// days -- the SEBI limit, and the normal case rather than an outlier. Every
// screen built on it trades six weeks of hindsight on the single most
// price-moving number a company publishes, and the backtest looks
// extraordinary.
//
// So `FundamentalStore` is keyed on FILED, `as_of` refuses to return anything
// filed after the asked-for instant, and a datum with no filing date is
// rejected at insertion rather than defaulted to its period end -- which is
// precisely the default that produces the bug.
//
// AND THE RESTATEMENT IS A SECOND DATUM, NOT A CORRECTION.
//
// A vendor that overwrites the original filing with the restated figure hands
// you a number nobody had at the time, and it is worse than the period-end bug
// because it is invisible: the filing date is still right. Both versions are
// stored here, `as_of` returns whichever was current at that instant, and the
// restatement is visible as a separate row.
//
// A NEGATIVE DENOMINATOR MAKES A MULTIPLE MEANINGLESS, AND IT SORTS FIRST.
//
// P10-03. A company with negative earnings has a negative P/E. Sort the
// universe ascending on P/E to find the cheapest and every loss-making company
// in it comes out ahead of every profitable one -- the screen returns exactly
// the names it was built to avoid, and the ordering looks perfectly sensible.
// Every ratio here returns `std::expected` and refuses a non-positive
// denominator rather than producing a signed number that sorts.

#pragma once

#include <core/time/timestamp.hpp>
#include <core/types/units.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

inline constexpr std::size_t kMaxFundamentalRows = 4096;
inline constexpr std::size_t kMaxUniverse = 256;

enum class FundamentalError : std::uint8_t {
    /// No datum was filed at or before the asked-for instant.
    NotYetFiled,
    /// The row had no filing date. Rejected rather than defaulted to the
    /// period end -- that default IS the bug.
    NoFilingDate,
    /// The filing date precedes the period it describes.
    FiledBeforePeriod,
    /// The store is full.
    Full,
    /// The denominator is zero or negative, so the ratio has no meaning.
    NonPositiveDenominator,
    /// Fewer names than a cross-sectional statistic needs.
    TooFewNames,
    /// The sector was not set.
    NoSector
};

/// One reported figure, with all three of its dates.
struct Fundamental {
    std::uint32_t instrument = 0;
    std::uint16_t sector = 0;
    /// The period the numbers describe.
    Timestamp period_end{};
    /// When they were PUBLISHED. The only date a strategy may key on.
    Timestamp filed{};
    /// Which revision this is. 0 is the original filing; 1+ are restatements,
    /// each with its own filing date.
    std::uint8_t revision = 0;

    /// All in paise (rule 3), all for the period, not annualised -- the
    /// annualisation convention is a decision and belongs to the caller.
    Notional revenue{};
    Notional ebitda{};
    Notional net_income{};
    Notional equity{};
    Notional total_debt{};
    Notional cash{};
    Notional free_cash_flow{};
    Qty shares_outstanding{};

    [[nodiscard]] bool complete() const noexcept {
        return instrument != 0 && filed.ns_since_epoch() != 0
            && period_end.ns_since_epoch() != 0
            && shares_outstanding.raw() > 0;
    }
};

/// A point-in-time store: nothing is visible before it was filed.
class FundamentalStore {
public:
    [[nodiscard]] std::expected<void, FundamentalError>
    insert(const Fundamental& f) noexcept {
        if (n_ >= kMaxFundamentalRows) {
            return std::unexpected(FundamentalError::Full);
        }
        if (f.filed.ns_since_epoch() == 0) {
            return std::unexpected(FundamentalError::NoFilingDate);
        }
        if (!f.complete()) {
            return std::unexpected(FundamentalError::NoFilingDate);
        }
        // A filing cannot predate the period it reports on. This catches the
        // commonest ingest bug -- a loader that put the period end in the
        // filing column -- at the door rather than in a backtest.
        if (f.filed < f.period_end) {
            return std::unexpected(FundamentalError::FiledBeforePeriod);
        }
        rows_[n_++] = f;
        return {};
    }

    /// The figure that was current for `instrument` AT `when`.
    ///
    /// Latest FILED at or before `when`; among rows with the same period, the
    /// highest revision that had itself been filed by then. A restatement
    /// published next year does not reach back into today.
    [[nodiscard]] std::expected<Fundamental, FundamentalError>
    as_of(std::uint32_t instrument, Timestamp when) const noexcept {
        const Fundamental* best = nullptr;
        for (std::size_t i = 0; i < n_; ++i) {
            const Fundamental& r = rows_[i];
            if (r.instrument != instrument) { continue; }
            if (r.filed > when) { continue; }       // NOT YET PUBLISHED
            if (best == nullptr
                || r.period_end > best->period_end
                || (r.period_end == best->period_end
                    && r.revision > best->revision)) {
                best = &r;
            }
        }
        if (best == nullptr) {
            return std::unexpected(FundamentalError::NotYetFiled);
        }
        return *best;
    }

    /// How many days after the period end this figure was published.
    ///
    /// Exposed because the number is the whole card: a screen keyed on period
    /// end is trading this many days of hindsight.
    [[nodiscard]] static double reporting_lag_days(
        const Fundamental& f) noexcept {
        const double ns = static_cast<double>((f.filed - f.period_end).raw());
        return ns / (24.0 * 3600.0 * 1e9);
    }

    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    [[nodiscard]] const Fundamental* at(std::size_t i) const noexcept {
        return i < n_ ? &rows_[i] : nullptr;
    }

private:
    Fundamental rows_[kMaxFundamentalRows] = {};
    std::size_t n_ = 0;
};

// ---------------------------------------------------------------------------
// P10-03: multiples and quality
// ---------------------------------------------------------------------------

/// Price-to-earnings. REFUSES a non-positive denominator.
///
/// A loss-making company has a negative P/E, and a universe sorted ascending
/// on it puts every one of them ahead of every profitable company. The screen
/// then returns exactly the names it was built to avoid, and the ordering
/// looks perfectly sensible.
[[nodiscard]] inline std::expected<double, FundamentalError>
price_to_earnings(Price price, const Fundamental& f) noexcept {
    if (f.net_income.raw() <= 0 || f.shares_outstanding.raw() <= 0) {
        return std::unexpected(FundamentalError::NonPositiveDenominator);
    }
    const double eps = static_cast<double>(f.net_income.raw())
                     / static_cast<double>(f.shares_outstanding.raw());
    return static_cast<double>(price.raw()) / eps;
}

/// Enterprise value over EBITDA. Same refusal.
///
/// EV = market cap + debt - cash, which is why the store carries all three:
/// a multiple computed on market cap alone compares a levered company with an
/// unlevered one and calls the difference cheapness.
[[nodiscard]] inline std::expected<double, FundamentalError>
ev_to_ebitda(Price price, const Fundamental& f) noexcept {
    if (f.ebitda.raw() <= 0) {
        return std::unexpected(FundamentalError::NonPositiveDenominator);
    }
    const double cap = static_cast<double>(price.raw())
                     * static_cast<double>(f.shares_outstanding.raw());
    const double ev = cap + static_cast<double>(f.total_debt.raw())
                    - static_cast<double>(f.cash.raw());
    return ev / static_cast<double>(f.ebitda.raw());
}

[[nodiscard]] inline std::expected<double, FundamentalError>
price_to_book(Price price, const Fundamental& f) noexcept {
    if (f.equity.raw() <= 0) {
        return std::unexpected(FundamentalError::NonPositiveDenominator);
    }
    const double cap = static_cast<double>(price.raw())
                     * static_cast<double>(f.shares_outstanding.raw());
    return cap / static_cast<double>(f.equity.raw());
}

/// Quality: return on equity, again refusing negative equity.
///
/// A company with negative book value has an ROE whose SIGN is meaningless --
/// a loss on negative equity comes out positive. Screening on ROE without this
/// check ranks the most distressed names as the highest quality.
[[nodiscard]] inline std::expected<double, FundamentalError>
return_on_equity(const Fundamental& f) noexcept {
    if (f.equity.raw() <= 0) {
        return std::unexpected(FundamentalError::NonPositiveDenominator);
    }
    return static_cast<double>(f.net_income.raw())
         / static_cast<double>(f.equity.raw());
}

/// Net debt to EBITDA. Leverage, where a HIGH number is bad.
[[nodiscard]] inline std::expected<double, FundamentalError>
net_debt_to_ebitda(const Fundamental& f) noexcept {
    if (f.ebitda.raw() <= 0) {
        return std::unexpected(FundamentalError::NonPositiveDenominator);
    }
    return static_cast<double>(f.total_debt.raw() - f.cash.raw())
         / static_cast<double>(f.ebitda.raw());
}

/// One name's place in its SECTOR, at one instant.
struct RelativeScore {
    double value = 0.0;
    /// The sector median the value is measured against.
    double sector_median = 0.0;
    /// How many names in the sector had a usable ratio. Small denominators
    /// make a median a coin flip, and this is what says so.
    std::size_t sector_n = 0;
    /// Ratio to the median. Below 1.0 is cheap on a valuation multiple and
    /// expensive on a quality one -- which is why the caller supplies the
    /// direction rather than this file assuming it.
    double relative = 0.0;
};

/// Rank one name against its sector at one instant.
///
/// The median is computed CROSS-SECTIONALLY over the names supplied, which
/// must all be from the same sector and all from the same instant. Comparing a
/// name against a median built from a different date is the same error P8-07
/// makes with a time-series z-score, one layer up.
[[nodiscard]] inline std::expected<RelativeScore, FundamentalError>
relative_to_sector(double value, double* peer_values,
                   std::size_t n) noexcept {
    // Four is the floor, not two: a median of three is one observation and a
    // median of two is a mean.
    if (n < 4) { return std::unexpected(FundamentalError::TooFewNames); }
    for (std::size_t i = 1; i < n; ++i) {
        const double v = peer_values[i];
        std::size_t j = i;
        while (j > 0 && peer_values[j - 1] > v) {
            peer_values[j] = peer_values[j - 1];
            --j;
        }
        peer_values[j] = v;
    }
    RelativeScore s{};
    s.value = value;
    s.sector_n = n;
    s.sector_median = (n % 2 == 1)
        ? peer_values[n / 2]
        : 0.5 * (peer_values[n / 2 - 1] + peer_values[n / 2]);
    if (!(std::fabs(s.sector_median) > 0.0)) {
        return std::unexpected(FundamentalError::NonPositiveDenominator);
    }
    s.relative = value / s.sector_median;
    return s;
}

} // namespace altair
