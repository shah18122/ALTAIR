#pragma once

// P0-05a — OS pages with honest hugepage reporting.
//
// Everything above this file — arenas, pools, lock-free rings, the tick store —
// needs memory obtained ONCE, at startup, and never again. CLAUDE.md rule 4
// forbids heap allocation inside ALTAIR_HOT, and that rule is only keepable if
// something acquires a large block up front. This is that something.
//
// Huge pages (2 MiB rather than 4 KiB) cut TLB misses on large working sets,
// and are frequently unavailable: Windows MEM_LARGE_PAGES needs
// SeLockMemoryPrivilege, Linux MAP_HUGETLB needs a pre-reserved pool. When they
// are refused we allocate ordinary pages, succeed, and SAY SO. A refused
// hugepage changes throughput, not meaning — unlike a missing invariant TSC in
// P0-03, which changes what every measurement means and is therefore an error.
//
// NOTHING HERE IS ALTAIR_HOT. Every function may syscall.

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
/// NOT hot-path safe: the first call queries the OS.
[[nodiscard]] std::size_t page_size() noexcept;

/// OS huge-page size, or 0 when the platform exposes none.
/// UNIT: bytes. PRECONDITION: none. Queried once and cached.
/// NOT hot-path safe: the first call queries the OS.
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
    /// NOT hot-path safe: this syscalls. Call it at startup.
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
    /// NOT hot-path safe: this syscalls.
    void reset() noexcept;

private:
    std::byte*  data_ = nullptr;
    std::size_t size_ = 0;
    PageBacking backing_ = PageBacking::Default;
};

} // namespace altair
