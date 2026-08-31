# P2-04 — `feed/normaliser`: gating, ordering, and source routing

> Phase 2 · Card 4 of 9 · Status: **DONE** 2026-08-31
> Depends on: P2-01 (`tick.hpp`) · P2-02 (`kite_decoder.hpp`) · P0-04 (`exchange_ts.hpp`)
> Feeds: P2-05 (failover), P2-06 (tick store), and every consumer downstream
>
> **Architect's note.** The decoder is a pure function. This is the stateful
> half: it owns per-instrument history, applies the look-ahead guard, decides
> what is publishable, and counts everything it refuses.
>
> The token→`InstrumentId` binding named in the phase plan already happened in
> P2-02 — a decoder that cannot resolve a token cannot produce a `Tick` at all,
> so putting the lookup anywhere else would have meant inventing an
> intermediate struct that carries a broker token, which is exactly what
> P0-09b forbade. What is left for this card is everything that needs *memory*.

---

## 1. THE TRAP AT THE CENTRE OF THIS CARD

`PlausibilityGate` holds **one** `last_`. It is a per-*source* object.

Setting `require_monotonic = true` on a gate shared across all instruments is
therefore wrong, and wrong in the most expensive way: instruments tick
independently, so NIFTY at 10:00:05 followed by BANKNIFTY at 10:00:04 is
completely ordinary. A shared monotonic gate would reject a large fraction of
perfectly good ticks, the rejection counter would climb, and it would look
exactly like a broken feed.

So the gate is used for what it genuinely is — a **per-source** floor and
ceiling check — with `require_monotonic` **off**, and per-instrument ordering
is tracked separately by this card.

---

## 2. THE SEVEN DECISIONS

**D1 — One gate per source, `require_monotonic` off.** §1.

**D2 — Per-instrument ordering is tracked here, and EQUAL timestamps are
normal.**
P2-02 established that Kite's `exchange_ts` is one-second resolution, so many
ticks legitimately share a second. Only a **strictly decreasing** timestamp for
the same instrument is anomalous. It is flagged and counted, never dropped: a
trade that happened still happened, and discarding it would put a hole in the
volume series to hide a timestamp problem.

**D3 — Only the ACTIVE source reaches the gate at all.**
*(Corrected during review — the card first said both feeds are gated.)*
A standby submission returns immediately, before the plausibility gate and
before any per-instrument history is touched. Two reasons. The gate holds one
`last_` and is a per-source object, so feeding it two interleaved sources would
make its floor/ceiling statistics meaningless. And the standby feed must not
move an instrument's high-water mark, or every active tick behind it would be
reported out of order.

Standby *health* is therefore not this card's job — it is P2-05's watchdog,
and P2-02 established that it must key on `recv_ts` rather than `exchange_ts`,
because Kite's exchange timestamps are one-second resolution and two of its
five modes carry none at all.

Publishing only the active source makes failover a pointer flip rather than a
content-based dedup problem. Dedup would be fragile: the same trade from two
feeds differs in `recv_ts` and can differ in `exchange_ts` rounding.

**D4 — `seq` is assigned HERE, so it survives a failover.**
Monotonic across a primary switch by construction, because it never came from
the feed. Rule 10's `tick_seqno`.

**D5 — The look-ahead guard DROPS. Everything else FLAGS.**
`AfterCeiling` means the tick claims to be from the future. Rule 7 is absolute:
no look-ahead, ever. That one is dropped and never enters the pipeline.
`BeforeFloor` and a per-instrument regression are *suspicious*, not
impossible — they get `TickFlag::Stale` and a counter, and the data still
flows. Losing a real trade to a clock problem is the worse failure.

**D6 — Nothing here reads a clock.** `local_now` is a parameter, so a replay
of the same inputs produces the same decisions (rule 6, rule 10).

**D7 — This card knows nothing about transport.**
It mutates a struct and returns a verdict; the caller owns the rings and calls
`note_dropped_full` when one backs up. A normaliser that owned the ring would
be untestable without one, and backpressure is the caller's information
anyway — it means *their* consumer is too slow.

---

## 3. FILE MANIFEST

```
CREATE   feed/normaliser.hpp
CREATE   feed/tests/test_normaliser.cpp
MODIFY   feed/CMakeLists.txt
```

---

## 4. INTERFACE CONTRACT

```cpp
#pragma once

#include <core/time/exchange_ts.hpp>
#include <feed/tick.hpp>

namespace altair {

/// What the caller should do with the struct it just submitted.
enum class Verdictum : std::uint8_t {
    Publish,       // normalised; push it
    DropFuture,    // AfterCeiling — the look-ahead guard fired (D5)
    DropInactive   // decoded from the standby source (D3)
};

class Normaliser {
public:
    struct Config {
        PlausibilityGate::Config gate;   // require_monotonic MUST be false
        FeedSource active;               // the publishing source
    };

    struct Stats {
        std::uint64_t submitted;
        std::uint64_t published;
        std::uint64_t dropped_future;    // rule 7 — never enters the pipeline
        std::uint64_t dropped_inactive;
        std::uint64_t flagged_stale;     // floor breach, flagged not dropped
        std::uint64_t out_of_order;      // per-instrument regression
        std::uint64_t dropped_ring_full; // reported by the caller
    };

    explicit Normaliser(Config cfg) noexcept;

    /// Normalise in place: assigns `seq`, sets flags, returns the verdict.
    [[nodiscard]] ALTAIR_HOT Verdictum
    submit(Tick& t, Timestamp local_now) noexcept;

    [[nodiscard]] ALTAIR_HOT Verdictum
    submit(DepthUpdate& d, Timestamp local_now) noexcept;

    /// The caller's ring was full. Backpressure is information, not noise.
    void note_dropped_full() noexcept;

    /// Switch the publishing source. P2-05 calls this; `seq` keeps counting.
    void set_active(FeedSource s) noexcept;
    [[nodiscard]] FeedSource active() const noexcept;

    [[nodiscard]] const Stats& stats() const noexcept;
    [[nodiscard]] std::uint32_t next_seq() const noexcept;
    void reset_session() noexcept;

private:
    // the implementation's own
};

} // namespace altair
```

---

## 5. REQUIREMENTS

1. The constructor **forces `gate.require_monotonic = false`** regardless of
   what the caller passed. §1 — this is not a preference to respect, it is a
   configuration that cannot be correct here, and silently honouring it would
   produce a feed that looks broken.
2. `submit` from a source other than `active()` returns `DropInactive`
   **before the gate and before any history update**, and without assigning a
   `seq`. A standby tick must consume no sequence number and must not move an
   instrument's high-water mark.
3. `AfterCeiling` returns `DropFuture`. Nothing else drops.
4. `BeforeFloor` sets `TickFlag::Stale`, counts, and publishes.
5. A strictly-decreasing `exchange_ts` for the same instrument sets
   `TickFlag::Stale`, counts in `out_of_order`, and publishes. An **equal**
   timestamp is normal and does none of this.
6. Per-instrument history is tracked for the **active source only**; switching
   sources does not reset it, because the instrument's timeline is continuous
   even when the feed reporting it changes.
   The high-water mark **never moves backwards** — a regressed tick is flagged
   but does not lower the mark, or every tick between it and the previous high
   would be reported as a further regression.
7. `seq` is assigned only to published structs, and is monotonic across a
   `set_active` call.
8. `reset_session` clears history, stats and `seq`.
9. No allocation, no exceptions, no `double`, no clock read.

---

## 6. ACCEPTANCE TESTS

1. **`monotonic_is_forced_off`** — construct with `require_monotonic = true`
   and assert the gate's config comes back `false`. Then interleave two
   instruments with decreasing timestamps across them and assert **both**
   publish — the case a shared monotonic gate would have rejected.
2. **`future_ticks_are_dropped`** — a tick beyond `local_now + max_future_skew`
   returns `DropFuture`, is counted, and consumes **no** sequence number.
   Rule 7.
3. **`floor_breach_flags_but_publishes`** — a tick before the floor publishes
   with `Stale` set and `flagged_stale` incremented. Assert the data is intact,
   so the test shows the trade was not discarded.
4. **`equal_timestamps_are_normal`** — three ticks for one instrument at the
   *same* `exchange_ts` all publish with no `out_of_order` and no `Stale`. This
   is the direct consequence of Kite's one-second resolution and the case a
   naive strict-monotonic check would break.
5. **`regression_flags_but_publishes`** — a strictly-decreasing timestamp for
   one instrument sets `Stale`, counts `out_of_order`, and still publishes.
6. **`inactive_source_is_not_published`** — a tick from the standby source is
   `DropInactive`, consumes no sequence, and does not disturb per-instrument
   history.
7. **`seq_survives_a_failover`** — publish some ticks, `set_active` to the
   other source, publish more, and assert the sequence is **continuous and
   monotonic across the switch** with no repeats and no gaps.
8. **`depth_shares_the_path`** — a `DepthUpdate` goes through the same gating
   and sequencing; a future one drops, a stale one flags, and the sequence is
   shared with ticks rather than being a separate counter.

---

## REVIEW RECORD — P2-04

Reviewed 2026-08-31. MSVC 19.51.36256, `/std:c++latest /W4 /permissive- /O2`.

| Gate | Verdict | Evidence |
|---|---|---|
| 1 compiles clean | PASS | zero warnings. One compile error first: `seconds` lives in `altair::duration`, not `altair`. |
| 2 contract honoured | PASS, **card corrected (D3)** | the implementation returns before the gate for a standby source; the card said both feeds were gated. The **code was right** — see below. |
| 3 manifest respected | PASS | `feed/normaliser.hpp`, `feed/tests/test_normaliser.cpp`, `feed/CMakeLists.txt`. |
| 4 tests pass | PASS | 8 named tests + session/backpressure + benchmark. |
| 5 no hot-path allocation | PASS | `ALTAIR_HOT`. Fixed arrays, no `new`/`malloc`/`vector`/`string`/`function`. |
| 6 latency budget | **PASS, measured** | see the run output; a `submit` is a gate check plus two array accesses. |
| 7 numerical / financial | PASS | no floating point. No arithmetic on money at all — this card only orders and routes. |
| 8 physics | PASS | equal timestamps are normal, because Kite's resolution is one second (P2-02). The look-ahead guard is the only drop: rule 7 admits no flag-and-continue. |

### The defect review found, in the card

D3 said "both feeds are gated and tracked so their staleness is observable".
The implementation returns `DropInactive` **before** the gate. The
implementation is correct and the card was wrong, for two reasons that only
became clear once both existed:

* `PlausibilityGate` holds one `last_` and is a **per-source** object. Feeding
  it two interleaved sources makes its floor and ceiling statistics
  meaningless — they would describe a mixture.
* A standby feed running ahead would move each instrument's high-water mark,
  and then **every active tick behind it would be reported out of order**. Test
  6 asserts exactly this: an active tick *earlier* than a standby one still
  publishes clean.

Standby health belongs to P2-05's watchdog, keyed on `recv_ts` — which is the
direct consequence of P2-02's finding that Kite's `exchange_ts` cannot measure
sub-second staleness.

### Corrected in passing

`sizeof(Normaliser)` is **72 KB**, not the "multi-megabyte" a test comment
claimed. It is comfortably stack-safe; it stays at file scope only so the
placement-new reset between tests is well defined.
