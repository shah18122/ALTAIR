# P2-01 — `feed/tick`: the normalised `Tick` and `DepthUpdate`

> Phase 2 · Card 1 of 9 · Status: **DONE** 2026-08-31
> Depends on: P0-02 (`timestamp`) · P0-04 (`exchange_ts`) · P1-01 (`contract_spec`)
> Feeds: P2-02, P2-03, P2-04, P2-05, P2-06, P2-08 — every remaining Phase 2 card
>
> **Architect's note.** This is the type every later phase reads. Getting it
> wrong is expensive in a way that does not show up until Phase 5, so the seven
> decisions below are fixed here rather than discovered later.
>
> It also **closes the P0-09b carried debt**, open since Phase 0: *a broker
> token is not an instrument key*. `ReplayTick` carries a bare `uint32 token`,
> which means different things depending on which feed produced it. This card
> replaces it with a canonical `InstrumentId`.

---

## 1. THE SEVEN DECISIONS

**D1 — A normalised tick carries `InstrumentId`. It NEVER carries a broker
token.**
Kite's `instrument_token` and XTS's `ExchangeInstrumentID` are *different
number spaces for the same contract*. P2-05 switches the primary feed mid-
session; with a broker token in the tick, every instrument would appear to
vanish and a stranger appear in its place, and the book, the ledger and every
strategy would follow it. Decoders map `(FeedSource, broker_token)` →
`InstrumentId` through `SpecStore::id_of` and **nothing downstream ever sees a
broker token.** The `source` field records which feed produced the tick, which
is a diagnostic, not an identity.

**D2 — Two timestamps, and they are not interchangeable.**
`exchange_ts` is when the exchange says it happened; `recv_ts` is when we
received it, off the TSC clock. Strategies read `exchange_ts` — rule 7, a
strategy never reads a wall clock. `recv_ts` exists for latency measurement and
for detecting a stalled feed (P2-05), and is **locally monotonic** where
`exchange_ts` is not: exchange clocks step, and P0-04's `PlausibilityGate`
exists precisely because they do.

**D3 — `Tick` and `DepthUpdate` are separate structs.**
Kite's full mode delivers price and depth in one packet; XTS delivers touchline
(1501) and depth (1502) separately. Forcing them into one struct would mean
**synthesising** depth on a touchline-only update, and a synthesised book level
is a fabricated measurement. Two structs, each emitted only when its data
actually arrived.

**D4 — All money is integer paise, and the decoder does the scaling.**
`ContractSpec::price_scale` is 100 for equity and F&O, so the wire value *is*
paise. It is 10'000'000 for NSE currency derivatives — finer than a paisa, and
rule 3 does not hold there. A `Tick` is **always paise**; the decoder normalises
and **refuses** what it cannot represent rather than truncating. This is the
P0-01 carried-debt row, and P2-01 pins where it gets resolved: at the decoder,
not in the strategy.

**D5 — `seq` is per-session and monotonic per instrument.**
Rule 10 puts `tick_seqno` in the reproducibility tuple. It is the sequence *we*
assign on normalisation, not the exchange's — an exchange sequence number is
per-feed and does not survive a failover.

**D6 — Five depth levels, fixed.**
Kite full mode publishes five. A fixed array means no allocation and a
trivially-copyable struct. `levels` records how many are actually populated:
five slots do not mean five levels of real liquidity, and a strategy reading a
zeroed level as a real one at price 0 would see infinite edge.

**D7 — Trivially copyable, and the layout is pinned by `static_assert`.**
These structs ride an SPSC ring and a seqlock (P0-06) and are memcpy'd into the
tick store (P2-06). Size and offsets are asserted so a field added in the middle
breaks the build rather than silently changing a file format that replay depends
on. `kTickWireVersion` moves whenever the layout does.

---

## 2. FILE MANIFEST

```
CREATE   feed/tick.hpp
CREATE   feed/tests/test_tick.cpp
MODIFY   feed/CMakeLists.txt
```

`add_test(NAME tick COMMAND altair_tick_test)`.
**Do not touch** `feed/replay.hpp` — migrating the replayer to emit `Tick` is
P2-06/P2-07, and doing it here would put two cards in one manifest.

---

## 3. INTERFACE CONTRACT

```cpp
#pragma once

#include <core/time/timestamp.hpp>
#include <core/types/units.hpp>
#include <instruments/contract_spec.hpp>

namespace altair {

/// Bumped whenever the layout of Tick or DepthUpdate changes. The tick store
/// and the replayer both persist these bytes; a silent layout change makes
/// every recorded session unreadable in a way that looks like corruption.
inline constexpr std::uint16_t kTickWireVersion = 1;

inline constexpr std::size_t kDepthLevels = 5;

enum class TickFlag : std::uint16_t {
    None        = 0,
    Snapshot    = 1u << 0,   // full image, not a delta
    Stale       = 1u << 1,   // exchange_ts failed the plausibility gate
    Synthetic   = 1u << 2,   // produced by replay, never by a live feed
    ClosingAuct = 1u << 3,   // inside the closing auction window
};

struct Tick {
    InstrumentId  id;            // D1 — canonical. NEVER a broker token.
    std::uint32_t seq;           // D5
    Timestamp     exchange_ts;   // D2 — what strategies read
    Timestamp     recv_ts;       // D2 — locally monotonic
    Price         last;
    Qty           last_qty;
    Qty           volume;        // cumulative for the session
    std::int64_t  oi;            // open interest; 0 for cash
    FeedSource    source;        // diagnostic only — NOT identity
    std::uint8_t  reserved;
    std::uint16_t flags;         // TickFlag bitset
    std::uint32_t reserved2;
};

struct DepthLevel {
    Price         px;
    Qty           qty;
    std::uint32_t orders;        // order count at this level
    std::uint32_t reserved;
};

struct DepthUpdate {
    InstrumentId  id;
    std::uint32_t seq;
    Timestamp     exchange_ts;
    Timestamp     recv_ts;
    DepthLevel    bid[kDepthLevels];
    DepthLevel    ask[kDepthLevels];
    FeedSource    source;
    std::uint8_t  bid_levels;    // D6 — how many are REAL
    std::uint8_t  ask_levels;
    std::uint8_t  reserved;
    std::uint16_t flags;
    std::uint16_t reserved2;
};

[[nodiscard]] constexpr bool has_flag(std::uint16_t flags, TickFlag f) noexcept;
[[nodiscard]] constexpr std::uint16_t set_flag(std::uint16_t flags, TickFlag f) noexcept;

/// Best bid / best ask, or nullptr when that side is empty.
/// Reading bid[0] directly is the bug this exists to prevent: an unpopulated
/// level is zeroed, and price 0 on the bid looks like free money.
[[nodiscard]] constexpr const DepthLevel* best_bid(const DepthUpdate&) noexcept;
[[nodiscard]] constexpr const DepthLevel* best_ask(const DepthUpdate&) noexcept;

/// UNIT: paise. Empty when either side is empty. NOT (bid+ask)/2 blindly —
/// a one-sided book has no mid, and returning the one side that exists is how
/// a spread of zero gets into a signal.
[[nodiscard]] constexpr std::optional<Price> mid(const DepthUpdate&) noexcept;

/// UNIT: paise. Ask minus bid; empty when either side is empty.
[[nodiscard]] constexpr std::optional<Price> spread(const DepthUpdate&) noexcept;

/// True when the book is crossed or locked (bid >= ask). A crossed book is a
/// decoder bug or a genuinely broken feed; either way it must not be traded on.
[[nodiscard]] constexpr bool is_crossed(const DepthUpdate&) noexcept;

} // namespace altair
```

---

## 4. REQUIREMENTS

1. Both structs are trivially copyable, `static_assert`ed.
2. `sizeof(Tick) == 64` and `sizeof(DepthUpdate) == 272`, `static_assert`ed,
   with the offsets of every field asserted too. A field inserted in the middle
   must break the build.
3. `best_bid`/`best_ask` return `nullptr` when `bid_levels`/`ask_levels` is 0.
4. `mid` and `spread` return empty unless **both** sides have at least one
   level. A one-sided book has no mid.
5. `mid` is computed without overflow: `(bid + ask) / 2` on two `int64` paise
   near the top of the range overflows. Use `bid + (ask - bid) / 2`.
6. `is_crossed` is true when `bid >= ask` — locked (equal) counts, because a
   locked book is not tradable either.
7. No allocation, no exceptions, no `double`.
8. Nothing in this header reads a clock.

---

## 5. ACCEPTANCE TESTS

1. **`layout_is_pinned`** — sizes and every field offset match the contract;
   both structs are trivially copyable; `kTickWireVersion` is 1.
2. **`no_broker_token_field`** — a compile-time check that `Tick` has an `id`
   of type `InstrumentId` and no `token` member, plus a runtime demonstration
   that two feeds' different tokens for the same contract normalise to the
   **same** `InstrumentId` through a `SpecStore`. This is P0-09b and the reason
   the card exists.
3. **`empty_side_has_no_mid`** — a book with bids and no asks returns `nullopt`
   from `mid` and `spread`, and `nullptr` from `best_ask`. Assert that reading
   `ask[0].px` directly would have given 0, so the test shows what it prevents.
4. **`mid_does_not_overflow`** — bid and ask near `Price::max()` produce the
   correct mid; assert that the naive `(a+b)/2` would have overflowed.
5. **`crossed_and_locked`** — `bid > ask` is crossed; `bid == ask` is also
   crossed; a normal book is not.
6. **`levels_bound_the_reads`** — with `bid_levels == 2`, levels 2..4 are
   zeroed and `best_bid` still returns level 0; a zero `bid_levels` returns
   nullptr even when the array holds stale non-zero data.
7. **`flags_round_trip`** — set/test each flag; flags compose; `None` tests
   false for everything.
8. **`rides_a_ring`** — push and pop a `Tick` and a `DepthUpdate` through the
   P0-06 SPSC ring unchanged, byte for byte. Proves D7 against the actual
   transport rather than against `is_trivially_copyable` alone.
