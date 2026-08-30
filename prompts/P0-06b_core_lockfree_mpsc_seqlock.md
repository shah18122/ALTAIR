# P0-06b — `core/lockfree/mpsc_ring` + `seqlock`: many writers, one reader

> Phase 0 · Card 6b of 14 · Status: DONE (implemented by Claude, 2026-08-29 — see LEDGER)
> Depends on: P0-01 (`core/types/units.hpp`) · P0-06a (`core/lockfree/spsc_ring.hpp`) — DONE
> Paste everything below the line into DeepSeek V4 as a single prompt.
>
> **Architect's note (not part of the prompt).** Second half of the P0-06 split.
> The same warning as P0-06a applies and is more serious here: **x86 is TSO, so
> every acquire/release compiles to a plain `mov`, and no TSAN or ASAN runs on
> this box.** The MPSC ring uses a compare-exchange loop and the seqlock uses an
> odd/even version protocol — both are algorithms where a wrong ordering is
> invisible on x86 and fatal on ARM. The orderings are specified, not discovered.
>
> One design decision: the MPSC ring uses **per-slot sequence numbers** (the
> Vyukov bounded-queue scheme), not a shared tail plus a "published" flag. The
> sequence number makes each slot self-describing, so a producer that wins the
> CAS but is descheduled before writing cannot be mistaken for a completed push.

---

## 1. CONTEXT

You are implementing the multi-producer queue and the snapshot primitive of
Altair, a C++23 low-latency trading engine for Indian equity markets.

Two shapes that P0-06a's SPSC ring cannot cover:

- **MPSC ring.** Several strategy threads emit orders into one OMS thread;
  several decoders feed one aggregator. Many producers, one consumer. This
  needs a compare-exchange on the producer side, which SPSC does not.
- **Seqlock.** One writer publishes a *value* — the current book top, a greeks
  snapshot, a config generation — that many readers sample. Readers must never
  see a half-written struct (a "torn" read), and must never block the writer.
  A mutex would let a slow reader stall the writer; a seqlock cannot.

The seqlock's trade is explicit: **readers may fail and retry, writers never
block.** That is the correct trade for market data, where a reader that missed
the last update wants the *next* one, not a stale one it waited for.

---

## 2. FILE MANIFEST

Create exactly these three files, and modify exactly one.

```
CREATE   core/lockfree/mpsc_ring.hpp
CREATE   core/lockfree/seqlock.hpp
CREATE   core/lockfree/tests/test_mpsc_seqlock.cpp
MODIFY   core/lockfree/CMakeLists.txt
```

Add `altair_mpsc_seqlock_test` from `tests/test_mpsc_seqlock.cpp`, linking
`Threads::Threads`, with
`add_test(NAME mpsc_seqlock COMMAND altair_mpsc_seqlock_test)`. `altair_lockfree`
stays INTERFACE — do not change its type, and leave `altair_spsc_ring_test` alone.

**Do not touch** `core/lockfree/spsc_ring.hpp`, `core/CMakeLists.txt`, the root
`CMakeLists.txt`, `vcpkg.json`, `core/types/`, `core/time/`, or `core/mem/`.

---

## 3. INTERFACE CONTRACT

### `core/lockfree/mpsc_ring.hpp`

```cpp
#pragma once

#include <lockfree/spsc_ring.hpp>   // for kCacheLine
#include <types/units.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// MpscRing — bounded lock-free queue for MANY producer threads and EXACTLY
// ONE consumer thread.
//
// Each slot carries its own sequence number, so a slot is self-describing:
// a producer that wins the CAS but is descheduled before writing cannot be
// mistaken for a completed push. Using it from two consumers is undefined
// behaviour.
// ─────────────────────────────────────────────────────────────────────────
template <typename T, std::size_t Capacity>
class MpscRing {
    static_assert(std::is_trivially_copyable_v<T>,
                  "MpscRing stores T by value with no destructor call");
    static_assert(Capacity >= 2, "a ring of one cannot distinguish full from empty");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    using value_type = T;

    /// Initialises slot i's sequence number to i. NOT constexpr: the atomics
    /// must be individually initialised.
    MpscRing() noexcept;

    MpscRing(const MpscRing&) = delete;
    MpscRing& operator=(const MpscRing&) = delete;
    MpscRing(MpscRing&&) = delete;
    MpscRing& operator=(MpscRing&&) = delete;

    /// Enqueue one element. SAFE FROM ANY NUMBER OF PRODUCER THREADS.
    /// UNIT: none. Returns false iff the ring is full. Never blocks, never
    /// allocates. Lock-free but not wait-free: contention retries the CAS.
    /// PRECONDITION: none.
    [[nodiscard]] ALTAIR_HOT bool try_push(const T& v) noexcept;

    /// Dequeue one element into `out`. CONSUMER THREAD ONLY.
    /// UNIT: none. Returns false iff the ring is empty; `out` is untouched then.
    /// PRECONDITION: called from exactly one thread, for the life of the ring.
    [[nodiscard]] ALTAIR_HOT bool try_pop(T& out) noexcept;

    /// Slots in the ring. UNIT: elements.
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

    /// Total successful pushes. UNIT: elements. APPROXIMATE under concurrency:
    /// a producer may have claimed a slot without having published it yet.
    [[nodiscard]] std::uint64_t pushed_approx() const noexcept;

    /// Total successful pops. UNIT: elements. Exact — there is one consumer.
    [[nodiscard]] std::uint64_t popped() const noexcept;

private:
    struct Slot {
        std::atomic<std::uint64_t> seq;
        T data;
    };

    alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};
    alignas(kCacheLine) Slot slots_[Capacity];
};

} // namespace altair
```

### `core/lockfree/seqlock.hpp`

```cpp
#pragma once

#include <lockfree/spsc_ring.hpp>   // for kCacheLine
#include <types/units.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// SeqlockSnapshot — one writer publishes a value, many readers sample it.
//
// Readers never block the writer and never see a torn value. The trade is
// that a reader CAN FAIL and must retry — correct for market data, where a
// reader that missed an update wants the next one, not a stale one.
//
// The version is even when stable and odd while a write is in flight.
// ─────────────────────────────────────────────────────────────────────────
template <typename T>
class SeqlockSnapshot {
    static_assert(std::is_trivially_copyable_v<T>,
                  "a seqlock copies T byte-wise while a writer may be racing it");

public:
    constexpr SeqlockSnapshot() noexcept = default;

    SeqlockSnapshot(const SeqlockSnapshot&) = delete;
    SeqlockSnapshot& operator=(const SeqlockSnapshot&) = delete;

    /// Publish a new value. WRITER THREAD ONLY — exactly one, for the life of
    /// the object. UNIT: none. Never blocks. Advances version() by 2.
    /// PRECONDITION: called from exactly one thread.
    ALTAIR_HOT void store(const T& v) noexcept;

    /// Sample the value. SAFE FROM ANY NUMBER OF READER THREADS.
    /// UNIT: none. Returns false iff a write was in flight, in which case
    /// `out` holds an INDETERMINATE value and must not be used.
    /// PRECONDITION: none.
    [[nodiscard]] ALTAIR_HOT bool try_load(T& out) const noexcept;

    /// Sample the value, retrying until it succeeds. SAFE FROM ANY READER.
    /// UNIT: none. Spins; on a single-core box a writer in a tight loop could
    /// starve this, so prefer try_load() on a latency-critical path.
    /// PRECONDITION: none.
    [[nodiscard]] ALTAIR_HOT T load() const noexcept;

    /// The current version counter. UNIT: writes x 2.
    /// EVEN when stable, ODD while a write is in flight. Zero before the first
    /// store(). Readers can use a change in this value to detect an update.
    [[nodiscard]] std::uint64_t version() const noexcept;

private:
    alignas(kCacheLine) std::atomic<std::uint64_t> seq_{0};
    alignas(kCacheLine) T value_{};
};

} // namespace altair
```

---

## 4. BEHAVIOURAL SPEC

Orderings are **mandatory and exact** — unverifiable on x86, so specified.

1. `MpscRing` constructor sets `slots_[i].seq` to `i`, with
   `memory_order_relaxed` (no reader exists yet).
2. `MpscRing::try_push`, per Vyukov: load `tail_` relaxed; read
   `slots_[pos & mask].seq` with **acquire**; compute
   `dif = static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos)`;
   - `dif == 0` → try `tail_.compare_exchange_weak(pos, pos + 1, relaxed)`;
     on success, break out and claim the slot;
   - `dif < 0` → the ring is **full**, return false;
   - `dif > 0` → another producer won; reload `tail_` relaxed and retry.
   After claiming: write `data`, then `seq.store(pos + 1, release)`. **The
   signed difference is essential** — comparing the unsigned values directly
   mis-orders across the 64-bit wrap.
3. `MpscRing::try_pop`: load `head_` relaxed; read that slot's `seq` with
   **acquire**; `dif = seq - (pos + 1)`;
   - `dif == 0` → `head_.store(pos + 1, relaxed)`, read `data`, then
     `seq.store(pos + Capacity, release)` to re-arm the slot for the next lap;
   - `dif < 0` → **empty**, return false, leave `out` untouched;
   - `dif > 0` → cannot happen with one consumer; treat as empty.
4. `pushed_approx()` is `tail_` loaded relaxed; `popped()` is `head_` loaded
   relaxed. `pushed_approx` is named for the truth: a producer may have won the
   CAS without having published, so it can exceed the number of *visible*
   elements. Do not rename it to `pushed()`.
5. `try_pop` leaves `out` untouched when it returns false.
6. `SeqlockSnapshot::store`: load `seq_` relaxed into `s`; `seq_.store(s + 1,
   release)` (now odd); write `value_`; `seq_.store(s + 2, release)` (now even).
   A `std::atomic_thread_fence(std::memory_order_release)` between the value
   write and the final store is required so the value write cannot sink past it.
7. `SeqlockSnapshot::try_load`: load `seq_` with **acquire** into `s0`; if `s0`
   is **odd**, return false immediately — a write is in flight; copy `value_`;
   `std::atomic_thread_fence(std::memory_order_acquire)`; load `seq_` acquire
   into `s1`; return `s0 == s1`. A differing `s1` means the value was rewritten
   under us and the copy may be torn.
8. `load()` calls `try_load` in a loop until it succeeds. It must not be marked
   `noexcept(false)` and must not have a retry cap — a cap would return a torn
   value, which is worse than spinning.
9. `version()` returns `seq_` loaded with acquire. It is **zero before the first
   store**, even after construction, and advances by exactly 2 per `store`.
10. `try_load` returning false leaves `out` **indeterminate**, not unchanged.
    Say so in the doc comment — a caller that uses `out` after a false return is
    reading a possibly-torn value, and this is the one place where "unchanged"
    would be a more expensive promise than it is worth.
11. Both types are non-copyable and non-movable.
12. `MpscRing`'s `tail_`, `head_`, and `slots_` are each `alignas(kCacheLine)`;
    `SeqlockSnapshot`'s `seq_` and `value_` likewise. Producers hammering
    `tail_` must not invalidate the consumer's `head_` line.

---

## 5. CONSTRAINTS

- C++23. Standard library only. **No new dependency.**
- Header-only, includes exactly as in the contract.
- No mutex, no condition variable, no `std::thread` in either header.
- No allocation, no exceptions, no `iostream`.
- Internal helpers in `namespace altair::detail`, never an anonymous namespace.
- No `using namespace` at file scope in a header.
- Compiles clean at `/W4` and `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion -Wold-style-cast`.
- Tests are a plain `int main()` and **must link `Threads::Threads`**.

---

## 6. ACCEPTANCE TESTS

`core/lockfree/tests/test_mpsc_seqlock.cpp`, plain `main()`, `check` helper,
returns 0 only if all pass. Use exactly these names:

```cpp
void test_mpsc_layout_and_traits();
void test_mpsc_single_thread_fifo();
void test_mpsc_fill_and_drain();
void test_mpsc_multi_producer_stress();
void test_seqlock_basic_store_load();
void test_seqlock_version_protocol();
void test_seqlock_concurrent_reader_never_tears();
```

**test_mpsc_layout_and_traits**
```
MpscRing<std::uint64_t, 8>::capacity() == 8
alignof(MpscRing<std::uint64_t, 8>) >= kCacheLine
!std::is_copy_constructible_v<MpscRing<std::uint64_t, 8>>
!std::is_move_constructible_v<MpscRing<std::uint64_t, 8>>
MpscRing<std::uint64_t, 8> r;  r.pushed_approx() == 0 && r.popped() == 0
```

**test_mpsc_single_thread_fifo**
```
MpscRing<int, 4> r;
r.try_push(1) && r.try_push(2) && r.try_push(3)
int v = 0;
r.try_pop(v) && v == 1
r.try_pop(v) && v == 2
r.try_pop(v) && v == 3
!r.try_pop(v)
```

**test_mpsc_fill_and_drain**
```
MpscRing<int, 4> r;
// All 4 slots usable.
r.try_push(10) && r.try_push(20) && r.try_push(30) && r.try_push(40)
!r.try_push(50)                          // full rejects
int v = 0;
r.try_pop(v) && v == 10                  // oldest survived
r.try_push(50)                           // slot re-armed for the next lap
r.pushed_approx() == 5 && r.popped() == 1

// Drain fully and confirm the re-arm worked across the wrap.
r.try_pop(v) && v == 20
r.try_pop(v) && v == 30
r.try_pop(v) && v == 40
r.try_pop(v) && v == 50
!r.try_pop(v)
```

**test_mpsc_multi_producer_stress**
The real test. **4 producer threads × 250'000 elements each = 1'000'000**,
one consumer.
```
Each producer p pushes values encoded as (p * 1'000'000 + i), so every value in
the run is unique and its producer is recoverable.

The consumer pops 1'000'000 values and records, per producer, how many it saw
and whether that producer's values arrived in ASCENDING order.

  total popped == 1'000'000
  each of the 4 producers contributed exactly 250'000
  each producer's own values arrived strictly in order   // per-producer FIFO
  no value seen twice                                    // use a seen-bitset
  r.popped() == 1'000'000
```
Per-producer ordering is the meaningful invariant: an MPSC ring does **not**
promise a global order across producers, but it must never reorder one
producer's own pushes, and must never duplicate or drop.

**test_seqlock_basic_store_load**
```
struct Big { std::uint64_t a, b, c, d; };
SeqlockSnapshot<Big> s;
s.version() == 0                          // zero before the first store

s.store(Big{1, 2, 3, 4});
Big out{};
s.try_load(out) && out.a == 1 && out.b == 2 && out.c == 3 && out.d == 4
s.load().a == 1

s.store(Big{5, 6, 7, 8});
s.load().d == 8
```

**test_seqlock_version_protocol**
```
SeqlockSnapshot<std::uint64_t> s;
s.version() == 0                          // even, and zero
s.store(42);
s.version() == 2                          // advances by exactly 2
s.store(43);
s.version() == 4
s.version() % 2 == 0                       // even when quiescent
// A reader can detect an update purely from the version.
const auto v0 = s.version();
s.store(44);
s.version() != v0
```

**test_seqlock_concurrent_reader_never_tears**
The one that matters. One writer thread, **two** reader threads.

**The amount of evidence must not depend on the scheduler.** Do not run the
readers for a fixed duration and then assert on how many succeeded — that
asserts on a scheduling outcome, not on the seqlock. Instead:

```
struct Big { std::uint64_t a, b, c, d; };
constexpr std::uint64_t kTargetPerReader = 100'000;
constexpr std::uint64_t kMaxAttempts     = 200'000'000;   // so a broken
                                                          // seqlock fails
                                                          // instead of hanging

// The writer always stores a CONSISTENT tuple: {n, n+1, n+2, n+3}.
// A torn read shows a value whose fields are not that pattern.
writer: while (readers_done < 2):
            s.store(Big{n, n+1, n+2, n+3}); ++n;
            <short bounded spin, ~200 iterations of an accumulator>

each reader: until it has kTargetPerReader SUCCESSFUL loads (or hits the cap):
    Big g{};
    if (s.try_load(g)):
        ++successes;
        if (g.b != g.a + 1 || g.c != g.a + 2 || g.d != g.a + 3) ++tears;
    else:
        ++retries;
    // then ++readers_done

  tears == 0                                  // THE assertion
  no reader hit kMaxAttempts
  successes >= 2 * kTargetPerReader           // AT LEAST - see below
// Print retries and the retry rate. Both vary with load - report, never assert.
```

**Why the writer needs the spin gap.** Without it the writer holds an odd
sequence for most of its ~1.6 ns cycle, readers retry ~99.98% of the time, and
the readers take far longer to reach their target. Real market data is not a
1.6 ns firehose, so the gap is also the more honest model. With it the retry
rate lands around 81–97% depending on load — and, crucially, the *evidence* is
200'000 checked reads regardless.

**Three traps this test fell into, all of which the card now forbids:**

1. **Flaky floor.** The first version asserted `successes > 100'000` after a
   fixed number of writes. Successes ranged 56'000–206'000 with machine load —
   it failed about one run in three, and `tears == 0` passed every time. It was
   asserting on a scheduling outcome.
2. **False positive from the initial state.** A default-constructed
   `SeqlockSnapshot<Big>` holds `{0,0,0,0}`, which does **not** satisfy
   `{n,n+1,n+2,n+3}`. A reader that outran the writer's first store reported
   200'000 "torn reads" that were really just the unwritten value. This is the
   dangerous kind of bug — it would have sent someone hunting a
   memory-ordering fault that does not exist. **The writer must publish one
   valid tuple and signal `writer_ready` before any reader samples.**
3. **Vacuous pass against a frozen value.** With the writer descheduled, readers
   drained all 200'000 reads of a single tuple — which satisfies the pattern
   trivially. Asserting "the value moved" afterwards just reintroduced
   flakiness (1 run in 14).

**The fix for 3 is the general lesson: put the evidence requirement in the loop's
termination condition, not in an assertion after it.** Each reader runs until it
has BOTH `kTargetPerReader` successes AND has observed the value change at least
`kMinChanges` (1'000) times, with `kMaxAttempts` as a loud backstop.

4. And one more, found only by running the full tree under load: asserting
   `successes == 2 * kTargetPerReader` was **also** wrong. The loop continues
   while *either* condition is unmet, so a reader holding its 100'000 successes
   but still short on value-changes keeps sampling and **overshoots**. Roughly
   1 run in 36. The loop was right; the assertion was not. It is `>=`: the
   guarantee is an evidence *floor*, not an exact count.

Verified across repeated full-tree runs under deliberate CPU load.

### Latency reporting — not a pass/fail assertion

Batch-timed (the P0-05b technique), single-threaded:
- `MpscRing::try_push` + `try_pop` pair — **budget < 40 ns** (a CAS costs more
  than the SPSC store; uncontended it is ~20 ns).
- `SeqlockSnapshot<Big>::store` and `try_load` — **budget < 20 ns each**.

Print; **do not assert.** Also print the SPSC pair time from P0-06a for
comparison if convenient, so the CAS cost is visible.

---

## 7. FORBIDDEN

- Adding a file not in the manifest. Modifying `spsc_ring.hpp`,
  `core/CMakeLists.txt`, the root `CMakeLists.txt`, `vcpkg.json`, or any other
  `core/` subdirectory.
- Changing any signature in the interface contract.
- **Changing any memory ordering in spec items 2, 3, 6, or 7.**
- Comparing sequence numbers as **unsigned**. The difference must be computed in
  `std::int64_t`; item 2. Unsigned comparison mis-orders across the wrap and the
  bug appears only after 2^64 operations — i.e. never in testing, and never
  diagnosably in production.
- Omitting the release fence in `store` or the acquire fence in `try_load`.
- Giving `load()` a retry cap. A capped retry returns a torn value.
- Claiming `try_load` leaves `out` unchanged on failure. It leaves it
  indeterminate; item 10.
- A mutex, spinlock, or condition variable anywhere.
- Renaming `pushed_approx()` to `pushed()`.
- A stress test that checks only totals. It must verify per-producer ordering
  and no duplicates.
- Asserting a hard latency number, or asserting the seqlock retry rate.
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
the measured ns for each operation, the seqlock tear count and retry rate, and
an explicit acknowledgement that the memory orderings were **not** validated by
any tool.
