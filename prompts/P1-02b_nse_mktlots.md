# P1-02b — `instruments/nse_mktlots`: `fo_mktlots.csv` → the lot-size table

> Phase 1 · Card 2b of 7 · Status: **BLOCKED — needs one sample file (§2)**
> Depends on: P1-01 DONE · P1-06 DONE · P1-02a (shares the trim/number helpers)
> Feeds: P1-02a (completes its `lot_size`) · P1-06
>
> **Architect's note.** This is a small file and a nasty one. It is the only
> NSE artifact whose shape is **wide** — one row per underlying, one *column
> per expiry month* — so the schema changes every month by construction, and a
> parser that hardcodes column positions is broken on the first rollover. It is
> also the file whose single number is rule 1's headline: lot size is the bug
> that silently scaled the predecessor's every P&L figure.

---

## 1. WHY IT IS SHAPED LIKE THIS

`fo_mktlots.csv` is believed to look roughly like:

```
UNDERLYING,SYMBOL,  MAR-26,  APR-26,  MAY-26
NIFTY 50           ,NIFTY  ,     75,     75,     75
NIFTY BANK         ,BANKNIFTY,   35,     35,     35
```

Three properties follow, and all three are hazards:

- **The columns are expiry months, and they roll.** Next month the first data
  column is `APR-26`. A parser must read the month from the **header** and
  match it against the contract's expiry, never take "column 2" to mean
  "near month".
- **Padding is everywhere** — in the header names and in the values.
- **`UNDERLYING` and `SYMBOL` are different strings.** `NIFTY 50` is the index
  name; `NIFTY` is the trading symbol. P1-06's D1 key uses `underlying`, so
  **which of the two columns feeds `ContractSpec::underlying` decides whether
  this file joins to anything at all.** It must be the one that matches what
  P1-02a and the Kite dump write. §2 settles it.

---

## 2. SCHEMA BLOCK — TO BE FILLED FROM THE SAMPLE FILE

> **BLOCKING.**

Needed: **one** `fo_mktlots.csv`, any recent month. `data/masters/nse/`.

1. The exact header line, byte for byte, **including padding**.
2. The month-column format — `MAR-26`, `Mar-26`, `MAR-2026`, `26-MAR`?
3. Whether `UNDERLYING` or `SYMBOL` matches what the F&O master and the Kite
   dump use as the underlying. **Cross-check against a real Kite row** — if
   they disagree, this card must carry an explicit mapping and say so.
4. What a *non-participating* underlying looks like in a month column: empty,
   `0`, `-`?
5. Whether the file contains any trailing notes/footer rows. NSE files
   sometimes do, and a footer parsed as data becomes a phantom instrument.

---

## 3. FILE MANIFEST

```
CREATE   instruments/nse_mktlots.hpp
CREATE   instruments/tests/test_nse_mktlots.cpp
MODIFY   instruments/CMakeLists.txt
```

`add_test(NAME nse_mktlots COMMAND altair_nse_mktlots_test)`.

---

## 4. INTERFACE CONTRACT

```cpp
#pragma once

#include <instruments/contract_spec.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

inline constexpr std::size_t kMaxLotUnderlyings = 512;
inline constexpr std::size_t kMaxLotMonths = 12;

enum class MktLotsError : std::uint8_t {
    EmptyInput, BadHeader, BadMonth, BadNumber, BadSymbol, TooMany, NotFound
};

/// A (year, month) expiry bucket. Not a Timestamp: this file names a MONTH,
/// not an instant, and pretending otherwise would invent a day-of-month that
/// the file does not contain.
struct ExpiryMonth {
    std::int32_t year;    // 2026
    std::uint8_t month;   // 1..12
    friend constexpr auto operator<=>(ExpiryMonth, ExpiryMonth) noexcept = default;
    friend constexpr bool operator==(ExpiryMonth, ExpiryMonth) noexcept = default;
};

/// Lot size by (underlying, expiry month). Fixed storage, no allocation.
/// ~64 KB — safe as a member, not a stack local.
class MktLotsTable {
public:
    MktLotsTable() noexcept;
    MktLotsTable(const MktLotsTable&) = delete;
    MktLotsTable& operator=(const MktLotsTable&) = delete;

    /// UNIT: units. Returns NotFound when this underlying does not trade that
    /// month — which is a real answer, not an error to paper over.
    [[nodiscard]] std::expected<LotSize, MktLotsError>
    lot_for(const char* underlying, ExpiryMonth m) const noexcept;

    [[nodiscard]] std::size_t underlyings() const noexcept;
    [[nodiscard]] std::size_t months() const noexcept;
    void clear() noexcept;

private:
    // the implementation's own
};

/// The month a Timestamp expiry falls in, in IST. UNIT: none.
/// An expiry is an IST midnight (P0-04), so the month must be read in IST —
/// reading it in UTC moves a 1st-of-month expiry back into the previous month.
[[nodiscard]] inline ExpiryMonth ist_expiry_month(Timestamp expiry) noexcept;

[[nodiscard]] inline std::expected<void, MktLotsError>
load_mktlots(const char* csv, std::size_t len, MktLotsTable& out) noexcept;

} // namespace altair
```

---

## 5. REQUIREMENTS

1. Month columns are resolved **from the header**, by parsing each month name
   into an `ExpiryMonth`. Column position carries no meaning.
2. A two-digit year in the header (`MAR-26`) resolves to 2000 + yy. State the
   window in a comment; this file will outlive the assumption.
3. `ist_expiry_month` reads the month **in IST**. An expiry at IST midnight on
   the 1st is 18:30 UTC on the *previous day*, so a UTC reading moves it into
   the previous month and every 1st-of-month expiry gets the wrong lot size.
   Use P0-02's `ist_ns_since_midnight` machinery; do not re-derive it.
4. A blank or `-` month cell means "does not trade that month" and produces
   `NotFound` from `lot_for`, never `LotSize{0}`.
5. Underlying comparison is case-sensitive after trimming, and must match what
   §2.3 established. If a mapping is needed, it is a **table in this header**,
   not a rule applied at the call site.
6. A footer or short row is skipped, not parsed. §2.5 defines what one looks
   like.
7. More than `kMaxLotUnderlyings` or `kMaxLotMonths` is `TooMany`, never a
   silent truncation.
8. No allocation, no exceptions, no floating point. Lot sizes are integers.

---

## 6. ACCEPTANCE TESTS

1. **`header_months_parse`** — the real header resolves to the right
   `ExpiryMonth` set; a **reordered** header gives the same mapping; a header
   with an unparseable month is `BadMonth`.
2. **`rollover_does_not_shift_lookup`** — build a table from a header starting
   `MAR-26`, then one starting `APR-26`, and assert the same
   `(underlying, month)` query returns the same lot from both. This is the
   whole point of resolving by name and the test that catches a positional
   parser.
3. **`ist_month_boundary`** — an expiry at IST midnight on the **1st** reports
   that month, not the previous one. Construct it from P0-02 and assert the UTC
   instant really is 18:30 the day before, so the test proves the hazard exists
   before proving it is handled.
4. **`absent_month_is_not_zero`** — a blank cell gives `NotFound`;
   `LotSize{0}` is never produced.
5. **`padding_and_case`** — padded header names and padded values parse;
   a value with an embedded space is `BadNumber`.
6. **`footer_rows_skipped`** — a file with §2.5's footer loads the right
   number of underlyings and no phantom.
7. **`capacity`** — over `kMaxLotUnderlyings` is `TooMany` and the table is
   left usable, not half-filled.
8. **`completes_a_fo_master_row`** — take a P1-02a spec with
   `lot_size == 0`, complete it from this table, and assert the result
   reconciles against a Kite spec as `Agreed` rather than `ValueConflict`.
   This is the card's actual job and the only test that proves the two halves
   fit together.

---

## 7. WHAT WILL GO WRONG IF YOU RUSH

- **Taking column 2 as "near month".** Correct for exactly one month.
- **Reading the expiry month in UTC.** Silently wrong for every 1st-of-month
  expiry, and NSE has moved expiries to varied weekdays, so this is not rare.
- **Joining on the wrong one of `UNDERLYING` / `SYMBOL`.** The table then
  matches nothing, `lot_for` returns `NotFound` everywhere, and P1-02a's specs
  keep `lot_size == 0` — which requirement 7 of P1-02a is what stops becoming
  a disaster.
