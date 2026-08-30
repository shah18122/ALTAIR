# P0-05a — `core/mem/page_alloc`: OS pages with honest hugepage reporting

> Phase 0 · Card 5a of 14 · Status: DONE (implemented by Claude, 2026-08-29 — see LEDGER)
> Depends on: P0-01 (`core/types/units.hpp`) — DONE
> Paste everything below the line into DeepSeek V4 as a single prompt.
>
> **Architect's note (not part of the prompt).** P0-05 as scoped in the LEDGER —
> "arena + pool allocators, hugepage backing" — needs six files: `page_alloc.hpp`,
> `page_alloc.cpp`, `arena.hpp`, `pool.hpp`, a `CMakeLists.txt`, and tests.
> PROTOCOL §8 says split at four. So it is split: **P0-05a** is the platform page
> layer, **P0-05b** is the two allocators built on top of it. Suffixed rather than
> renumbered, so P0-06…P0-09 keep their identity (precedent: `P0-01c1`).
>
> One decision worth stating: a refused hugepage is **not an error**. That looks
> inconsistent with P0-03, where a missing invariant TSC *is* one. The difference
> is what the fallback changes. A `steady_clock` substituted for a TSC changes the
> *semantics* of every measurement — the numbers mean something else. Ordinary
> 4 KiB pages are *semantically identical* to huge pages; only throughput differs.
> Degrade silently on performance, fail loud on meaning. Both report what they
> actually got, and `backing()` is the only honest answer to "did we get them?"

---

## 1. CONTEXT

You are implementing the page layer of Altair, a C++23 low-latency trading
engine for Indian equity markets.

Everything above this file — arenas, pools, lock-free rings, the tick store —
needs memory that is obtained **once, at startup**, and never again. CLAUDE.md
rule 4: no heap allocation inside `ALTAIR_HOT`. That rule is only keepable if
something acquires a large block up front, and this is that something.

Huge pages (2 MiB instead of 4 KiB) cut TLB misses on large working sets. They
are also **frequently unavailable**: on Windows `MEM_LARGE_PAGES` requires the
`SeLockMemoryPrivilege` right, which a normal user account does not have; on
Linux `MAP_HUGETLB` needs pre-reserved pages in the pool. The correct behaviour
when they are refused is to allocate ordinary pages, succeed, and **say so** —
never to pretend, and never to fail.

This card is the platform boundary. P0-05b builds the arena and the pool on top
of the bytes it returns.

---

## 2. FILE MANIFEST

Create exactly these four files. Nothing else.

```
core/mem/page_alloc.hpp
core/mem/page_alloc.cpp
core/mem/CMakeLists.txt
core/mem/tests/test_page_alloc.cpp
```

`core/CMakeLists.txt` discovers `mem/` automatically via its `EXISTS` guard —
**do not modify it.** `core/mem/CMakeLists.txt` declares `altair_mem` as a
**STATIC** library (it has a `.cpp`), aliases it `altair::mem`, exports
`${CMAKE_CURRENT_SOURCE_DIR}/..` as the include root so consumers write
`#include <mem/page_alloc.hpp>`, links `altair_types` and `altair_flags` PUBLIC,
and under `if(ALTAIR_BUILD_TESTS)` registers `altair_page_alloc_test` from
`tests/test_page_alloc.cpp` with
`add_test(NAME page_alloc COMMAND altair_page_alloc_test)`. Mirror
`core/time/CMakeLists.txt` in structure.

**Do not touch** `core/CMakeLists.txt`, the root `CMakeLists.txt`, `vcpkg.json`,
`core/types/`, or `core/time/`.

**Add no dependency.** `vcpkg.json` stays empty — this card needs only the
standard library and OS headers.

---

## 3. INTERFACE CONTRACT

Implement **exactly** these declarations in `core/mem/page_alloc.hpp`, in this
order, inside `namespace altair`. Do not add, remove, rename, or re-order any
public entity.

```cpp
#pragma once

#include <types/units.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// Why a page reservation failed.
// ─────────────────────────────────────────────────────────────────────────
enum class PageError : std::uint8_t {
    ZeroSize,       // a zero-byte reservation is a caller bug, not an allocation
    OutOfMemory,    // the OS refused the mapping
    Unsupported     // no page API on this target
};

// ─────────────────────────────────────────────────────────────────────────
// What the OS actually gave us. Never what was asked for — what arrived.
// ─────────────────────────────────────────────────────────────────────────
enum class PageBacking : std::uint8_t {
    Default,   // ordinary pages (typically 4 KiB)
    Huge       // large pages (typically 2 MiB)
};

/// OS page size. UNIT: bytes. Always a power of two, always >= 1.
/// PRECONDITION: none. Queried once and cached.
[[nodiscard]] std::size_t page_size() noexcept;

/// OS huge-page size, or 0 when the platform exposes none.
/// UNIT: bytes. PRECONDITION: none. Queried once and cached.
[[nodiscard]] std::size_t huge_page_size() noexcept;

// ─────────────────────────────────────────────────────────────────────────
// PageBlock — one OS mapping, owned. Move-only: two owners would double-free.
//
// Obtained once at startup. Nothing in ALTAIR_HOT ever constructs one.
// ─────────────────────────────────────────────────────────────────────────
class PageBlock {
public:
    /// An empty block. Owns nothing; data() is nullptr and size() is 0.
    PageBlock() noexcept = default;

    ~PageBlock() noexcept;

    PageBlock(PageBlock&& other) noexcept;
    PageBlock& operator=(PageBlock&& other) noexcept;

    PageBlock(const PageBlock&) = delete;
    PageBlock& operator=(const PageBlock&) = delete;

    /// Reserve and commit at least `bytes`, rounded up to a page boundary.
    /// UNIT: bytes. `want_huge` is a REQUEST, not a requirement: if the OS
    /// refuses huge pages this still succeeds with PageBacking::Default, and
    /// backing() reports the truth. Check backing(), never assume.
    /// PRECONDITION: bytes > 0, else PageError::ZeroSize.
    [[nodiscard]] static std::expected<PageBlock, PageError>
    reserve(std::size_t bytes, bool want_huge) noexcept;

    /// First byte of the mapping, or nullptr when empty.
    /// Aligned to at least page_size(). UNIT: none.
    [[nodiscard]] std::byte* data() const noexcept;

    /// Usable bytes — the ROUNDED-UP size, never the requested size.
    /// UNIT: bytes. PRECONDITION: none.
    [[nodiscard]] std::size_t size() const noexcept;

    /// What the OS actually provided. UNIT: none. PRECONDITION: none.
    [[nodiscard]] PageBacking backing() const noexcept;

    /// True iff this block owns no mapping.
    [[nodiscard]] bool empty() const noexcept;

    /// Release the mapping early. Idempotent; leaves the block empty.
    void reset() noexcept;

private:
    std::byte*  data_ = nullptr;
    std::size_t size_ = 0;
    PageBacking backing_ = PageBacking::Default;
};

} // namespace altair
```

---

## 4. BEHAVIOURAL SPEC

1. `page_size()` returns the OS page size — `GetSystemInfo().dwPageSize` on
   Windows, `sysconf(_SC_PAGESIZE)` on POSIX. Cached after the first call.
   Never returns 0; if the query fails, return 4096.
2. `huge_page_size()` returns `GetLargePageMinimum()` on Windows and 2 MiB on
   Linux (the near-universal default). Returns 0 where unavailable. Cached.
3. `reserve(0, _)` returns `PageError::ZeroSize`. A zero-byte block is a caller
   bug — do not paper over it by returning an empty block.
4. `reserve` rounds the request **up** to a multiple of the page size actually
   used, and `size()` reports that rounded value. A caller asking for 1 byte
   gets `page_size()` bytes and must be told so.
5. When `want_huge` is true, attempt the huge mapping first, rounding up to a
   multiple of `huge_page_size()`. **If it fails for any reason, fall back to a
   default-page mapping and succeed**, with `backing() == PageBacking::Default`.
   Only report `OutOfMemory` when the *fallback* also fails.
6. `want_huge == false` never attempts huge pages, and always reports
   `PageBacking::Default`.
7. The returned memory is readable and writable. It is **not** required to be
   zeroed — do not promise what the platform does not guarantee, and do not
   memset it, which would fault in every page and defeat lazy commit.
8. The destructor releases the mapping. `reset()` does the same early and is
   idempotent: calling it twice, or on an empty block, is well-defined and does
   nothing the second time.
9. Move construction transfers ownership and leaves the source **empty**:
   `data() == nullptr`, `size() == 0`, `empty() == true`. Move assignment
   releases any mapping the target already held, first.
10. Self-move-assignment (`b = std::move(b)`) must not release the mapping. Guard
    it. This is the classic double-free in move-only RAII types.
11. `PageBlock` is move-only. The copy constructor and copy assignment are
    deleted, and `std::is_copy_constructible_v<PageBlock>` must be false.
12. No function in this card is `ALTAIR_HOT`. Every one of them may make a
    syscall; none may ever be called from a hot path. Say so in the doc comments.

---

## 5. CONSTRAINTS

- C++23. Standard library plus OS headers only. **No new dependency** —
  `vcpkg.json` stays empty.
- Platform code lives in `page_alloc.cpp` behind
  `#if defined(_WIN32)` / `#elif defined(__linux__)` / `#else`, with an `#else`
  that compiles and returns `PageError::Unsupported`. **The header must contain
  no platform `#if` and must not include `<windows.h>` or `<sys/mman.h>`** —
  `windows.h` in a widely-included header is how a codebase acquires `min`/`max`
  macro damage.
- Internal helpers go in an anonymous namespace **inside the `.cpp`** (correct
  there — one translation unit, no ODR hazard) or in `namespace altair::detail`.
  Never an anonymous namespace in the header.
- No exceptions, no `throw`, no `iostream`. Errors are `std::expected`.
- No `using namespace` at file scope in the header.
- Compiles clean at `/W4` and `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion -Wold-style-cast`.
- On Windows, define `WIN32_LEAN_AND_MEAN` and `NOMINMAX` **before** including
  `<windows.h>` in the `.cpp`.
- Tests are a plain `int main()`. Catch2 is still unavailable — `vcpkg.json`'s
  `testing` feature is declared but not enabled, and the comment claiming it
  arrives at P0-03 is stale.

---

## 6. ACCEPTANCE TESTS

`core/mem/tests/test_page_alloc.cpp`, plain `main()`, `check(bool, const char*)`
helper counting failures, returns 0 only if all pass. Use exactly these names:

```cpp
void test_page_size_sane();
void test_page_block_reserve_and_size();
void test_page_block_writable();
void test_page_block_alignment();
void test_page_block_move_semantics();
void test_page_block_huge_request_reports_truth();
void test_page_block_error_paths();
void test_page_block_raii_release();
```

**This card touches the OS, so bounds are properties, not magic numbers.** Assert
invariants that hold on every machine — never "the page size is 4096".

**test_page_size_sane**
```
page_size() >= 1
(page_size() & (page_size() - 1)) == 0        // a power of two
page_size() >= 4096                            // no supported target is smaller
huge_page_size() == 0 || huge_page_size() > page_size()
huge_page_size() == 0 || (huge_page_size() & (huge_page_size() - 1)) == 0
// Print both, so the reviewer sees what this box reported.
```

**test_page_block_reserve_and_size**
```
auto b = PageBlock::reserve(1, false);
b.has_value()
b->size() >= page_size()                       // rounded UP, not 1
b->size() % page_size() == 0
!b->empty()
b->data() != nullptr

auto big = PageBlock::reserve(page_size() * 4 + 1, false);
big->size() >= page_size() * 5                 // rounded up past the +1
big->size() % page_size() == 0
```

**test_page_block_writable**
```
auto b = PageBlock::reserve(page_size() * 2, false);
// Touch the first, middle, and LAST byte - the last one catches an off-by-one
// in the rounding that a first-byte-only test would miss entirely.
b->data()[0] = std::byte{0xAB};
b->data()[b->size() / 2] = std::byte{0xCD};
b->data()[b->size() - 1] = std::byte{0xEF};
b->data()[0] == std::byte{0xAB}
b->data()[b->size() / 2] == std::byte{0xCD}
b->data()[b->size() - 1] == std::byte{0xEF}
```

**test_page_block_alignment**
```
auto b = PageBlock::reserve(page_size(), false);
reinterpret_cast<std::uintptr_t>(b->data()) % page_size() == 0
// and comfortably over-aligned for anything the allocators will want:
reinterpret_cast<std::uintptr_t>(b->data()) % 64 == 0     // cache line
```

**test_page_block_move_semantics**
```
!std::is_copy_constructible_v<PageBlock>
!std::is_copy_assignable_v<PageBlock>
std::is_move_constructible_v<PageBlock>
std::is_move_assignable_v<PageBlock>

auto a = PageBlock::reserve(page_size(), false);
std::byte* p = a->data();
PageBlock moved{std::move(*a)};
moved.data() == p                 // the mapping followed
a->empty() && a->data() == nullptr && a->size() == 0    // source emptied

// Move assignment releases what the target held first.
auto x = PageBlock::reserve(page_size(), false);
auto y = PageBlock::reserve(page_size(), false);
*x = std::move(*y);
x->data() != nullptr && y->empty()

// SELF-move must not release. The classic move-only double-free.
PageBlock& ref = *x;
ref = std::move(ref);
x->data() != nullptr && !x->empty()
x->data()[0] = std::byte{0x11};   // still mapped and writable
```

**test_page_block_huge_request_reports_truth**
```
// want_huge is a REQUEST. On most dev boxes it is refused (Windows needs
// SeLockMemoryPrivilege; Linux needs a pre-reserved pool). Either outcome is a
// PASS - what must hold is that backing() tells the truth and the block works.
auto h = PageBlock::reserve(4u * 1024u * 1024u, true);
h.has_value()                                   // NEVER fails just for huge
h->size() >= 4u * 1024u * 1024u
h->data() != nullptr
h->data()[h->size() - 1] = std::byte{0x5A};     // usable either way

if (h->backing() == PageBacking::Huge):
    huge_page_size() > 0
    h->size() % huge_page_size() == 0
    reinterpret_cast<std::uintptr_t>(h->data()) % huge_page_size() == 0
else:
    h->size() % page_size() == 0
// Print which one happened.

// want_huge == false must never report Huge.
auto d = PageBlock::reserve(4u * 1024u * 1024u, false);
d->backing() == PageBacking::Default
```

**test_page_block_error_paths**
```
PageBlock::reserve(0, false).error() == PageError::ZeroSize
PageBlock::reserve(0, true).error()  == PageError::ZeroSize

// A default-constructed block is empty and safe to touch.
PageBlock e;
e.empty() && e.data() == nullptr && e.size() == 0
e.reset();                     // idempotent on an empty block
e.empty()
```

**test_page_block_raii_release**
```
// Reserve and drop in a loop. If the destructor leaks, this exhausts the
// address space long before it finishes; if it double-frees, it crashes.
// 200 x 4 MiB = 800 MiB of churn through a scope that must stay flat.
for 200 iterations:
    auto b = PageBlock::reserve(4u * 1024u * 1024u, false);
    b.has_value()
    b->data()[0] = std::byte{1};
    // b destructs here
// Reaching this line at all is the assertion.

// Explicit reset is equally clean and idempotent.
auto b2 = PageBlock::reserve(page_size(), false);
b2->reset();
b2->empty();
b2->reset();
b2->empty();
```

---

## 7. FORBIDDEN

- Adding a file not in the manifest. Modifying `core/CMakeLists.txt`, the root
  `CMakeLists.txt`, `vcpkg.json`, `core/types/`, or `core/time/`.
- Changing any signature in the interface contract.
- **Adding any dependency.** `vcpkg.json` stays empty.
- `#include <windows.h>` or `<sys/mman.h>` in the header, or any platform `#if`
  there.
- Including `<windows.h>` without `WIN32_LEAN_AND_MEAN` and `NOMINMAX` first.
- **Returning an error because huge pages were refused.** Fall back, succeed,
  and report `PageBacking::Default`. See spec item 5.
- Reporting `PageBacking::Huge` when the mapping is not actually huge-backed.
  An optimistic `backing()` is worse than no `backing()` at all.
- `memset`-ing the block. It faults in every page and defeats lazy commit.
- Marking anything `ALTAIR_HOT`. Everything here can syscall.
- Returning the *requested* size from `size()` rather than the rounded size.
- Any raw `new`, `malloc`, or `std::vector` as the backing store. The whole point
  is to go to the OS directly.
- Writing a test that asserts whatever your implementation happens to produce.
  The properties in §6 are the specification.

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
the OS you built on, the reported `page_size()` and `huge_page_size()`, and
whether the 4 MiB huge request in
`test_page_block_huge_request_reports_truth` was granted or refused.
