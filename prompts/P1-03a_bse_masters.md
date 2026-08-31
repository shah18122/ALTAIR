# P1-03a — `instruments/bse_master`: the BSE scrip and contract masters → `ContractSpec`

> Phase 1 · Card 3a of 7 · Status: **BLOCKED — needs one sample file (§2)**
> Depends on: P1-01 DONE · P1-06 DONE · P1-02a/c (shares the helpers)
> Feeds: P1-06 (primary for **BSE**) · P2-04
>
> **Architect's note.** BSE is not "NSE with a different URL", and the places it
> differs are exactly the places that break a parser written by analogy. Three
> of them are structural and one is financial. They are enumerated in §1
> because a DeepSeek prompt that says "like P1-02a but BSE" will get all four
> wrong, confidently.
>
> Cash and derivatives are **one card** here, not two, because BSE publishes
> them in a shared scrip-master shape — but see §3: if the sample shows two
> genuinely different layouts, this splits into P1-03a and P1-03b before
> implementation, and that is a judgement to make **after** reading the file,
> not before.

---

## 1. THE FOUR WAYS BSE IS NOT NSE

**1 — The scrip code is a number, and it is not a Kite token.**
BSE identifies instruments by a numeric `SC_CODE` (500325 is Reliance). It is
stable, it is public, and it is **a third number space** on top of Kite's
`instrument_token` and XTS's `ExchangeInstrumentID`. It goes in
`ContractSpec::symbol`/`underlying` handling only as provenance — it must
**never** be written into `ContractSpec::token[]`, which is indexed by
`FeedSource` and means *broker* token. Writing it there is the P0-09b failure
with a new source, and it would make the store resolve BSE scrip codes as if
they were Kite tokens.

**2 — Sensex options exist and their underlying is not "SENSEX 30".**
BSE's derivatives (SENSEX, BANKEX) have their own lot sizes and weekly
expiries on a different weekday from NSE's. P1-06's D1 key uses `underlying`,
so the string BSE writes must match what Kite writes for the same contract or
nothing joins. §2.4 settles it; this is the same trap as P1-02b's
`UNDERLYING` vs `SYMBOL`, and it has bitten once already.

**3 — BSE cash tick size is not uniform.** BSE has historically used
price-banded tick sizes — finer ticks for low-priced scrips. If the sample
confirms it, `tick_size` is **per-scrip from the file** and any single default
is wrong for part of the universe. Rule 1, with teeth.

**4 — The financial one: STT applies to the sell side, and BSE's charge
schedule is its own.** This card does **not** compute charges — that is
`config/charges.toml` and P3-09 — but it must carry `exchange ==
Exchange::BSE` faithfully on every row, because that field is what later
selects the right schedule. A BSE contract mislabelled NSE is priced with the
wrong exchange transaction charge and every net-of-cost signal built on it is
wrong in the same direction. **Gate 7 is checked here even though this card
does no arithmetic.**

---

## 2. SCHEMA BLOCK — TO BE FILLED FROM THE SAMPLE FILE

> **BLOCKING.**

Needed: BSE's equity scrip master **and**, if separate, its derivatives
contract master. Any recent day. `data/masters/bse/`.

1. Exact header(s), byte for byte. Are cash and derivatives one layout or two?
   (This decides §3.)
2. Delimiter and quoting rules. Whether company names carry commas.
3. `SC_CODE` / instrument-id column name and whether it is zero-padded.
4. **The underlying string for a SENSEX option**, and the same contract's
   `name`/`tradingsymbol` in a real Kite dump row, side by side. Without both
   halves this cannot be settled, and settling it wrong means BSE joins
   nothing.
5. The group/series vocabulary (BSE's `A`, `B`, `T`, `Z`, `SC_GROUP`…) — the
   equivalent of NSE's `SERIES`, and the same blocking rule applies: `T` and
   `Z` do not net intraday.
6. Expiry date format, and the option-type spelling (`CE`/`PE` vs `CA`/`PA` —
   BSE has used the latter).
7. Whether a tick-size column exists and whether it varies by scrip (§1.3).
8. Whether numbers are rupees or paise.

---

## 3. FILE MANIFEST

```
CREATE   instruments/bse_master.hpp
CREATE   instruments/tests/test_bse_master.cpp
MODIFY   instruments/CMakeLists.txt
```

`add_test(NAME bse_master COMMAND altair_bse_master_test)`.

**If §2.1 shows two genuinely different layouts, STOP and split** into
`bse_equity_master` (P1-03a) and `bse_fo_master` (P1-03b) before writing code.
PROTOCOL §8: one header carrying two unrelated formats will exceed 400 lines
and will be the file nobody wants to touch.

---

## 4. INTERFACE CONTRACT

Same shape as P1-02a/c, deliberately.

```cpp
#pragma once

#include <instruments/contract_spec.hpp>
#include <instruments/reconcile.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class BseParseError : std::uint8_t {
    EmptyInput, BadHeader, TooFewFields, BadNumber, BadDecimal, BadDate,
    BadSymbol, UnknownGroup, UnknownOptionType, DisallowedGroup, Overflow
};

struct BseColumns {
    int sc_code = -1;
    int sc_name = -1;
    int group = -1;
    int instrument = -1;     // -1 for a cash-only file
    int expiry = -1;
    int strike = -1;
    int option_typ = -1;
    int lot_size = -1;
    int tick_size = -1;
    int isin = -1;
};

/// Groups admitted to the universe, by name. T and Z do not net intraday.
struct GroupFilter {
    const char* const* allowed;
    std::size_t count;
};

[[nodiscard]] inline std::expected<BseColumns, BseParseError>
parse_bse_header(const char* header, std::size_t len) noexcept;

/// One row -> one ContractSpec, source == SpecSource::BseMaster,
/// exchange == Exchange::BSE. See §1.4: that field is load-bearing.
[[nodiscard]] inline std::expected<ContractSpec, BseParseError>
parse_bse_row(const char* row, std::size_t len, const BseColumns& cols,
              const GroupFilter& groups, Timestamp snapshot_at) noexcept;

struct BseLoadReport {
    std::size_t added;
    std::size_t rejected_group;
    std::size_t unparseable;
    BseParseError first_error;
    std::size_t first_error_row;
};

[[nodiscard]] inline std::expected<BseLoadReport, BseParseError>
load_bse_master(const char* csv, std::size_t len, const GroupFilter& groups,
                Reconciler& rec, Timestamp snapshot_at) noexcept;

} // namespace altair
```

---

## 5. REQUIREMENTS

1. `exchange == Exchange::BSE` on **every** row, unconditionally. §1.4.
2. `SC_CODE` is **never** written to `ContractSpec::token[]`. That array is
   indexed by `FeedSource` and holds broker tokens only. §1.1.
3. Option type accepts exactly the spelling §2.6 establishes and rejects
   everything else as `UnknownOptionType`. Do not accept both `CE` and `CA`
   "to be safe" — accepting a spelling the file does not use hides a format
   change, which is the thing that must fail loudly.
4. Group outside the allow-list increments `rejected_group`, produces no spec,
   and is not an error.
5. `tick_size` is read per row from the file when §2.7 says it varies. No
   single default. §1.3.
6. Cash rows use `Timestamp::epoch()` for expiry — the same convention as
   P1-02c and P1-04, so BSE cash joins across sources.
7. `price_scale` is 100. A currency-derivative instrument type returns
   `UnknownGroup` rather than being parsed at the wrong scale, per the P0-01
   carried-debt row.
8. Field splitting honours quotes, and trims cast to `unsigned char` — the
   P1-02c requirements 1 and 4, for the same reasons.
9. No allocation, no exceptions, no floating point.

---

## 6. ACCEPTANCE TESTS

1. **`exchange_is_always_bse`** — every spec from every row, cash and
   derivative, carries `Exchange::BSE`. Trivial to write, and it is the field
   that selects the charge schedule, so it gets its own test.
2. **`sc_code_never_becomes_a_token`** — after loading, `token[Kite]` and
   `token[Xts]` are both 0 on every spec, and a `SpecStore::id_of(Kite,
   <sc_code>)` lookup returns `NotFound`. This proves the P0-09b class of bug
   is absent rather than merely unwritten.
3. **`sensex_option_joins_a_kite_row`** — the underlying string from §2.4
   reconciles with a real Kite SENSEX option spec into **one** contract with
   `Verdict::Agreed`. If §2.4 was settled wrong this test fails, which is
   exactly what it is for.
4. **`option_type_spelling_is_exact`** — the real spelling parses; the other
   spelling is `UnknownOptionType`.
5. **`group_filter`** — an admitted group loads; `T` and `Z` are counted in
   `rejected_group` and produce nothing.
6. **`per_scrip_tick`** — two scrips with different tick sizes both come back
   with their own value. If §2.7 finds the tick is uniform, this test asserts
   that instead and says so, rather than being deleted.
7. **`bse_cash_has_epoch_expiry`** — as P1-02c test 3, for BSE.
8. **`load_feeds_the_reconciler`** — `added + rejected_group + unparseable`
   equals the data-row count; the first error and row number are reported; bad
   rows do not abort the load.

---

## 7. WHAT WILL GO WRONG IF YOU RUSH

- **Writing `SC_CODE` into `token[]`.** It resolves, it looks right, and the
  first BSE tick maps to whatever Kite instrument shares that number.
- **Assuming NSE's `CE`/`PE`.** Every BSE option row is then rejected, or
  worse, silently accepted as `None` and becomes a future.
- **A single default tick.** Wrong for part of the universe, and the part it is
  wrong for is the low-priced scrips where the tick is the largest fraction of
  the spread.
- **Copying P1-02a and changing the strings.** The four differences in §1 are
  not cosmetic and none of them announce themselves at compile time.
