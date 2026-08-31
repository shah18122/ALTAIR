# P1-04 — `instruments/kite_dump`: the Kite instruments CSV → `ContractSpec`

> Phase 1 · Card 4 of 7 · Status: TODO
> Depends on: P1-01 (`instruments/contract_spec.hpp`) — DONE
> Paste everything below the line into DeepSeek V4 as a single prompt.
>
> **Architect's note (not part of the prompt).** The schema below is not guessed
> — it is Zerodha's own, read from `gokiteconnect/market.go`:
>
> ```go
> type Instrument struct {
>     InstrumentToken int         `csv:"instrument_token"`
>     ExchangeToken   int         `csv:"exchange_token"`
>     Tradingsymbol   string      `csv:"tradingsymbol"`
>     Name            string      `csv:"name"`
>     LastPrice       float64     `csv:"last_price"`
>     Expiry          models.Time `csv:"expiry"`
>     StrikePrice     float64     `csv:"strike"`
>     TickSize        float64     `csv:"tick_size"`
>     LotSize         float64     `csv:"lot_size"`
>     InstrumentType  string      `csv:"instrument_type"`
>     Segment         string      `csv:"segment"`
>     Exchange        string      `csv:"exchange"`
> }
> ```
>
> Two things about it drive this card. **The numeric fields are rupees as
> decimal text** — `tick_size` reads `0.05`, `strike` reads `25000`. They must
> become integer paise, and the conversion is where a silent 100x lives. And
> **`expiry` is a zoneless date string**, which the Go client parses with
> `ParseInLocation(..., Asia/Kolkata)` — so it is IST wall-clock, exactly the
> `ist_naive` case P0-04 was built for.
>
> This card **parses only**. It does not download: the dump is public and
> unauthenticated at `https://api.kite.trade/instruments`, but fetching is
> P2's problem and a Phase 1 parser that opens a socket is a Phase 1 parser
> that needs credentials it should not have.

---

## 1. CONTEXT

You are implementing the Kite instruments parser of Altair, a C++23 low-latency
trading engine for Indian equity markets.

Kite publishes its whole tradable universe as one CSV, refreshed daily. It is
the **primary source for Kite tokens** and a **cross-check** for lot size, tick
size and expiry — ROADMAP §6.1 makes the NSE and BSE exchange masters primary
for those, precisely so a broker's convenience copy can never quietly become
the authority.

Everything here is rule 1: **no lot size, tick size, strike step or expiry is
ever a literal.** This card turns text into `ContractSpec`, and every number it
produces is one that would otherwise have been hardcoded.

Two conversions carry all the risk.

**Rupees to paise.** The CSV says `0.05`; the engine needs `Price{5}`. Parsing
that through `double` and multiplying by 100 gives `5.000000000000001` on some
inputs and `4.999999999999999` on others, and `static_cast<int64_t>` turns the
second into **4**. A tick size of 4 paise instead of 5 makes every rounded
order price wrong. Parse the decimal as text and scale exactly.

**Dates.** `expiry` is a zoneless `YYYY-MM-DD`, meaning IST. Read as UTC it
lands 5 h 30 m early — and on expiry day that is the difference between a
contract that is live and one that is not.

---

## 2. FILE MANIFEST

Create exactly these two files, and modify exactly one.

```
CREATE   instruments/kite_dump.hpp
CREATE   instruments/tests/test_kite_dump.cpp
MODIFY   instruments/CMakeLists.txt
```

Add `altair_kite_dump_test` from `tests/test_kite_dump.cpp` with
`add_test(NAME kite_dump COMMAND altair_kite_dump_test)`. `altair_instruments`
stays INTERFACE; leave the `altair_spec_store_test` registration alone.

**Do not touch** `instruments/contract_spec.hpp`, the root `CMakeLists.txt`,
`vcpkg.json`, or any other directory. **One component, one directory**
(CLAUDE.md): this card lives entirely in `instruments/`.

---

## 3. INTERFACE CONTRACT

```cpp
#pragma once

#include <instruments/contract_spec.hpp>
#include <time/timestamp.hpp>
#include <types/units.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

/// Why a row could not be turned into a ContractSpec.
enum class KiteParseError : std::uint8_t {
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
/// matter — only presence. Kite has reordered this CSV before.
struct KiteColumns {
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

/// Parse a decimal rupee string into integer paise, EXACTLY.
/// UNIT: text in, paise out. `"0.05"` -> 5, `"25000"` -> 2'500'000,
/// `"-1.5"` -> -150. Never goes through double: `0.05 * 100` is not 5 on
/// binary floating point, and a truncating cast turns it into 4.
/// Returns BadNumber for non-numeric text, BadDecimal for more than two
/// decimal places, Overflow past int64 paise.
/// PRECONDITION: `text` need not be NUL-terminated; `len` bounds it.
[[nodiscard]] inline std::expected<Price, KiteParseError>
parse_rupees_to_paise(const char* text, std::size_t len) noexcept;

/// Parse a zoneless `YYYY-MM-DD` as an IST date, returned as a UTC instant at
/// IST midnight. UNIT: text in, ns since the Unix epoch out.
/// An EMPTY string is not an error — cash instruments have no expiry — and
/// yields Timestamp::epoch().
/// Kite writes this zoneless and its own client parses it in Asia/Kolkata, so
/// reading it as UTC lands 5 h 30 m early. On expiry day that is the
/// difference between a live contract and a dead one.
[[nodiscard]] inline std::expected<Timestamp, KiteParseError>
parse_kite_expiry(const char* text, std::size_t len) noexcept;

/// Resolve column positions from the CSV header line.
/// UNIT: none. Returns BadHeader when a required column is absent.
/// PRECONDITION: `header` is one line, without its newline.
[[nodiscard]] inline std::expected<KiteColumns, KiteParseError>
parse_kite_header(const char* header, std::size_t len) noexcept;

/// Turn one data row into a ContractSpec.
/// UNIT: none. `snapshot_at` stamps the spec's provenance; `valid_from` is set
/// to it and `valid_to` to Timestamp::max().
/// Sets source to SpecSource::KiteDump and price_scale to 100, and leaves the
/// XTS token zero — this source knows nothing about XTS.
/// PRECONDITION: `cols` came from parse_kite_header on the same file.
[[nodiscard]] inline std::expected<ContractSpec, KiteParseError>
parse_kite_row(const char* row, std::size_t len,
               const KiteColumns& cols, Timestamp snapshot_at) noexcept;

/// Outcome of loading a whole dump.
struct KiteLoadReport {
    /// Rows that produced a spec and were added. UNIT: rows.
    std::size_t added;
    /// Rows the store refused — a duplicate token, or Full. UNIT: rows.
    std::size_t rejected_by_store;
    /// Rows that would not parse. UNIT: rows.
    std::size_t unparseable;
    /// The first parse error seen, for diagnosis. Only meaningful when
    /// unparseable > 0.
    KiteParseError first_error;
    /// The row number of that first error, 1-based, header excluded.
    std::size_t first_error_row;
};

/// Parse a whole in-memory dump and add every row to the store.
/// UNIT: none. Skips rows that will not parse and KEEPS GOING — one malformed
/// contract must not cost the session its whole universe — but counts them,
/// and the caller decides whether the count is tolerable.
/// PRECONDITION: `csv` is the complete file, header first. Not NUL-terminated
/// is fine; `len` bounds it. Allocates nothing.
[[nodiscard]] inline std::expected<KiteLoadReport, KiteParseError>
load_kite_dump(const char* csv, std::size_t len,
               SpecStore& store, Timestamp snapshot_at) noexcept;

} // namespace altair
```

---

## 4. BEHAVIOURAL SPEC

1. `parse_rupees_to_paise` handles an optional leading `-`, then digits, then
   optionally `.` and **at most two** more digits. `"0.05"` → 5, `"0.5"` → 50,
   `"25000"` → 2'500'000, `"1.23"` → 123, `"-1.5"` → -150. Three or more
   decimal places is `BadDecimal`.
2. **It never uses `double`, `atof`, `strtod`, or `std::from_chars` on a
   floating type.** Accumulate the integer part, then pad the fraction to
   exactly two digits. `0.05 * 100` is `5.000000000000000277…` in binary
   floating point and a truncating cast of the neighbouring case gives 4.
3. An empty numeric field is `BadNumber`, not zero. A field of only `"-"`,
   only `"."`, or containing any non-digit is `BadNumber`.
4. `parse_kite_expiry` accepts exactly `YYYY-MM-DD`. It computes days since the
   Unix epoch with a civil-date algorithm, takes IST midnight of that date, and
   returns the corresponding UTC instant — i.e. `days * 86400 s - 19800 s`.
   **Subtract the IST offset**: IST midnight is 18:30 UTC the previous day.
5. An **empty** expiry yields `Timestamp::epoch()` and is not an error. Cash
   instruments have none. A malformed non-empty expiry is `BadDate`.
6. `parse_kite_header` matches column names case-sensitively against the Go
   struct's tags and records their index. **Column order must not matter** —
   Kite has reordered this file before. All eleven are required; any missing
   one is `BadHeader`.
7. `parse_kite_row` maps `exchange`: `"NSE"`/`"NFO"`/`"CDS"` → `Exchange::NSE`,
   `"BSE"`/`"BFO"`/`"BCD"` → `Exchange::BSE`. Anything else is
   `UnknownExchange`. It maps `instrument_type`: `"CE"` → `OptionType::CE`,
   `"PE"` → `OptionType::PE`, anything else → `OptionType::None`.
8. `parse_kite_row` derives `segment` from the exchange string, not from the
   CSV's own `segment` column, which carries values like `"NFO-OPT"`:
   `"NSE"`/`"BSE"` → `Cash`, `"NFO"`/`"BFO"` → `Fut` unless
   `instrument_type` is CE or PE in which case `Opt`, `"CDS"`/`"BCD"` →
   `Currency`.
9. `parse_kite_row` sets `token[Kite]` from `instrument_token`,
   `token[Xts]` to **0**, `price_scale` to **100**, `source` to
   `SpecSource::KiteDump`, `stale` to false, `valid_from` to `snapshot_at`,
   `valid_to` to `Timestamp::max()`, and `snapshot_at` to `snapshot_at`.
10. `parse_kite_row` computes `source_hash` from the row's own bytes, so two
    identical rows hash identically and any edit moves it. Use the FNV-1a in
    `contract_spec.hpp`'s neighbourhood or an equivalent; do not invent a
    second hash family.
11. `load_kite_dump` skips the header, parses each subsequent non-empty line,
    and on a parse failure **increments `unparseable` and continues**. One
    malformed contract must not cost the session its whole universe. The first
    error and its row number are recorded for diagnosis.
12. `load_kite_dump` allocates nothing and returns `EmptyInput` for an empty or
    header-only file. A row the store refuses (duplicate token, Full) counts in
    `rejected_by_store` and is not an error.

---

## 5. CONSTRAINTS

- C++23. Standard library plus P0-01, P0-02 and P1-01 headers.
  **No new dependency.**
- Header-only.
- **No `<fstream>`, no `<cstdio>`, no network.** This card parses a buffer the
  caller already has. Fetching is P2's problem, and a Phase 1 parser that opens
  a socket is one that needs credentials it should not have.
- **No `double`, `float`, `atof`, `strtod`, or `std::stod` anywhere.** See
  item 2 — this is the card's central hazard.
- No `std::string`, `std::vector`, or any allocation. Fields are
  `(const char*, size_t)` views into the caller's buffer.
- No exceptions, no `throw`.
- Internal helpers in `namespace altair::detail`, never an anonymous namespace.
  Name them distinctly from the existing members (`spec_mix64`,
  `spec_str_len`, `config_mix64`, `ledger_add_overflows`, …).
- No `using namespace` at file scope.
- Compiles clean at `/W4` and `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion -Wold-style-cast`.

---

## 6. ACCEPTANCE TESTS

`instruments/tests/test_kite_dump.cpp`, plain `main()`,
`check(bool, const char*)`, returns 0 only if all pass. Exactly these names:

```cpp
void test_kite_rupees_to_paise();
void test_kite_rupees_rejects_bad_input();
void test_kite_expiry_is_ist();
void test_kite_header_column_order_independent();
void test_kite_row_to_spec();
void test_kite_row_rejects_bad_input();
void test_kite_load_dump_skips_bad_rows();
void test_kite_load_dump_into_store();
```

**test_kite_rupees_to_paise**
The one the whole card turns on.
```
"0.05"    -> Price{5}          // the tick size that a double turns into 4
"0.5"     -> Price{50}
"0.50"    -> Price{50}
"1"       -> Price{100}
"25000"   -> Price{2'500'000}
"25000.5" -> Price{2'500'050}
"1.23"    -> Price{123}
"0"       -> Price{0}
"0.00"    -> Price{0}
"-1.5"    -> Price{-150}
"-0.05"   -> Price{-5}
".5"      -> Price{50}         // a leading dot is legal
"5."      -> Price{500}        // a trailing dot is legal

// Every hundredth from 0.01 to 1.00 round-trips EXACTLY. This is the loop a
// double implementation fails somewhere in the middle.
for i in 1..100:  "<i/100 as text>" -> Price{i}
```

**test_kite_rupees_rejects_bad_input**
```
""        -> BadNumber        // empty is NOT zero
"-"       -> BadNumber
"."       -> BadNumber
"abc"     -> BadNumber
"1.2.3"   -> BadNumber
"1e5"     -> BadNumber        // no exponent form
"1 2"     -> BadNumber
"0.001"   -> BadDecimal       // three places: finer than a paisa
"1.234"   -> BadDecimal
"99999999999999999999" -> Overflow
```

**test_kite_expiry_is_ist**
```
// IST midnight is 18:30 UTC the PREVIOUS day.
"2026-08-28" -> Timestamp{1787875200000000000 - 19'800'000'000'000}
// cross-check: that instant's ist_ns_since_midnight() is 0
ist_ns_since_midnight(parse_kite_expiry("2026-08-28")) == 0

"1970-01-01" -> Timestamp{-19'800'000'000'000}    // IST midnight, pre-epoch
""           -> Timestamp::epoch()                 // cash: not an error

"2026-8-28"   -> BadDate      // no zero padding
"2026/08/28"  -> BadDate
"28-08-2026"  -> BadDate
"2026-13-01"  -> BadDate      // month out of range
"2026-08-32"  -> BadDate      // day out of range
"garbage"     -> BadDate
```

**test_kite_header_column_order_independent**
```
The real header, in Kite's documented order:
  "instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,
   strike,tick_size,lot_size,instrument_type,segment,exchange"
  -> every index resolved, and instrument_token == 0, exchange == 11

The SAME columns REVERSED
  -> parses, and instrument_token == 11, exchange == 0
// Order must not matter; Kite has reordered this file before.

A header missing `lot_size` -> BadHeader
An empty header             -> BadHeader
```

**test_kite_row_to_spec**
Use a real-shaped NIFTY option row.
```
row: "12345,678,NIFTY26SEP25000CE,NIFTY,0,2026-09-24,25000,0.05,75,CE,NFO-OPT,NFO"

spec.token[Kite] == 12345
spec.token[Xts]  == 0                       // this source knows no XTS
spec.symbol      == "NIFTY26SEP25000CE"
spec.underlying  == "NIFTY"
spec.strike      == Price{2'500'000}
spec.tick_size   == Price{5}                // 0.05 rupees, NOT 4
spec.lot_size    == LotSize{75}
spec.opt_type    == OptionType::CE
spec.exchange    == Exchange::NSE
spec.segment     == Segment::Opt            // NFO + CE
spec.price_scale == 100
spec.source      == SpecSource::KiteDump
spec.stale       == false
spec.valid_from  == snapshot_at
spec.valid_to    == Timestamp::max()
ist_ns_since_midnight(spec.expiry) == 0     // IST midnight of 2026-09-24

// A futures row: NFO with a non-option type is Fut.
"...,NIFTY26SEPFUT,NIFTY,0,2026-09-24,0,0.05,75,FUT,NFO-FUT,NFO"
  -> segment == Segment::Fut, opt_type == OptionType::None

// A cash row: empty expiry, zero strike.
"...,RELIANCE,RELIANCE INDUSTRIES,0,,0,0.05,1,EQ,NSE,NSE"
  -> segment == Segment::Cash, expiry == Timestamp::epoch()

// The same row twice hashes the same; one edited byte does not.
source_hash(row) == source_hash(same row)
source_hash(row) != source_hash(row with lot_size 75 -> 50)
```

**test_kite_row_rejects_bad_input**
```
a row with too few fields          -> TooFewFields
lot_size "abc"                     -> BadNumber
tick_size "0.001"                  -> BadDecimal
expiry "2026-13-01"                -> BadDate
exchange "MCX"                     -> UnknownExchange
tradingsymbol empty                -> BadSymbol
tradingsymbol 32 chars             -> BadSymbol
tradingsymbol 31 chars             -> parses
```

**test_kite_load_dump_skips_bad_rows**
```
A dump of 5 rows where rows 2 and 4 are malformed (a bad number and a bad
date), embedded in a valid header:

  report.added == 3
  report.unparseable == 2
  report.first_error_row == 2
  report.first_error is the error from row 2
  the three good rows are in the store and resolve by token

// One malformed contract must NOT cost the session its whole universe.
// A dump where EVERY row is bad still returns a report, not an error:
  report.added == 0 && report.unparseable == <n>

An empty buffer      -> EmptyInput
A header-only buffer -> EmptyInput
```

**test_kite_load_dump_into_store**
```
Load a small dump into a real SpecStore, then use it as the engine would:
  store.size() == report.added
  store.id_of(FeedSource::Kite, 12345) resolves
  store.current(id)->lot_size == LotSize{75}
  store.token_of(id, FeedSource::Xts) is NotFound     // Kite-only so far

// A second load of the SAME dump: every row is a duplicate token.
  second.added == 0
  second.rejected_by_store == first.added
  store.size() is unchanged
// The store's DuplicateToken guard is what makes a re-load idempotent.
```

### Latency reporting — not a pass/fail assertion

Batch-timed: `load_kite_dump` over a synthesised 10'000-row dump. Report
rows/second and total wall time. **No budget** — this runs once pre-open, not
on a tick. Print it so a future regression is visible.

---

## 7. FORBIDDEN

- Adding a file not in the manifest. Touching `contract_spec.hpp`, the root
  `CMakeLists.txt`, `vcpkg.json`, or any directory other than `instruments/`.
- Changing any signature in the interface contract.
- **`double`, `float`, `atof`, `strtod`, `std::stod`, or `std::from_chars` on a
  floating type.** Item 2 is the card.
- Reading the expiry as UTC. It is IST; subtract the offset.
- Treating an empty expiry as an error. Cash instruments have none.
- Treating an empty numeric field as zero. It is `BadNumber`.
- Requiring a fixed column order.
- Aborting the whole load on one bad row.
- `std::string`, `std::vector`, allocation, file I/O, or a socket.
- Setting `token[Xts]` to anything but 0 — this source knows nothing about XTS.
- Trusting the CSV's own `segment` column over the exchange string; item 8.
- Writing a test that asserts whatever your implementation happens to produce.

---

## 8. RULES

```
RULES — violating any of these fails review:
1. Produce complete files. No "...", no "rest unchanged", no placeholder bodies.
2. Do not create, rename, or delete any file not in the File Manifest.
3. Do not change any signature in the Interface Contract. If you believe a
   signature is wrong, implement it as specified AND add a comment block at the
   top of the file titled "CONTRACT OBJECTION" explaining why. Do not act on it.
4. Do not add any third-party dependency. Only what vcpkg.json already lists.
5. No exceptions on the hot path. Return std::expected<T, Error>.
6. No dynamic allocation inside any function marked ALTAIR_HOT.
7. No `using namespace` at file scope in a header.
8. Every public function gets a doc comment stating units and preconditions.
9. Write the acceptance tests exactly as named. Do not rename or merge them.
10. If a requirement is ambiguous, implement the most conservative reading and
    list the ambiguity under "ASSUMPTIONS" at the end of your response.
```

Return the three files in full, then your ASSUMPTIONS, stating the measured
rows/second and confirming that no floating-point type appears anywhere.

---

## REVIEW RECORD — P1-04

Reviewed 2026-08-31. Built with MSVC 19.51.36256, `/std:c++latest /W4 /permissive- /O2`.

| Gate | Verdict | Evidence |
|---|---|---|
| 1 compiles clean | PASS (after one fix) | initial build raised **C4146** at `kite_dump.hpp:134` — Hinnant's `m + (m > 2 ? -3 : 9)` spelled with unsigned literals is `-3u`. It wraps back correctly on the add, so every expiry test passed, but it is warned-on and unreadable. Rewritten as an explicit branch. Rebuild: zero warnings. |
| 2 contract honoured | PASS, **card amended** | all five public signatures match byte-for-byte except the implementation adds `inline`. Header-only free functions must be `inline` or two TUs including this header is a duplicate-symbol link error. The **card was under-specified**, not the code wrong — this is the identical defect found in P0-10, and the contract above is amended rather than the code weakened. Internal helpers are confined to `altair::detail`. |
| 3 manifest respected | PASS | `git status` shows exactly `M instruments/CMakeLists.txt`, `?? instruments/kite_dump.hpp`, `?? instruments/tests/test_kite_dump.cpp`. Nothing outside `instruments/`. |
| 4 tests pass | PASS | 92 checks, 8 named tests, zero failures. |
| 5 no hot-path allocation | PASS (n/a) | no `ALTAIR_HOT` marking — this is a pre-open path, correctly. Verified anyway: no `new`/`malloc`/`vector`/`string`/`shared_ptr`/`function` in the file. |
| 6 latency budget | PASS (no budget) | 2.82 M rows/s. Runs once before the open, never on a tick. |
| 7 numerical / financial | PASS | the whole card. **No floating point anywhere in the parser** — the only two occurrences of `double` in the file are in comments explaining why it is absent. `"0.05"` → `Price{5}`, not 4. Every hundredth `0.01..1.00` round-trips exactly. Three-decimal input is rejected as finer than a paisa rather than silently truncated. Past-int64 paise is `Overflow`, not a wrap. Empty expiry is cash, not an error. |
| 8 physics | PASS | expiry is an IST midnight, cross-checked against `ist_ns_since_midnight` from P0-02 rather than against my own arithmetic. Strike and tick carry `Price`; lot size carries `LotSize`; no bare number crosses the boundary. |

### Findings recorded, not fixed here

1. **`kite_dump.hpp` is 504 lines**, over PROTOCOL §8's ~400-line split guide.
   Roughly 40% is comment. It is one cohesive parser and splitting it at the
   `parse_*` seam would put the header parser in a different TU from the row
   parser that consumes its `KiteColumns`, which is worse. Recorded as a
   deliberate overrun, not an oversight.
2. **The throughput test fills the store.** 10,000 rows in, 8,192 added, 1,808
   rejected — `SpecStore::kMaxInstruments` is 8,192 and the real Kite dump is
   ~100,000 rows. `load_kite_dump` loads *everything it is given*. This is
   correct behaviour for this card (it reports `rejected_by_store` rather than
   failing), but it means **nobody may hand it a raw full dump.** The universe
   filter belongs upstream. See LEDGER carried debt.
