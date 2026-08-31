// instruments/nse_fo_master.hpp — NSE F&O contract master → ContractSpec.
//
// P1-02a. ***SKELETON. THE PARSER BODIES ARE NOT IMPLEMENTED.***
//
// Every function here returns `SchemaNotConfigured`. That is deliberate and it
// is not a TODO left lying around: this is ROADMAP §6.1's PRIMARY authority for
// NSE lot size, tick size and expiry, and its byte layout is not yet known.
// Writing it against a guessed schema is the confident-plausible-wrong failure
// gate 7 exists to catch, and no test would find it because the test would
// share the guess. Rule 9: failing loud beats trading wrong.
//
// The interface below is FINAL — decided in prompts/P1-02a_nse_fo_master.md.
// It exists now so that P2-04 and the reconciler compile against the real
// signatures rather than against placeholders that change later.
//
// TO COMPLETE: fill in §2 of the card from one sample file, then implement the
// four parse functions. Nothing in this header's shape should need to move.

#pragma once

#include <instruments/contract_spec.hpp>
#include <instruments/reconcile.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class NseParseError : std::uint8_t {
    EmptyInput,
    BadHeader,
    TooFewFields,
    BadNumber,
    BadDecimal,
    BadDate,
    BadSymbol,
    UnknownInstrumentType,
    MissingLotSize,
    Overflow,
    /// The schema block of P1-02a has not been filled in from a sample file.
    /// Every entry point returns this until it is. NOT a runtime condition to
    /// handle — it means this parser has not been written yet.
    SchemaNotConfigured
};

/// Resolved column positions. -1 for a column this file does not carry.
struct NseFoColumns {
    int instrument = -1;
    int symbol     = -1;
    int expiry     = -1;
    int strike     = -1;
    int option_typ = -1;
    int lot_size   = -1;      // -1 => this file has none; P1-02b must supply it
    int tick_size  = -1;      // -1 => ditto
};

struct NseLoadReport {
    std::size_t added = 0;
    std::size_t unparseable = 0;
    std::size_t missing_lot_size = 0;   // rows needing P1-02b to complete them
    NseParseError first_error = NseParseError::EmptyInput;
    std::size_t first_error_row = 0;
};

/// UNIT: paise. When implemented this must never go through double — see
/// P1-04's parse_rupees_to_paise, which is the reference and must not be
/// reimplemented differently.
[[nodiscard]] inline std::expected<Price, NseParseError>
parse_nse_rupees_to_paise(const char* text, std::size_t len) noexcept {
    (void)text;
    (void)len;
    return std::unexpected(NseParseError::SchemaNotConfigured);
}

/// `DD-MMM-YYYY` (legacy bhavcopy) or `YYYY-MM-DD` (UDiFF) — the sample decides
/// which, and they are different parsers. Guessing fails only where day <= 12,
/// so a smoke test on a 25th passes and the bug ships.
[[nodiscard]] inline std::expected<Timestamp, NseParseError>
parse_nse_expiry(const char* text, std::size_t len) noexcept {
    (void)text;
    (void)len;
    return std::unexpected(NseParseError::SchemaNotConfigured);
}

[[nodiscard]] inline std::expected<NseFoColumns, NseParseError>
parse_nse_fo_header(const char* header, std::size_t len) noexcept {
    (void)header;
    (void)len;
    return std::unexpected(NseParseError::SchemaNotConfigured);
}

/// One row -> one ContractSpec with source == SpecSource::NseMaster.
[[nodiscard]] inline std::expected<ContractSpec, NseParseError>
parse_nse_fo_row(const char* row, std::size_t len, const NseFoColumns& cols,
                 Timestamp snapshot_at) noexcept {
    (void)row;
    (void)len;
    (void)cols;
    (void)snapshot_at;
    return std::unexpected(NseParseError::SchemaNotConfigured);
}

/// Feeds the RECONCILER, not the store. Everything Phase 1 parses is one of
/// several opinions about a contract; only P1-06 decides which is true, and a
/// parser that could write the store directly could bypass the three-way check.
[[nodiscard]] inline std::expected<NseLoadReport, NseParseError>
load_nse_fo_master(const char* csv, std::size_t len,
                   Reconciler& rec, Timestamp snapshot_at) noexcept {
    (void)csv;
    (void)len;
    (void)rec;
    (void)snapshot_at;
    return std::unexpected(NseParseError::SchemaNotConfigured);
}

} // namespace altair
