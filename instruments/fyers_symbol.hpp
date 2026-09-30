// instruments/fyers_symbol.hpp -- read a FYERS ticker into its parts.
//
// GETS screens (Greek market watch, portfolio Greeks, expense report) start
// from FYERS account rows, and a FYERS row names its contract by ticker only:
// "NSE:NIFTY26SEP25000CE", "NSE:BANKNIFTY26SEPFUT", "NSE:SBIN-EQ",
// "NSE:NIFTY50-INDEX". This header reads that ticker. It opens no file and
// knows no lot size; the instrument master stays the authority for those.
//
// THE FORMAT, FROM FYERS' SYMBOL FORMAT PAGE
//
//   equity          EX:UNDERLYING-SERIES          NSE:SBIN-EQ
//   index           EX:NAME-INDEX                 NSE:NIFTY50-INDEX
//   future          EX:UNDERLYING YY MMM FUT      NSE:NIFTY26SEPFUT
//   monthly option  EX:UNDERLYING YY MMM STRIKE CE|PE
//   weekly option   EX:UNDERLYING YY M DD STRIKE CE|PE
//                   (M is 1..9 for Jan..Sep, then O, N, D)
//
// A MONTHLY TICKER DOES NOT CARRY ITS EXPIRY DAY. The exchange sets it by
// rule and holiday calendar, and those rules have changed. `day` is therefore
// 0 for a monthly contract and a caller that needs the date must take it from
// the instrument master. This reader never guesses a last-Tuesday.
//
// RULE 11. Underlyings may end in digits (NIFTYNXT50, 360ONE), and a weekly
// date runs straight into the strike, so "NIFTY2623125000CE" (31 Feb) has a
// false reading as underlying "NIFTY2", year 2062. Two things close that:
// the caller passes a reference year and a derivative's year must fall in
// [reference - 1, reference + 6] (NSE lists index options about five years
// out); and every split is tried, one valid reading is accepted, none is
// Unrecognised and more than one is Ambiguous. A ticker is never half-parsed.
#pragma once

#include <core/time/timestamp.hpp>
#include <core/types/units.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace altair::instruments {

enum class FyersSymbolKind : std::uint8_t { Equity, Index, Future, Option };
enum class FyersRight : std::uint8_t { None, Call, Put };

enum class FyersSymbolError : std::uint8_t {
    Empty,
    TooLong,       ///< longer than kFyersSymbolMax
    NoExchange,    ///< no "EX:" prefix of 1..7 capital letters
    Unrecognised,  ///< no reading of the ticker fits the format
    Ambiguous      ///< more than one reading fits; refused, never picked
};

inline constexpr std::size_t kFyersSymbolMax = 64;
inline constexpr std::size_t kFyersUnderlyingMax = 40;

struct FyersSymbol {
    std::array<char, 8> exchange{};                        ///< "NSE", "BSE", "MCX"
    std::array<char, kFyersUnderlyingMax + 1> underlying{};///< "NIFTY", "SBIN", "NIFTY50"
    std::array<char, 8> series{};                          ///< "EQ" for equity, else empty
    FyersSymbolKind kind{FyersSymbolKind::Equity};
    FyersRight right{FyersRight::None};
    Price strike{};         ///< paise; options only
    int year{};             ///< derivatives only, e.g. 2026
    unsigned month{};       ///< 1..12, derivatives only
    unsigned day{};         ///< 1..31 for a weekly option; 0 when the ticker has none
};

namespace fyers_symbol_detail {

[[nodiscard]] constexpr bool upper(char c) noexcept { return c >= 'A' && c <= 'Z'; }
[[nodiscard]] constexpr bool digit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] constexpr unsigned month_of(std::string_view m) noexcept {
    constexpr std::array<std::string_view, 12> names{
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    for (unsigned i = 0; i < names.size(); ++i)
        if (names[i] == m) return i + 1;
    return 0;
}

[[nodiscard]] constexpr unsigned weekly_month_of(char c) noexcept {
    if (c >= '1' && c <= '9') return static_cast<unsigned>(c - '0');
    if (c == 'O') return 10;
    if (c == 'N') return 11;
    if (c == 'D') return 12;
    return 0;
}

[[nodiscard]] constexpr unsigned days_in(int year, unsigned month) noexcept {
    constexpr std::array<unsigned, 12> days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29u : days[month - 1];
}

/// Strike text to paise: digits with at most one '.', at most two decimals,
/// no leading zero before the point, and greater than zero.
[[nodiscard]] constexpr std::optional<Price> strike_paise(std::string_view s) noexcept {
    if (s.empty() || s.size() > 12 || s.front() == '.') return std::nullopt;
    if (s.front() == '0' && (s.size() == 1 || s[1] != '.')) return std::nullopt;
    std::int64_t whole = 0;
    std::int64_t frac = 0;
    unsigned frac_digits = 0;
    bool dot = false;
    for (const char c : s) {
        if (c == '.') {
            if (dot) return std::nullopt;
            dot = true;
            continue;
        }
        if (!digit(c)) return std::nullopt;
        if (dot) {
            if (frac_digits == 2) return std::nullopt;   // sub-paisa strike
            frac = frac * 10 + (c - '0');
            ++frac_digits;
        } else {
            whole = whole * 10 + (c - '0');
        }
    }
    if (dot && frac_digits == 0) return std::nullopt;
    if (frac_digits == 1) frac *= 10;
    const std::int64_t paise = whole * 100 + frac;
    if (paise <= 0) return std::nullopt;
    return Price{paise};
}

[[nodiscard]] constexpr bool copy_text(std::string_view text, char* out,
                                       std::size_t capacity) noexcept {
    if (text.empty() || text.size() >= capacity) return false;
    for (std::size_t i = 0; i < text.size(); ++i) out[i] = text[i];
    out[text.size()] = '\0';
    return true;
}

[[nodiscard]] constexpr bool underlying_ok(std::string_view u) noexcept {
    if (u.empty() || u.size() > kFyersUnderlyingMax) return false;
    for (const char c : u)
        if (!(upper(c) || digit(c) || c == '&' || c == '-' || c == '_')) return false;
    return true;
}

/// One reading of a derivative stem: underlying, expiry and (options) strike.
struct Reading {
    std::string_view underlying;
    int year{};
    unsigned month{};
    unsigned day{};
    Price strike{};
};

/// Try every split of `stem` into UNDERLYING + YY + (MMM | M DD) [+ STRIKE].
/// Returns the count of valid readings; `out` holds the first.
[[nodiscard]] constexpr int read_stem(std::string_view stem, bool option,
                                      int reference_year, Reading& out) noexcept {
    int found = 0;
    for (std::size_t i = 1; i + 2 < stem.size(); ++i) {
        const std::string_view under = stem.substr(0, i);
        if (!underlying_ok(under)) continue;
        if (!digit(stem[i]) || !digit(stem[i + 1])) continue;
        const int year = 2000 + (stem[i] - '0') * 10 + (stem[i + 1] - '0');
        if (year < reference_year - 1 || year > reference_year + 6) continue;
        const std::string_view rest = stem.substr(i + 2);

        // Monthly: MMM, then the strike (options) or nothing (futures).
        if (rest.size() >= 3) {
            const unsigned m = month_of(rest.substr(0, 3));
            if (m != 0) {
                const std::string_view tail = rest.substr(3);
                if (!option && tail.empty()) {
                    if (found++ == 0) out = Reading{under, year, m, 0, Price{}};
                } else if (option) {
                    if (const auto k = strike_paise(tail)) {
                        if (found++ == 0) out = Reading{under, year, m, 0, *k};
                    }
                }
            }
        }
        // Weekly (options only): M, DD, then the strike.
        if (option && rest.size() >= 4) {
            const unsigned m = weekly_month_of(rest[0]);
            if (m != 0 && digit(rest[1]) && digit(rest[2])) {
                const unsigned d = static_cast<unsigned>((rest[1] - '0') * 10 + (rest[2] - '0'));
                if (d >= 1 && d <= days_in(year, m)) {
                    if (const auto k = strike_paise(rest.substr(3))) {
                        if (found++ == 0) out = Reading{under, year, m, d, *k};
                    }
                }
            }
        }
    }
    return found;
}

} // namespace fyers_symbol_detail

/// Parse a FYERS ticker. See the header comment for the accepted shapes.
/// `reference_year` is the current calendar year (e.g. 2026); it bounds the
/// expiry year of a derivative and is ignored for equities and indices.
[[nodiscard]] constexpr std::expected<FyersSymbol, FyersSymbolError>
parse_fyers_symbol(std::string_view text, int reference_year) noexcept {
    namespace d = fyers_symbol_detail;
    if (text.empty()) return std::unexpected(FyersSymbolError::Empty);
    if (text.size() > kFyersSymbolMax) return std::unexpected(FyersSymbolError::TooLong);
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon > 7)
        return std::unexpected(FyersSymbolError::NoExchange);
    for (std::size_t i = 0; i < colon; ++i)
        if (!d::upper(text[i])) return std::unexpected(FyersSymbolError::NoExchange);

    FyersSymbol out{};
    (void)d::copy_text(text.substr(0, colon), out.exchange.data(), out.exchange.size());
    const std::string_view body = text.substr(colon + 1);
    if (body.empty()) return std::unexpected(FyersSymbolError::Unrecognised);

    constexpr std::string_view kIndex = "-INDEX";
    if (body.size() > kIndex.size() && body.ends_with(kIndex)) {
        const std::string_view name = body.substr(0, body.size() - kIndex.size());
        if (!d::copy_text(name, out.underlying.data(), out.underlying.size()))
            return std::unexpected(FyersSymbolError::Unrecognised);
        out.kind = FyersSymbolKind::Index;
        return out;
    }

    const bool future = body.size() > 3 && body.ends_with("FUT");
    const bool option = body.size() > 2 && (body.ends_with("CE") || body.ends_with("PE"));
    if (future || option) {
        const std::string_view stem = body.substr(0, body.size() - (future ? 3 : 2));
        d::Reading r{};
        const int n = d::read_stem(stem, option, reference_year, r);
        if (n > 1) return std::unexpected(FyersSymbolError::Ambiguous);
        if (n == 1) {
            (void)d::copy_text(r.underlying, out.underlying.data(), out.underlying.size());
            out.kind = future ? FyersSymbolKind::Future : FyersSymbolKind::Option;
            out.right = !option ? FyersRight::None
                      : body.ends_with("CE") ? FyersRight::Call : FyersRight::Put;
            out.strike = r.strike;
            out.year = r.year;
            out.month = r.month;
            out.day = r.day;
            return out;
        }
        // No derivative reading: an equity such as "XYZFUT-EQ" still ends in
        // a series, and is tried below.
    }

    const std::size_t dash = body.rfind('-');
    if (dash != std::string_view::npos && dash > 0 && dash + 1 < body.size()) {
        const std::string_view series = body.substr(dash + 1);
        for (const char c : series)
            if (!(d::upper(c) || d::digit(c)))
                return std::unexpected(FyersSymbolError::Unrecognised);
        if (!d::copy_text(body.substr(0, dash), out.underlying.data(), out.underlying.size())
            || !d::copy_text(series, out.series.data(), out.series.size()))
            return std::unexpected(FyersSymbolError::Unrecognised);
        out.kind = FyersSymbolKind::Equity;
        return out;
    }
    return std::unexpected(FyersSymbolError::Unrecognised);
}

/// The instant a contract with a known expiry DATE stops trading: 15:30 IST.
/// Returns nullopt when the date is not in the ticker (monthly contracts).
[[nodiscard]] constexpr std::optional<Timestamp>
fyers_ticker_expiry(const FyersSymbol& s) noexcept {
    if (s.day == 0 || s.month < 1 || s.month > 12) return std::nullopt;
    // days_from_civil (Howard Hinnant), proleptic Gregorian.
    const int y = s.year - (s.month <= 2 ? 1 : 0);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned mp = s.month > 2 ? s.month - 3 : s.month + 9;
    const unsigned doy = (153 * mp + 2) / 5 + s.day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + doe - 719468;
    // 15:30 IST is 10:00 UTC on the same date.
    return Timestamp{(days * 86'400LL + 10 * 3'600LL) * 1'000'000'000LL};
}

/// The spot ticker a derivative's Greeks are taken against, or empty when the
/// underlying has no cash or index quote this reader can name (MCX, currency).
/// Index names follow feed/fyers_index_map.hpp.
[[nodiscard]] inline std::string fyers_spot_symbol(const FyersSymbol& s) {
    const std::string_view ex{s.exchange.data()};
    const std::string_view u{s.underlying.data()};
    if (s.kind == FyersSymbolKind::Equity || s.kind == FyersSymbolKind::Index) {
        std::string out{ex};
        out += ':';
        out += u;
        out += s.kind == FyersSymbolKind::Index ? "-INDEX" : "-";
        if (s.kind == FyersSymbolKind::Equity) out += s.series.data();
        return out;
    }
    if (ex == "NSE") {
        if (u == "NIFTY") return "NSE:NIFTY50-INDEX";
        if (u == "BANKNIFTY") return "NSE:NIFTYBANK-INDEX";
        if (u == "FINNIFTY") return "NSE:FINNIFTY-INDEX";
        if (u == "MIDCPNIFTY") return "NSE:MIDCPNIFTY-INDEX";
        if (u == "NIFTYNXT50") return "NSE:NIFTYNXT50-INDEX";
        return "NSE:" + std::string{u} + "-EQ";
    }
    if (ex == "BSE") {
        if (u == "SENSEX") return "BSE:SENSEX-INDEX";
        if (u == "BANKEX") return "BSE:BANKEX-INDEX";
    }
    return {};
}

} // namespace altair::instruments
