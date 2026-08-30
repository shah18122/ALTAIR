# P0-05b — `core/mem/arena` + `core/mem/pool`: O(1) allocation off the hot path

> Phase 0 · Card 5b of 14 · Status: DONE (implemented by Claude, 2026-08-29 — see LEDGER)
> Depends on: P0-01 (`core/types/units.hpp`) · P0-05a (`core/mem/page_alloc.hpp`) — DONE
> Paste everything below the line into DeepSeek V4 as a single prompt.
>
> **Architect's note (not part of the prompt).** Second half of the P0-05 split;
> P0-05a delivered the OS page layer. Both allocators here operate on a
> **caller-supplied byte span**, not on memory they own. That is deliberate: it
> keeps them testable with a stack buffer, keeps the platform code in exactly one
> file, and means replay and live construct them identically (CLAUDE.md rule 6).
>
> Gate 6 needs a different technique than P0-03. An arena bump is 1–2 ns, which
> is *below* the ~12 ns cost of the RDTSCP pair used to measure it — subtracting
> the harness is no longer enough, because the quantum is larger than the signal.
> Time a **batch** of N operations between one pair of clock reads and divide.
> The card specifies this; do not measure per-call.

---

## 1. CONTEXT

You are implementing the two allocators of Altair, a C++23 low-latency trading
engine for Indian equity markets.

CLAUDE.md rule 4: **no heap allocation inside `ALTAIR_HOT`.** `malloc` is not
merely slow — it is *unpredictably* slow, taking a lock and occasionally a
syscall, so its p99 is orders of magnitude worse than its p50. On a path that
runs per tick, the tail is the only number that matters.

Two shapes cover essentially everything above this layer:

- An **arena** bumps a pointer and frees nothing. Per-tick scratch: feature
  vectors, decode buffers, anything whose lifetime ends when the tick does.
  `reset()` at the top of each tick reclaims all of it in one store.
- A **pool** hands out fixed-size blocks from an intrusive free list. Objects
  with individual lifetimes: orders, book nodes, pending fills.

Both are O(1) with no branch on anything but a bounds check, and neither ever
touches the OS. P0-05a already went to the OS, once.

---

## 2. FILE MANIFEST

Create exactly these three files, and modify exactly one.

```
CREATE   core/mem/arena.hpp
CREATE   core/mem/pool.hpp
CREATE   core/mem/tests/test_arena_pool.cpp
MODIFY   core/mem/CMakeLists.txt
```

Both headers are header-only — no `.cpp`. In `core/mem/CMakeLists.txt`, add an
`altair_arena_pool_test` executable from `tests/test_arena_pool.cpp` with
`add_test(NAME arena_pool COMMAND altair_arena_pool_test)`. `altair_mem` is
already STATIC from P0-05a — do not change its type, and leave the
`altair_page_alloc_test` registration alone.

**Do not touch** `core/mem/page_alloc.hpp`, `core/mem/page_alloc.cpp`,
`core/CMakeLists.txt`, the root `CMakeLists.txt`, `vcpkg.json`, `core/types/`,
or `core/time/`.

---

## 3. INTERFACE CONTRACT

### `core/mem/arena.hpp`

```cpp
#pragma once

#include <types/units.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <type_traits>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// Why an allocation failed. Shared by Arena and Pool.
// ─────────────────────────────────────────────────────────────────────────
enum class MemError : std::uint8_t {
    Exhausted,      // not enough space remains
    BadAlignment,   // alignment is zero or not a power of two
    BadBlockSize,   // pool block smaller than a free-list link
    NoCapacity      // the span cannot hold even one block
};

// ─────────────────────────────────────────────────────────────────────────
// Arena — a bump allocator over a caller-supplied span.
//
// Does NOT own its memory and does NOT run destructors. Allocate only
// trivially destructible types; reset() abandons everything at once.
// Not thread-safe: one arena per thread, or per tick pipeline.
// ─────────────────────────────────────────────────────────────────────────
class Arena {
public:
    /// An arena over nothing. Every allocate() returns Exhausted.
    constexpr Arena() noexcept = default;

    /// Take a span. UNIT: bytes. The arena borrows; the caller keeps ownership
    /// and must outlive it. PRECONDITION: base is non-null when bytes > 0.
    constexpr Arena(std::byte* base, std::size_t bytes) noexcept;

    /// Bump-allocate `bytes` aligned to `align`.
    /// UNIT: bytes for both. PRECONDITION: align is a power of two.
    /// Returns Exhausted when the aligned request does not fit, BadAlignment
    /// when `align` is zero or not a power of two. Never returns nullptr in
    /// the success case. A zero-byte request succeeds and returns an aligned
    /// pointer — it consumes only the alignment padding.
    /// NOT constexpr: correct alignment requires inspecting the ADDRESS, and
    /// reinterpret_cast is not permitted in a constant expression. Aligning the
    /// offset instead would be constexpr and WRONG — a 64-byte-aligned span
    /// would hand back a "128-aligned" pointer that is nothing of the sort.
    [[nodiscard]] ALTAIR_HOT std::expected<void*, MemError>
    allocate(std::size_t bytes, std::size_t align) noexcept;

    /// Typed convenience. Allocates n * sizeof(T) at alignof(T).
    /// PRECONDITION: T is trivially destructible — the arena never destroys.
    /// Returns Exhausted if n * sizeof(T) would overflow std::size_t.
    /// NOT constexpr, for the same reason as allocate().
    template <typename T>
    [[nodiscard]] ALTAIR_HOT std::expected<T*, MemError>
    allocate_n(std::size_t n) noexcept;

    /// Abandon every allocation. O(1), one store. Does NOT run destructors and
    /// does NOT clear high_water().
    ALTAIR_HOT constexpr void reset() noexcept;

    /// Bytes handed out, including alignment padding. UNIT: bytes.
    [[nodiscard]] constexpr std::size_t used() const noexcept;

    /// Total span size. UNIT: bytes.
    [[nodiscard]] constexpr std::size_t capacity() const noexcept;

    /// capacity() - used(). UNIT: bytes.
    [[nodiscard]] constexpr std::size_t remaining() const noexcept;

    /// Peak used() ever reached, across resets. UNIT: bytes.
    /// This is the sizing evidence: an arena whose high water never approaches
    /// capacity is oversized, and one that touches it is a latent Exhausted.
    [[nodiscard]] constexpr std::size_t high_water() const noexcept;

private:
    std::byte*  base_ = nullptr;
    std::size_t cap_ = 0;
    std::size_t used_ = 0;
    std::size_t high_ = 0;
};

} // namespace altair
```

### `core/mem/pool.hpp`

```cpp
#pragma once

#include <mem/arena.hpp>
#include <types/units.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// Pool — fixed-size blocks from an intrusive free list.
//
// The free-list link lives INSIDE each free block, so the pool costs no
// memory beyond the blocks themselves. Does NOT own its memory and does NOT
// run destructors. Not thread-safe.
// ─────────────────────────────────────────────────────────────────────────
class Pool {
public:
    /// An empty pool. acquire() always returns Exhausted.
    constexpr Pool() noexcept = default;

    /// Carve `bytes` at `base` into blocks of at least `block_size`, each
    /// aligned to `align`. UNIT: bytes throughout.
    /// PRECONDITION: align is a power of two and >= alignof(void*);
    /// block_size >= sizeof(void*), because a free block stores the next link
    /// in its own first word.
    /// Returns BadAlignment, BadBlockSize, or NoCapacity as appropriate.
    [[nodiscard]] static std::expected<Pool, MemError>
    create(std::byte* base, std::size_t bytes,
           std::size_t block_size, std::size_t align) noexcept;

    /// Take a block. UNIT: none. O(1), one pointer load and one store.
    /// Returns Exhausted when every block is out. The returned memory is
    /// uninitialised and is NOT zeroed.
    [[nodiscard]] ALTAIR_HOT std::expected<void*, MemError> acquire() noexcept;

    /// Return a block. UNIT: none. O(1).
    /// A null pointer is ignored, as free() ignores one. A pointer this pool
    /// does not own is ignored AND counted in foreign_releases() — corrupting
    /// the free list silently would be far worse than a counter.
    ALTAIR_HOT void release(void* p) noexcept;

    /// True iff `p` points at the start of a block of this pool.
    /// UNIT: none. PRECONDITION: none; safe for any pointer value.
    [[nodiscard]] ALTAIR_HOT bool owns(const void* p) const noexcept;

    /// Distance between consecutive blocks — block_size rounded up to align.
    /// UNIT: bytes.
    [[nodiscard]] constexpr std::size_t stride() const noexcept;

    /// Total blocks carved. UNIT: blocks.
    [[nodiscard]] constexpr std::size_t capacity() const noexcept;

    /// Blocks currently handed out. UNIT: blocks.
    [[nodiscard]] constexpr std::size_t in_use() const noexcept;

    /// Peak in_use() ever reached. UNIT: blocks. Sizing evidence.
    [[nodiscard]] constexpr std::size_t high_water() const noexcept;

    /// Count of release() calls with a pointer this pool does not own.
    /// UNIT: count. Non-zero is a bug upstream, and P0-09 will trip on it.
    [[nodiscard]] constexpr std::uint64_t foreign_releases() const noexcept;

private:
    std::byte*  base_ = nullptr;
    void*       free_ = nullptr;
    std::size_t stride_ = 0;
    std::size_t cap_ = 0;
    std::size_t in_use_ = 0;
    std::size_t high_ = 0;
    std::uint64_t foreign_ = 0;
};

} // namespace altair
```

---

## 4. BEHAVIOURAL SPEC

1. `Arena::allocate` rejects `align == 0` or a non-power-of-two `align` with
   `BadAlignment`, **before** touching any pointer arithmetic.
2. `Arena::allocate` aligns the current bump pointer up, then checks the fit.
   The alignment padding counts toward `used()` — it is genuinely consumed.
3. `Arena::allocate` must not overflow. Compute the fit as
   `offset > cap_ || bytes > cap_ - offset`, never `offset + bytes > cap_`,
   which wraps for a large `bytes`.
4. A zero-byte `allocate` succeeds, returns a correctly aligned pointer, and
   consumes only padding. It never returns nullptr on success.
5. `Arena::allocate_n<T>` `static_assert`s that `T` is trivially destructible.
   It returns `Exhausted` when `n * sizeof(T)` would overflow `std::size_t` —
   check with `n > SIZE_MAX / sizeof(T)` before multiplying.
6. `Arena::reset()` sets `used_` to zero and leaves `high_` untouched. High
   water is a property of the workload, not of the current tick.
7. `Pool::create` validates in this order: `BadAlignment` (align zero, not a
   power of two, or below `alignof(void*)`), then `BadBlockSize`
   (`block_size < sizeof(void*)`), then `NoCapacity` (fewer than one block
   fits after aligning `base` up).
8. `stride()` is `block_size` rounded up to a multiple of `align`, so every
   block is aligned, not just the first.
9. `Pool::create` threads the free list through the blocks in **ascending
   address order**, so a fresh pool hands out block 0 first. That makes the
   tests deterministic and the access pattern cache-friendly.
10. `Pool::acquire` pops the head. `Pool::release` pushes onto the head, so a
    just-released block is the next one acquired — the hottest block stays
    hottest.
11. `Pool::owns(p)` is true iff `p` lies within `[base_, base_ + cap_*stride_)`
    **and** is exactly on a block boundary. A pointer into the middle of a block
    is not owned; accepting it would misalign the whole free list.
12. `Pool::release(nullptr)` is a silent no-op and does **not** count as
    foreign. `release` of a non-owned, non-null pointer increments
    `foreign_releases()` and changes nothing else.

---

## 5. CONSTRAINTS

- C++23. Standard library only. **No new dependency** — `vcpkg.json` stays empty.
- Both files are header-only and every function must be `constexpr` where the
  contract says so. `Pool::acquire`, `release`, and `owns` manipulate raw
  pointers stored in memory and are **not** `constexpr` — that is why the
  contract marks only the observers `constexpr` on `Pool`. Do not add
  `constexpr` where the contract omits it.
- No `<memory>`, no `std::pmr`, no `operator new`, no `malloc`, no
  `std::vector`. This layer exists so that nothing above it needs them.
- No exceptions, no `throw`, no `iostream`.
- Internal helpers go in `namespace altair::detail`, **never an anonymous
  namespace** — these are headers. Name anything you add distinctly so it cannot
  collide with `detail::mul_overflows` (P0-01) or `detail::add_overflows_i64`
  (P0-04).
- No `using namespace` at file scope in a header.
- Compiles clean at `/W4` and `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion -Wold-style-cast`.
- Casting between `std::byte*` and `void*`/`std::uintptr_t` is unavoidable here.
  Use `reinterpret_cast`, never a C cast — `-Wold-style-cast` will catch it.
- Tests are a plain `int main()`.

---

## 6. ACCEPTANCE TESTS

`core/mem/tests/test_arena_pool.cpp`, plain `main()`, `check(bool, const char*)`
helper counting failures, returns 0 only if all pass. Use exactly these names:

```cpp
void test_arena_basic_bump();
void test_arena_alignment();
void test_arena_exhaustion();
void test_arena_reset_and_high_water();
void test_pool_create_validation();
void test_pool_acquire_release_cycle();
void test_pool_ownership_and_foreign_release();
```

Back the tests with a real `PageBlock` from P0-05a where a large span is needed,
and with a stack `alignas(64) std::byte buf[N]` where a small one is clearer.
Using both proves the allocators do not care where the bytes came from.

**test_arena_basic_bump**
```
alignas(64) std::byte buf[1024];
Arena a{buf, sizeof(buf)};

a.capacity() == 1024 && a.used() == 0 && a.remaining() == 1024

auto p1 = a.allocate(16, 8);
p1.has_value() && p1.value() != nullptr
a.used() == 16 && a.remaining() == 1008

auto p2 = a.allocate(32, 8);
// Distinct, non-overlapping, and in ascending order.
static_cast<std::byte*>(p2.value()) == static_cast<std::byte*>(p1.value()) + 16
a.used() == 48

// A zero-byte request succeeds and does not return nullptr.
auto z = a.allocate(0, 8);
z.has_value() && z.value() != nullptr

// A default-constructed arena has no room at all.
Arena empty;
empty.capacity() == 0
empty.allocate(1, 1).error() == MemError::Exhausted
```

**test_arena_alignment**
```
alignas(64) std::byte buf[1024];
Arena a{buf, sizeof(buf)};

(void)a.allocate(1, 1);                       // deliberately misalign the cursor
auto p = a.allocate(8, 64);
reinterpret_cast<std::uintptr_t>(p.value()) % 64 == 0
a.used() >= 64                                 // the padding was charged

// Every power of two up to 256 is honoured.
for align in {1, 2, 4, 8, 16, 32, 64, 128, 256}:
    reinterpret_cast<std::uintptr_t>(a.allocate(1, align).value()) % align == 0

// Bad alignments are rejected, and reject BEFORE consuming anything.
const std::size_t before = a.used();
a.allocate(8, 0).error() == MemError::BadAlignment
a.allocate(8, 3).error() == MemError::BadAlignment
a.allocate(8, 24).error() == MemError::BadAlignment
a.used() == before

// allocate_n is aligned for its type.
Arena b{buf, sizeof(buf)};
auto arr = b.allocate_n<std::uint64_t>(4);
reinterpret_cast<std::uintptr_t>(arr.value()) % alignof(std::uint64_t) == 0
b.used() == 32
```

**test_arena_exhaustion**
```
alignas(64) std::byte buf[128];
Arena a{buf, sizeof(buf)};

a.allocate(128, 1).has_value()                 // exactly the capacity fits
a.remaining() == 0
a.allocate(1, 1).error() == MemError::Exhausted

Arena b{buf, sizeof(buf)};
b.allocate(129, 1).error() == MemError::Exhausted     // one past
b.used() == 0                                          // a failure consumes nothing

// The overflow guard: a huge request must report Exhausted, not wrap.
Arena c{buf, sizeof(buf)};
c.allocate(std::numeric_limits<std::size_t>::max(), 1).error() == MemError::Exhausted
c.allocate(std::numeric_limits<std::size_t>::max() - 8, 8).error() == MemError::Exhausted

// allocate_n overflow guard.
Arena d{buf, sizeof(buf)};
d.allocate_n<std::uint64_t>(std::numeric_limits<std::size_t>::max() / 4).error()
    == MemError::Exhausted
```

**test_arena_reset_and_high_water**
```
alignas(64) std::byte buf[1024];
Arena a{buf, sizeof(buf)};

(void)a.allocate(400, 8);
a.used() == 400 && a.high_water() == 400
a.reset();
a.used() == 0
a.high_water() == 400                          // reset does NOT clear high water
a.remaining() == 1024

(void)a.allocate(100, 8);
a.high_water() == 400                          // a smaller peak does not lower it
(void)a.allocate(500, 8);
// 604, NOT 600: used was 100, the 8-alignment bumps the second allocation's
// offset to 104, and that 4 bytes of padding is genuinely consumed (item 2).
// Asserting 600 here would be asserting that padding is free.
a.used() == 604
a.high_water() == 604                          // a larger peak raises it
```

**test_pool_create_validation**
```
alignas(64) std::byte buf[1024];

Pool::create(buf, sizeof(buf), 64, 0).error()  == MemError::BadAlignment
Pool::create(buf, sizeof(buf), 64, 3).error()  == MemError::BadAlignment
Pool::create(buf, sizeof(buf), 64, 1).error()  == MemError::BadAlignment  // < alignof(void*)
Pool::create(buf, sizeof(buf), 1, 8).error()   == MemError::BadBlockSize  // < sizeof(void*)
Pool::create(buf, 8, 64, 8).error()            == MemError::NoCapacity    // not one block

auto p = Pool::create(buf, sizeof(buf), 64, 64);
p.has_value()
p->stride() == 64
p->capacity() == 16
p->in_use() == 0 && p->high_water() == 0 && p->foreign_releases() == 0

// stride rounds block_size UP to align.
auto q = Pool::create(buf, sizeof(buf), 40, 16);
q->stride() == 48
q->capacity() == 1024 / 48                     // 21

Pool empty;
empty.capacity() == 0
empty.acquire().error() == MemError::Exhausted
```

**test_pool_acquire_release_cycle**
```
alignas(64) std::byte buf[1024];
auto p = Pool::create(buf, sizeof(buf), 64, 64);

// A fresh pool hands out ascending addresses.
void* a0 = p->acquire().value();
void* a1 = p->acquire().value();
static_cast<std::byte*>(a1) == static_cast<std::byte*>(a0) + 64
p->in_use() == 2 && p->high_water() == 2

// LIFO: the just-released block comes back next.
p->release(a1);
p->in_use() == 1
p->acquire().value() == a1
p->in_use() == 2

// Drain completely, then exhaust.
while (p->in_use() < p->capacity()) (void)p->acquire();
p->in_use() == 16
p->acquire().error() == MemError::Exhausted
p->high_water() == 16

// Every block is aligned and distinct - collect and verify.
// Release everything, then confirm the pool is fully reusable.
... release all 16 ...
p->in_use() == 0
p->high_water() == 16                          // high water survives
16 further acquires all succeed
```

**test_pool_ownership_and_foreign_release**
```
alignas(64) std::byte buf[1024];
alignas(64) std::byte other[64];
auto p = Pool::create(buf, sizeof(buf), 64, 64);

void* b = p->acquire().value();
p->owns(b) == true
p->owns(nullptr) == false
p->owns(other) == false                        // a different buffer entirely
p->owns(buf + 1024) == false                   // one past the end
p->owns(buf + 32) == false                     // mid-block, NOT a boundary
p->owns(buf + 64) == true                      // the next boundary

// A null release is a silent no-op, NOT a foreign release.
p->release(nullptr);
p->foreign_releases() == 0
p->in_use() == 1

// A foreign release is counted and changes nothing else.
p->release(other);
p->foreign_releases() == 1
p->in_use() == 1

// A mid-block pointer is foreign too - this is the one that would silently
// corrupt the free list if owns() only did a range check.
p->release(buf + 32);
p->foreign_releases() == 2
p->in_use() == 1

// The pool still works perfectly afterwards.
p->release(b);
p->in_use() == 0
p->acquire().has_value()
```

### Latency reporting — not a pass/fail assertion

After the tests, report throughput for `Arena::allocate(32, 8)` and for a
`Pool::acquire()` + `release()` pair.

**These operations are 1–3 ns, below the ~12 ns cost of the RDTSCP pair that
would measure them.** Subtracting the harness — the P0-03 technique — is not
enough when the measurement quantum exceeds the signal. Instead:

- time a **batch** of 1'000'000 operations between a single pair of
  `std::chrono::steady_clock` reads, and divide;
- keep a running sink the optimiser cannot discard, or the whole loop vanishes;
- `reset()` the arena every 1'000 allocations so the batch does not exhaust it,
  and exclude nothing — the resets are part of the measured cost and are named
  as such in the output.

**Budget: arena `allocate` < 10 ns/op, pool `acquire`+`release` < 15 ns/pair.**
Print both; **do not assert them.** Gate 6 is a human judgement against
ROADMAP §11.

---

## 7. FORBIDDEN

- Adding a file not in the manifest. Modifying `page_alloc.*`,
  `core/CMakeLists.txt`, the root `CMakeLists.txt`, `vcpkg.json`, `core/types/`,
  or `core/time/`.
- Changing any signature in the interface contract.
- `operator new`, `malloc`, `std::vector`, `std::pmr`, or `<memory>` anywhere.
  This layer is what makes those unnecessary above it.
- Any syscall, or any call into `page_alloc`, from `Arena` or `Pool`. They
  operate on a span someone else obtained.
- Running destructors, or promising zeroed memory.
- Clearing `high_water()` in `reset()`. It is sizing evidence across the whole
  session.
- Computing the fit as `offset + bytes > capacity`. That wraps. See item 3.
- An `owns()` that only range-checks without confirming the block boundary.
  A mid-block pointer accepted into the free list corrupts it silently, and the
  corruption surfaces thousands of allocations later.
- Aborting, asserting, or throwing on a foreign `release`. Count it.
- Measuring the allocators per-call with a clock pair. See the latency section.
- Asserting a hard latency number in the test. Print it.
- Writing a test that asserts whatever your implementation happens to produce.
  The expected values in §6 are the specification.

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

Return the four files in full, then your ASSUMPTIONS section, which must report
the measured ns/op for both allocators and state explicitly that they were timed
in batches rather than per call.
