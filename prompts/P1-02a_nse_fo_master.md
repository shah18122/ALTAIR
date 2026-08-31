# P1-02a — `instruments/nse_fo_master`: the NSE F&O contract master → `ContractSpec`

> Phase 1 · Card 2a of 7 · Status: **BLOCKED — needs one sample file (§2)**
> Depends on: P1-01 (`contract_spec.hpp`) DONE · P1-06 (`reconcile.hpp`) DONE
> Feeds: P1-06 (as the **primary** source for NSE) · P2-04
>
> **Architect's note.** ROADMAP §6.1 makes this the **primary** authority for
> NSE lot size, tick size and expiry — Kite and XTS are cross-checks precisely
> so a broker's convenience copy can never quietly become the authority. That
> makes this the most consequential parser in Phase 1: whatever it says wins
> the D4 precedence contest in P1-06, and every order NSE-side is sized and
> priced by its output.
>
> **This card cannot be implemented until §2 is filled in.** Everything else —
> the interface, the requirements, the tests, the hazards — is decided and
> final. Only the byte layout is missing, and it is missing on purpose: a
> parser written against a guessed schema is the confident-plausible-wrong
> failure gate 7 exists to catch, and **no test can find it, because the test
> would share the guess.**

---

## 1. CONTEXT

NSE does not publish one file that contains everything. It publishes several,
and the contract detail Altair needs is spread across them. That is the first
thing this card has to pin down, and it is why the sample matters more than
usual.

Candidates, and what each is believed to carry:

| Artifact | Believed to carry | Believed NOT to carry |
|---|---|---|
| **UDiFF Bhavcopy** — `BhavCopy_NSE_FO_0_0_0_<YYYYMMDD>_F_0000.csv.gz` (the post-July-2024 format) | contract identity, expiry, strike, option type, **and** lot size + tick size in the newer column set | — |
| **Legacy F&O Bhavcopy** — `fo<DDMMMYYYY>bhav.csv.zip` | `INSTRUMENT, SYMBOL, EXPIRY_DT, STRIKE_PR, OPTION_TYP` + OHLC/OI | **no lot size, no tick size** |
| **`fo_mktlots.csv`** | lot size per underlying per expiry month | no strike, no tick, no per-contract row |

If the UDiFF file carries lot and tick, this card is self-sufficient and P1-02b
becomes a pure cross-check. If it does not, lot size **must** come from
P1-02b's `fo_mktlots.csv` and this card produces specs with `lot_size` unset,
which P1-06 then fills. **The sample file decides which, and I will not guess.**

---

## 2. SCHEMA BLOCK — TO BE FILLED FROM THE SAMPLE FILE

> **BLOCKING.** Do not implement past this point until this section is replaced
> with the real thing.

Needed: **one** NSE F&O contract master or UDiFF bhavcopy, any recent trading
day, uncompressed. Drop it at `data/masters/nse/` (gitignored).

What will be read off it, and why each matters:

1. **The exact header line, byte for byte.** Column order, spelling, case, and
   whether there is leading/trailing whitespace. NSE files are notorious for
   `  MAR-24 ` with padding, and a parser that trims when it should not — or
   does not when it should — silently fails to match a column.
2. **Whether the file is comma-separated or pipe-separated**, and whether any
   field is quoted.
3. **The expiry date format.** The legacy bhavcopy writes `25-JUL-2024`
   (`DD-MMM-YYYY`, month abbreviated, upper case). UDiFF is believed to write
   `2024-07-25`. **These are different parsers.** Getting this wrong does not
   fail loudly — it fails on the ~8% of dates where day ≤ 12 and the two
   readings are both valid but different, which is the worst possible failure
   mode.
4. **Whether numbers are rupees or paise, and how many decimals.** `STRIKE_PR`
   as `25000` vs `25000.00` vs `2500000`. Same hazard as P1-04.
5. **The `INSTRUMENT` vocabulary** — the full set of values present
   (`FUTIDX`, `FUTSTK`, `OPTIDX`, `OPTSTK`, `FUTIVX`, …). This maps to
   `Segment` + `OptionType` and an unrecognised value must **block**, not
   default (rule 9).
6. **What an empty field looks like** — a futures row's `STRIKE_PR` and
   `OPTION_TYP`: empty, `0`, `-`, or `XX`.
7. **Whether lot size and tick size are present at all**, and under what
   column names. This decides §1's open question.

---

## 3. FILE MANIFEST

```
CREATE   instruments/nse_fo_master.hpp
CREATE   instruments/tests/test_nse_fo_master.cpp
MODIFY   instruments/CMakeLists.txt
```

`add_test(NAME nse_fo_master COMMAND altair_nse_fo_master_test)`.
**Do not touch** `contract_spec.hpp`, `kite_dump.hpp`, `reconcile.hpp`, the
root `CMakeLists.txt`, or any other directory.

---

## 4. INTERFACE CONTRACT

Deliberately the **same shape as P1-04's** `kite_dump.hpp`. Five parsers will
exist by the end of Phase 1, and if they share a shape, P1-06 consumes all of
them identically and a reader who has understood one has understood all five.
Consistency here is a design decision, not an accident.

```cpp
#pragma once

#include <instruments/contract_spec.hpp>
#include <instruments/reconcile.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class NseParseError : std::uint8_t {
    EmptyInput, BadHeader, TooFewFields, BadNumber, BadDecimal,
    BadDate, BadSymbol, UnknownInstrumentType, MissingLotSize, Overflow
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

/// UNIT: paise. Never goes through double — see P1-04.
[[nodiscard]] inline std::expected<Price, NseParseError>
parse_nse_rupees_to_paise(const char* text, std::size_t len) noexcept;

/// `DD-MMM-YYYY` (legacy) or `YYYY-MM-DD` (UDiFF), decided by §2.
/// Returns an IST-midnight instant, the ist_naive case P0-04 was built for.
[[nodiscard]] inline std::expected<Timestamp, NseParseError>
parse_nse_expiry(const char* text, std::size_t len) noexcept;

[[nodiscard]] inline std::expected<NseFoColumns, NseParseError>
parse_nse_fo_header(const char* header, std::size_t len) noexcept;

/// One row -> one ContractSpec with source == SpecSource::NseMaster.
/// lot_size is left ZERO when the file does not carry it; the caller must
/// then complete it from P1-02b before reconciling, and requirement 7 makes
/// shipping an incomplete spec impossible to do by accident.
[[nodiscard]] inline std::expected<ContractSpec, NseParseError>
parse_nse_fo_row(const char* row, std::size_t len, const NseFoColumns& cols,
                 Timestamp snapshot_at) noexcept;

struct NseLoadReport {
    std::size_t added;
    std::size_t unparseable;
    std::size_t missing_lot_size;   // rows needing P1-02b to complete them
    NseParseError first_error;
    std::size_t first_error_row;
};

/// Feeds the RECONCILER, not the store. Everything Phase 1 parses is one of
/// several opinions about a contract; only P1-06 decides which is true, and a
/// parser that could write the store directly is a parser that could bypass
/// the three-way check.
[[nodiscard]] inline std::expected<NseLoadReport, NseParseError>
load_nse_fo_master(const char* csv, std::size_t len,
                   Reconciler& rec, Timestamp snapshot_at) noexcept;

} // namespace altair
```

---

## 5. REQUIREMENTS

1. **No floating point anywhere.** The only permitted mentions of `double` are
   comments explaining its absence. P1-04's `parse_rupees_to_paise` is the
   reference; do not reimplement it differently.
2. Header parsing resolves columns **by name**, never by position. NSE has
   changed column order between formats and will again.
3. Leading and trailing whitespace is stripped from every field before
   comparison, and the header comparison is **case-insensitive**. This is not
   optional: NSE pads.
4. An `INSTRUMENT` value not in the vocabulary from §2.5 is
   `UnknownInstrumentType`. It is **never** defaulted to a segment. Rule 9.
5. Expiry becomes an IST-midnight `Timestamp`, cross-checked in the tests
   against P0-02's `ist_ns_since_midnight` rather than against this file's own
   arithmetic.
6. A futures row has `opt_type == OptionType::None` and `strike == Price{0}`.
   An options row must have both, and a row with an option type but no strike
   (or the reverse) is `BadNumber` — a half-identified option is not a
   contract.
7. When the file carries no lot size, `lot_size` is `LotSize{0}` and the row is
   counted in `missing_lot_size`. **A spec with `lot_size == 0` must never reach
   P1-06 as authoritative** — requirement 9 of P1-06 would otherwise let a zero
   win a precedence contest against a correct broker value, and a zero lot size
   is rule 1's exact failure mode with the sign flipped.
8. `source` is `SpecSource::NseMaster`; `exchange` is `Exchange::NSE`.
   `snapshot_at` is the caller's; `stale` is false here — staleness is P1-08's
   decision, not the parser's.
9. `source_hash` is a hash of the row's own bytes, so P1-06's merge can tell
   two snapshots apart.
10. `price_scale` is 100 for all NSE F&O. Currency derivatives are **out of
    scope for this card** and an `INSTRUMENT` indicating one must return
    `UnknownInstrumentType` rather than be parsed with the wrong scale — see
    the P0-01 carried-debt row.
11. No allocation, no exceptions.

---

## 6. ACCEPTANCE TESTS

1. **`rupees_to_paise_exact`** — the P1-04 battery, re-run against this
   parser: `"0.05"` → 5 not 4; every hundredth `0.01..1.00` round-trips
   exactly; three decimals rejected as finer than a paisa; past-int64 is
   `Overflow`.
2. **`expiry_is_an_ist_midnight`** — the real format from §2.3 parses; the
   result is cross-checked against P0-02; the *other* format is **rejected**,
   so a file that silently changes format fails loudly instead of producing
   plausible wrong dates.
3. **`expiry_ambiguity_is_not_silent`** — a date where day ≤ 12 (both readings
   valid) parses to exactly one instant, and the test states which and why.
4. **`header_resolves_by_name`** — the real header parses; a header with the
   columns **reordered** parses to the same logical mapping; padded and
   mixed-case headers parse; a header missing a required column is `BadHeader`.
5. **`instrument_vocabulary`** — every value from §2.5 maps to the right
   `(Segment, OptionType)`; an unknown value is `UnknownInstrumentType` and
   **not** a default.
6. **`futures_and_options_rows`** — a futures row has no strike and no option
   type; an options row has both; a half-identified option is rejected.
7. **`missing_lot_size_is_counted_not_defaulted`** — when the file has no lot
   column, specs come back with `lot_size == 0`, `missing_lot_size` equals the
   row count, and nothing silently substitutes a plausible number.
8. **`load_feeds_the_reconciler`** — a multi-row file loads into a
   `Reconciler`; bad rows are counted and skipped without aborting the load;
   the first error and its row number are reported; adding a Kite spec for the
   same contract produces **one** contract with `NseMaster` winning precedence.

---

## 7. WHAT WILL GO WRONG IF YOU RUSH

- **Guessing the expiry format.** It fails only where day ≤ 12, so a smoke
  test on a 25th passes and the bug ships.
- **Defaulting an unknown `INSTRUMENT` to `Fut`.** Every unrecognised row then
  becomes a tradable future at the wrong scale.
- **Letting `lot_size == 0` through as authoritative.** It is primary, so it
  wins, and every order sized from it is zero — or worse, the size check is
  the thing that divides by it.
- Trimming with `isspace` on a signed `char`. UB on bytes ≥ 0x80, which NSE
  files do contain in company names.
