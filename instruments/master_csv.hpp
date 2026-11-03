#pragma once

// P1-04 — ALTAIR's instrument master CSV (data/instruments.csv) -> ContractSpec.
//
// live/instrument_master.hpp writes data/instruments.csv every day from
// FYERS's public symbol masters. It is the PRIMARY source for ALTAIR's wire
// tokens (FeedSource::Wire) and a CROSS-CHECK for lot size, tick size and
// expiry — ROADMAP §6.1 makes the NSE and BSE exchange masters primary for
// those, precisely so a broker's convenience copy never quietly becomes the
// authority.
//
// Columns: instrument_token, exchange_token, tradingsymbol, name, last_price,
// expiry, strike, tick_size, lot_size, instrument_type, segment, exchange.
// Two conversions carry all the risk:
//
//   RUPEES TO PAISE. The CSV says "0.05"; the engine needs Price{5}. Through a
//   double, 0.05 * 100 is 5.000000000000000277..., and the neighbouring case
//   truncates to 4. A tick size of 4 paise makes every rounded order price
//   wrong. Parsed as text and scaled exactly — no floating point appears in
//   this file at all.
//
//   DATES. `expiry` is a zoneless YYYY-MM-DD, the IST date of the expiry. Read as UTC it lands 5h30m early, and on expiry day that is
//   the difference between a live contract and a dead one.
//
// This card PARSES ONLY: a parser that opens a socket is one that needs
// credentials it should not have.

#include <instruments/contract_spec.hpp>
#include <instruments/reconcile.hpp>
#include <time/timestamp.hpp>
#include <types/units.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>

namespace altair {

/// Why a row could not be turned into a ContractSpec.
enum class MasterParseError : std::uint8_t {
    EmptyInput,       // nothing to parse
    BadHeader,        // the header is missing a required column
    TooFewFields,     // a row has fewer columns than the header
    BadNumber,        // a numeric field is not a number
    BadDecimal,       // more decimal places than paise can represent
    BadDate,          // an expiry that is not YYYY-MM-DD
    BadSymbol,        // empty, or longer than kMaxSymbolLen
    UnknownExchange,  // an exchange string this build does not map
    Overflow          // a value that does not fit its target type
};

/// Column indices resolved from the header line, so column ORDER does not
/// matter — only presence. Brokers have reordered such CSVs before.
struct MasterColumns {
    int instrument_token = -1;
    int exchange_token   = -1;
    int tradingsymbol    = -1;
    int name             = -1;
    int expiry           = -1;
    int strike           = -1;
    int tick_size        = -1;
    int lot_size         = -1;
    int instrument_type  = -1;
    int segment          = -1;
    int exchange         = -1;
};

namespace detail {

/// A bounded view into the caller's buffer. No allocation, no ownership.
struct MasterField {
    const char* p = nullptr;
    std::size_t n = 0;
};

/// Split `row` on commas into `out`, returning how many fields were found.
/// UNIT: fields. Never writes past `cap`. The master is not RFC-4180 quoted,
/// so a plain split is correct here and a quote-aware reader would be
/// pretending to a robustness this format does not have.
[[nodiscard]] inline std::size_t master_split(const char* row, std::size_t len,
                                            MasterField* out, std::size_t cap) noexcept {
    std::size_t n = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= len; ++i) {
        if (i == len || row[i] == ',') {
            if (n < cap) {
                std::size_t b = start;
                std::size_t e = i;
                // Strip ONE layer of surrounding double quotes.
                //
                // The master quotes the `name` column and nothing else: a real row
                // reads ...,NIFTY26SEPFUT,"NIFTY",0,... Left in place, the
                // underlying becomes the four characters " N I F T Y " and
                // P1-06's D1 key can NEVER match an exchange master that
                // writes NIFTY unquoted -- so every contract reconciles as
                // SingleSource and the three-way check silently does nothing.
                // Found by running a real 106'150-row master, not by reading
                // the schema.
                if (e - b >= 2 && row[b] == '"' && row[e - 1] == '"') {
                    ++b;
                    --e;
                }
                out[n].p = row + b;
                out[n].n = e - b;
            }
            ++n;
            start = i + 1;
        }
    }
    return n;
}

/// True iff `f` equals the NUL-terminated `lit`, case-sensitively.
[[nodiscard]] inline bool master_field_is(const MasterField& f, const char* lit) noexcept {
    const std::size_t l = std::strlen(lit);
    return f.n == l && std::memcmp(f.p, lit, l) == 0;
}

/// Parse an unsigned decimal integer. UNIT: none.
/// Returns false on empty, non-digit, or overflow past `limit`.
[[nodiscard]] inline bool master_parse_u64(const char* p, std::size_t n,
                                         std::uint64_t limit,
                                         std::uint64_t& out) noexcept {
    if (n == 0) {
        return false;
    }
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const char c = p[i];
        if (c < '0' || c > '9') {
            return false;
        }
        const auto d = static_cast<std::uint64_t>(c - '0');
        if (v > (limit - d) / 10) {
            return false;
        }
        v = v * 10 + d;
    }
    out = v;
    return true;
}

/// Days from 1970-01-01 to y-m-d, proleptic Gregorian (Howard Hinnant).
[[nodiscard]] constexpr std::int64_t master_days_from_civil(std::int64_t y, unsigned m,
                                                          unsigned d) noexcept {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    // Written as an explicit branch rather than Hinnant's `m + (m > 2 ? -3 : 9)`.
    // His form assumes signed arithmetic; spelled with unsigned literals it is
    // `-3u`, which wraps to a huge value, happens to wrap back on the add, and
    // trips MSVC's C4146. Correct either way, but only one of them is readable.
    const unsigned shifted = (m > 2) ? (m - 3u) : (m + 9u);
    const unsigned doy = (153u * shifted + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097LL + static_cast<std::int64_t>(doe) - 719468LL;
}

/// Days in month m of year y. UNIT: days.
[[nodiscard]] constexpr unsigned master_days_in_month(std::int64_t y, unsigned m) noexcept {
    constexpr unsigned k[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2) {
        const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
        return leap ? 29u : 28u;
    }
    return k[m - 1];
}

/// FNV-1a 64 over raw bytes. Reuses the family already in the codebase rather
/// than inventing a second one. UNIT: none.
[[nodiscard]] inline std::uint64_t master_fnv1a(const char* p, std::size_t n) noexcept {
    std::uint64_t h = 14695981039346656037ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(p[i]));
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace detail

/// Parse a decimal rupee string into integer paise, EXACTLY.
/// UNIT: text in, paise out. `"0.05"` -> 5, `"25000"` -> 2'500'000,
/// `"-1.5"` -> -150. Never goes through double.
/// Returns BadNumber for non-numeric text, BadDecimal for more than two
/// decimal places, Overflow past int64 paise.
/// PRECONDITION: `text` need not be NUL-terminated; `len` bounds it.
[[nodiscard]] inline std::expected<Price, MasterParseError>
parse_rupees_to_paise(const char* text, std::size_t len) noexcept {
    if (text == nullptr || len == 0) {
        return std::unexpected(MasterParseError::BadNumber);
    }

    std::size_t i = 0;
    bool neg = false;
    if (text[0] == '-') {
        neg = true;
        i = 1;
    }
    if (i >= len) {
        return std::unexpected(MasterParseError::BadNumber);   // just "-"
    }

    // Split on the decimal point without ever forming a floating value.
    std::size_t dot = len;
    for (std::size_t k = i; k < len; ++k) {
        if (text[k] == '.') {
            if (dot != len) {
                return std::unexpected(MasterParseError::BadNumber);   // "1.2.3"
            }
            dot = k;
        }
    }

    const std::size_t int_len = (dot == len ? len : dot) - i;
    const std::size_t frac_len = (dot == len) ? 0 : len - dot - 1;

    // ".5" and "5." are both legal; "" and "." are not.
    if (int_len == 0 && frac_len == 0) {
        return std::unexpected(MasterParseError::BadNumber);
    }
    if (frac_len > 2) {
        return std::unexpected(MasterParseError::BadDecimal);
    }

    constexpr std::uint64_t kMax = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max());

    std::uint64_t whole = 0;
    if (int_len > 0 && !detail::master_parse_u64(text + i, int_len, kMax / 100, whole)) {
        // Distinguish a non-digit from a genuine overflow.
        for (std::size_t k = i; k < i + int_len; ++k) {
            if (text[k] < '0' || text[k] > '9') {
                return std::unexpected(MasterParseError::BadNumber);
            }
        }
        return std::unexpected(MasterParseError::Overflow);
    }

    std::uint64_t frac = 0;
    if (frac_len > 0) {
        if (!detail::master_parse_u64(text + dot + 1, frac_len, 99, frac)) {
            return std::unexpected(MasterParseError::BadNumber);
        }
        if (frac_len == 1) {
            frac *= 10;   // "0.5" is 50 paise, not 5
        }
    }

    const std::uint64_t paise = whole * 100 + frac;
    if (paise > kMax) {
        return std::unexpected(MasterParseError::Overflow);
    }
    const auto v = static_cast<std::int64_t>(paise);
    return Price{neg ? -v : v};
}

/// Parse a zoneless `YYYY-MM-DD` as an IST date, returned as a UTC instant at
/// IST midnight. UNIT: text in, ns since the Unix epoch out.
/// An EMPTY string is not an error — cash instruments have no expiry — and
/// yields Timestamp::epoch().
[[nodiscard]] inline std::expected<Timestamp, MasterParseError>
parse_master_expiry(const char* text, std::size_t len) noexcept {
    if (text == nullptr || len == 0) {
        return Timestamp::epoch();   // cash: no expiry, not an error
    }
    if (len != 10 || text[4] != '-' || text[7] != '-') {
        return std::unexpected(MasterParseError::BadDate);
    }

    std::uint64_t y = 0, m = 0, d = 0;
    if (!detail::master_parse_u64(text, 4, 9999, y) ||
        !detail::master_parse_u64(text + 5, 2, 99, m) ||
        !detail::master_parse_u64(text + 8, 2, 99, d)) {
        return std::unexpected(MasterParseError::BadDate);
    }
    if (m < 1 || m > 12) {
        return std::unexpected(MasterParseError::BadDate);
    }
    const auto yi = static_cast<std::int64_t>(y);
    const auto mu = static_cast<unsigned>(m);
    if (d < 1 || d > detail::master_days_in_month(yi, mu)) {
        return std::unexpected(MasterParseError::BadDate);
    }

    const std::int64_t days =
        detail::master_days_from_civil(yi, mu, static_cast<unsigned>(d));
    // IST midnight is 18:30 UTC the PREVIOUS day. Subtract the offset.
    const std::int64_t ns = days * 86'400'000'000'000LL - kIstOffset.raw();
    return Timestamp{ns};
}

/// Resolve column positions from the CSV header line.
/// UNIT: none. Returns BadHeader when a required column is absent.
/// PRECONDITION: `header` is one line, without its newline.
[[nodiscard]] inline std::expected<MasterColumns, MasterParseError>
parse_master_header(const char* header, std::size_t len) noexcept {
    if (header == nullptr || len == 0) {
        return std::unexpected(MasterParseError::BadHeader);
    }
    constexpr std::size_t kMaxCols = 32;
    detail::MasterField f[kMaxCols];
    const std::size_t n = detail::master_split(header, len, f, kMaxCols);
    if (n == 0 || n > kMaxCols) {
        return std::unexpected(MasterParseError::BadHeader);
    }

    MasterColumns c{};
    for (std::size_t i = 0; i < n; ++i) {
        const auto ix = static_cast<int>(i);
        if (detail::master_field_is(f[i], "instrument_token")) { c.instrument_token = ix; }
        else if (detail::master_field_is(f[i], "exchange_token")) { c.exchange_token = ix; }
        else if (detail::master_field_is(f[i], "tradingsymbol"))  { c.tradingsymbol = ix; }
        else if (detail::master_field_is(f[i], "name"))           { c.name = ix; }
        else if (detail::master_field_is(f[i], "expiry"))         { c.expiry = ix; }
        else if (detail::master_field_is(f[i], "strike"))         { c.strike = ix; }
        else if (detail::master_field_is(f[i], "tick_size"))      { c.tick_size = ix; }
        else if (detail::master_field_is(f[i], "lot_size"))       { c.lot_size = ix; }
        else if (detail::master_field_is(f[i], "instrument_type")){ c.instrument_type = ix; }
        else if (detail::master_field_is(f[i], "segment"))        { c.segment = ix; }
        else if (detail::master_field_is(f[i], "exchange"))       { c.exchange = ix; }
    }

    if (c.instrument_token < 0 || c.exchange_token < 0 || c.tradingsymbol < 0 ||
        c.name < 0 || c.expiry < 0 || c.strike < 0 || c.tick_size < 0 ||
        c.lot_size < 0 || c.instrument_type < 0 || c.segment < 0 || c.exchange < 0) {
        return std::unexpected(MasterParseError::BadHeader);
    }
    return c;
}

/// Turn one data row into a ContractSpec.
/// UNIT: none. `snapshot_at` stamps the spec's provenance; `valid_from` is set
/// to it and `valid_to` to Timestamp::max().
/// Sets source to SpecSource::InstrumentMaster and price_scale to 100, and leaves the
/// XTS token zero — this source knows nothing about XTS.
/// PRECONDITION: `cols` came from parse_master_header on the same file.
[[nodiscard]] inline std::expected<ContractSpec, MasterParseError>
parse_master_row(const char* row, std::size_t len,
               const MasterColumns& cols, Timestamp snapshot_at,
               bool* out_truncated = nullptr) noexcept {
    if (row == nullptr || len == 0) {
        return std::unexpected(MasterParseError::TooFewFields);
    }
    constexpr std::size_t kMaxCols = 32;
    detail::MasterField f[kMaxCols];
    const std::size_t n = detail::master_split(row, len, f, kMaxCols);

    auto need = [&](int ix) noexcept -> bool {
        return ix >= 0 && static_cast<std::size_t>(ix) < n &&
               static_cast<std::size_t>(ix) < kMaxCols;
    };
    if (!need(cols.instrument_token) || !need(cols.tradingsymbol) ||
        !need(cols.name) || !need(cols.expiry) || !need(cols.strike) ||
        !need(cols.tick_size) || !need(cols.lot_size) ||
        !need(cols.instrument_type) || !need(cols.exchange)) {
        return std::unexpected(MasterParseError::TooFewFields);
    }

    const detail::MasterField& sym = f[cols.tradingsymbol];
    if (sym.n == 0 || sym.n > kMaxSymbolLen) {
        return std::unexpected(MasterParseError::BadSymbol);
    }

    std::uint64_t tok = 0;
    if (!detail::master_parse_u64(f[cols.instrument_token].p,
                                f[cols.instrument_token].n, 0xFFFF'FFFFull, tok)) {
        return std::unexpected(MasterParseError::BadNumber);
    }

    const auto strike = parse_rupees_to_paise(f[cols.strike].p, f[cols.strike].n);
    if (!strike.has_value()) { return std::unexpected(strike.error()); }
    const auto tick = parse_rupees_to_paise(f[cols.tick_size].p, f[cols.tick_size].n);
    if (!tick.has_value()) { return std::unexpected(tick.error()); }
    // lot_size may be written as a decimal ("75" or "75.0"); paise-scale it
    // then divide back out, so the same exact parser covers it.
    const auto lot_p = parse_rupees_to_paise(f[cols.lot_size].p, f[cols.lot_size].n);
    if (!lot_p.has_value()) { return std::unexpected(lot_p.error()); }

    const auto expiry = parse_master_expiry(f[cols.expiry].p, f[cols.expiry].n);
    if (!expiry.has_value()) { return std::unexpected(expiry.error()); }

    // Exchange, and the segment DERIVED from it — not from the CSV's own
    // `segment` column, which carries composite values like "NFO-OPT".
    const detail::MasterField& ex = f[cols.exchange];
    Exchange exch{};
    Segment seg{};
    const detail::MasterField& it = f[cols.instrument_type];
    const bool is_opt = detail::master_field_is(it, "CE") || detail::master_field_is(it, "PE");

    if (detail::master_field_is(ex, "NSE")) { exch = Exchange::NSE; seg = Segment::Cash; }
    else if (detail::master_field_is(ex, "BSE")) { exch = Exchange::BSE; seg = Segment::Cash; }
    else if (detail::master_field_is(ex, "NFO")) {
        exch = Exchange::NSE; seg = is_opt ? Segment::Opt : Segment::Fut;
    } else if (detail::master_field_is(ex, "BFO")) {
        exch = Exchange::BSE; seg = is_opt ? Segment::Opt : Segment::Fut;
    } else if (detail::master_field_is(ex, "CDS")) {
        exch = Exchange::NSE; seg = Segment::Currency;
    } else if (detail::master_field_is(ex, "BCD")) {
        exch = Exchange::BSE; seg = Segment::Currency;
    } else {
        return std::unexpected(MasterParseError::UnknownExchange);
    }

    ContractSpec s{};
    s.id = InstrumentId::Invalid;                 // the store assigns identity
    s.token[static_cast<std::size_t>(FeedSource::Wire)] =
        static_cast<std::uint32_t>(tok);
    s.token[static_cast<std::size_t>(FeedSource::Xts)] = 0;   // knows no XTS
    s.lot_size = LotSize{lot_p->raw() / 100};
    s.tick_size = *tick;
    s.strike = *strike;
    s.freeze_qty = Qty{0};                        // not in this dump
    s.band_lower = Price{0};
    s.band_upper = Price{0};
    s.price_scale = 100;
    s.expiry = *expiry;
    s.valid_from = snapshot_at;
    s.valid_to = Timestamp::max();
    s.source_hash = detail::master_fnv1a(row, len);
    s.snapshot_at = snapshot_at;
    s.exchange = exch;
    s.segment = seg;
    s.opt_type = detail::master_field_is(it, "CE") ? OptionType::CE
               : detail::master_field_is(it, "PE") ? OptionType::PE
                                                 : OptionType::None;
    s.source = SpecSource::InstrumentMaster;
    s.stale = false;

    std::memcpy(s.symbol, sym.p, sym.n);
    s.symbol[sym.n] = '\0';

    // THE UNDERLYING OF A CASH INSTRUMENT IS ITSELF, and it is the TRADING
    // SYMBOL rather than the `name` column.
    //
    // For derivatives the two agree: a NIFTY future has name "NIFTY", which is
    // exactly what fo_mktlots' SYMBOL and UDiFF's TckrSymb carry. For cash they
    // do not. The master's RELIANCE row has tradingsymbol RELIANCE and name
    // "RELIANCE INDUSTRIES", while EQUITY_L has SYMBOL RELIANCE and NAME OF
    // COMPANY "Reliance Industries Limited". Only the trading symbol agrees, so
    // keying cash on `name` would join nothing.
    //
    // It also collided 6'531 real BSE cash rows whose `name` is EMPTY onto a
    // single key, plus 373 more where two ETFs from the same fund house share a
    // company name. Found by loading a real 106'150-row master.
    const detail::MasterField& src =
        (seg == Segment::Cash) ? sym : f[cols.name];
    // RULE 11: THIS TRUNCATES, SO IT COUNTS.
    //
    // `underlying` is 23 characters and 2,121 of the 108,411 rows in a real
    // master are longer. Worse than the loss of characters: 137 distinct
    // 23-character prefixes map to MORE THAN ONE company --
    // "PROCTER & GAMBLE HEALTH L" and "PROCTER & GAMBLE HEALTH LIMITE" become
    // the same key -- so anything grouping by underlying silently merges two
    // issuers.
    //
    // Refusing the row is worse than accepting a marked one: these are real
    // instruments and dropping 2,121 of them to protect a field width would
    // block trading in every one. So this takes rule 11's third arm -- the
    // truncation is COUNTED, carried in MasterLoadReport, and shown. A known,
    // measured 2,121 is a different thing from a silent one.
    const bool truncated = src.n > kMaxUnderlyingLen;
    const std::size_t un = truncated ? kMaxUnderlyingLen : src.n;
    std::memcpy(s.underlying, src.p, un);
    s.underlying[un] = '\0';
    if (out_truncated != nullptr && truncated) { *out_truncated = true; }
    return s;
}

/// Outcome of loading a whole dump.
struct MasterLoadReport {
    /// Rows that produced a spec and were added. UNIT: rows.
    std::size_t added;
    /// Rows the store refused — a duplicate token, or Full. UNIT: rows.
    /// Refused by the destination — a duplicate token for a SpecStore, a
    /// full table for a Reconciler. Named for the sink rather than the
    /// store because there are now two sinks and only one of them is a store.
    std::size_t rejected_by_sink;
    /// Rows that would not parse. UNIT: rows.
    std::size_t unparseable;
    /// Rows whose UNDERLYING NAME did not fit in kMaxUnderlyingLen and was
    /// truncated. UNIT: rows. P33-05, rule 11.
    ///
    /// Not an error and not a rejection -- the spec is usable and the row is
    /// added. It is here because truncation can COLLIDE two issuers onto one
    /// underlying key (137 prefixes do, on a real dump), and a caller
    /// grouping by underlying deserves the count rather than discovering it
    /// as a merged position.
    std::size_t underlying_truncated;
    /// The first parse error seen, for diagnosis. Only meaningful when
    /// unparseable > 0.
    MasterParseError first_error;
    /// The row number of that first error, 1-based, header excluded.
    std::size_t first_error_row;
};

/// Parse a whole in-memory dump and add every row to the store.
/// UNIT: none. Skips rows that will not parse and KEEPS GOING — one malformed
/// contract must not cost the session its whole universe — but counts them,
/// and the caller decides whether the count is tolerable.
/// PRECONDITION: `csv` is the complete file, header first. Not NUL-terminated
/// is fine; `len` bounds it. Allocates nothing.
namespace detail {

/// The row loop, parameterised on where accepted specs go.
///
/// `sink` is any callable `bool(const ContractSpec&)` returning whether the
/// spec was accepted. A template rather than std::function: no allocation and
/// no indirect call, which matters because this same loop will one day run
/// over a 100k-row dump.
template <class Sink>
[[nodiscard]] inline std::expected<MasterLoadReport, MasterParseError>
load_instrument_master_into(const char* csv, std::size_t len,
                    Sink sink, Timestamp snapshot_at) noexcept {
    if (csv == nullptr || len == 0) {
        return std::unexpected(MasterParseError::EmptyInput);
    }

    auto line_end = [&](std::size_t from) noexcept -> std::size_t {
        std::size_t e = from;
        while (e < len && csv[e] != '\n') { ++e; }
        return e;
    };
    auto trim_cr = [&](std::size_t from, std::size_t e) noexcept -> std::size_t {
        return (e > from && csv[e - 1] == '\r') ? e - 1 : e;
    };

    std::size_t pos = 0;
    const std::size_t he = line_end(pos);
    const auto cols = parse_master_header(csv + pos, trim_cr(pos, he) - pos);
    if (!cols.has_value()) {
        return std::unexpected(cols.error());
    }
    pos = he < len ? he + 1 : len;
    if (pos >= len) {
        return std::unexpected(MasterParseError::EmptyInput);   // header only
    }

    MasterLoadReport rep{};
    rep.first_error = MasterParseError::EmptyInput;
    std::size_t row_no = 0;

    while (pos < len) {
        const std::size_t e = line_end(pos);
        const std::size_t stop = trim_cr(pos, e);
        if (stop > pos) {
            ++row_no;
            bool trunc = false;
            const auto spec =
                parse_master_row(csv + pos, stop - pos, *cols, snapshot_at,
                               &trunc);
            if (trunc) { ++rep.underlying_truncated; }
            if (!spec.has_value()) {
                // One malformed contract must not cost the session its whole
                // universe. Count it, remember the first, keep going.
                if (rep.unparseable == 0) {
                    rep.first_error = spec.error();
                    rep.first_error_row = row_no;
                }
                ++rep.unparseable;
            } else if (sink(*spec)) {
                ++rep.added;
            } else {
                ++rep.rejected_by_sink;
            }
        }
        pos = e < len ? e + 1 : len;
    }

    if (row_no == 0) {
        return std::unexpected(MasterParseError::EmptyInput);
    }
    return rep;
}

} // namespace detail

/// Load the instrument master into the RECONCILER. This is the correct path.
///
/// It is a CROSS-CHECK, not an authority (ROADMAP §6.1). Feeding the
/// reconciler is what subjects it to the three-way agreement check, so a
/// master lot size that disagrees with the exchange master blocks the symbol instead
/// of silently winning. Every later master parser takes a `Reconciler&` for
/// exactly this reason: a parser that CAN write the store is a parser that can
/// bypass reconciliation.
[[nodiscard]] inline std::expected<MasterLoadReport, MasterParseError>
load_instrument_master(const char* csv, std::size_t len,
               Reconciler& rec, Timestamp snapshot_at) noexcept {
    return detail::load_instrument_master_into(
        csv, len,
        [&rec](const ContractSpec& s) noexcept { return rec.add(s).has_value(); },
        snapshot_at);
}

/// Load straight into a SpecStore, with NO reconciliation.
///
/// Retained for single-source tests and for the pre-open smoke path, where
/// there is by construction nothing to reconcile against. **Do not use it once
/// a second source exists** — it is the bypass the overload above closes.
[[nodiscard]] inline std::expected<MasterLoadReport, MasterParseError>
load_instrument_master_unreconciled(const char* csv, std::size_t len,
                            SpecStore& store, Timestamp snapshot_at) noexcept {
    return detail::load_instrument_master_into(
        csv, len,
        [&store](const ContractSpec& s) noexcept { return store.add(s).has_value(); },
        snapshot_at);
}

} // namespace altair
