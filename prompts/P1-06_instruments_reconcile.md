# P1-06 — `instruments/reconcile`: three-way agreement, disagreement flags, symbol blocking

> Phase 1 · Card 6 of 7 · Status: TODO
> Depends on: P1-01 (`contract_spec.hpp`) DONE · P1-04 (`kite_dump.hpp`) DONE
> Feeds: P1-02, P1-03, P1-05 (the other two source parsers) · P2-04 (session load)
>
> **Architect's note (not part of the prompt).** This card is implementable now
> even though two of its three input parsers are not. It is pure logic over
> `ContractSpec` values: it does not read a file, open a socket, or know what a
> CSV is. Synthetic three-source inputs exercise every path, and the logic stays
> correct unchanged when P1-02/03/05 land. That is the whole reason it is being
> built ahead of its inputs.
>
> Nine design decisions are **fixed in the contract below** (D9 was added
> during review) and are not open.
> They are recorded here with their reasoning so a later reader knows they were
> chosen rather than defaulted.

---

## 1. CONTEXT

Three authorities describe the same contract:

| Source | Role (ROADMAP §6.1) |
|---|---|
| `NseMaster` / `BseMaster` | **primary** for lot size, tick size, expiry |
| `KiteDump` | cross-check, and the only source of the Kite token |
| `XtsMaster` | cross-check, and the only source of the XTS token |

When they agree, the contract is tradable and the merged spec carries both
broker tokens. When they disagree, **rule 9 applies: failing loud beats trading
wrong.** A lot-size disagreement is not a rounding difference to be averaged —
it is one file being stale, and nobody knows which. Trading the wrong lot size
is precisely the bug that silently scaled the predecessor's every P&L number.

This card is also where the **bidirectional token map** is assembled, which
discharges the P1-01 carried-debt row: the merged spec's `token[Kite]` comes
from the Kite source and `token[Xts]` from the XTS source, so downstream code
maps `(source, broker_token)` to `InstrumentId` through the store and never
compares a token across sources.

---

## 2. THE NINE FIXED DECISIONS

**D1 — The cross-source key is the economic identity tuple, never a token and
never a symbol.**

```
(exchange, segment, underlying, expiry, strike, opt_type)
```

Broker tokens are *different number spaces for the same contract* (P0-09b), so
they cannot join anything. Symbol strings are per-source formatting: Kite writes
`NIFTY25SEP25000CE`, an exchange master writes the components separately. The
tuple is what the contract **is**, independent of who is describing it.

**D2 — Exactly three fields are compared. Everything else is taken from the
winning source.**

Compared: `lot_size`, `tick_size`, `price_scale`. These three, and only these,
change the size or the price of an order. A disagreement in any one blocks.

Not compared: `freeze_qty`, `band_lower`, `band_upper` — brokers frequently
publish zero for these, meaning "not published" rather than "zero", so a
comparison would fire constantly on absence rather than on conflict. They are
taken from the winning source. `expiry`, `strike`, `segment`, `exchange` and
`underlying` cannot conflict because they are in the key; a difference there
makes the rows *different contracts*, which surfaces as a presence problem.

**D3 — Comparison is exact. There is no tolerance.**
All three compared fields are integers. Two integers are equal or they are not.

**D4 — Source precedence for the non-compared fields.**
`Manual` > the primary matching the contract's exchange (`NseMaster` for NSE,
`BseMaster` for BSE) > `KiteDump` > `XtsMaster`. `Manual` wins everything and is
always flagged, per the `SpecSource` comment in P1-01.

**D5 — The reconciler reports; it does not halt, and it does not write the
store.**
It returns verdicts. The caller (P2-04 session startup) decides whether a
session may begin. Reason: session lifecycle is not this component's business,
and `SpecStore::add` already owns capacity and duplicate-token policy — the
reconciler must not duplicate it.

**D6 — A blocked contract is still added to the store, then blocked.**
Never silently dropped. A tick for a blocked instrument must resolve to a
`Blocked` error, which is loud, rather than to `NotFound`, which looks like an
unknown instrument and gets lost in the noise. This is rule 9 and the reason
`SpecStore::block` exists.

**D7 — "No primary was loaded at all" is a property of the run, not of a
contract.**
With only the Kite parser built, *every* contract lacks a primary. Blocking the
entire universe for that would be correct and useless. So the report carries
`primary_source_seen`; when it is false the caller knows the whole run was
cross-checked against nothing, and decides. When it is true, an individual
contract missing its primary **is** blocked — that is a real anomaly.

**D8 — The merged `source_hash` is order-independent.**
`reconcile(kite, xts)` and `reconcile(xts, kite)` must produce byte-identical
specs or rule 10 reproducibility fails. Contributing hashes are combined with a
commutative mix (wrapping sum of `spec_mix64`), not a sequential hash.

**D9 — An unreconciled verdict is never served.** *(Added during review — see
the review record. It is D9 rather than a silent edit because it changes the
contract.)*
`Verdict::Agreed` is ordinal 0, so a default-constructed `ContractVerdict`
claims agreement. Without a guard, **forgetting to call `reconcile()` reads as
"everything agreed, nothing blocked"** — a silent all-clear from the one
component whose entire purpose is to fail loud. `verdict_at` therefore returns
`NotReconciled` before the first `reconcile()` and after any later `add()`.
`apply_to_store` takes a mutable reference and reconciles for itself, removing
the failure mode rather than reporting it through an error type that cannot
name it.

---

## 3. FILE MANIFEST

```
CREATE   instruments/reconcile.hpp
CREATE   instruments/tests/test_reconcile.cpp
MODIFY   instruments/CMakeLists.txt
```

Add `altair_reconcile_test` with `add_test(NAME reconcile COMMAND
altair_reconcile_test)`. **Do not touch** `contract_spec.hpp`, `kite_dump.hpp`,
the root `CMakeLists.txt`, or any other directory. One component, one directory.

---

## 4. INTERFACE CONTRACT

```cpp
#pragma once

#include <instruments/contract_spec.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

inline constexpr std::size_t kSpecSourceCount = 5;   // SpecSource has 5 values

/// Why a contract was accepted or blocked. D3, D6, D7.
enum class Verdict : std::uint8_t {
    Agreed,          // every source present agreed on all three compared fields
    SingleSource,    // only one source described it — accepted, uncorroborated
    MissingPrimary,  // a primary WAS loaded but does not carry this — BLOCK
    NoBroker,        // no broker carries it, so it cannot be routed — BLOCK
    ValueConflict    // sources disagree on lot, tick or scale — BLOCK
};

/// Which of the three compared fields conflicted. D2.
enum class ConflictField : std::uint8_t { None = 0, LotSize, TickSize, PriceScale };

enum class ReconcileError : std::uint8_t {
    Full,          // more distinct contracts than kMaxInstruments
    BadSymbol,     // underlying null, empty, or over-long
    BadSource,     // SpecSource out of range
    OutOfRange,    // verdict_at called past size()
    NotReconciled  // verdicts requested before reconcile(), or stale after add()
};

/// One contract's outcome. Carries enough to debug a conflict at 08:20
/// without re-reading the source files.
struct ContractVerdict {
    ContractSpec merged;                    // the spec to add to the store
    Verdict      verdict;
    ConflictField field;                    // None unless verdict == ValueConflict
    std::uint8_t present_mask;              // bit i set == SpecSource(i) carried it
    std::uint8_t conflict_mask;             // bit i set == source i disagreed
    /// The conflicting values, indexed by SpecSource. Meaningful only where
    /// present_mask has the bit set. UNIT: as the named field.
    std::int64_t values[kSpecSourceCount];
};

[[nodiscard]] constexpr bool verdict_blocks(Verdict v) noexcept {
    return v == Verdict::MissingPrimary || v == Verdict::NoBroker
        || v == Verdict::ValueConflict;
}

struct ReconcileReport {
    std::size_t contracts;         // distinct contracts seen
    std::size_t agreed;
    std::size_t single_source;
    std::size_t blocked;           // == missing_primary + no_broker + conflicts
    std::size_t missing_primary;
    std::size_t no_broker;
    std::size_t conflicts;
    bool primary_source_seen;      // D7 — false means nothing was cross-checked
};

/// Accumulates specs from several sources, then reconciles them.
///
/// Multi-megabyte: owns fixed storage for kMaxInstruments contracts and
/// allocates nothing. NOT a stack object — hold it as a member, like SpecStore.
class Reconciler {
public:
    Reconciler() noexcept;
    Reconciler(const Reconciler&) = delete;
    Reconciler& operator=(const Reconciler&) = delete;

    /// Feed one source's view of one contract. Order-independent (D8).
    /// The same (key, source) twice is last-writer-wins, so re-loading a
    /// source is idempotent rather than a false conflict.
    [[nodiscard]] std::expected<void, ReconcileError>
    add(const ContractSpec& spec) noexcept;

    /// Decide every accumulated contract. Idempotent.
    [[nodiscard]] ReconcileReport reconcile() noexcept;

    [[nodiscard]] std::size_t size() const noexcept;

    /// Verdict i, in insertion order. i < size().
    /// NotReconciled before the first reconcile(), and again after any add()
    /// that followed one — see D9.
    [[nodiscard]] std::expected<const ContractVerdict*, ReconcileError>
    verdict_at(std::size_t i) const noexcept;

    /// False when a verdict would be stale or absent. D9.
    [[nodiscard]] bool reconciled() const noexcept;

    void clear() noexcept;

private:
    // the implementation's own
};

/// Convenience for P2-04: add every verdict to the store, blocking the ones
/// that must be blocked (D6). Returns the number blocked, or the first store
/// error. Adds nothing further after a failure.
/// Takes a MUTABLE reference and reconciles first (D9).
[[nodiscard]] std::expected<std::size_t, SpecError>
apply_to_store(Reconciler& rec, SpecStore& store) noexcept;

} // namespace altair
```

---

## 5. REQUIREMENTS

1. `add` keys on D1's tuple. Two specs with the same tuple from *different*
   sources are the same contract; the same tuple from the *same* source
   replaces.
2. Underlying comparison is over the NUL-terminated content, capped at
   `kMaxUnderlyingLen`. Empty or null underlying is `BadSymbol`.
3. A `SpecSource` value outside 0..4 is `BadSource`. Never index an array with
   it first.
4. More than `kMaxInstruments` distinct contracts is `Full`.
5. `reconcile()` decides each contract in this order: `ValueConflict` if any two
   present sources differ on `lot_size`, `tick_size` or `price_scale` — report
   the **first** field in that order, and set `conflict_mask` for every source
   whose value differs from the winning source's; else `MissingPrimary` if
   `primary_source_seen` and no primary matching the contract's exchange is
   present; else `NoBroker` if neither `KiteDump` nor `XtsMaster` is present;
   else `SingleSource` if exactly one source is present; else `Agreed`.
6. `merged` takes non-compared fields from the D4 precedence winner, and
   `token[Kite]` / `token[Xts]` from the Kite and XTS sources respectively —
   0 where that source is absent.
7. `merged.source_hash` is the wrapping sum of `spec_mix64(h)` over every
   present source's `source_hash` (D8). `merged.stale` is true if **any**
   contributing source was stale. `merged.snapshot_at` is the **oldest**
   contributing snapshot — reporting the freshest would be a lie about the
   weakest link.
8. `merged.source` is the D4 precedence winner's source.
9. `apply_to_store` adds each verdict, then calls `block` for those where
   `verdict_blocks` is true (D6), and returns how many it blocked.
10. `reconcile()` is idempotent: calling it twice returns an equal report and
    leaves the verdicts unchanged.
11. No allocation, no exceptions, no `double` anywhere.

---

## 6. ACCEPTANCE TESTS

1. **`three_sources_agree`** — NSE + Kite + XTS with identical lot/tick/scale
   gives `Agreed`, `present_mask` has three bits, the merged spec carries
   **both** broker tokens, and `blocked == 0`.
2. **`lot_size_conflict_blocks`** — Kite says 75, NSE says 50: `ValueConflict`,
   `field == LotSize`, `values[]` holds both, `verdict_blocks` true.
3. **`tick_and_scale_conflicts`** — a tick-only conflict reports `TickSize`; a
   scale-only conflict reports `PriceScale`; a spec conflicting on **both lot
   and tick** reports `LotSize`, proving the requirement-5 ordering.
4. **`missing_primary_only_when_a_primary_was_loaded`** — Kite alone across the
   whole run gives `SingleSource`, **not** `MissingPrimary`, with
   `primary_source_seen == false`. Then a run where most contracts have an NSE
   row and one does not: that one is `MissingPrimary` and
   `primary_source_seen == true`. This is D7 and it is the subtlest rule here.
5. **`no_broker_blocks`** — an NSE master row alone for a contract, in a run
   where a broker source exists for others, gives `NoBroker`.
6. **`order_independence`** — add {NSE, Kite, XTS} in all 6 permutations and
   assert the merged spec is **byte-identical** every time (`memcmp` over the
   struct). This is D8 and rule 10.
7. **`merge_provenance`** — merged `stale` is true if any source is stale;
   `snapshot_at` is the oldest; `source` is the precedence winner; re-adding
   the same (key, source) does not create a phantom conflict.
8. **`apply_to_store_blocks`** — a mixed set through `apply_to_store`: every
   agreed contract resolves via `id_of`; every blocked one is **present in the
   store** and returns `Blocked` rather than `NotFound` (D6); and the returned
   count equals the report's `blocked`.

9. **`unreconciled_never_reads_as_agreed`** — before `reconcile()`,
   `verdict_at` returns `NotReconciled` rather than a zeroed `Agreed`;
   `reconciled()` is false after an `add()` that followed a `reconcile()`, and
   the stale verdict is refused; `apply_to_store` works on an unreconciled
   reconciler because it reconciles for itself (D9).

---

## 7. WHAT WILL GO WRONG IF YOU RUSH

- Keying on `symbol` instead of D1's tuple. It compiles, it passes a
  single-source test, and it silently fails to join anything the moment two real
  sources with different symbol conventions meet.
- Comparing `freeze_qty` or the bands. Every contract conflicts, the whole
  universe blocks, and the reconciler looks broken when it is the comparison
  that is wrong (D2).
- Making `MissingPrimary` unconditional, which blocks everything today (D7).
- A sequential `source_hash` — the specs then differ by input order. Test 6
  catches it, but only if it actually permutes rather than asserting on one
  order.

---

## REVIEW RECORD — P1-06

Reviewed 2026-08-31. MSVC 19.51.36256, `/std:c++latest /W4 /permissive- /O2`.

| Gate | Verdict | Evidence |
|---|---|---|
| 1 compiles clean | PASS | zero warnings. One compile error on the first attempt: `Timestamp` exposes `ns_since_epoch()`, not `raw()` — `raw()` belongs to `StrongInt`, and `Timestamp` is an affine point, not a `StrongInt`. The type system caught a category error, which is what P0-02 built it for. |
| 2 contract honoured | PASS, **card amended (D9)** | all signatures match except the three D9 changes, which are recorded above as a numbered decision rather than a silent edit. |
| 3 manifest respected | PASS | only `instruments/reconcile.hpp`, `instruments/tests/test_reconcile.cpp`, `instruments/CMakeLists.txt`. |
| 4 tests pass | PASS | 9 named tests + error paths, 77 checks, zero failures. |
| 5 no hot-path allocation | PASS | no `ALTAIR_HOT` — pre-open only. No `new`/`malloc`/`vector`/`string`/`shared_ptr`/`function`. All storage is fixed member arrays. |
| 6 latency budget | PASS (no budget) | runs once pre-open. |
| 7 numerical / financial | PASS | no floating point at all. Comparison is exact integer equality (D3) — the three compared fields are the three that change the size or the price of an order, and nothing else is compared (D2). |
| 8 physics | PASS | `snapshot_at` merges to the **oldest** contributing snapshot, not the freshest: a merge is only as fresh as its weakest source, and reporting the newest would be a measurement claiming precision it does not have. `stale` ORs. `source_hash` is order-independent (D8), proven by permuting all 6 orderings and `memcmp`-ing the result. |

### The defect review found, which the tests as first written did not

`dirty_` was written in three places and **read in none**. Two consequences,
both silent:

1. `Verdict::Agreed` is ordinal 0, so a default-constructed `ContractVerdict`
   claims agreement. **Never calling `reconcile()` therefore read as "everything
   agreed, nothing blocked"** — a silent all-clear from the component whose only
   job is to fail loud. This is the same shape as the P0-06b seqlock bug: an
   unwritten initial state that happens to satisfy the checker.
2. `add()` after `reconcile()` served the stale verdict without complaint.

Fixed as D9, with test 9 as the regression. Test 9 fails against the pre-fix
header, which is the only evidence that makes it worth having.

### A tooling hazard, also fixed

One check description contained the word "FAILS". The standing full-tree
technique is to run under CPU load and grep every line for `FAIL` — that
description would have shown up as a phantom failure in every future sweep.
Reworded to "is REFUSED". **No check description in this repo may contain the
substring `FAIL`.**

### Sizes, measured rather than estimated

`ContractSpec` 176 B · `ContractVerdict` 224 B · `SpecStore` 1.63 MB ·
`Reconciler` **5.25 MB**. Neither is a stack object; both are file-scope in the
tests and must be members in production. A `SpecStore` local would overflow
MSVC's 1 MB default stack on its own.
