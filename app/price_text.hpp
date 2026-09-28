// app/price_text.hpp -- a price written to dataset/ is the price Kite sent.
//
// CX02-A1a. Fixes C14-001.
//
// WHAT WAS WRONG.
//
// Both dataset writers formatted prices at SIX SIGNIFICANT DIGITS:
// altair_kite_update with `%g`, altair_kite_fetch with an ostream at its
// default precision. NIFTY at 24,123.45 has seven, so it was written as
// 24123.5 -- and BANKNIFTY at 51,234.65 as 51234.7. Every bar either tool
// touched lost its paise, silently, and nothing downstream could tell: the file
// parses, the numbers are plausible, and the rounding is below what a chart
// shows.
//
// WHAT THIS DOES INSTEAD.
//
// The SHORTEST FIXED-NOTATION decimal text that parses back to the identical
// double (`std::to_chars` with `chars_format::fixed` and no precision). Kite's
// JSON carries a price such as 24123.45; the parser turns that into the
// nearest double; this turns the double back into "24123.45". Nothing is
// assumed about how many decimals the instrument uses -- a two-decimal index
// and a four-decimal rate both survive -- and the round trip is exact by
// construction rather than by choosing a precision that happens to be enough
// today.
//
// FIXED, NOT PLAIN SHORTEST. Unqualified `to_chars` picks whichever of %f and
// %e is shorter, so a price of exactly 1,00,000 comes out as "1e+05". strtod
// reads that; a hand-rolled CSV field parser need not, and dataset/ has never
// held an exponent.
//
// It REFUSES rather than writes: a NaN, an infinity, a negative price, ZERO,
// or one beyond any traded magnitude is not a bar, and writing "nan" into a
// CSV hands the problem to whichever reader meets it first (rule 9).

#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <system_error>

namespace altair::dataset {

enum class PriceTextError : std::uint8_t {
    NotFinite,
    Negative,
    /// `cap` could not hold the text. Refused, never truncated: a price cut
    /// short is a different price.
    BufferTooSmall,
    // CX02-A2c: APPENDED, not inserted. The frozen contract was
    // {NotFinite, Negative, BufferTooSmall} and CX02-A1a put OutOfRange in the
    // MIDDLE of it, shifting BufferTooSmall's ordinal (R-AB-034).
    /// At or above `kPriceTextCeiling`.
    OutOfRange,
    /// Zero. A traded price is never zero: Kite returns 0 for a halted or
    /// malformed candle, and writing `,0,0,0,0,` asserts a market that traded
    /// at nothing (R-AB-026). The fetcher already refuses an all-zero VOLUME
    /// on exactly this argument.
    Zero
};

/// No price on any Indian exchange is within nine orders of magnitude of this.
/// It exists so the fixed-notation text has a proven maximum length.
inline constexpr double kPriceTextCeiling = 1e15;

/// Buffer size that cannot hit BufferTooSmall for any value in [1e-29, 1e15).
///
/// PROOF: below the ceiling the integer part is at most 15 digits, and a
/// shortest round-trip needs at most 17 significant digits, so a value >= 1
/// needs at most 17 digits plus the point = 18 characters. A value in (0, 1)
/// needs "0." plus leading zeros plus at most 17 digits; at 1e-29 that is
/// 2 + 28 + 17 = 47. Anything smaller is refused as BufferTooSmall, which is
/// a refusal and not a truncation.
inline constexpr std::size_t kPriceTextMax = 48;

/// Write `v` as the shortest fixed-notation text that round-trips to exactly
/// `v`.
///
/// Returns the number of characters written. Not NUL-terminated.
[[nodiscard]] inline std::expected<std::size_t, PriceTextError>
format_price(double v, char* out, std::size_t cap) noexcept {
    if (!std::isfinite(v)) {
        return std::unexpected(PriceTextError::NotFinite);
    }
    if (std::signbit(v) && v != 0.0) {
        return std::unexpected(PriceTextError::Negative);
    }
    if (v == 0.0) {
        return std::unexpected(PriceTextError::Zero);
    }
    if (v >= kPriceTextCeiling) {
        return std::unexpected(PriceTextError::OutOfRange);
    }
    if (out == nullptr) {
        return std::unexpected(PriceTextError::BufferTooSmall);
    }
    // Zero and every negative are refused above, so this is a positive
    // finite value. fabs stays as a belt on the sign bit.
    const double w = std::fabs(v);
    const auto r = std::to_chars(out, out + cap, w, std::chars_format::fixed);
    if (r.ec != std::errc{}) {
        return std::unexpected(PriceTextError::BufferTooSmall);
    }
    return static_cast<std::size_t>(r.ptr - out);
}

/// Does `text` parse to exactly `v`? The property every writer here must
/// keep, exposed so a test can hold ANY formatter to it -- including the old
/// `%g` one, which it must reject.
[[nodiscard]] inline bool round_trips(const char* text, std::size_t len,
                                      double v) noexcept {
    double back = 0.0;
    const auto r = std::from_chars(text, text + len, back);
    return r.ec == std::errc{} && r.ptr == text + len && back == v;
}

} // namespace altair::dataset
