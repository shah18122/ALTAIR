# P2-09a — `book/microstructure`: imbalance and microprice

> Phase 2 · Card 9a of 12 · Status: **DONE** 2026-08-31
> Depends on: P2-08 (`book/l2_book.hpp`)
> Feeds: P5 (feature registry), P6 (10-minute forecast), P10 (hedge book)
>
> **Architect's note.** P2-09 as planned was six things — OBI, weighted OBI,
> microprice, VPIN, Kyle's λ and queue position — with three different state
> requirements. PROTOCOL §8 splits it:
>
> | Card | Content | State |
> |---|---|---|
> | **P2-09a** | OBI, weighted OBI, microprice | **none** — pure functions of a `BookState` |
> | P2-09b | VPIN, Kyle's λ | rolling windows over trade flow |
> | P2-09c | queue position | needs live order state — really Phase 4 |
>
> This card is the stateless third, and it is the one every later feature reads.

---

## 1. THE HANDOFF FROM P2-08

**Every signal here returns empty when the book is not tradable.**

P2-08 stores crossed books rather than rejecting them, because the pre-open
auction legitimately crosses. That decision only pays off if the consumers
honour it: an imbalance or a microprice computed from a crossed book is
arithmetic on a state that **cannot be traded**, and it would be the exact
"free money" signal Phase 5 exists to disprove. `is_tradable()` is the gate,
and it is checked first in every function here.

An empty result is a real answer — "there is no microprice right now" — and
not an error to paper over with a default.

---

## 2. THE SIX DECISIONS

**D1 — The microprice weighting is CROSSED, and getting it backwards is
invisible.**

```
microprice = (bid_px * ask_qty + ask_px * bid_qty) / (bid_qty + ask_qty)
```

Bid **price** is weighted by ask **quantity**. The intuition: heavy size
resting on the bid means the next trade is more likely to lift the offer, so
the fair price sits nearer the ask — and it is `bid_qty` that multiplies
`ask_px`. Swap the weights and the microprice leans the wrong way while
**still sitting between bid and ask**, so it looks entirely plausible and every
model built on it learns the inverse of the truth. Tested directly.

**D2 — Microprice is exact integer arithmetic, and overflow returns empty.**
It is a `Price` in paise and it must not go through a double: it is a fair-value
estimate that a limit price gets rounded from, and a drift of one unit is an
order at the wrong tick. Products are checked with `detail::mul_overflows`
before they are formed, and an overflow yields `nullopt` rather than a wrapped
number.

**D3 — Imbalance is a `double`, and that is deliberate.**
OBI is a dimensionless ratio in [-1, +1]; there is no integer representation
that is not a lie about precision. CLAUDE.md rule 3 permits doubles in
analytics and bans them in the ledger, and this is squarely analytics. The
boundary is explicit: **no ledger value is ever derived from an OBI without
going back through integer paise.** The numerator and denominator are computed
as exact integers, so the only rounding is the final division.

**D4 — Weighted OBI uses harmonic weights, `w(k) = 1/(1+k)`.**
Level 0 dominates, which matches how queues actually behave — the touch is
where the next trade happens. Weighting by *price distance* instead is
defensible and sometimes better, but level spacing varies by instrument and
that makes it a **feature-registry** decision (Phase 5, versioned) rather than
a book primitive. This card provides the simple robust one and says so.

**D5 — Depth beyond the populated levels is not imagined.**
A request for 5 levels on a book holding 2 sums 2. It does not treat the zeroed
slots as zero-size liquidity at price zero, and it does not scale up to
pretend. `levels_used` is reported so the caller knows what it actually got.

**D6 — Zero total size is empty, not zero imbalance.**
A book with levels present but no quantity would divide by zero. An imbalance
of 0.0 means "perfectly balanced", which is a strong claim; the truth here is
"no information". Empty.

---

## 3. FILE MANIFEST

```
CREATE   book/microstructure.hpp
CREATE   book/tests/test_microstructure.cpp
MODIFY   book/CMakeLists.txt
```

---

## 4. INTERFACE CONTRACT

```cpp
#pragma once

#include <book/l2_book.hpp>

namespace altair {

/// Aggregate size on one side, over the top `levels` populated entries.
struct SideDepth {
    Qty          qty;          // UNIT: units
    std::uint8_t levels_used;  // how many were actually summed (D5)
};

[[nodiscard]] ALTAIR_HOT std::optional<SideDepth>
bid_depth(const BookState&, std::uint8_t levels) noexcept;

[[nodiscard]] ALTAIR_HOT std::optional<SideDepth>
ask_depth(const BookState&, std::uint8_t levels) noexcept;

/// Order book imbalance over the top `levels`. UNIT: dimensionless, [-1, +1].
/// Positive means more resting size on the bid.
/// Empty when the book is not tradable, or total size is zero (D6).
[[nodiscard]] ALTAIR_HOT std::optional<double>
obi(const BookState&, std::uint8_t levels = 1) noexcept;

/// As `obi`, with harmonic weights w(k) = 1/(1+k) (D4).
[[nodiscard]] ALTAIR_HOT std::optional<double>
weighted_obi(const BookState&, std::uint8_t levels = kDepthLevels) noexcept;

/// Size-weighted fair price at the touch. UNIT: paise.
/// (bid_px * ask_qty + ask_px * bid_qty) / (bid_qty + ask_qty)  -- D1.
/// Exact integer arithmetic; empty on overflow or an untradable book.
[[nodiscard]] ALTAIR_HOT std::optional<Price>
microprice(const BookState&) noexcept;

} // namespace altair
```

---

## 5. REQUIREMENTS

1. Every function checks `is_tradable()` first and returns empty if false.
2. `levels` is clamped to the populated count; never reads past `*_levels`.
3. `levels == 0` is empty, not a division by zero.
4. Microprice uses `detail::mul_overflows` before each product and returns
   empty on overflow. No `double` anywhere in it.
5. **Microprice always lies within `[best_bid, best_ask]`** — it is a convex
   combination, and a result outside that range is a bug by construction.
6. OBI is `+1` when the ask side is empty of size and `-1` when the bid side
   is, with both reachable only through zero quantities rather than zero
   levels — a zero-level side already failed `is_tradable`.
7. Total size of zero returns empty (D6).
8. No allocation, no exceptions, no clock read.

---

## 6. ACCEPTANCE TESTS

1. **`untradable_books_yield_nothing`** — a crossed book, a locked book and a
   one-sided book each return empty from `obi`, `weighted_obi` and
   `microprice`. Assert the crossed book's levels are still *readable*, so the
   test shows the data exists and the signal is declining to use it.
2. **`microprice_weights_are_crossed`** — with heavy size on the bid, the
   microprice sits **nearer the ask**. Then assert the swapped formula would
   have put it nearer the bid, so the test proves the direction rather than
   just a number.
3. **`microprice_is_within_the_touch`** — across many size combinations the
   result is always in `[bid, ask]`; equal sizes put it exactly at the mid.
4. **`microprice_is_exact_and_integer`** — a case whose true value has a
   fractional part truncates deterministically; a case near `Price::max()`
   returns empty on overflow rather than a wrapped number.
5. **`obi_range_and_sign`** — all size on the bid gives `+1`, all on the ask
   `-1`, equal sizes `0`; every result is within `[-1, +1]`.
6. **`depth_does_not_imagine_levels`** — asking for 5 levels on a 2-level book
   sums 2 and reports `levels_used == 2`. Assert the unpopulated slots are
   zeroed, so the test shows what D5 prevents.
7. **`weighted_obi_favours_the_touch`** — two books with identical total size
   but the imbalance at different depths give different weighted OBI, and the
   one imbalanced at level 0 is larger in magnitude. Unweighted OBI over the
   same books is identical, which is the whole point of having both.
8. **`zero_size_is_empty_not_balanced`** — levels present with zero quantity
   returns empty rather than `0.0`, because a perfectly balanced book is a
   strong claim and the truth is that there is no information.
