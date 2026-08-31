// instruments/udiff_master.hpp — the UDiFF F&O bhavcopy → ContractSpec.
//
// P1-02a (NSE) and P1-03a (BSE), which turned out to be ONE parser.
//
// The P1-03a card was written on the premise that BSE differs from NSE in four
// structural ways. For the DERIVATIVES master that premise is wrong: BSE
// publishes the identical UDiFF layout -- same 34 columns, same
// FinInstrmTp vocabulary, same ISO dates, same decimal strikes. Verified by
// downloading both. The only difference that survives is the `Exchange` stamped
// on the output, and that is a parameter rather than a second parser.
//
// The four BSE differences the card listed still apply to the BSE *scrip*
// master (cash), which is a separate artefact and a separate card.
//
// SCHEMA, read from the real files (2026-08-31 session):
//
//   FinInstrmTp    IDF | IDO | STF | STO   -- index/stock, future/option.
//                  NOT the legacy FUTIDX/OPTIDX vocabulary.
//   TckrSymb       NIFTY            -- joins to Kite's `name`
//   FinInstrmNm    NIFTY26OCTFUT    -- the same shape as Kite's tradingsymbol
//   XpryDt         2026-10-27       -- ISO, same as Kite's expiry
//   StrkPric       410.00           -- decimal rupees; EMPTY for futures
//   OptnTp         CE | PE          -- EMPTY for futures
//   NewBrdLotQty   3100             -- integer. The lot size, and the reason
//                                      this file is the PRIMARY authority.
//
// There is NO tick-size column. `tick_size` is left zero for P1-06 to fill
// from a broker, exactly as the card specified.

#pragma once

#include <instruments/contract_spec.hpp>
#include <instruments/kite_dump.hpp>
#include <instruments/reconcile.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>

namespace altair {

enum class UdiffError : std::uint8_t {
    EmptyInput,
    BadHeader,
    TooFewFields,
    BadNumber,
    BadDecimal,
    BadDate,
    BadSymbol,
    UnknownInstrumentType,
    Overflow
};

/// Resolved column positions. -1 when the file does not carry the column.
struct UdiffColumns {
    int fin_instrm_tp = -1;
    int tckr_symb     = -1;
    int fin_instrm_nm = -1;
    int xpry_dt       = -1;
    int strk_pric     = -1;
    int optn_tp       = -1;
    int lot_qty       = -1;
};

struct UdiffLoadReport {
    std::size_t added = 0;
    std::size_t unparseable = 0;
    std::size_t rejected_by_sink = 0;
    UdiffError first_error = UdiffError::EmptyInput;
    std::size_t first_error_row = 0;
};

namespace detail {

/// KiteParseError -> UdiffError. The two files share their date and decimal
/// grammar exactly, so this parser REUSES P1-04's converters rather than
/// growing a second pair that could drift. One implementation, one set of
/// tests, one place for the 0.05-becomes-4-paise bug not to live.
[[nodiscard]] constexpr UdiffError from_kite_err(KiteParseError e) noexcept {
    switch (e) {
        case KiteParseError::BadNumber:  return UdiffError::BadNumber;
        case KiteParseError::BadDecimal: return UdiffError::BadDecimal;
        case KiteParseError::BadDate:    return UdiffError::BadDate;
        case KiteParseError::Overflow:   return UdiffError::Overflow;
        default:                         return UdiffError::BadNumber;
    }
}

/// Trim ASCII spaces from both ends. UDiFF itself is unpadded, but the NSE
/// family of files is not consistent about it and a stray space in a symbol
/// silently breaks P1-06's key match.
[[nodiscard]] inline KiteField udiff_trim(KiteField f) noexcept {
    while (f.n > 0 && f.p[0] == ' ') { ++f.p; --f.n; }
    while (f.n > 0 && f.p[f.n - 1] == ' ') { --f.n; }
    return f;
}

} // namespace detail

/// Resolve columns BY NAME. UDiFF has 34 columns and has gained them over
/// time; position is not a contract.
[[nodiscard]] inline std::expected<UdiffColumns, UdiffError>
parse_udiff_header(const char* header, std::size_t len) noexcept {
    if (header == nullptr || len == 0) {
        return std::unexpected(UdiffError::BadHeader);
    }
    detail::KiteField f[64];
    const std::size_t n = detail::kite_split(header, len, f, 64);
    if (n < 2 || n > 64) {
        return std::unexpected(UdiffError::BadHeader);
    }

    UdiffColumns c{};
    for (std::size_t i = 0; i < n; ++i) {
        const detail::KiteField t = detail::udiff_trim(f[i]);
        const int idx = static_cast<int>(i);
        if (detail::kite_field_is(t, "FinInstrmTp"))       { c.fin_instrm_tp = idx; }
        else if (detail::kite_field_is(t, "TckrSymb"))     { c.tckr_symb = idx; }
        else if (detail::kite_field_is(t, "FinInstrmNm"))  { c.fin_instrm_nm = idx; }
        else if (detail::kite_field_is(t, "XpryDt"))       { c.xpry_dt = idx; }
        else if (detail::kite_field_is(t, "StrkPric"))     { c.strk_pric = idx; }
        else if (detail::kite_field_is(t, "OptnTp"))       { c.optn_tp = idx; }
        else if (detail::kite_field_is(t, "NewBrdLotQty")) { c.lot_qty = idx; }
    }
    // Every one of these is load-bearing. NewBrdLotQty especially: without it
    // this file is not the primary authority for anything.
    if (c.fin_instrm_tp < 0 || c.tckr_symb < 0 || c.fin_instrm_nm < 0
        || c.xpry_dt < 0 || c.strk_pric < 0 || c.optn_tp < 0 || c.lot_qty < 0) {
        return std::unexpected(UdiffError::BadHeader);
    }
    return c;
}

/// One row -> one ContractSpec.
///
/// `exchange` is a PARAMETER and is stamped unconditionally. It selects the
/// charge schedule downstream, so a BSE contract labelled NSE is priced with
/// the wrong exchange transaction charge and every net-of-cost signal built on
/// it is wrong in the same direction.
[[nodiscard]] inline std::expected<ContractSpec, UdiffError>
parse_udiff_row(const char* row, std::size_t len, const UdiffColumns& cols,
                Exchange exchange, Timestamp snapshot_at) noexcept {
    if (row == nullptr || len == 0) {
        return std::unexpected(UdiffError::TooFewFields);
    }
    detail::KiteField f[64];
    const std::size_t n = detail::kite_split(row, len, f, 64);
    const int need = cols.lot_qty;
    if (n <= static_cast<std::size_t>(need)) {
        return std::unexpected(UdiffError::TooFewFields);
    }

    const detail::KiteField tp   = detail::udiff_trim(f[cols.fin_instrm_tp]);
    const detail::KiteField sym  = detail::udiff_trim(f[cols.tckr_symb]);
    const detail::KiteField nm   = detail::udiff_trim(f[cols.fin_instrm_nm]);
    const detail::KiteField xp   = detail::udiff_trim(f[cols.xpry_dt]);
    const detail::KiteField sk   = detail::udiff_trim(f[cols.strk_pric]);
    const detail::KiteField ot   = detail::udiff_trim(f[cols.optn_tp]);
    const detail::KiteField lot  = detail::udiff_trim(f[cols.lot_qty]);

    ContractSpec s{};
    s.id = InstrumentId::Invalid;
    s.exchange = exchange;
    s.price_scale = 100;
    s.source = SpecSource::NseMaster;
    if (exchange == Exchange::BSE) {
        s.source = SpecSource::BseMaster;
    }
    s.snapshot_at = snapshot_at;
    s.valid_from = snapshot_at;
    s.valid_to = Timestamp::max();
    s.stale = false;

    // FinInstrmTp. An unrecognised value BLOCKS rather than defaulting: every
    // unknown row would otherwise become a tradable future at some guessed
    // segment (rule 9).
    if (detail::kite_field_is(tp, "IDF") || detail::kite_field_is(tp, "STF")) {
        s.segment = Segment::Fut;
        s.opt_type = OptionType::None;
    } else if (detail::kite_field_is(tp, "IDO")
               || detail::kite_field_is(tp, "STO")) {
        s.segment = Segment::Opt;
    } else {
        return std::unexpected(UdiffError::UnknownInstrumentType);
    }

    if (sym.n == 0 || sym.n > kMaxUnderlyingLen) {
        return std::unexpected(UdiffError::BadSymbol);
    }
    if (nm.n == 0 || nm.n > kMaxSymbolLen) {
        return std::unexpected(UdiffError::BadSymbol);
    }
    std::memcpy(s.underlying, sym.p, sym.n);
    s.underlying[sym.n] = '\0';
    std::memcpy(s.symbol, nm.p, nm.n);
    s.symbol[nm.n] = '\0';

    // Expiry. Same ISO grammar as Kite, so the same proven converter.
    const auto xpry = parse_kite_expiry(xp.p, xp.n);
    if (!xpry) {
        return std::unexpected(detail::from_kite_err(xpry.error()));
    }
    s.expiry = *xpry;

    // Futures leave BOTH strike and option type EMPTY -- not "0", not "XX".
    if (s.segment == Segment::Fut) {
        if (sk.n != 0 || ot.n != 0) {
            // A future carrying a strike is not a future. Refuse rather than
            // silently drop the field.
            return std::unexpected(UdiffError::UnknownInstrumentType);
        }
        s.strike = Price{0};
    } else {
        if (detail::kite_field_is(ot, "CE")) {
            s.opt_type = OptionType::CE;
        } else if (detail::kite_field_is(ot, "PE")) {
            s.opt_type = OptionType::PE;
        } else {
            return std::unexpected(UdiffError::UnknownInstrumentType);
        }
        // Decimal rupees -> paise, through P1-04's converter. Never a double:
        // a strike of 0.05 read through one truncates to 4 paise.
        const auto strike = parse_rupees_to_paise(sk.p, sk.n);
        if (!strike) {
            return std::unexpected(detail::from_kite_err(strike.error()));
        }
        s.strike = *strike;
    }

    // The lot size. Rule 1's headline number, and the whole reason this file
    // is primary.
    std::uint64_t lotv = 0;
    if (!detail::kite_parse_u64(lot.p, lot.n, 0x7FFF'FFFFull, lotv)
        || lotv == 0) {
        return std::unexpected(UdiffError::BadNumber);
    }
    s.lot_size = LotSize{static_cast<std::int64_t>(lotv)};

    // No tick-size column in UDiFF. Left ZERO for P1-06 to fill from a broker
    // rather than defaulted to 5 paise, which would be a literal and a guess.
    s.tick_size = Price{0};

    s.source_hash = detail::kite_fnv1a(row, len);
    return s;
}

/// Load a whole UDiFF bhavcopy into the RECONCILER.
///
/// Feeds the reconciler rather than the store: this file is one opinion about
/// a contract -- an authoritative one, but only P1-06 decides.
[[nodiscard]] inline std::expected<UdiffLoadReport, UdiffError>
load_udiff_master(const char* csv, std::size_t len, Exchange exchange,
                  Reconciler& rec, Timestamp snapshot_at) noexcept {
    if (csv == nullptr || len == 0) {
        return std::unexpected(UdiffError::EmptyInput);
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
    const auto cols = parse_udiff_header(csv + pos, trim_cr(pos, he) - pos);
    if (!cols) {
        return std::unexpected(cols.error());
    }
    pos = he < len ? he + 1 : len;
    if (pos >= len) {
        return std::unexpected(UdiffError::EmptyInput);
    }

    UdiffLoadReport rep{};
    std::size_t row_no = 0;
    while (pos < len) {
        const std::size_t e = line_end(pos);
        const std::size_t stop = trim_cr(pos, e);
        if (stop > pos) {
            ++row_no;
            const auto spec = parse_udiff_row(csv + pos, stop - pos, *cols,
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
        return std::unexpected(UdiffError::EmptyInput);
    }
    return rep;
}

} // namespace altair
