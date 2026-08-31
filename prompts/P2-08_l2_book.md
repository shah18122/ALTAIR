# P2-08 — `book/l2_book`: the L2 order book

> Phase 2 · Card 8 of 12 · Status: **DONE** 2026-08-31
> Depends on: P2-01 (`feed/tick.hpp`) · P2-04 (normaliser)
> Feeds: P2-09 (OBI, microprice, VPIN, Kyle λ), and every strategy after it
>
> **Architect's note.** No new sources were needed for this card. The depth
> semantics come from gokiteconnect v4.4.2, already cloned and already read for
> P2-02; the rest is a data structure whose design is mine. Cloning something
> for the sake of it would have been noise.
>
> This card **corrects the skeleton's central decision**, which was wrong in a
> way that would have shown up every trading morning. See D2.

---

## 1. WHY THIS IS NOT A PRICE-KEYED MAP

Kite's full mode and XTS 1502 both deliver a **five-level snapshot**, not
deltas. So applying an update is a *replace*, and O(1) is trivial at this
depth. A price-keyed map — the shape a real exchange feed with per-order
messages would need — would be slower, would allocate, and would model a
problem Altair does not have.

If a tick-by-tick feed with order-level messages ever arrives, this becomes a
different card, not an extension of this one.

---

## 2. THE SIX DECISIONS

**D1 — A book that has never been updated is `NotFound`, not empty.**
A default `BookState` has `bid_levels == 0` and `ask_levels == 0`, which reads
as *"no liquidity"* rather than *"no data"*. Those are entirely different
claims, and the first one is a lie a strategy would act on. `at()` therefore
distinguishes them explicitly. This is the same phantom-default-state bug that
P0-06b and P1-06 both produced; it gets designed out here rather than found
again.

**D2 — A crossed book is STORED AND MARKED, never rejected.**
*(This reverses the skeleton, which said reject.)*

During NSE's pre-open call auction, orders are collected **without matching**,
so the book legitimately crosses while the equilibrium price is discovered.
Rejecting a crossed update would leave the previous continuous-session book in
place and present it as current — **stale data masquerading as live**, which is
strictly worse than crossed data correctly labelled.

Rejecting also throws away a real measurement to prevent a downstream bug,
which is the wrong layer to fix it at. So the book stores what it was told,
sets a flag, and `is_tradable()` returns false. The consumer cannot trade on it
by accident, and nobody has lied about what the market was doing.

This follows P2-04's rule exactly: flag, do not drop. Only a rule-7 look-ahead
violation drops.

**D3 — An out-of-order update IS rejected, and the previous state survives.**
Unlike a crossed book, a stale sequence carries no information — it is a
strictly worse view of a moment already superseded. Applying it would move the
book *backwards*. `seq` comes from P2-04 and is monotonic across a failover by
construction, so this test is meaningful across a primary switch.

**D4 — Rejections are counted per instrument, not just globally.**
A single symbol with a broken feed is the realistic failure; a global counter
would show a slow climb and name nothing. `consecutive_rejects` resets on any
accepted update, so a non-zero value means *right now*, not *ever*.

**D5 — Fixed capacity, no allocation, and `InstrumentId` indexes directly.**
`InstrumentId` is dense from 0 (P1-01 assigns it that way), so it is the array
index. No hash, no probe.

**D6 — The book stores, it does not analyse.**
Imbalance, microprice, VPIN and Kyle's λ are P2-09. This card provides the
state they read and nothing more. A book that computed signals would make
every signal change a book change.

---

## 3. FILE MANIFEST

```
MODIFY   book/l2_book.hpp          (the skeleton becomes the implementation)
CREATE   book/tests/test_l2_book.cpp
MODIFY   book/CMakeLists.txt
```

---

## 4. INTERFACE CONTRACT

```cpp
#pragma once

#include <feed/tick.hpp>

namespace altair {

enum class BookError : std::uint8_t {
    NotFound,        // never updated — NOT the same as empty (D1)
    BadInstrument,   // id outside the addressable range
    StaleSequence    // seq not greater than the stored one (D3)
};

/// One instrument's book. Trivially copyable so it can ride a seqlock out to
/// the analytics thread.
struct BookState {
    InstrumentId  id;
    std::uint32_t seq;
    Timestamp     exchange_ts;
    Timestamp     recv_ts;
    DepthLevel    bid[kDepthLevels];
    DepthLevel    ask[kDepthLevels];
    std::uint8_t  bid_levels;
    std::uint8_t  ask_levels;
    bool          crossed;              // D2 — stored, marked, not tradable
    std::uint8_t  reserved;
    std::uint16_t consecutive_rejects;  // D4
    std::uint16_t flags;
};

/// True only when both sides have liquidity and the book is not crossed.
/// The single question a strategy should ask before acting on a book.
[[nodiscard]] constexpr bool is_tradable(const BookState&) noexcept;

class L2Book {
public:
    struct Stats {
        std::uint64_t applied;
        std::uint64_t crossed;
        std::uint64_t stale_sequence;
        std::uint64_t bad_instrument;
        std::uint64_t instruments;
    };

    L2Book() noexcept;

    [[nodiscard]] ALTAIR_HOT std::expected<void, BookError>
    apply(const DepthUpdate& d) noexcept;

    [[nodiscard]] ALTAIR_HOT std::expected<const BookState*, BookError>
    at(InstrumentId id) const noexcept;

    [[nodiscard]] bool has(InstrumentId id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const Stats& stats() const noexcept;
    void clear() noexcept;

private:
    // the implementation's own
};

} // namespace altair
```

---

## 5. REQUIREMENTS

1. `at()` on an instrument never updated returns `NotFound`. An instrument
   updated with **zero levels on both sides** returns a `BookState` — that is a
   real observation of an empty book, and distinct from no observation.
2. A crossed or locked book (`bid >= ask`, both sides present) is stored with
   `crossed == true` and `is_tradable()` false. It is **not** an error.
3. An update whose `seq` is not strictly greater than the stored one returns
   `StaleSequence` and leaves the stored state **byte-identical**.
4. `consecutive_rejects` increments on a rejection and resets to zero on any
   accepted update.
5. An `InstrumentId` outside the addressable range is `BadInstrument`, checked
   before any array access.
6. `is_tradable` requires both sides non-empty **and** not crossed. An
   empty-sided book is not tradable even though it is not crossed.
7. No allocation, no exceptions, no `double`, no clock read.

---

## 6. ACCEPTANCE TESTS

1. **`never_updated_is_not_empty`** — `at()` on an untouched id is `NotFound`;
   after an update carrying zero levels it returns a state whose
   `bid_levels == 0`. Assert both, so the test shows the two are different
   claims rather than asserting one of them.
2. **`crossed_is_stored_and_marked`** — a crossed book applies successfully,
   `crossed` is true, `is_tradable` is false, and the **levels are readable**.
   Then a normal book replaces it and clears the flag. This is D2 and the
   pre-open case.
3. **`locked_counts_as_crossed`** — `bid == ask` is crossed too. A zero spread
   is not a free trade; it is a book that cannot be traded.
4. **`stale_sequence_rejected_state_survives`** — an older `seq` returns
   `StaleSequence` and the stored book is byte-identical to before
   (`memcmp`), so the rejection cannot have partially written.
5. **`consecutive_rejects_tracks_now_not_ever`** — three rejections raise the
   counter to 3; one accepted update resets it to 0. A per-instrument counter
   names the broken symbol, which a global one cannot.
6. **`snapshot_replaces_rather_than_merges`** — a 5-level book followed by a
   2-level book leaves `bid_levels == 2` and levels 2..4 zeroed, not the old
   values showing through. This is the whole consequence of D1's snapshot
   semantics.
7. **`is_tradable_needs_both_sides`** — a one-sided book is not tradable even
   though it is not crossed; a two-sided uncrossed book is.
8. **`bad_instrument_and_capacity`** — `InstrumentId::Invalid` and an id past
   the range are `BadInstrument` before any indexing; `size()` counts distinct
   instruments, not updates.

---

## 7. WHAT WILL GO WRONG IF YOU RUSH

- **Rejecting a crossed book.** It breaks every pre-open, and the failure looks
  like a stalled feed rather than a policy decision.
- **Treating a never-updated book as empty.** A strategy reads a bid of zero
  and a spread of the entire price.
- **Merging instead of replacing.** Old levels show through beneath a shorter
  new book, inventing liquidity that is not there.
