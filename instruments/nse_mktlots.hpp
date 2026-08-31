// instruments/nse_mktlots.hpp — fo_mktlots.csv → the lot-size table.
//
// P1-02b. ***SKELETON. THE PARSER BODY IS NOT IMPLEMENTED.***
//
// `load_mktlots` returns `SchemaNotConfigured` until §2 of
// prompts/P1-02b_nse_mktlots.md is filled in from a sample file.
//
// This file is WIDE — one row per underlying, one COLUMN PER EXPIRY MONTH — so
// its schema changes every month by construction. A parser that hardcodes
// column positions is correct for exactly one month. It also carries rule 1's
// headline number: lot size is the bug that silently scaled the predecessor's
// every P&L figure.
//
// `ExpiryMonth` and `ist_expiry_month` ARE implemented: they are the card's own
// logic, not the file's schema, and the month-boundary hazard they exist for is
// real and testable today.

#pragma once

#include <instruments/contract_spec.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

inline constexpr std::size_t kMaxLotUnderlyings = 512;
inline constexpr std::size_t kMaxLotMonths = 12;

enum class MktLotsError : std::uint8_t {
    EmptyInput, BadHeader, BadMonth, BadNumber, BadSymbol, TooMany, NotFound,
    /// §2 of P1-02b has not been filled in from a sample file.
    SchemaNotConfigured
};

/// A (year, month) expiry bucket.
///
/// NOT a Timestamp: this file names a MONTH, not an instant, and pretending
/// otherwise would invent a day-of-month the file does not contain.
struct ExpiryMonth {
    std::int32_t year = 0;     // 2026
    std::uint8_t month = 0;    // 1..12
    friend constexpr auto operator<=>(ExpiryMonth, ExpiryMonth) noexcept = default;
    friend constexpr bool operator==(ExpiryMonth, ExpiryMonth) noexcept = default;
};

namespace detail {

/// Civil year/month/day from a day number since 1970-01-01. Hinnant's
/// civil_from_days, needed because reading the month off an expiry is this
/// card's own logic rather than the CSV's schema.
struct CivilDate {
    std::int64_t year;
    unsigned month;
    unsigned day;
};

[[nodiscard]] constexpr CivilDate civil_from_days(std::int64_t z) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned long long>(z - era * 146097);
    const unsigned long long yoe =
        (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned long long mp = (5 * doy + 2) / 153;
    const unsigned d = static_cast<unsigned>(doy - (153 * mp + 2) / 5 + 1);
    const unsigned m = static_cast<unsigned>(mp < 10 ? mp + 3 : mp - 9);
    return CivilDate{y + (m <= 2), m, d};
}

} // namespace detail

/// The month an expiry falls in, read in IST. UNIT: none.
///
/// An expiry is an IST midnight (P0-04), which is 18:30 UTC the PREVIOUS day.
/// Read the month in UTC and every 1st-of-month expiry moves into the previous
/// month and gets the wrong lot size. Built on P0-02's `ist_days_between`
/// rather than re-derived.
[[nodiscard]] inline ExpiryMonth ist_expiry_month(Timestamp expiry) noexcept {
    const std::int64_t day = ist_days_between(Timestamp::epoch(), expiry);
    const detail::CivilDate c = detail::civil_from_days(day);
    return ExpiryMonth{static_cast<std::int32_t>(c.year),
                       static_cast<std::uint8_t>(c.month)};
}

/// Lot size by (underlying, expiry month). Fixed storage, no allocation.
/// ~64 KB — safe as a member, not a stack local.
class MktLotsTable {
public:
    MktLotsTable() noexcept = default;
    MktLotsTable(const MktLotsTable&) = delete;
    MktLotsTable& operator=(const MktLotsTable&) = delete;

    /// UNIT: units. `NotFound` when this underlying does not trade that month —
    /// a real answer, not an error to paper over, and never `LotSize{0}`.
    [[nodiscard]] std::expected<LotSize, MktLotsError>
    lot_for(const char* underlying, ExpiryMonth m) const noexcept {
        (void)underlying;
        (void)m;
        return std::unexpected(MktLotsError::SchemaNotConfigured);
    }

    [[nodiscard]] std::size_t underlyings() const noexcept { return count_; }
    [[nodiscard]] std::size_t months() const noexcept { return month_count_; }
    void clear() noexcept { count_ = 0; month_count_ = 0; }

private:
    std::size_t count_ = 0;
    std::size_t month_count_ = 0;
};

[[nodiscard]] inline std::expected<void, MktLotsError>
load_mktlots(const char* csv, std::size_t len, MktLotsTable& out) noexcept {
    (void)csv;
    (void)len;
    (void)out;
    return std::unexpected(MktLotsError::SchemaNotConfigured);
}

} // namespace altair
