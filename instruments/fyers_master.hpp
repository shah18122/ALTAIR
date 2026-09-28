// instruments/fyers_master.hpp -- parse the FYERS public symbol master.
//
// P3-01 (parser half). instruments/fyers_identity.hpp already maps a FYERS
// token to canonical identity; it takes structured records and had no reader.
// This is the reader, and it is written against the ACTUAL file, not a guess.
//
// THE SCHEMA IS VERIFIED, NOT ASSUMED.
//
// The 21-column CSV at https://public.fyers.in/sym_details/<SEG>.csv carries no
// header, and the KB article does not define the columns. So each column below
// was pinned by cross-referencing the CSV against the self-describing JSON
// master (https://public.fyers.in/sym_details/<SEG>_sym_master.json) for the
// SAME fyToken, across a cash row, an option row and a futures row -- the three
// shapes whose values differ enough to disambiguate every field that matters.
// Live data, 2026-09-24:
//
//   col  field (JSON name)     cash            option           future
//   0    fyToken               101000000016921 101126092935000  101126092968390
//   1    symbolDetails         20 MICRONS LTD   BANKNIFTY..CE    BANKNIFTY..FUT
//   2    exInstType            0               14               11
//   3    minLotSize            1               30               30
//   4    tickSize (rupees)     0.01            0.05             0.2
//   5    isin                  INE144J01027    (empty)          (empty)
//   6    tradingSession        0915-1530|...   0915-1540|...    0915-1540|...
//   7    lastUpdate            2026-09-24      2026-09-24       2026-09-24
//   8    expiryDate            (empty)         1790676600       1790676600  (epoch s)
//   9    symTicker             NSE:20MICRONS-EQ NSE:BANKNIFTY..CE NSE:BANKNIFTY..FUT
//   10   exchange              10              10               10          (NSE)
//   11   segment               10 (CM)         11 (FO)          11 (FO)
//   12   exToken               16921           35000            68390
//   13   exSymbol / underSym   20MICRONS       BANKNIFTY        BANKNIFTY
//   14   underlying exToken    16921           26009            26009
//   15   strikePrice (rupees)  -1.0            72600.0          -1.0
//   16   optType               XX              CE               XX
//   17   underFyTok            101000000016921 101000000026009  101000000026009
//   18   originalExpDate       None            None             None
//   19   is_mtf_tradable       1               0                0
//   20   faceValue / mtf       2.0             0.0              0.0
//
// SEGMENT is derived, and the derivation is the reason this is not a guess:
// FYERS segment 11 is "F&O", both futures AND options. optType splits them --
// CE/PE is an option, XX at segment 11 is a future, segment 10 is cash. That
// is verified against all three rows above.
//
// fyToken IS 15 DIGITS. 101126092968390 needs 47 bits, so it is uint64. The
// exchange token (exToken) is small and stays uint32 to match ContractSpec.
// NOTE (carried debt): instruments/fyers_identity.hpp stores fy_token as
// uint32, which CANNOT hold a real fyToken. This reader emits the correct
// uint64; wiring it into FyersIdentityMap needs that field widened first.
//
// MONEY IS PAISE (rule 3). tickSize and strikePrice are quoted in RUPEES in the
// file; they are parsed straight to integer paise here, never through a double.
//
// RULE 11: a row that is not exactly 21 columns, or carries a field this reader
// does not understand, is REFUSED with a named error. It is never partially
// accepted and never guessed.

#pragma once

#include "cross_venue.hpp"
#include "fyers_identity.hpp"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string_view>

namespace altair::instruments {

inline constexpr std::size_t kFyersMasterColumns = 21;
inline constexpr std::size_t kIsinLen = 12;          // ISINs are 12 chars
inline constexpr std::size_t kFyersTickerLen = 47;   // "NSE:<sym>" headroom

// FYERS integer codes for the columns this reader classifies.
inline constexpr int kFyersExchangeNse = 10;
inline constexpr int kFyersSegmentCash = 10;
inline constexpr int kFyersSegmentFno  = 11;

enum class FyersMasterError : std::uint8_t {
    ShortRow,       ///< not exactly kFyersMasterColumns fields
    BadFyToken,     ///< col0 not a base-10 uint64
    BadExToken,     ///< col12 not a uint32
    BadNumber,      ///< a numeric field malformed, or finer than one paisa
    BadExchange,    ///< col10 is an exchange this reader does not map
    BadSegment,     ///< col11 is a segment this reader does not map
    BadOption,      ///< col16 is not XX / CE / PE
    SymbolTooLong,  ///< underlying or ticker exceeds its buffer
    BadIsin         ///< isin present but not kIsinLen chars
};

/// One parsed row of the FYERS symbol master. Prices are integer paise; time is
/// ns since the Unix epoch; tokens are as documented above.
struct FyersMasterRow {
    std::uint64_t fy_token{};     ///< col0, the FYERS canonical id (64-bit)
    std::uint32_t ex_token{};     ///< col12, the exchange token
    Exchange   exchange{Exchange::NSE};
    Segment    segment{Segment::Cash};
    OptionType opt_type{OptionType::None};
    LotSize    lot_size{};        ///< col3, units per contract
    Price      tick_size{};       ///< col4, paise
    Price      strike{};          ///< col15, paise; 0 when not an option
    Timestamp  expiry{};          ///< col8, ns; Timestamp::epoch() for cash
    char isin[kIsinLen + 1]{};    ///< col5, empty for most derivatives
    char underlying[kMaxUnderlyingLen + 1]{};  ///< col13
    char symbol[kMaxSymbolLen + 1]{};          ///< col9 without "NSE:"
    char ticker[kFyersTickerLen + 1]{};        ///< col9, the API symbol string
};

namespace detail {

/// Signed decimal RUPEES -> integer PAISE, exactly, no double. Refuses more
/// than two fractional digits (finer than a paisa) rather than truncating.
[[nodiscard]] inline std::expected<std::int64_t, FyersMasterError>
rupees_to_paise(std::string_view s) noexcept {
    if (s.empty()) { return std::unexpected(FyersMasterError::BadNumber); }
    bool neg = false;
    std::size_t i = 0;
    if (s[0] == '-') { neg = true; i = 1; }
    else if (s[0] == '+') { i = 1; }
    std::int64_t whole = 0;
    bool any = false;
    for (; i < s.size() && s[i] != '.'; ++i) {
        if (s[i] < '0' || s[i] > '9') { return std::unexpected(FyersMasterError::BadNumber); }
        whole = whole * 10 + (s[i] - '0');
        any = true;
    }
    std::int64_t frac = 0;
    int frac_digits = 0;
    if (i < s.size() && s[i] == '.') {
        for (++i; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') { return std::unexpected(FyersMasterError::BadNumber); }
            if (frac_digits < 2) { frac = frac * 10 + (s[i] - '0'); }
            else if (s[i] != '0') { return std::unexpected(FyersMasterError::BadNumber); }
            ++frac_digits;
            any = true;
        }
    }
    if (!any) { return std::unexpected(FyersMasterError::BadNumber); }
    if (frac_digits == 1) { frac *= 10; }        // "0.2" -> 20 paise
    const std::int64_t paise = whole * 100 + frac;
    return neg ? -paise : paise;
}

[[nodiscard]] inline bool copy_field(char* dst, std::size_t cap,
                                     std::string_view v) noexcept {
    if (v.size() > cap) { return false; }
    std::memcpy(dst, v.data(), v.size());
    dst[v.size()] = '\0';
    return true;
}

} // namespace detail

/// Parse one comma-separated line of the FYERS symbol master. UNIT: none.
/// PRECONDITION: `line` has no trailing newline and no embedded commas inside a
/// field (the master does not quote fields; a field with a comma is refused as
/// ShortRow, never split wrongly). Returns the row or the first error.
[[nodiscard]] inline std::expected<FyersMasterRow, FyersMasterError>
parse_fyers_master_row(std::string_view line) noexcept {
    std::string_view col[kFyersMasterColumns];
    std::size_t n = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            if (n >= kFyersMasterColumns) {
                return std::unexpected(FyersMasterError::ShortRow);  // too many
            }
            col[n++] = line.substr(start, i - start);
            start = i + 1;
        }
    }
    if (n != kFyersMasterColumns) {
        return std::unexpected(FyersMasterError::ShortRow);
    }

    FyersMasterRow r{};

    // col0 fyToken (uint64).
    {
        const auto b = col[0].data();
        const auto e = b + col[0].size();
        const auto res = std::from_chars(b, e, r.fy_token);
        if (res.ec != std::errc{} || res.ptr != e || col[0].empty()) {
            return std::unexpected(FyersMasterError::BadFyToken);
        }
    }
    // col12 exToken (uint32).
    {
        std::uint64_t t = 0;
        const auto b = col[12].data();
        const auto e = b + col[12].size();
        const auto res = std::from_chars(b, e, t);
        if (res.ec != std::errc{} || res.ptr != e || col[12].empty()
            || t > 0xFFFF'FFFFull) {
            return std::unexpected(FyersMasterError::BadExToken);
        }
        r.ex_token = static_cast<std::uint32_t>(t);
    }
    // col3 minLotSize.
    {
        std::int64_t lot = 0;
        const auto b = col[3].data();
        const auto e = b + col[3].size();
        const auto res = std::from_chars(b, e, lot);
        if (res.ec != std::errc{} || res.ptr != e || lot <= 0) {
            return std::unexpected(FyersMasterError::BadNumber);
        }
        r.lot_size = LotSize{lot};
    }
    // col4 tickSize, col15 strikePrice: rupees -> paise.
    {
        const auto tick = detail::rupees_to_paise(col[4]);
        if (!tick.has_value()) { return std::unexpected(tick.error()); }
        if (*tick <= 0) { return std::unexpected(FyersMasterError::BadNumber); }
        r.tick_size = Price{*tick};
    }
    // col10 exchange.
    {
        int ex = 0;
        const auto b = col[10].data();
        const auto e = b + col[10].size();
        const auto res = std::from_chars(b, e, ex);
        if (res.ec != std::errc{} || res.ptr != e) {
            return std::unexpected(FyersMasterError::BadExchange);
        }
        if (ex == kFyersExchangeNse) { r.exchange = Exchange::NSE; }
        else { return std::unexpected(FyersMasterError::BadExchange); }
    }
    // col16 optType.
    if (col[16] == "XX") { r.opt_type = OptionType::None; }
    else if (col[16] == "CE") { r.opt_type = OptionType::CE; }
    else if (col[16] == "PE") { r.opt_type = OptionType::PE; }
    else { return std::unexpected(FyersMasterError::BadOption); }

    // col11 segment, split with optType.
    {
        int seg = 0;
        const auto b = col[11].data();
        const auto e = b + col[11].size();
        const auto res = std::from_chars(b, e, seg);
        if (res.ec != std::errc{} || res.ptr != e) {
            return std::unexpected(FyersMasterError::BadSegment);
        }
        if (seg == kFyersSegmentCash) {
            r.segment = Segment::Cash;
        } else if (seg == kFyersSegmentFno) {
            r.segment = (r.opt_type == OptionType::None) ? Segment::Fut
                                                         : Segment::Opt;
        } else {
            return std::unexpected(FyersMasterError::BadSegment);
        }
    }
    // col15 strike: only meaningful for options; -1.0 elsewhere -> 0.
    if (r.opt_type != OptionType::None) {
        const auto strike = detail::rupees_to_paise(col[15]);
        if (!strike.has_value()) { return std::unexpected(strike.error()); }
        if (*strike <= 0) { return std::unexpected(FyersMasterError::BadNumber); }
        r.strike = Price{*strike};
    }
    // col8 expiry: epoch seconds, empty for cash.
    if (col[8].empty()) {
        r.expiry = Timestamp::epoch();
    } else {
        std::int64_t secs = 0;
        const auto b = col[8].data();
        const auto e = b + col[8].size();
        const auto res = std::from_chars(b, e, secs);
        if (res.ec != std::errc{} || res.ptr != e || secs < 0
            || secs > 9'000'000'000LL) {   // year ~2255, guards the *1e9 below
            return std::unexpected(FyersMasterError::BadNumber);
        }
        r.expiry = Timestamp{secs * 1'000'000'000LL};
    }
    // col5 isin: empty allowed; otherwise exactly kIsinLen.
    if (!col[5].empty()) {
        if (col[5].size() != kIsinLen) {
            return std::unexpected(FyersMasterError::BadIsin);
        }
        if (!detail::copy_field(r.isin, kIsinLen, col[5])) {
            return std::unexpected(FyersMasterError::BadIsin);
        }
    }
    // col13 underlying, col9 ticker.
    if (!detail::copy_field(r.underlying, kMaxUnderlyingLen, col[13])) {
        return std::unexpected(FyersMasterError::SymbolTooLong);
    }
    if (!detail::copy_field(r.ticker, kFyersTickerLen, col[9])) {
        return std::unexpected(FyersMasterError::SymbolTooLong);
    }
    const std::size_t colon = col[9].find(':');
    const std::string_view symbol = colon == std::string_view::npos
        ? col[9] : col[9].substr(colon + 1);
    if (symbol.empty() || !detail::copy_field(r.symbol, kMaxSymbolLen, symbol)) {
        return std::unexpected(FyersMasterError::SymbolTooLong);
    }
    return r;
}

/// Cold-path adapters from one verified master row into the two identity
/// structures it feeds. The 64-bit canonical FYERS token is never narrowed to
/// the 32-bit exchange token used by market-data packets.
[[nodiscard]] inline FyersInstrumentRecord
fyers_identity_record(const FyersMasterRow& row) noexcept {
    return FyersInstrumentRecord{row.fy_token, row.symbol, row.underlying,
                                 row.exchange, row.segment, row.opt_type,
                                 row.expiry, row.strike};
}

[[nodiscard]] inline CrossVenueLeg
cross_venue_leg(const FyersMasterRow& row, InstrumentId id) noexcept {
    CrossVenueLeg leg{};
    std::memcpy(leg.isin, row.isin, kIsinChars + 1);
    leg.exchange = row.exchange;
    leg.segment = row.segment;
    leg.id = id;
    return leg;
}

} // namespace altair::instruments
