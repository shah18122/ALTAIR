# P1-02c — `instruments/nse_equity_master`: `EQUITY_L.csv` → cash `ContractSpec`

> Phase 1 · Card 2c of 7 · Status: **BLOCKED — needs one sample file (§2)**
> Depends on: P1-01 DONE · P1-06 DONE
> Feeds: P1-06 (primary for NSE cash) · P2-04
>
> **Architect's note.** The simplest of the five parsers, and the only one that
> produces `Segment::Cash`. Its risk is not complexity — it is that a cash
> instrument has **no expiry**, and P1-06's D1 key includes expiry. Every cash
> row must therefore agree on what "no expiry" is, byte for byte, or NIFTY-cash
> from NSE and NIFTY-cash from Kite become two different contracts that never
> join. P1-04 already fixed the convention: `Timestamp::epoch()`. This card
> must use exactly that and the tests must prove it.

---

## 1. CONTEXT

`EQUITY_L.csv` is NSE's cash security master. It is believed to carry:

```
SYMBOL,NAME OF COMPANY,SERIES,DATE OF LISTING,PAID UP VALUE,MARKET LOT,ISIN NUMBER,FACE VALUE
```

Two things about it matter here.

**`SERIES` is a filter, not decoration.** `EQ` is the normal rolling-settlement
series. `BE` is trade-to-trade — no intraday netting, so a strategy that buys
and sells within a session in a `BE` scrip does not flatten, it takes delivery
on both legs. `BZ` is surveillance. Loading them all as if they were `EQ` puts
instruments in the universe that the engine's own position logic is wrong
about. This card **blocks** anything outside the configured series allow-list.

**`MARKET LOT` is 1 for cash and is still not a literal.** Rule 1 has no
carve-out for "obviously 1".

---

## 2. SCHEMA BLOCK — TO BE FILLED FROM THE SAMPLE FILE

> **BLOCKING.**

Needed: **one** `EQUITY_L.csv`. `data/masters/nse/`.

1. The exact header, byte for byte — spaces inside names (`NAME OF COMPANY`)
   mean naive splitting on whitespace is wrong, and quoting rules matter.
2. Whether any field is **quoted**, and whether a quoted field can contain a
   comma. Company names contain commas (`Ltd., The`). A CSV splitter that does
   not honour quotes will shift every column after it.
3. The full set of `SERIES` values present.
4. `DATE OF LISTING` format.
5. Whether non-ASCII bytes appear in company names (they do, in practice) —
   which decides the trim implementation, see requirement 4.
6. Tick size: does this file carry one? NSE cash tick is believed to be ₹0.05
   for most scrips but **must not be a literal** (rule 1). If the file has no
   tick column, say so and this card leaves `tick_size == Price{0}` for P1-06
   to fill from a broker, exactly as P1-02a does for lot size.

---

## 3. FILE MANIFEST

```
CREATE   instruments/nse_equity_master.hpp
CREATE   instruments/tests/test_nse_equity_master.cpp
MODIFY   instruments/CMakeLists.txt
```

`add_test(NAME nse_equity_master COMMAND altair_nse_equity_master_test)`.

---

## 4. INTERFACE CONTRACT

```cpp
#pragma once

#include <instruments/contract_spec.hpp>
#include <instruments/reconcile.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class EquityMasterError : std::uint8_t {
    EmptyInput, BadHeader, TooFewFields, BadNumber, BadSymbol,
    UnquotedComma, DisallowedSeries, Overflow
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
/// A trade-to-trade or surveillance scrip does not net intraday, so the
/// engine's position logic is wrong about it and it must not be loaded as
/// though it were EQ.
struct SeriesFilter {
    const char* const* allowed;   // e.g. {"EQ"}
    std::size_t count;
};

[[nodiscard]] inline std::expected<EquityColumns, EquityMasterError>
parse_equity_header(const char* header, std::size_t len) noexcept;

/// One row -> one cash ContractSpec, source == SpecSource::NseMaster.
/// expiry is Timestamp::epoch() — the SAME convention P1-04 uses, so cash
/// contracts from different sources share a P1-06 key.
[[nodiscard]] inline std::expected<ContractSpec, EquityMasterError>
parse_equity_row(const char* row, std::size_t len, const EquityColumns& cols,
                 const SeriesFilter& series, Timestamp snapshot_at) noexcept;

struct EquityLoadReport {
    std::size_t added;
    std::size_t rejected_series;   // refused by the allow-list, on purpose
    std::size_t unparseable;
    EquityMasterError first_error;
    std::size_t first_error_row;
};

[[nodiscard]] inline std::expected<EquityLoadReport, EquityMasterError>
load_equity_master(const char* csv, std::size_t len, const SeriesFilter& series,
                   Reconciler& rec, Timestamp snapshot_at) noexcept;

} // namespace altair
```

---

## 5. REQUIREMENTS

1. The field splitter **honours double quotes**, including a comma inside a
   quoted field and a doubled `""` escape. A splitter that does not is the
   single most likely defect in this card and shifts every later column.
2. An unterminated quote is `UnquotedComma`, never a best-effort recovery.
3. `SERIES` outside the allow-list increments `rejected_series` and produces no
   spec. It is not an error — refusing a `BE` scrip is the card working.
4. Trimming casts to `unsigned char` before any `<cctype>` call. Company names
   contain bytes ≥ 0x80 and passing a negative `char` to `isspace` is UB.
5. `segment == Segment::Cash`, `opt_type == OptionType::None`,
   `strike == Price{0}`, and **`expiry == Timestamp::epoch()`** — the P1-04
   convention, not a new one.
6. `price_scale` is 100. `exchange` is `Exchange::NSE`.
7. `lot_size` comes from `MARKET LOT`, parsed, never assumed to be 1.
8. When the file carries no tick column, `tick_size == Price{0}` and P1-06
   fills it. Do not substitute 5 paise.
9. No allocation, no exceptions, no floating point.

---

## 6. ACCEPTANCE TESTS

1. **`quoted_fields`** — a company name containing a comma parses with every
   later column intact; a doubled `""` becomes one quote; an unterminated
   quote is `UnquotedComma`. Assert on the **symbol and ISIN of the row after**
   the quoted field, because that is where a broken splitter shows up.
2. **`series_filter`** — `EQ` is admitted; `BE` and `BZ` are counted in
   `rejected_series` and produce no spec; the allow-list is honoured as given
   rather than hardcoded.
3. **`cash_has_epoch_expiry`** — `expiry.is_epoch()` is true, and a Kite cash
   spec for the same symbol reconciles to **one** contract. This is the card's
   real risk and the test that retires it.
4. **`market_lot_is_parsed`** — a row with `MARKET LOT` of 1 gives
   `LotSize{1}`; a row with any other value gives that value; a non-numeric
   lot is `BadNumber`. Include a scrip whose lot is not 1 if the sample has
   one, and say so if it does not.
5. **`high_byte_names`** — a company name with bytes ≥ 0x80 parses and does
   not trip the trim. Run this build under `/analyze` or ASan if available.
6. **`missing_tick_is_zero_not_five`** — with no tick column,
   `tick_size == Price{0}`; nothing substitutes a plausible default.
7. **`header_by_name`** — reordered and mixed-case headers resolve; a missing
   required column is `BadHeader`.
8. **`load_feeds_the_reconciler`** — a multi-row file loads; bad rows are
   counted and skipped; the first error and row number are reported;
   `added + rejected_series + unparseable` equals the data-row count, so no row
   can vanish unaccounted for.

---

## 7. WHAT WILL GO WRONG IF YOU RUSH

- **Splitting on commas without honouring quotes.** Every column after the
  first comma-bearing company name shifts by one, so ISIN lands in the tick
  field and the numbers still parse. Silent.
- **Using a different "no expiry" than `Timestamp::epoch()`.** Cash contracts
  then never join across sources and every one of them looks single-source.
- **Assuming `MARKET LOT` is 1.** Rule 1 has no exceptions.
