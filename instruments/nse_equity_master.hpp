// instruments/nse_equity_master.hpp — EQUITY_L.csv → cash ContractSpec.
//
// P1-02c. Implemented 2026-09-01 from the real file; the skeleton this
// replaces returned SchemaNotConfigured.
//
// SCHEMA, read from the real file (2574 rows):
//
//   SYMBOL,NAME OF COMPANY, SERIES, DATE OF LISTING, PAID UP VALUE, MARKET LOT, ISIN NUMBER, FACE VALUE
//   20MICRONS,20 Microns Limited,EQ,06-OCT-2008,5,1,INE144J01027,5
//
// Note the LEADING SPACE on every header name after the first. Header matching
// therefore trims; a parser comparing raw text finds only SYMBOL and reports a
// missing column for everything else.
//
// SERIES counts in the real file: EQ 2301, BE 235, BZ 38. All three are
// present in quantity, so the filter is not theoretical.
//
//   EQ  rolling settlement, nets intraday
//   BE  trade-to-trade -- does NOT net intraday, so a strategy that buys and
//       sells within a session takes delivery on BOTH legs
//   BZ  surveillance
//
// There is no tick-size column. `tick_size` is left zero for P1-06 to fill,
// exactly as UDiFF's is.
//
// THE UNDERLYING OF A CASH INSTRUMENT IS ITSELF.
//
// EQUITY_L's SYMBOL is RELIANCE; its NAME OF COMPANY is "Reliance Industries
// Limited". Kite's cash rows carry tradingsymbol RELIANCE and name "RELIANCE
// INDUSTRIES". Only the trading symbol agrees, so that is what both sides put
// in `underlying` -- see the matching change in kite_dump.hpp. Using the
// company name would also have collided 6'531 BSE rows whose `name` is empty
// onto a single key.

#pragma once

#include <instruments/contract_spec.hpp>
#include <instruments/cross_venue.hpp>
#include <instruments/kite_dump.hpp>
#include <instruments/reconcile.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>

namespace altair {

enum class EquityMasterError : std::uint8_t {
    EmptyInput, BadHeader, TooFewFields, BadNumber, BadSymbol,
    UnquotedComma, Overflow
};

struct EquityColumns {
    int symbol = -1;
    int name = -1;
    int series = -1;
    int listing_date = -1;
    int market_lot = -1;
    int isin = -1;
};

/// Series admitted to the universe. Refused by name, never by guess.
struct SeriesFilter {
    const char* const* allowed = nullptr;
    std::size_t count = 0;
};

/// The default: rolling settlement only.
inline constexpr const char* kSeriesEqOnly[] = {"EQ"};
inline constexpr SeriesFilter kDefaultSeriesFilter{kSeriesEqOnly, 1};

struct EquityLoadReport {
    std::size_t added = 0;
    std::size_t rejected_series = 0;   // refused by the allow-list, on purpose
    std::size_t unparseable = 0;
    std::size_t rejected_by_sink = 0;
    EquityMasterError first_error = EquityMasterError::EmptyInput;
    std::size_t first_error_row = 0;
};

/// Contract plus the exchange-independent identity intentionally kept outside
/// ContractSpec. This is the separate ISIN->InstrumentId path permitted by the
/// Phase 7 contract, so the hot-path spec layout does not grow for a cold join.
struct EquityMasterIdentity {
    ContractSpec spec{};
    char isin[instruments::kIsinChars + 1]{};
};

namespace detail {

[[nodiscard]] inline KiteField eq_trim(KiteField f) noexcept {
    while (f.n > 0 && (f.p[0] == ' ' || f.p[0] == '\t')) { ++f.p; --f.n; }
    while (f.n > 0 && (f.p[f.n - 1] == ' ' || f.p[f.n - 1] == '\t')) { --f.n; }
    return f;
}

/// Split honouring double quotes.
///
/// The real file has NO quoted fields today -- verified, zero quote characters
/// in 2574 rows. But a company name containing a comma would need them, and a
/// splitter that ignored quotes would shift every later column so that ISIN
/// landed in the lot field and still parsed as a number. Handled now rather
/// than the first time a company is renamed.
[[nodiscard]] inline std::size_t
eq_split(const char* row, std::size_t len, KiteField* out, std::size_t cap,
         bool& unterminated) noexcept {
    unterminated = false;
    std::size_t n = 0;
    std::size_t i = 0;
    while (i <= len) {
        std::size_t start = i;
        bool quoted = (i < len && row[i] == '"');
        if (quoted) {
            ++i;
            start = i;
            while (i < len && row[i] != '"') { ++i; }
            if (i >= len) {
                unterminated = true;
                return n;
            }
            if (n < cap) {
                out[n].p = row + start;
                out[n].n = i - start;
            }
            ++n;
            ++i;                            // past the closing quote
            while (i < len && row[i] != ',') { ++i; }
            ++i;                            // past the comma
            continue;
        }
        while (i < len && row[i] != ',') { ++i; }
        if (n < cap) {
            out[n].p = row + start;
            out[n].n = i - start;
        }
        ++n;
        ++i;
    }
    return n;
}

} // namespace detail

/// Resolve columns by name, TRIMMED — the real header pads every name after
/// the first with a leading space.
[[nodiscard]] inline std::expected<EquityColumns, EquityMasterError>
parse_equity_header(const char* header, std::size_t len) noexcept {
    if (header == nullptr || len == 0) {
        return std::unexpected(EquityMasterError::BadHeader);
    }
    detail::KiteField f[32];
    bool unterminated = false;
    const std::size_t n = detail::eq_split(header, len, f, 32, unterminated);
    if (unterminated) {
        return std::unexpected(EquityMasterError::UnquotedComma);
    }
    if (n < 6 || n > 32) {
        return std::unexpected(EquityMasterError::BadHeader);
    }

    EquityColumns c{};
    for (std::size_t i = 0; i < n; ++i) {
        const detail::KiteField t = detail::eq_trim(f[i]);
        const int idx = static_cast<int>(i);
        if (detail::kite_field_is(t, "SYMBOL"))                { c.symbol = idx; }
        else if (detail::kite_field_is(t, "NAME OF COMPANY"))  { c.name = idx; }
        else if (detail::kite_field_is(t, "SERIES"))           { c.series = idx; }
        else if (detail::kite_field_is(t, "DATE OF LISTING"))  { c.listing_date = idx; }
        else if (detail::kite_field_is(t, "MARKET LOT"))       { c.market_lot = idx; }
        else if (detail::kite_field_is(t, "ISIN NUMBER"))      { c.isin = idx; }
    }
    if (c.symbol < 0 || c.series < 0 || c.market_lot < 0) {
        return std::unexpected(EquityMasterError::BadHeader);
    }
    return c;
}

/// One row -> one cash ContractSpec.
///
/// Returns `BadSymbol` with `rejected_series` semantics handled by the caller:
/// a series outside the allow-list is NOT an error, it is the filter working,
/// so `load_equity_master` counts it separately.
[[nodiscard]] inline std::expected<ContractSpec, EquityMasterError>
parse_equity_row(const char* row, std::size_t len, const EquityColumns& cols,
                 Exchange exchange, Timestamp snapshot_at) noexcept {
    if (row == nullptr || len == 0) {
        return std::unexpected(EquityMasterError::TooFewFields);
    }
    detail::KiteField f[32];
    bool unterminated = false;
    const std::size_t n = detail::eq_split(row, len, f, 32, unterminated);
    if (unterminated) {
        return std::unexpected(EquityMasterError::UnquotedComma);
    }
    const int need = cols.market_lot > cols.symbol ? cols.market_lot : cols.symbol;
    if (n <= static_cast<std::size_t>(need)) {
        return std::unexpected(EquityMasterError::TooFewFields);
    }

    const detail::KiteField sym = detail::eq_trim(f[cols.symbol]);
    const detail::KiteField lot = detail::eq_trim(f[cols.market_lot]);

    if (sym.n == 0 || sym.n > kMaxSymbolLen || sym.n > kMaxUnderlyingLen) {
        return std::unexpected(EquityMasterError::BadSymbol);
    }

    ContractSpec s{};
    s.id = InstrumentId::Invalid;
    s.exchange = exchange;
    s.segment = Segment::Cash;
    s.opt_type = OptionType::None;
    s.strike = Price{0};
    s.price_scale = 100;
    s.source = (exchange == Exchange::BSE) ? SpecSource::BseMaster
                                           : SpecSource::NseMaster;
    s.snapshot_at = snapshot_at;
    s.valid_from = snapshot_at;
    s.valid_to = Timestamp::max();

    // Cash has NO expiry, and "no expiry" is Timestamp::epoch() -- the same
    // convention P1-04 uses. If the two sources disagreed about what "none"
    // is, NIFTY-cash from NSE and NIFTY-cash from Kite would be two different
    // contracts that never join.
    s.expiry = Timestamp::epoch();

    // The underlying of a cash instrument is ITSELF, and it is the TRADING
    // SYMBOL rather than the company name -- see the file header.
    std::memcpy(s.symbol, sym.p, sym.n);
    s.symbol[sym.n] = '\0';
    std::memcpy(s.underlying, sym.p, sym.n);
    s.underlying[sym.n] = '\0';

    // MARKET LOT is 1 for every row in the real file. It is still parsed:
    // rule 1 has no carve-out for "obviously 1".
    std::uint64_t lotv = 0;
    if (!detail::kite_parse_u64(lot.p, lot.n, 0x7FFF'FFFFull, lotv)
        || lotv == 0) {
        return std::unexpected(EquityMasterError::BadNumber);
    }
    s.lot_size = LotSize{static_cast<std::int64_t>(lotv)};

    // No tick column in EQUITY_L. Zero, for P1-06 to fill from a broker.
    s.tick_size = Price{0};
    s.source_hash = detail::kite_fnv1a(row, len);
    return s;
}

/// True when `series` is in the allow-list.
[[nodiscard]] inline bool
series_allowed(const detail::KiteField& series, const SeriesFilter& f) noexcept {
    for (std::size_t i = 0; i < f.count; ++i) {
        if (detail::kite_field_is(series, f.allowed[i])) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline std::expected<EquityLoadReport, EquityMasterError>
load_equity_master(const char* csv, std::size_t len, const SeriesFilter& series,
                   Exchange exchange, Reconciler& rec,
                   Timestamp snapshot_at) noexcept {
    if (csv == nullptr || len == 0) {
        return std::unexpected(EquityMasterError::EmptyInput);
    }

    auto line_end = [&](std::size_t from) noexcept {
        std::size_t e = from;
        while (e < len && csv[e] != '\n') { ++e; }
        return e;
    };
    auto trim_cr = [&](std::size_t from, std::size_t e) noexcept {
        return (e > from && csv[e - 1] == '\r') ? e - 1 : e;
    };

    std::size_t pos = 0;
    const std::size_t he = line_end(pos);
    const auto cols = parse_equity_header(csv + pos, trim_cr(pos, he) - pos);
    if (!cols) {
        return std::unexpected(cols.error());
    }
    pos = he < len ? he + 1 : len;
    if (pos >= len) {
        return std::unexpected(EquityMasterError::EmptyInput);
    }

    EquityLoadReport rep{};
    std::size_t row_no = 0;
    while (pos < len) {
        const std::size_t e = line_end(pos);
        const std::size_t stop = trim_cr(pos, e);
        if (stop > pos) {
            ++row_no;
            // The series filter runs BEFORE parsing, so a refused scrip is
            // never counted as unparseable -- deliberate refusal and breakage
            // must not share a bucket.
            detail::KiteField f[32];
            bool unterm = false;
            const std::size_t n = detail::eq_split(csv + pos, stop - pos, f, 32,
                                                   unterm);
            if (!unterm && n > static_cast<std::size_t>(cols->series)
                && !series_allowed(detail::eq_trim(f[cols->series]), series)) {
                ++rep.rejected_series;
                pos = e < len ? e + 1 : len;
                continue;
            }

            const auto spec = parse_equity_row(csv + pos, stop - pos, *cols,
                                               exchange, snapshot_at);
            if (!spec) {
                if (rep.unparseable == 0) {
                    rep.first_error = spec.error();
                    rep.first_error_row = row_no;
                }
                ++rep.unparseable;
            } else if (rec.add(*spec).has_value()) {
                ++rep.added;
            } else {
                ++rep.rejected_by_sink;
            }
        }
        pos = e < len ? e + 1 : len;
    }
    if (row_no == 0) {
        return std::unexpected(EquityMasterError::EmptyInput);
    }
    return rep;
}

[[nodiscard]] inline std::expected<EquityMasterIdentity, EquityMasterError>
parse_equity_identity_row(const char* row, std::size_t len,
                          const EquityColumns& cols, Exchange exchange,
                          Timestamp snapshot_at) noexcept {
    const auto spec = parse_equity_row(row, len, cols, exchange, snapshot_at);
    if (!spec) return std::unexpected(spec.error());

    detail::KiteField fields[32];
    bool unterminated = false;
    const std::size_t count = detail::eq_split(row, len, fields, 32, unterminated);
    if (unterminated || cols.isin < 0
        || count <= static_cast<std::size_t>(cols.isin)) {
        return std::unexpected(EquityMasterError::TooFewFields);
    }
    const auto isin = detail::eq_trim(fields[cols.isin]);
    if (isin.n != instruments::kIsinChars) {
        return std::unexpected(EquityMasterError::BadSymbol);
    }
    EquityMasterIdentity out{};
    out.spec = *spec;
    std::memcpy(out.isin, isin.p, isin.n);
    out.isin[isin.n] = '\0';
    return out;
}

[[nodiscard]] inline instruments::CrossVenueLeg
cross_venue_leg(const EquityMasterIdentity& row, InstrumentId id) noexcept {
    instruments::CrossVenueLeg leg{};
    std::memcpy(leg.isin, row.isin, instruments::kIsinChars + 1);
    leg.exchange = row.spec.exchange;
    leg.segment = row.spec.segment;
    leg.id = id;
    return leg;
}

} // namespace altair
