// instruments/nse_equity_master.hpp — EQUITY_L.csv → cash ContractSpec.
//
// P1-02c. ***SKELETON. THE PARSER BODIES ARE NOT IMPLEMENTED.***
//
// Returns `SchemaNotConfigured` until §2 of
// prompts/P1-02c_nse_equity_master.md is filled in from a sample file.
//
// Two things about this one are already decided and must not drift:
//
//   * Cash has NO expiry, and "no expiry" is `Timestamp::epoch()` — the SAME
//     convention P1-04 already uses. P1-06's D1 key includes expiry, so if the
//     two sources disagree about what "none" is, NIFTY-cash from NSE and
//     NIFTY-cash from Kite become two different contracts that never join.
//   * `SERIES` is a filter, not decoration. `EQ` is rolling settlement; `BE` is
//     trade-to-trade, which does NOT net intraday, so a strategy that buys and
//     sells within a session takes delivery on both legs. Loading `BE` as
//     though it were `EQ` puts instruments in the universe the engine's own
//     position logic is wrong about.

#pragma once

#include <instruments/contract_spec.hpp>
#include <instruments/reconcile.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class EquityMasterError : std::uint8_t {
    EmptyInput, BadHeader, TooFewFields, BadNumber, BadSymbol,
    UnquotedComma, DisallowedSeries, Overflow,
    /// §2 of P1-02c has not been filled in from a sample file.
    SchemaNotConfigured
};

struct EquityColumns {
    int symbol = -1;
    int name = -1;
    int series = -1;
    int listing_date = -1;
    int market_lot = -1;
    int isin = -1;
    int tick_size = -1;      // -1 when the file carries none
};

/// Series admitted to the universe. Anything else is refused by name.
struct SeriesFilter {
    const char* const* allowed = nullptr;   // e.g. {"EQ"}
    std::size_t count = 0;
};

struct EquityLoadReport {
    std::size_t added = 0;
    std::size_t rejected_series = 0;   // refused by the allow-list, on purpose
    std::size_t unparseable = 0;
    EquityMasterError first_error = EquityMasterError::EmptyInput;
    std::size_t first_error_row = 0;
};

[[nodiscard]] inline std::expected<EquityColumns, EquityMasterError>
parse_equity_header(const char* header, std::size_t len) noexcept {
    (void)header;
    (void)len;
    return std::unexpected(EquityMasterError::SchemaNotConfigured);
}

/// One row -> one cash ContractSpec, source == SpecSource::NseMaster.
/// When implemented: expiry is `Timestamp::epoch()`, segment is Cash, and the
/// field splitter must honour double quotes — company names contain commas
/// (`Ltd., The`), and a splitter that ignores them shifts every later column
/// so ISIN lands in the tick field and still parses.
[[nodiscard]] inline std::expected<ContractSpec, EquityMasterError>
parse_equity_row(const char* row, std::size_t len, const EquityColumns& cols,
                 const SeriesFilter& series, Timestamp snapshot_at) noexcept {
    (void)row;
    (void)len;
    (void)cols;
    (void)series;
    (void)snapshot_at;
    return std::unexpected(EquityMasterError::SchemaNotConfigured);
}

[[nodiscard]] inline std::expected<EquityLoadReport, EquityMasterError>
load_equity_master(const char* csv, std::size_t len, const SeriesFilter& series,
                   Reconciler& rec, Timestamp snapshot_at) noexcept {
    (void)csv;
    (void)len;
    (void)series;
    (void)rec;
    (void)snapshot_at;
    return std::unexpected(EquityMasterError::SchemaNotConfigured);
}

} // namespace altair
