# P0-09b — `feed/replay`: a replayer that cannot expose a future tick

> Phase 0 · Card 9b of 14 · Status: DONE (implemented by Claude, 2026-08-29 — see LEDGER)
> Depends on: P0-01 · P0-02 (`timestamp`) · P0-09a — all DONE
> Paste everything below the line into DeepSeek V4 as a single prompt.
>
> **Architect's note (not part of the prompt).** Second half of the P0-09 split.
> This is the first card outside `core/`, so it also un-comments
> `add_subdirectory(feed)` in the root `CMakeLists.txt` — the only root edit any
> card has made, and it is in the manifest.
>
> The design decision that matters: **look-ahead is prevented structurally, not
> by discipline.** There is no `peek()`, no `at(i)`, no `operator[]`, no
> iterator. The only way to obtain a tick is `next()`, which advances the
> cursor. A strategy *cannot* read ahead because the type offers no way to,
> and the acceptance tests assert that absence with concepts — the same
> technique P0-01 used to prove `Price * Lots` does not compile.
>
> `ReplayTick` here is a **skeleton**, not the real tick. P2-01 defines the
> normalised `Tick` and `DepthUpdate`; this carries the minimum a null strategy
> needs so the Phase 0 exit criterion can run end to end.

---

## 1. CONTEXT

You are implementing the tick replayer of Altair, a C++23 low-latency trading
engine for Indian equity markets.

CLAUDE.md rule 7: **no look-ahead, ever** — "a replayer that physically cannot
expose a future tick", and "strategies read time off the tick, never from a wall
clock". Rule 6: **backtest and live share the same code path**, so the replayer
emits the same struct into the same pipeline a live feed would.

Look-ahead in a backtest is the most expensive bug in this domain, because it
does not crash — it produces a *better* result, and the better result is what
gets believed. A replayer that exposes `ticks[i+1]` will eventually have someone
read it, and no test of the strategy will ever catch it. So the guarantee has to
live in the type, not in a code review.

`now()` is the other half of the same rule. It returns the timestamp of the tick
*last delivered* — never the next one, never a wall clock. A strategy asking
"what time is it?" gets the market's answer, and in replay that is the only
answer that reproduces.

---

## 2. FILE MANIFEST

Create exactly these three files, and modify exactly one.

```
CREATE   feed/replay.hpp
CREATE   feed/CMakeLists.txt
CREATE   feed/tests/test_replay.cpp
MODIFY   CMakeLists.txt          (root — uncomment add_subdirectory(feed))
```

In the root `CMakeLists.txt`, change the commented line
`# add_subdirectory(feed)            # Phase 2` to an active
`add_subdirectory(feed)` and update its trailing comment to note that Phase 0
lands the replay skeleton. **Change nothing else in that file** — not the
options, not the flags, not the other commented subdirectories.

`feed/CMakeLists.txt` declares `altair_feed` as an **INTERFACE** library,
aliases it `altair::feed`, exports `${CMAKE_CURRENT_SOURCE_DIR}/..` as the
include root so consumers write `#include <feed/replay.hpp>`, links
`altair_types`, `altair_time`, `altair_invariant` and `altair_flags`, and under
`if(ALTAIR_BUILD_TESTS)` registers `altair_replay_test` with
`add_test(NAME replay COMMAND altair_replay_test)`.

**Do not touch** `vcpkg.json`, `core/CMakeLists.txt`, or anything under `core/`.

---

## 3. INTERFACE CONTRACT

```cpp
#pragma once

#include <invariant/conservation.hpp>
#include <time/timestamp.hpp>
#include <types/units.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// Why a replay step failed.
// ─────────────────────────────────────────────────────────────────────────
enum class ReplayError : std::uint8_t {
    Exhausted,      // no ticks remain
    Empty,          // the replayer holds no ticks at all
    NotMonotonic    // the source is out of order — it is not a replayable session
};

// ─────────────────────────────────────────────────────────────────────────
// ReplayTick — the SKELETON tick. P2-01 defines the real normalised Tick and
// DepthUpdate; this carries the minimum a null strategy needs so the Phase 0
// exit criterion can run end to end.
//
// Trivially copyable and 40 bytes, so it can ride an SpscRing unchanged when
// the live path arrives.
// ─────────────────────────────────────────────────────────────────────────
struct ReplayTick {
    /// Exchange instant. UNIT: nanoseconds since the Unix epoch, UTC.
    Timestamp ts;
    /// Monotonic sequence number within the session. UNIT: none.
    std::uint64_t seqno;
    /// Instrument token. UNIT: none. A token, never a symbol string.
    std::uint32_t token;
    std::uint32_t reserved;
    /// Last traded price. UNIT: paise.
    Price last;
    /// Last traded quantity. UNIT: units.
    Qty qty;
};

// ─────────────────────────────────────────────────────────────────────────
// Replayer — a cursor over a session, and nothing more.
//
// THERE IS DELIBERATELY NO peek(), at(), operator[], begin(), OR ANY OTHER
// WAY TO SEE A TICK THE CURSOR HAS NOT REACHED. That is rule 7 enforced by
// the type rather than by discipline: a strategy cannot read ahead because
// the interface offers no means to.
//
// Borrows the tick array; the caller owns it and must outlive the Replayer.
// Not thread-safe: one replayer per replay thread.
// ─────────────────────────────────────────────────────────────────────────
class Replayer {
public:
    /// An empty replayer. Every next() returns Empty.
    constexpr Replayer() noexcept = default;

    /// Borrow a session. UNIT: none.
    /// PRECONDITION: `ticks` points to `n` ticks and outlives this object.
    /// Does NOT validate ordering — call validate() for that, once, up front.
    constexpr Replayer(const ReplayTick* ticks, std::size_t n) noexcept;

    /// Deliver the next tick and advance. UNIT: none.
    /// Returns Empty when the replayer holds nothing, Exhausted at the end,
    /// and NotMonotonic if the next tick would move seqno or time backwards —
    /// a corrupt source must stop the replay, not silently reorder it.
    /// PRECONDITION: called from exactly one thread.
    [[nodiscard]] ALTAIR_HOT std::expected<ReplayTick, ReplayError> next() noexcept;

    /// The current market instant: the timestamp of the tick LAST DELIVERED.
    /// UNIT: nanoseconds since the Unix epoch, UTC.
    /// Timestamp::epoch() before the first delivery. NEVER the next tick's
    /// time, and never a wall clock — this is the only clock a strategy may
    /// read during replay (CLAUDE.md rule 7).
    [[nodiscard]] ALTAIR_HOT Timestamp now() const noexcept;

    /// Ticks delivered so far. UNIT: count.
    [[nodiscard]] constexpr std::size_t delivered() const noexcept;

    /// Ticks not yet delivered. UNIT: count.
    [[nodiscard]] constexpr std::size_t remaining() const noexcept;

    /// Total ticks in the session. UNIT: count.
    [[nodiscard]] constexpr std::size_t size() const noexcept;

    /// True iff every tick has been delivered. UNIT: none.
    [[nodiscard]] constexpr bool exhausted() const noexcept;

    /// Verify the WHOLE source is monotonic in both seqno and timestamp.
    /// UNIT: none. O(n) — call once before replaying, never on the hot path.
    /// Returns Empty for an empty source, NotMonotonic on the first violation.
    [[nodiscard]] std::expected<void, ReplayError> validate() const noexcept;

    /// Return the cursor to the start. UNIT: none. For a second pass over the
    /// same session — walk-forward folds in P6-06 will need it.
    void rewind() noexcept;

private:
    const ReplayTick* ticks_ = nullptr;
    std::size_t n_ = 0;
    std::size_t cursor_ = 0;
    Timestamp now_{};
    std::uint64_t last_seqno_ = 0;
    bool started_ = false;
};

} // namespace altair
```

---

## 4. BEHAVIOURAL SPEC

1. `next()` returns `Empty` when `ticks_ == nullptr || n_ == 0`, and
   `Exhausted` when `cursor_ == n_`. `Empty` and `Exhausted` are distinct: an
   empty source is a setup bug, a finished one is normal termination.
2. On success `next()` copies `ticks_[cursor_]`, advances `cursor_`, sets
   `now_` to that tick's timestamp, records its seqno, sets `started_`, and
   returns the tick **by value**. It never hands back a pointer or reference
   into the array — a reference would let a caller do pointer arithmetic and
   walk forward.
3. `next()` returns `NotMonotonic` **without advancing** if the candidate
   tick's `seqno <= last_seqno_` or its `ts < now_`, once `started_`. A corrupt
   source stops the replay; it is never silently reordered.
4. Note the asymmetry in item 3: seqno must **strictly** increase, timestamps
   may **repeat**. Two trades can share a nanosecond; two ticks cannot share a
   sequence number.
5. `now()` returns `Timestamp::epoch()` until the first successful `next()`,
   and thereafter the timestamp of the tick last delivered. **It never reads a
   wall clock and never returns a tick the caller has not received.**
6. `delivered()` is `cursor_`, `remaining()` is `n_ - cursor_`, `size()` is
   `n_`, `exhausted()` is `cursor_ == n_` (and true for an empty replayer).
7. `validate()` walks the whole array once: every `seqno` strictly increasing,
   every `ts` non-decreasing. Returns `Empty` for `n_ == 0`. It is `const` and
   does not move the cursor.
8. `rewind()` sets `cursor_` to 0, `now_` to `Timestamp::epoch()`,
   `last_seqno_` to 0, and `started_` to false — the replayer is exactly as
   constructed.
9. **The class exposes no way to read a tick at an index the cursor has not
   reached.** No `peek`, `at`, `operator[]`, `begin`, `end`, `data`, or
   iterator. This is the card's central requirement and the tests assert the
   absence with concepts.
10. `ReplayTick` is trivially copyable, `sizeof(ReplayTick) == 40`, and
    `alignof(ReplayTick) >= 8`, so it rides an `SpscRing` unchanged when the
    live path arrives.
11. `next()` and `now()` are `ALTAIR_HOT`: no allocation, no exception, no
    syscall, no clock read.
12. Nothing in this file reads a clock, opens a file, or allocates. Loading a
    session from disk is P2-06/P2-07.

---

## 5. CONSTRAINTS

- C++23. Standard library plus P0-01, P0-02 and P0-09a headers.
  **No new dependency.**
- Header-only.
- No `<chrono>`, no `<fstream>`, no `<filesystem>`, no `<iostream>`.
- No exceptions, no `throw`, no allocation.
- No `using namespace` at file scope in a header.
- Internal helpers in `namespace altair::detail`, never an anonymous namespace.
- Compiles clean at `/W4` and `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion -Wold-style-cast`.
- Tests are a plain `int main()`.

---

## 6. ACCEPTANCE TESTS

`feed/tests/test_replay.cpp`, plain `main()`, `check(bool, const char*)` helper
counting failures, returns 0 only if all pass. Use exactly these names:

```cpp
void test_replay_tick_layout();
void test_replay_sequential_delivery();
void test_replay_now_never_leads();
void test_replay_no_lookahead_api();
void test_replay_rejects_non_monotonic();
void test_replay_validate();
void test_replay_exhaustion_and_rewind();
void test_replay_drives_a_null_strategy();
```

Build sessions from a static array. Anchor timestamps on the P0-02 instant,
`Timestamp{1787888700000000000LL}` — 2026-08-28 09:15:00 IST.

**test_replay_tick_layout**
```
sizeof(ReplayTick) == 40
alignof(ReplayTick) >= 8
std::is_trivially_copyable_v<ReplayTick>
```

**test_replay_sequential_delivery**
```
3 ticks, seqno 1,2,3, ts kOpen, kOpen+1ms, kOpen+2ms, prices 100,101,102.
Replayer r{ticks, 3};
r.size() == 3 && r.delivered() == 0 && r.remaining() == 3 && !r.exhausted()

r.next().value().last == Price{100};  r.delivered() == 1; r.remaining() == 2
r.next().value().last == Price{101};  r.delivered() == 2
r.next().value().seqno == 3;          r.exhausted()
r.next().error() == ReplayError::Exhausted
r.delivered() == 3                     // a failed next() does not advance
```

**test_replay_now_never_leads**
The heart of rule 7.
```
Replayer r{ticks, 3};
r.now() == Timestamp::epoch()          // BEFORE the first tick, not ticks[0].ts

const auto t0 = r.next().value();
r.now() == t0.ts                        // exactly the tick just delivered
r.now() != ticks[1].ts                  // and NOT the next one

const auto t1 = r.next().value();
r.now() == t1.ts
r.now() > t0.ts                         // time advances only on delivery

// now() is stable between deliveries - it is not a clock.
const auto a = r.now(); const auto b = r.now();
a == b
```

**test_replay_no_lookahead_api**
The structural guarantee, asserted the way P0-01 asserted `Price * Lots`.
Use the named-concept pattern (MSVC hard-errors on a bare negative
requires-expression — verified in P0-01: 19.51.36256, C2678/C2679).
```cpp
namespace probes {
template <typename R> concept has_peek     = requires(R r) { r.peek(); };
template <typename R> concept has_at       = requires(R r) { r.at(0); };
template <typename R> concept has_index    = requires(R r) { r[0]; };
template <typename R> concept has_begin    = requires(R r) { r.begin(); };
template <typename R> concept has_data     = requires(R r) { r.data(); };
} // namespace probes

static_assert(!probes::has_peek<altair::Replayer>,
              "a replayer that can peek can leak the future");
static_assert(!probes::has_at<altair::Replayer>);
static_assert(!probes::has_index<altair::Replayer>);
static_assert(!probes::has_begin<altair::Replayer>);
static_assert(!probes::has_data<altair::Replayer>);

// next() must return BY VALUE, not by reference into the array - a reference
// would let a caller do pointer arithmetic and walk forward.
static_assert(std::is_same_v<
    decltype(std::declval<altair::Replayer&>().next()),
    std::expected<altair::ReplayTick, altair::ReplayError>>);
```

**test_replay_rejects_non_monotonic**
```
// seqno must STRICTLY increase.
ticks with seqno 1, 1 (repeated)  -> second next() is NotMonotonic
                                   -> delivered() stays 1 (no advance)
// timestamps may REPEAT - two trades can share a nanosecond.
ticks with seqno 1,2 and IDENTICAL ts -> both deliver fine
// but time must not go BACKWARDS.
ticks with seqno 1,2 and ts kOpen, kOpen-1ms -> second is NotMonotonic
```

**test_replay_validate**
```
a well-formed 3-tick session       -> validate() succeeds
an empty replayer                  -> validate() is Empty
seqno 1,3,2                        -> validate() is NotMonotonic
ts going backwards                 -> validate() is NotMonotonic
validate() does not move the cursor: delivered() == 0 after it
```

**test_replay_exhaustion_and_rewind**
```
drain all 3 ticks; r.exhausted() && r.remaining() == 0
r.rewind();
r.delivered() == 0 && r.remaining() == 3 && !r.exhausted()
r.now() == Timestamp::epoch()          // rewind resets the clock too
r.next().value().seqno == 1             // and the sequence restarts
// A second full pass delivers exactly the same ticks.

// The empty replayer.
Replayer e;
e.size() == 0 && e.exhausted()
e.next().error() == ReplayError::Empty  // Empty, NOT Exhausted
e.now() == Timestamp::epoch()
Replayer z{nullptr, 5};
z.next().error() == ReplayError::Empty
```

**test_replay_drives_a_null_strategy**
The Phase 0 exit criterion in miniature: a session runs end to end through a
null strategy with the conservation ledger armed, and the invariant holds at
every tick.
```
Build a 1'000-tick session (seqno 1..1000, ts kOpen + i ms, alternating
prices). Run it:

  ConservationLedger ledger{Notional{100'000'000}};
  Replayer r{ticks, 1000};
  r.validate().has_value()
  while (auto t = r.next()) {
      // The null strategy: reads the tick, trades on alternate ticks, and
      // reads time ONLY from the replayer.
      if (t->seqno % 100 == 0) {
          ledger.on_fill(Qty{1}, t->last, Notional{10});
      }
      ledger.check_and_trip().has_value()      // armed, every tick
      r.now() == t->ts                          // the clock IS the tick
  }

  r.delivered() == 1000
  !ledger.is_breached()
  ledger.check().has_value()
  ledger.fill_count() == 10
// Report ticks/second.
```

### Latency reporting — not a pass/fail assertion

Batch-timed: `next()` over a large session. **Budget: < 10 ns** — it is a bounds
check, a copy, and two stores. Print; do not assert.

---

## 7. FORBIDDEN

- Adding a file not in the manifest. Editing anything in the root
  `CMakeLists.txt` other than the single `add_subdirectory(feed)` line.
- Touching `vcpkg.json`, `core/CMakeLists.txt`, or anything under `core/`.
- Changing any signature in the interface contract.
- **Adding `peek()`, `at()`, `operator[]`, `begin()`, `end()`, `data()`, an
  iterator, or any other way to observe a tick the cursor has not reached.**
  This is the card. Rule 7.
- Returning a reference or pointer to a tick instead of a copy.
- `now()` reading a wall clock, or returning the *next* tick's timestamp.
- Advancing the cursor on a failed `next()`.
- Silently reordering or skipping a non-monotonic tick.
- Requiring timestamps to strictly increase — two trades can share a nanosecond.
- Any file I/O, `<chrono>`, allocation, or exception.
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

Return the four files in full, then your ASSUMPTIONS section, which must state
the measured ns for `next()` and confirm in your own words why the absence of
`peek()` is a stronger guarantee than a comment telling callers not to use it.
