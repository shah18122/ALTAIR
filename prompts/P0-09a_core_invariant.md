# P0-09a — `core/invariant`: the conservation ledger

> Phase 0 · Card 9a of 14 · Status: DONE (implemented by Claude, 2026-08-29 — see LEDGER)
> Depends on: P0-01 (`core/types/units.hpp`) — DONE
> Paste everything below the line into DeepSeek V4 as a single prompt.
>
> **Architect's note (not part of the prompt).** P0-09 splits: `core/invariant`
> is 3 files, the `feed/replay` skeleton needs 4 of its own (including a root
> `CMakeLists.txt` edit), and PROTOCOL §8 caps a card at 4. **P0-09b** is the
> replayer. Card total 113 → 114; Phase 0 goes to 13.
>
> **Scope cut, stated plainly.** ROADMAP §3.6 lists four conservation laws.
> Two of them — `Σ(leg_delta) − hedge_delta == net_delta` and the order-book
> depth monotonicity — depend on greeks (P3) and an L2 book (P2-08), neither of
> which exists. This card delivers the two that are implementable now, both
> about cash, and the enforcement shape the other two will reuse. Do not stub
> the missing two; a stub that always passes is worse than an absent check.
>
> One design decision: the ledger maintains the identity **by construction**,
> so `check()` can only fail on overflow or on a cash movement that bypassed
> `on_fill`. That is the point. The ledger is the single choke point for money,
> and the check proves nobody went around it.

---

## 1. CONTEXT

You are implementing the conservation invariants of Altair, a C++23 low-latency
trading engine for Indian equity markets.

CLAUDE.md's physics discipline: **conservation laws become runtime invariants.**
ROADMAP §3.6 states the first one exactly:

```
Σ(fills) + Σ(costs) + cash_delta == 0     exactly, in paise
```

Money is neither created nor destroyed. Every paisa that leaves the cash balance
is either the notional of a fill or a charge, and the three must sum to zero at
every instant. Not "within a rounding tolerance" — **exactly**, because
CLAUDE.md rule 3 puts every money amount in integer paise precisely so this
identity can be checked with `==` rather than `abs(x) < epsilon`.

The second law is the accounting identity behind equity:

```
Σ(position_i × price_i) + cash == equity
```

A breach means the books are wrong, and a system whose books are wrong must
stop trading immediately — CLAUDE.md rule 9. So a breach **latches**: once
tripped, the ledger refuses further fills until someone explicitly resets it. A
transient breach that clears itself is a breach you never find.

---

## 2. FILE MANIFEST

Create exactly these three files. Nothing else.

```
core/invariant/conservation.hpp
core/invariant/CMakeLists.txt
core/invariant/tests/test_conservation.cpp
```

`core/CMakeLists.txt` discovers `invariant/` via its `EXISTS` guard — **do not
modify it.** `core/invariant/CMakeLists.txt` declares `altair_invariant` as an
**INTERFACE** library, aliases it `altair::invariant`, exports
`${CMAKE_CURRENT_SOURCE_DIR}/..` as the include root, links `altair_types` and
`altair_flags`, and under `if(ALTAIR_BUILD_TESTS)` registers
`altair_conservation_test` with
`add_test(NAME conservation COMMAND altair_conservation_test)`.

**Do not touch** `core/CMakeLists.txt`, the root `CMakeLists.txt`, `vcpkg.json`,
or any other `core/` subdirectory.

---

## 3. INTERFACE CONTRACT

```cpp
#pragma once

#include <types/units.hpp>

#include <cstdint>
#include <expected>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// Which conservation law was violated. None is never returned in an error.
// ─────────────────────────────────────────────────────────────────────────
enum class Breach : std::uint8_t {
    CashConservation,   // fills + costs + cash_delta != 0
    Overflow,           // an accumulator would exceed int64 paise
    Latched             // a previous breach has not been cleared
};

// ─────────────────────────────────────────────────────────────────────────
// ConservationLedger — the single choke point for money, and the proof that
// nothing went around it.
//
// SINGLE INSTRUMENT. Portfolio-level aggregation across instruments is P4-03;
// this is the per-symbol ledger it will be built from.
//
// SIGN CONVENTION, and it is the thing to get right:
//   qty > 0 is a BUY  — cash goes DOWN by (price x qty) and by the cost
//   qty < 0 is a SELL — cash goes UP   by (price x |qty|), and DOWN by the cost
// Costs are ALWAYS a debit, on both sides. A cost credited on a sell is the
// classic sign error, and it makes every short look profitable.
// ─────────────────────────────────────────────────────────────────────────
class ConservationLedger {
public:
    /// A ledger starting flat with the given cash. UNIT: paise.
    constexpr explicit ConservationLedger(Notional initial_cash) noexcept;

    /// Record a fill. UNIT: qty in units, px in paise per unit, cost in paise.
    /// PRECONDITION: cost >= 0 — a cost is always a debit. A negative cost is
    /// a rebate and must be modelled as a cash adjustment, not a negative cost.
    /// Returns Latched if a breach is outstanding, Overflow if any accumulator
    /// would exceed int64 paise. On any error NOTHING is mutated.
    [[nodiscard]] ALTAIR_HOT std::expected<void, Breach>
    on_fill(Qty qty, Price px, Notional cost) noexcept;

    /// Record a cash movement that is NOT a fill: a margin transfer, a
    /// dividend, a rebate. UNIT: paise, signed. Tracked separately so the
    /// conservation identity stays exact.
    /// Returns Latched or Overflow. On any error NOTHING is mutated.
    [[nodiscard]] std::expected<void, Breach>
    on_cash_adjustment(Notional amount) noexcept;

    /// Verify ROADMAP §3.6 law 1, EXACTLY:
    ///     fills + costs + (cash - initial_cash - adjustments) == 0
    /// UNIT: none. Returns CashConservation on a breach, Latched if one is
    /// already outstanding. Does NOT latch a new breach by itself — call
    /// trip() for that, or use check_and_trip().
    [[nodiscard]] ALTAIR_HOT std::expected<void, Breach> check() const noexcept;

    /// check(), and latch on failure. UNIT: none. This is what a tick loop
    /// calls; check() alone is for a reporting path that must not alter state.
    [[nodiscard]] ALTAIR_HOT std::expected<void, Breach> check_and_trip() noexcept;

    /// ROADMAP §3.6 law 2: position x mark + cash.
    /// UNIT: paise. PRECONDITION: none. Returns Overflow if position x mark
    /// does not fit in int64 paise.
    [[nodiscard]] std::expected<Notional, Breach> equity(Price mark) const noexcept;

    /// Realised cash movement since construction, i.e. cash - initial_cash.
    /// UNIT: paise. At a FLAT position this is realised P&L net of all costs.
    [[nodiscard]] constexpr Notional cash_delta() const noexcept;

    // ── State. All UNIT: paise except position (units) and counts. ──
    [[nodiscard]] constexpr Notional cash() const noexcept;
    [[nodiscard]] constexpr Notional initial_cash() const noexcept;
    [[nodiscard]] constexpr Notional fills_notional() const noexcept;
    [[nodiscard]] constexpr Notional total_costs() const noexcept;
    [[nodiscard]] constexpr Notional adjustments() const noexcept;
    [[nodiscard]] constexpr Qty position() const noexcept;
    [[nodiscard]] constexpr std::uint64_t fill_count() const noexcept;

    /// True iff the position is exactly zero. UNIT: none.
    [[nodiscard]] constexpr bool is_flat() const noexcept;

    // ── The kill switch. ──

    /// True iff a breach is latched. UNIT: none. While true, every on_fill and
    /// on_cash_adjustment returns Latched.
    [[nodiscard]] constexpr bool is_breached() const noexcept;

    /// The latched breach, or Breach::Overflow when none — callers must check
    /// is_breached() first. UNIT: none.
    [[nodiscard]] constexpr Breach latched_breach() const noexcept;

    /// Breaches latched since construction. UNIT: count. Never reset by
    /// clear_breach() — the history is the audit trail.
    [[nodiscard]] constexpr std::uint64_t breach_count() const noexcept;

    /// Latch a breach explicitly. UNIT: none. Idempotent: a second trip does
    /// not overwrite the first breach, because the FIRST one is the diagnosis.
    void trip(Breach b) noexcept;

    /// Clear the latch. UNIT: none. Deliberately NOT called "reset" — it does
    /// not touch the balances, only the kill switch, and a human decides.
    void clear_breach() noexcept;

private:
    Notional initial_cash_{};
    Notional cash_{};
    Notional fills_{};
    Notional costs_{};
    Notional adjustments_{};
    Qty position_{};
    std::uint64_t fill_count_ = 0;
    std::uint64_t breach_count_ = 0;
    Breach latched_ = Breach::Overflow;
    bool breached_ = false;
};

} // namespace altair
```

---

## 4. BEHAVIOURAL SPEC

1. `on_fill(qty, px, cost)` computes `notional = px * qty` via
   `notional_of(px, qty)` from P0-01 — **not** a raw multiply, so the overflow
   check comes for free. It then updates, in this order, only if every step is
   representable:
   `fills_ += notional`, `costs_ += cost`, `position_ += qty`,
   `cash_ -= (notional + cost)`.
2. **Cash decreases by `notional + cost` on both sides.** For a buy, `notional`
   is positive so cash falls. For a sell, `notional` is negative so cash rises
   by `|notional|` — and `cost` still *subtracts*. A cost that credits on a sell
   is the classic sign error and makes every short look profitable.
3. `on_fill` rejects a negative `cost` — return `Breach::Overflow` is wrong
   here, so **reject it as `CashConservation`**: a negative cost is a rebate
   and belongs in `on_cash_adjustment`, and silently accepting one breaks the
   identity's meaning.
4. Every accumulation is overflow-checked before it is applied. On any overflow,
   return `Breach::Overflow` and **mutate nothing** — a partially applied fill
   is a guaranteed breach on the next check.
5. `on_fill` and `on_cash_adjustment` return `Breach::Latched` without mutating
   anything when `is_breached()`.
6. `check()` verifies, with `==` on integer paise and no tolerance:
   `fills_ + costs_ + (cash_ - initial_cash_ - adjustments_) == 0`.
   It is `const` and never latches.
7. `check_and_trip()` calls `check()` and, on failure, `trip()`s with the
   returned breach before returning it.
8. `trip(b)` sets `breached_`, increments `breach_count_`, and records `b` in
   `latched_` **only if no breach is currently latched** — the first breach is
   the diagnosis; a later one is a consequence.
9. `clear_breach()` clears `breached_` and leaves `latched_`, `breach_count_`
   and every balance untouched.
10. `equity(mark)` is `position_ * mark + cash_`, using `notional_of(mark,
    position_)` so the multiply is overflow-checked. Returns `Overflow` rather
    than wrapping.
11. `cash_delta()` is `cash_ - initial_cash_`. At a flat position it is realised
    P&L **net of all costs and adjustments** — that is the number a session
    report shows.
12. The whole class is trivially copyable and allocates nothing. `on_fill`,
    `check`, and `check_and_trip` are `ALTAIR_HOT`; nothing here throws.

---

## 5. CONSTRAINTS

- C++23. Standard library plus `<types/units.hpp>`. **No new dependency.**
- Header-only, one file.
- **Every money quantity is `Notional` (integer paise).** No `double` anywhere —
  CLAUDE.md rule 3, and the whole reason the identity can use `==`.
- Use `notional_of` from P0-01 for every price × quantity. Do not hand-roll the
  multiply; the overflow check is the point.
- No exceptions, no `throw`, no allocation, no `iostream`.
- Internal helpers in `namespace altair::detail`, never an anonymous namespace.
  Name them distinctly from the existing members (`mul_overflows`,
  `add_overflows_i64`, `is_pow2_size`, `round_up_pow2`, `pack_log_arg`,
  `config_mix64`, `config_key_len`).
- No `using namespace` at file scope in a header.
- Compiles clean at `/W4` and `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion -Wold-style-cast`.
- Tests are a plain `int main()`.

---

## 6. ACCEPTANCE TESTS

`core/invariant/tests/test_conservation.cpp`, plain `main()`,
`check(bool, const char*)` helper counting failures, returns 0 only if all pass.
Use exactly these names:

```cpp
void test_ledger_sign_conventions();
void test_ledger_single_buy();
void test_ledger_buy_sell_roundtrip();
void test_ledger_cash_adjustment();
void test_ledger_equity();
void test_ledger_conservation_over_many_fills();
void test_ledger_overflow_rejects_and_mutates_nothing();
void test_ledger_breach_latches_and_blocks();
```

Work in round numbers so the arithmetic is checkable by eye: a price of
`Price{2500000}` is ₹25,000.00, and a cost of `Notional{12345}` is ₹123.45.

**test_ledger_sign_conventions**
The one that catches the cost-side error.
```
ConservationLedger L{Notional{100'000'000}};      // Rs 10,00,000.00

// BUY 10 @ 25000.00, cost Rs 123.45
L.on_fill(Qty{10}, Price{2500000}, Notional{12345}).has_value()
L.position() == Qty{10}
L.fills_notional() == Notional{25'000'000}         // 2500000 * 10
L.total_costs() == Notional{12345}
L.cash() == Notional{100'000'000 - 25'000'000 - 12345}   // BOTH subtract
L.check().has_value()

// SELL 10 @ 25100.00, cost Rs 123.45
L.on_fill(Qty{-10}, Price{2510000}, Notional{12345}).has_value()
L.position() == Qty{0} && L.is_flat()
// The sell's notional is NEGATIVE, so cash rises by it; the cost still falls.
L.fills_notional() == Notional{25'000'000 - 25'100'000}   // == -100'000
L.total_costs() == Notional{24690}
L.check().has_value()

// Realised: gained Rs 1000.00 on the trade, paid Rs 246.90 in costs.
L.cash_delta() == Notional{100'000 - 24'690}       // == +75'310 paise
// If costs had been CREDITED on the sell this would read 100'000, and every
// short would look Rs 246.90 better than it was.

// A negative cost is rejected outright - it is a rebate, not a fill cost.
L.on_fill(Qty{1}, Price{100}, Notional{-1}).error() == Breach::CashConservation
L.fill_count() == 2                                 // and nothing was recorded
```

**test_ledger_single_buy**
```
ConservationLedger L{Notional{1'000'000}};
L.initial_cash() == Notional{1'000'000}
L.cash() == Notional{1'000'000}
L.position() == Qty{0} && L.is_flat()
L.fill_count() == 0
L.check().has_value()                               // an empty ledger balances

L.on_fill(Qty{5}, Price{1000}, Notional{50}).has_value()
L.fill_count() == 1
L.fills_notional() == Notional{5000}
L.cash() == Notional{1'000'000 - 5000 - 50}
L.cash_delta() == Notional{-5050}
L.check().has_value()
```

**test_ledger_buy_sell_roundtrip**
```
ConservationLedger L{Notional{1'000'000}};
L.on_fill(Qty{100}, Price{500}, Notional{25});      // buy  100 @ 5.00
L.on_fill(Qty{-100}, Price{520}, Notional{26});     // sell 100 @ 5.20

L.is_flat()
L.fills_notional() == Notional{50'000 - 52'000}     // == -2000
L.total_costs() == Notional{51}
L.cash_delta() == Notional{2000 - 51}               // == 1949
L.cash() == Notional{1'000'000 + 1949}
L.check().has_value()

// A LOSING round trip: the sign must carry through.
ConservationLedger M{Notional{1'000'000}};
M.on_fill(Qty{100}, Price{520}, Notional{26});
M.on_fill(Qty{-100}, Price{500}, Notional{25});
M.cash_delta() == Notional{-2000 - 51}              // == -2051
M.check().has_value()
```

**test_ledger_cash_adjustment**
```
ConservationLedger L{Notional{1'000'000}};
L.on_fill(Qty{10}, Price{1000}, Notional{100});
const auto before = L.cash();

L.on_cash_adjustment(Notional{500'000}).has_value();     // margin deposit
L.cash() == before + Notional{500'000}
L.adjustments() == Notional{500'000}
L.check().has_value()                                     // STILL balances

L.on_cash_adjustment(Notional{-200'000}).has_value();    // withdrawal
L.adjustments() == Notional{300'000}
L.check().has_value()

// Adjustments are NOT costs and NOT fills.
L.total_costs() == Notional{100}
L.fills_notional() == Notional{10'000}
```

**test_ledger_equity**
```
ConservationLedger L{Notional{1'000'000}};
L.equity(Price{999}).value() == Notional{1'000'000}      // flat: equity == cash

L.on_fill(Qty{10}, Price{1000}, Notional{0});
// cash = 1'000'000 - 10'000 = 990'000; position 10 @ mark 1000 = 10'000
L.equity(Price{1000}).value() == Notional{1'000'000}     // unchanged at cost
L.equity(Price{1100}).value() == Notional{1'001'000}     // +1000 unrealised
L.equity(Price{900}).value()  == Notional{999'000}       // -1000 unrealised

// A mark that would overflow the multiply is reported, not wrapped.
ConservationLedger H{Notional{0}};
H.on_fill(Qty{1'000'000'000}, Price{1}, Notional{0});
H.equity(Price{Price::max().raw()}).error() == Breach::Overflow
```

**test_ledger_conservation_over_many_fills**
The identity must hold at **every** step, not just at the end.
```
ConservationLedger L{Notional{1'000'000'000}};
Deterministic pseudo-random walk (a fixed LCG - NO std::random, the sequence
must be identical on every box and every run):
  10'000 iterations, each either a fill with qty in [-50, 50] \ {0},
  price in [100, 10'000], cost in [0, 500], or occasionally an adjustment.
  After EVERY operation: L.check().has_value()

  every check passed
  L.fill_count() == <the number of fills actually issued>
  !L.is_breached()
// Then verify the identity by hand one final time, recomputing the three
// accumulators independently from the running totals the test kept itself:
  L.fills_notional() == expected_fills
  L.total_costs()    == expected_costs
  L.cash()           == expected_cash
```

**test_ledger_overflow_rejects_and_mutates_nothing**
```
ConservationLedger L{Notional{0}};
// A fill whose notional overflows int64 paise.
L.on_fill(Qty{Qty::max().raw()}, Price{2}, Notional{0}).error() == Breach::Overflow
L.fill_count() == 0                                 // NOTHING was recorded
L.position() == Qty{0}
L.cash() == Notional{0}
L.fills_notional() == Notional{0}
!L.is_breached()                                    // a rejected fill is not a breach
L.check().has_value()                                // and the books still balance

// Overflow on the cost ACCUMULATOR, not the notional. Note the headroom:
// a cost of exactly Notional::max() makes notional + cost unrepresentable and
// is refused on the FIRST fill, which would exercise the wrong guard.
const Notional huge{Notional::max().raw() - 1000};
ConservationLedger M{Notional{0}};
M.on_fill(Qty{1}, Price{1}, huge).has_value()
M.on_fill(Qty{1}, Price{1}, huge).error() == Breach::Overflow
M.fill_count() == 1                                 // the second was rejected whole
M.check().has_value()

// And the guard the case above would otherwise have hidden: an outflow that
// cannot be represented even though each accumulator individually could.
ConservationLedger N{Notional{0}};
N.on_fill(Qty{1}, Price{1}, Notional{Notional::max().raw()}).error()
    == Breach::Overflow
N.fill_count() == 0
```

**test_ledger_breach_latches_and_blocks**
```
ConservationLedger L{Notional{1'000'000}};
L.on_fill(Qty{1}, Price{100}, Notional{5});
!L.is_breached() && L.breach_count() == 0

L.trip(Breach::CashConservation);
L.is_breached()
L.latched_breach() == Breach::CashConservation
L.breach_count() == 1

// While latched, EVERYTHING is refused - the kill switch.
L.on_fill(Qty{1}, Price{100}, Notional{5}).error() == Breach::Latched
L.on_cash_adjustment(Notional{1}).error() == Breach::Latched
L.check().error() == Breach::Latched
L.fill_count() == 1                                 // and nothing got through

// A second trip does NOT overwrite the first - the FIRST is the diagnosis.
L.trip(Breach::Overflow);
L.latched_breach() == Breach::CashConservation
L.breach_count() == 2

L.clear_breach();
!L.is_breached()
L.breach_count() == 2                               // the audit trail survives
L.on_fill(Qty{1}, Price{100}, Notional{5}).has_value()
L.check().has_value()                                // balances were never touched
```

### Latency reporting — not a pass/fail assertion

Batch-timed (the P0-05b technique): `on_fill` and `check_and_trip`.
ROADMAP §11 budgets "Risk checks" at 3 µs total, and this runs inside that, so
**budget: < 20 ns each**. Print; do not assert.

---

## 7. FORBIDDEN

- Adding a file not in the manifest, or touching another `core/` subdirectory.
- Changing any signature in the interface contract.
- **Any `double`, anywhere.** Money is integer paise; rule 3.
- **A tolerance in `check()`.** It is `==`. An epsilon here would hide exactly
  the class of bug the invariant exists to catch.
- A raw `px * qty` multiply. Use `notional_of`; item 1.
- **Crediting the cost on a sell.** Costs always debit; item 2.
- Mutating any state on a rejected or overflowing operation.
- Letting `trip()` overwrite an already-latched breach.
- `clear_breach()` touching any balance or the breach count.
- Stubbing the two §3.6 laws this card does not implement. A check that always
  passes is worse than an absent one.
- `std::random` in the many-fills test — the sequence must be identical on every
  box, or a failure is not reproducible.
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

Return the three files in full, then your ASSUMPTIONS section, which must state
the measured ns for `on_fill` and `check_and_trip`, and confirm in your own
words which direction cash moves for a sell and what happens to the cost.
