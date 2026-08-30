# P0-06a — `core/lockfree/spsc_ring`: the feed→strategy hot path

> Phase 0 · Card 6a of 14 · Status: DONE (implemented by Claude, 2026-08-29 — see LEDGER)
> Depends on: P0-01 (`core/types/units.hpp`) — DONE
> Paste everything below the line into DeepSeek V4 as a single prompt.
>
> **Architect's note (not part of the prompt).** P0-06 splits like P0-05 did:
> three lock-free structures need five-plus files and well over eight tests.
> **P0-06a** is the SPSC ring (the one on the tick path); **P0-06b** is the MPSC
> ring and the seqlock. Card total 111 → 112.
>
> **Read this before trusting any result from this card.** Neither TSAN nor ASAN
> is available here — the root `CMakeLists.txt` guards both with `AND NOT MSVC`,
> and there is no GCC, Clang, or WSL on the dev box. Worse, **x86 is TSO**:
> `memory_order_acquire` and `memory_order_release` compile to plain `mov` on
> x86-64, so a missing or wrong ordering annotation is *completely invisible* to
> every test that runs here, and would surface only on ARM. A green stress test
> on this machine is evidence about the algorithm's logic, **not** about its
> memory ordering. Both facts go in the ledger as debt against the Phase 0 gate.
> That is why this card specifies the orderings explicitly rather than leaving
> them to the implementer: they cannot be discovered by testing on this hardware.

---

## 1. CONTEXT

You are implementing the single-producer/single-consumer ring of Altair, a C++23
low-latency trading engine for Indian equity markets.

This is the queue between the feed decoder thread and the strategy thread — the
hottest handoff in the system, crossed by every tick. A mutex here would cost a
syscall on contention and put a scheduler decision on the critical path.

Two threads, one producing and one consuming, need **no** mutual exclusion: the
producer owns the write index, the consumer owns the read index, and a single
release/acquire pair publishes each element. What they must not do is share a
cache line. If the head and tail indices land on the same 64-byte line, every
push invalidates the consumer's copy and every pop invalidates the producer's —
**false sharing**, which can cost 10x and is invisible in single-threaded tests.
The padding in the contract is load-bearing, not decoration.

P0-06b covers the multi-producer ring and the seqlock.

---

## 2. FILE MANIFEST

Create exactly these three files. Nothing else.

```
core/lockfree/spsc_ring.hpp
core/lockfree/CMakeLists.txt
core/lockfree/tests/test_spsc_ring.cpp
```

`core/CMakeLists.txt` discovers `lockfree/` automatically via its `EXISTS`
guard — **do not modify it.** `core/lockfree/CMakeLists.txt` declares
`altair_lockfree` as an **INTERFACE** library (header-only), aliases it
`altair::lockfree`, exports `${CMAKE_CURRENT_SOURCE_DIR}/..` as the include root
so consumers write `#include <lockfree/spsc_ring.hpp>`, links `altair_types` and
`altair_flags`, and under `if(ALTAIR_BUILD_TESTS)` registers
`altair_spsc_ring_test` from `tests/test_spsc_ring.cpp`, linking
`Threads::Threads`, with `add_test(NAME spsc_ring COMMAND altair_spsc_ring_test)`.
Mirror `core/types/CMakeLists.txt` in structure.

**Do not touch** `core/CMakeLists.txt`, the root `CMakeLists.txt`, `vcpkg.json`,
`core/types/`, `core/time/`, or `core/mem/`.

---

## 3. INTERFACE CONTRACT

```cpp
#pragma once

#include <types/units.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace altair {

/// Bytes to separate two independently-written atomics so they cannot share a
/// cache line. 64 on every architecture Altair targets.
/// std::hardware_destructive_interference_size is not used: it is not portable
/// across the toolchains here and libstdc++ warns on its ABI instability.
inline constexpr std::size_t kCacheLine = 64;

// ─────────────────────────────────────────────────────────────────────────
// SpscRing — bounded lock-free queue for EXACTLY ONE producer thread and
// EXACTLY ONE consumer thread.
//
// Using it from two producers, or two consumers, is undefined behaviour and
// will silently corrupt the queue. That is not a limitation to work around,
// it is the contract that makes it this fast.
//
// Capacity must be a power of two so the index wrap is a single AND. The
// indices themselves are monotonically increasing 64-bit counters and are
// never wrapped, so full and empty are distinguishable without sacrificing a
// slot — all Capacity slots are usable.
// ─────────────────────────────────────────────────────────────────────────
template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscRing stores T by value with no destructor call");
    static_assert(std::is_trivially_destructible_v<T>,
                  "SpscRing never runs a destructor");
    static_assert(Capacity >= 2, "a ring of one cannot distinguish full from empty");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    using value_type = T;

    constexpr SpscRing() noexcept = default;

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&) = delete;
    SpscRing& operator=(SpscRing&&) = delete;

    /// Enqueue one element. PRODUCER THREAD ONLY.
    /// UNIT: none. Returns false iff the ring is full; never blocks, never
    /// allocates, never overwrites an unconsumed element.
    /// PRECONDITION: called from exactly one thread, for the life of the ring.
    [[nodiscard]] ALTAIR_HOT bool try_push(const T& v) noexcept;

    /// Dequeue one element into `out`. CONSUMER THREAD ONLY.
    /// UNIT: none. Returns false iff the ring is empty; `out` is untouched then.
    /// PRECONDITION: called from exactly one thread, for the life of the ring.
    [[nodiscard]] ALTAIR_HOT bool try_pop(T& out) noexcept;

    /// Elements currently queued. UNIT: elements.
    /// APPROXIMATE when called concurrently — it is a snapshot of two indices
    /// read at different instants, and is exact only when the other side is
    /// quiescent. Safe from either thread. Never use it to decide whether a
    /// subsequent push or pop will succeed; check that call's return value.
    [[nodiscard]] std::size_t size_approx() const noexcept;

    /// size_approx() == 0. Same approximation caveat.
    [[nodiscard]] bool empty_approx() const noexcept;

    /// size_approx() == capacity(). Same approximation caveat.
    [[nodiscard]] bool full_approx() const noexcept;

    /// Slots in the ring. UNIT: elements. All of them are usable.
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

    /// Total successful pushes since construction. UNIT: elements.
    /// Monotonic. Safe from either thread.
    [[nodiscard]] std::uint64_t pushed() const noexcept;

    /// Total successful pops since construction. UNIT: elements.
    /// Monotonic. Safe from either thread.
    [[nodiscard]] std::uint64_t popped() const noexcept;

private:
    // Producer's line: the write index, plus its cached view of the read index.
    alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};
    std::uint64_t cached_head_{0};

    // Consumer's line: the read index, plus its cached view of the write index.
    alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};
    std::uint64_t cached_tail_{0};

    // Storage on its own line again, so the consumer's index does not share
    // with slot 0.
    alignas(kCacheLine) T slots_[Capacity]{};
};

} // namespace altair
```

---

## 4. BEHAVIOURAL SPEC

The memory orderings below are **mandatory and exact**. They cannot be validated
by any test on this hardware (x86 is TSO; see the architect's note), so they are
specified rather than discovered. Do not "simplify" one to `relaxed` or
"harden" one to `seq_cst`.

1. `try_push`:
   a. load `tail_` with **`memory_order_relaxed`** — the producer owns it, so no
      synchronisation is needed to read its own index;
   b. if `tail - cached_head_ == Capacity`, refresh
      `cached_head_ = head_.load(std::memory_order_acquire)` and re-test; return
      `false` if still full;
   c. write `slots_[tail & (Capacity - 1)] = v`;
   d. `tail_.store(tail + 1, std::memory_order_release)`.
   The release in (d) is what publishes the slot write in (c). Reversing (c) and
   (d) exposes an unwritten slot to the consumer.
2. `try_pop` is the mirror image: relaxed load of `head_`, refresh
   `cached_tail_ = tail_.load(acquire)` when it looks empty, read the slot, then
   `head_.store(head + 1, release)`.
3. The **cached index is the point of the design**. Without it every push reads
   the consumer's cache line and every pop reads the producer's, which is the
   false sharing the padding exists to prevent. Refresh the cache only when the
   ring *appears* full (producer) or empty (consumer).
4. Full is `tail - head == Capacity`; empty is `tail == head`. Because the
   indices are monotonic 64-bit counters and never wrapped, these are
   unambiguous and **all `Capacity` slots are usable** — do not sacrifice a slot.
5. Unsigned wraparound of the 64-bit counters is well-defined and correct: the
   difference `tail - head` stays valid. At 10 M ticks/second a `uint64_t`
   counter takes ~58,000 years to wrap. Do not add a guard for it.
6. `try_push` never overwrites an element the consumer has not taken, and
   `try_pop` never returns a slot the producer has not published. A full ring
   rejects; it does not drop the oldest.
7. `try_pop` leaves `out` **untouched** when it returns false.
8. `pushed()` and `popped()` are the raw `tail_` and `head_` counters, loaded
   with `memory_order_relaxed`. They are monotonic and never reset.
9. `size_approx()` is `pushed() - popped()`, and is safe to call from either
   thread. Its doc comment must state that it is approximate under concurrency.
10. `pushed() - popped() == size_approx()` is a **conservation invariant** and
    must hold exactly at any quiescent moment. Nothing is created or destroyed
    in the ring; every element is pushed once and popped at most once.
11. The ring is neither copyable nor movable. A moved-from ring observed by a
    live producer or consumer is a use-after-move race with no diagnosis.
12. `sizeof(SpscRing<T, N>)` must place `tail_` and `head_` at least
    `kCacheLine` bytes apart. This is testable with `offsetof` and **must** be
    asserted, because the padding is a performance contract that a careless
    reorder would silently break.

---

## 5. CONSTRAINTS

- C++23. Standard library only. **No new dependency**; `vcpkg.json` stays empty.
- Header-only. `<atomic>`, `<cstddef>`, `<cstdint>`, `<new>`, `<type_traits>`
  only — the exact include list in the contract.
- **No mutex, no condition variable, no `std::thread` in the header.** Threads
  appear only in the test.
- No allocation anywhere. The storage is an inline array.
- **Do not use `std::hardware_destructive_interference_size`.** libstdc++ warns
  about its ABI instability and it is inconsistently available across the
  toolchains this project targets. `kCacheLine` is the contract.
- No exceptions, no `throw`, no `iostream`.
- No `using namespace` at file scope in the header.
- Compiles clean at `/W4` and `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion -Wold-style-cast`.
- Tests are a plain `int main()` and **must link `Threads::Threads`**.

---

## 6. ACCEPTANCE TESTS

`core/lockfree/tests/test_spsc_ring.cpp`, plain `main()`,
`check(bool, const char*)` helper counting failures, returns 0 only if all pass.
Use exactly these names:

```cpp
void test_spsc_layout_and_traits();
void test_spsc_single_thread_fifo();
void test_spsc_fill_and_drain();
void test_spsc_wraparound();
void test_spsc_conservation();
void test_spsc_pop_leaves_out_untouched();
void test_spsc_two_thread_stress();
```

**test_spsc_layout_and_traits**
```
// The padding is a performance contract. Assert it.
SpscRing<std::uint64_t, 8> r;
offsetof-style distance between tail_ and head_ >= kCacheLine
// Measure it without touching privates: the object must be at least large
// enough for two padded lines plus the storage.
sizeof(SpscRing<std::uint64_t, 8>) >= 2 * kCacheLine + 8 * sizeof(std::uint64_t)
alignof(SpscRing<std::uint64_t, 8>) >= kCacheLine

SpscRing<std::uint64_t, 8>::capacity() == 8
!std::is_copy_constructible_v<SpscRing<std::uint64_t, 8>>
!std::is_move_constructible_v<SpscRing<std::uint64_t, 8>>

// A fresh ring is empty and its counters are zero.
r.empty_approx() && !r.full_approx()
r.size_approx() == 0 && r.pushed() == 0 && r.popped() == 0
```

**test_spsc_single_thread_fifo**
```
SpscRing<int, 4> r;
r.try_push(1) && r.try_push(2) && r.try_push(3)
r.size_approx() == 3

int v = 0;
r.try_pop(v) && v == 1                  // FIFO, not LIFO
r.try_pop(v) && v == 2
r.try_pop(v) && v == 3
!r.try_pop(v)                            // empty
r.size_approx() == 0
```

**test_spsc_fill_and_drain**
```
SpscRing<int, 4> r;
// ALL FOUR slots are usable - a ring that sacrifices one fails here.
r.try_push(10) && r.try_push(20) && r.try_push(30) && r.try_push(40)
r.size_approx() == 4
r.full_approx()
!r.try_push(50)                          // full rejects
r.size_approx() == 4                     // and changes nothing
r.pushed() == 4

int v = 0;
r.try_pop(v) && v == 10                  // the OLDEST survived - no overwrite
r.try_push(50)                           // one slot freed, one push fits
r.size_approx() == 4
```

**test_spsc_wraparound**
```
SpscRing<int, 4> r;
// Push and pop 1000 times through a 4-slot ring: 250 full wraps.
int v = 0;
for i in 0..999:
    r.try_push(i)
    r.try_pop(v)
    v == i                               // every element survives the wrap
r.size_approx() == 0
r.pushed() == 1000 && r.popped() == 1000

// Interleave so the ring stays partly full across the wrap boundary.
SpscRing<int, 4> s;
s.try_push(1); s.try_push(2);
for i in 0..999:
    s.try_push(i)
    s.try_pop(v)
s.size_approx() == 2                     // the two stragglers are still there
```

**test_spsc_conservation**
```
// pushed() - popped() == size_approx(), at every quiescent point.
SpscRing<int, 8> r;
int v = 0;
bool ok = true;
for i in 0..999:
    if (i % 3 != 0) (void)r.try_push(i);
    if (i % 5 == 0) (void)r.try_pop(v);
    if (r.pushed() - r.popped() != r.size_approx()) ok = false;
ok
r.pushed() >= r.popped()                 // never popped more than pushed
r.size_approx() <= r.capacity()          // never over capacity
```

**test_spsc_pop_leaves_out_untouched**
```
SpscRing<int, 4> r;
int v = 0xBEEF;
!r.try_pop(v)
v == 0xBEEF                              // a failed pop must not scribble
```

**test_spsc_two_thread_stress**
The real test. One producer thread, one consumer thread, **1'000'000 elements**.
```
constexpr int kN = 1'000'000;
SpscRing<std::uint64_t, 1024> r;

producer: for i in 0..kN-1: spin until try_push(i) succeeds
consumer: for i in 0..kN-1: spin until try_pop(v) succeeds; require v == i

// The consumer verifies the FULL SEQUENCE, not just the count. A ring that
// duplicates or drops an element under contention fails on the value, and a
// count-only check would miss a matched drop-and-duplicate.
every popped value equalled its expected index
r.pushed() == kN && r.popped() == kN
r.size_approx() == 0
// Report elapsed time and throughput.
```
Give the spin loops a `std::this_thread::yield()` on the empty/full path so the
test does not livelock on a single-core CI box.

### Latency reporting — not a pass/fail assertion

Report single-threaded `try_push` + `try_pop` throughput, **batch-timed**: these
are ~2 ns operations, below the RDTSCP measurement floor, so time 1'000'000
pairs between one pair of `std::chrono::steady_clock` reads and divide (the
P0-05b technique, not the P0-03 one).

Also report the two-thread stress throughput in elements/second.

**Budget: single-threaded push+pop pair < 20 ns.** Print both; **do not assert
them.** Gate 6 is a human judgement against ROADMAP §11.

---

## 7. FORBIDDEN

- Adding a file not in the manifest. Modifying `core/CMakeLists.txt`, the root
  `CMakeLists.txt`, `vcpkg.json`, `core/types/`, `core/time/`, or `core/mem/`.
- Changing any signature in the interface contract.
- **Changing any memory ordering in spec item 1 or 2.** They are unverifiable on
  x86 and must be exactly as written.
- A mutex, a spinlock, a condition variable, or `std::thread` in the header.
- `std::hardware_destructive_interference_size`.
- Any allocation, any exception, any `iostream`.
- Sacrificing a slot to distinguish full from empty. The monotonic counters make
  that unnecessary; item 4.
- Overwriting an unconsumed element when full. This is a queue, not a ring
  buffer with drop-oldest semantics — a dropped tick is a silent data loss that
  no downstream check would catch.
- Making `size_approx()` claim to be exact, or using it to gate a push or pop.
- Adding a wraparound guard on the 64-bit counters; item 5.
- Making the ring copyable or movable.
- A stress test that checks only the element *count*. It must verify the
  sequence, or a matched drop-and-duplicate passes.
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
the measured single-threaded ns/pair, the two-thread throughput, and an explicit
acknowledgement that the memory orderings were **not** validated by any tool.
