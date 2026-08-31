// instruments/nse_mktlots.hpp — fo_mktlots.csv → the lot-size table.
//
// P1-02b. Implemented 2026-09-01 from the real file; the skeleton this
// replaces returned SchemaNotConfigured.
//
// SCHEMA, read from the real file:
//
//   UNDERLYING                          ,SYMBOL    ,SEP-26     ,OCT-26     ,...
//   NIFTY 50                            ,NIFTY     ,65         ,65         ,...
//   NIFTY BANK                          ,BANKNIFTY ,30         ,30         ,
//
// Four properties, all confirmed and all hazards:
//
//   * WIDE. One column per expiry month, so the schema changes every month by
//     construction and a parser that hardcodes column positions is correct for
//     exactly one month. Months are resolved from the HEADER.
//   * Padded to fixed width on every field, header included.
//   * UNDERLYING and SYMBOL are DIFFERENT: "NIFTY 50" vs "NIFTY". SYMBOL is
//     the one that matches Kite's `name` and UDiFF's TckrSymb, so SYMBOL is
//     the join key. Checked against all three real files.
//   * A blank cell means the underlying does not trade that month. It is not
//     a lot size of zero.
//
// Month format is MMM-YY, upper case: SEP-26.

#pragma once

#include <instruments/contract_spec.hpp>
#include <instruments/kite_dump.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>

namespace altair {

inline constexpr std::size_t kMaxLotUnderlyings = 512;
inline constexpr std::size_t kMaxLotMonths = 24;

enum class MktLotsError : std::uint8_t {
    EmptyInput, BadHeader, BadMonth, BadNumber, BadSymbol, TooMany, NotFound
};

/// A (year, month) expiry bucket.
///
/// NOT a Timestamp: this file names a MONTH, not an instant, and pretending
/// otherwise would invent a day-of-month the file does not contain.
struct ExpiryMonth {
    std::int32_t year = 0;
    std::uint8_t month = 0;
    friend constexpr auto operator<=>(ExpiryMonth, ExpiryMonth) noexcept = default;
    friend constexpr bool operator==(ExpiryMonth, ExpiryMonth) noexcept = default;
};

namespace detail {

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

/// "SEP" -> 9. Upper case only: the file is upper case, and accepting mixed
/// case would hide a format change rather than reveal it.
[[nodiscard]] inline unsigned month_from_abbrev(const char* p) noexcept {
    static constexpr char kM[12][4] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                       "JUL","AUG","SEP","OCT","NOV","DEC"};
    for (unsigned i = 0; i < 12; ++i) {
        if (std::memcmp(p, kM[i], 3) == 0) {
            return i + 1;
        }
    }
    return 0;
}

/// "SEP-26" -> {2026, 9}. Returns month 0 on failure.
[[nodiscard]] inline ExpiryMonth parse_month_header(KiteField f) noexcept {
    // Trim padding first: the header is fixed-width.
    while (f.n > 0 && f.p[0] == ' ') { ++f.p; --f.n; }
    while (f.n > 0 && f.p[f.n - 1] == ' ') { --f.n; }
    if (f.n != 6 || f.p[3] != '-') {
        return ExpiryMonth{};
    }
    const unsigned m = month_from_abbrev(f.p);
    if (m == 0) {
        return ExpiryMonth{};
    }
    if (f.p[4] < '0' || f.p[4] > '9' || f.p[5] < '0' || f.p[5] > '9') {
        return ExpiryMonth{};
    }
    const int yy = (f.p[4] - '0') * 10 + (f.p[5] - '0');
    // Two-digit years are 2000..2099. This file will outlive the assumption;
    // it is written down rather than left implicit.
    return ExpiryMonth{2000 + yy, static_cast<std::uint8_t>(m)};
}

[[nodiscard]] inline KiteField mktlots_trim(KiteField f) noexcept {
    while (f.n > 0 && (f.p[0] == ' ' || f.p[0] == '\t')) { ++f.p; --f.n; }
    while (f.n > 0 && (f.p[f.n - 1] == ' ' || f.p[f.n - 1] == '\t')) { --f.n; }
    return f;
}

} // namespace detail

/// The month an expiry falls in, read in IST. UNIT: none.
///
/// An expiry is an IST midnight (P0-04), which is 18:30 UTC the PREVIOUS day.
/// Read the month in UTC and every 1st-of-month expiry moves into the previous
/// month and gets the wrong lot size.
[[nodiscard]] inline ExpiryMonth ist_expiry_month(Timestamp expiry) noexcept {
    const std::int64_t day = ist_days_between(Timestamp::epoch(), expiry);
    const detail::CivilDate c = detail::civil_from_days(day);
    return ExpiryMonth{static_cast<std::int32_t>(c.year),
                       static_cast<std::uint8_t>(c.month)};
}

/// Lot size by (underlying, expiry month). Fixed storage, no allocation.
class MktLotsTable {
public:
    MktLotsTable() noexcept = default;
    MktLotsTable(const MktLotsTable&) = delete;
    MktLotsTable& operator=(const MktLotsTable&) = delete;

    /// UNIT: units. `NotFound` when this underlying does not trade that month
    /// — a real answer, not an error to paper over, and never `LotSize{0}`.
    [[nodiscard]] std::expected<LotSize, MktLotsError>
    lot_for(const char* underlying, ExpiryMonth m) const noexcept {
        if (underlying == nullptr || underlying[0] == '\0') {
            return std::unexpected(MktLotsError::BadSymbol);
        }
        std::size_t col = kMaxLotMonths;
        for (std::size_t k = 0; k < month_count_; ++k) {
            if (months_[k] == m) {
                col = k;
                break;
            }
        }
        if (col == kMaxLotMonths) {
            return std::unexpected(MktLotsError::NotFound);
        }
        for (std::size_t i = 0; i < count_; ++i) {
            if (std::strcmp(symbol_[i], underlying) == 0) {
                const std::int64_t v = lot_[i][col];
                if (v <= 0) {
                    // A blank cell: this underlying does not trade that month.
                    return std::unexpected(MktLotsError::NotFound);
                }
                return LotSize{v};
            }
        }
        return std::unexpected(MktLotsError::NotFound);
    }

    [[nodiscard]] std::size_t underlyings() const noexcept { return count_; }
    [[nodiscard]] std::size_t months() const noexcept { return month_count_; }
    [[nodiscard]] ExpiryMonth month_at(std::size_t i) const noexcept {
        return i < month_count_ ? months_[i] : ExpiryMonth{};
    }
    void clear() noexcept { count_ = 0; month_count_ = 0; }

    friend std::expected<void, MktLotsError>
    load_mktlots(const char* csv, std::size_t len, MktLotsTable& out) noexcept;

private:
    char         symbol_[kMaxLotUnderlyings][kMaxUnderlyingLen + 1]{};
    std::int64_t lot_[kMaxLotUnderlyings][kMaxLotMonths]{};
    ExpiryMonth  months_[kMaxLotMonths]{};
    std::size_t  count_ = 0;
    std::size_t  month_count_ = 0;
};

/// Parse the whole file. The table is CLEARED first: this is a snapshot of one
/// day's lot table, not something to merge into.
[[nodiscard]] inline std::expected<void, MktLotsError>
load_mktlots(const char* csv, std::size_t len, MktLotsTable& out) noexcept {
    if (csv == nullptr || len == 0) {
        return std::unexpected(MktLotsError::EmptyInput);
    }
    out.clear();

    auto line_end = [&](std::size_t from) noexcept {
        std::size_t e = from;
        while (e < len && csv[e] != '\n') { ++e; }
        return e;
    };
    auto trim_cr = [&](std::size_t from, std::size_t e) noexcept {
        return (e > from && csv[e - 1] == '\r') ? e - 1 : e;
    };

    // ── header: the months live here, and only here ──────────────────────
    std::size_t pos = 0;
    const std::size_t he = line_end(pos);
    detail::KiteField hf[64];
    const std::size_t hn = detail::kite_split(csv + pos, trim_cr(pos, he) - pos,
                                              hf, 64);
    if (hn < 3) {
        return std::unexpected(MktLotsError::BadHeader);
    }
    // Columns 0 and 1 are UNDERLYING and SYMBOL; the rest are months.
    if (!detail::kite_field_is(detail::mktlots_trim(hf[0]), "UNDERLYING")
        || !detail::kite_field_is(detail::mktlots_trim(hf[1]), "SYMBOL")) {
        return std::unexpected(MktLotsError::BadHeader);
    }
    for (std::size_t i = 2; i < hn; ++i) {
        const detail::KiteField t = detail::mktlots_trim(hf[i]);
        if (t.n == 0) {
            continue;                       // trailing empty column
        }
        const ExpiryMonth m = detail::parse_month_header(hf[i]);
        if (m.month == 0) {
            return std::unexpected(MktLotsError::BadMonth);
        }
        if (out.month_count_ >= kMaxLotMonths) {
            return std::unexpected(MktLotsError::TooMany);
        }
        out.months_[out.month_count_++] = m;
    }
    if (out.month_count_ == 0) {
        return std::unexpected(MktLotsError::BadHeader);
    }

    // ── rows ─────────────────────────────────────────────────────────────
    pos = he < len ? he + 1 : len;
    while (pos < len) {
        const std::size_t e = line_end(pos);
        const std::size_t stop = trim_cr(pos, e);
        if (stop > pos) {
            detail::KiteField f[64];
            const std::size_t n = detail::kite_split(csv + pos, stop - pos, f, 64);
            // A short row is a footer or a note, not data. NSE files carry
            // them, and parsing one would invent a phantom underlying.
            if (n >= 3) {
                const detail::KiteField sym = detail::mktlots_trim(f[1]);
                if (sym.n > 0 && sym.n <= kMaxUnderlyingLen) {
                    if (out.count_ >= kMaxLotUnderlyings) {
                        return std::unexpected(MktLotsError::TooMany);
                    }
                    const std::size_t r = out.count_;
                    std::memcpy(out.symbol_[r], sym.p, sym.n);
                    out.symbol_[r][sym.n] = '\0';
                    for (std::size_t k = 0; k < out.month_count_; ++k) {
                        const std::size_t col = k + 2;
                        std::int64_t v = 0;
                        if (col < n) {
                            const detail::KiteField c = detail::mktlots_trim(f[col]);
                            std::uint64_t u = 0;
                            if (c.n > 0
                                && detail::kite_parse_u64(c.p, c.n,
                                                          0x7FFF'FFFFull, u)) {
                                v = static_cast<std::int64_t>(u);
                            }
                            // A blank cell -- or one that will not parse --
                            // stays ZERO, which lot_for reports as NotFound.
                            // "Does not trade that month" is a real answer.
                        }
                        out.lot_[r][k] = v;
                    }
                    ++out.count_;
                }
            }
        }
        pos = e < len ? e + 1 : len;
    }

    if (out.count_ == 0) {
        return std::unexpected(MktLotsError::EmptyInput);
    }
    return {};
}

} // namespace altair
