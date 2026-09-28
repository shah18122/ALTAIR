#pragma once

// P0-05b — a bump allocator over a caller-supplied span.
//
// CLAUDE.md rule 4: no heap allocation inside ALTAIR_HOT. malloc is not merely
// slow, it is UNPREDICTABLY slow — it takes a lock and occasionally a syscall,
// so its p99 is orders of magnitude worse than its p50. On a path that runs per
// tick, the tail is the only number that matters.
//
// The arena does not own its memory and does not run destructors. It borrows a
// span (from a PageBlock, or a stack buffer in tests) and bumps a pointer.
// reset() reclaims everything in one store. Not thread-safe: one per thread.

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

namespace detail {

/// True iff `a` is a power of two and non-zero.
/// UNIT: none. PRECONDITION: none.
[[nodiscard]] constexpr bool is_pow2_size(std::size_t a) noexcept {
    return a != 0 && (a & (a - 1)) == 0;
}

/// Round `n` up to a multiple of `to`, saturating to 0 on overflow.
/// UNIT: bytes. PRECONDITION: to is a power of two and non-zero.
[[nodiscard]] constexpr std::size_t round_up_pow2(std::size_t n, std::size_t to) noexcept {
    const std::size_t rem = n & (to - 1);
    if (rem == 0) {
        return n;
    }
    const std::size_t add = to - rem;
    return n > (static_cast<std::size_t>(-1) - add) ? 0 : n + add;
}

} // namespace detail

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
    constexpr Arena(std::byte* base, std::size_t bytes) noexcept
        : base_(base), cap_(bytes) {}

    /// CX02-D2 (C01-004). MOVE-ONLY, for the same reason as Pool: two copies
    /// bump the same span independently, so both hand out the same bytes and
    /// each believes the other's allocations are free space. P0-05b's contract
    /// never forbade a copy. A moved-from arena holds nothing.
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    constexpr Arena(Arena&& o) noexcept
        : base_(o.base_), cap_(o.cap_), used_(o.used_), high_(o.high_) {
        o.disown();
    }
    constexpr Arena& operator=(Arena&& o) noexcept {
        if (this != &o) {
            base_ = o.base_;
            cap_ = o.cap_;
            used_ = o.used_;
            high_ = o.high_;
            o.disown();
        }
        return *this;
    }

    /// Bump-allocate `bytes` aligned to `align`.
    /// UNIT: bytes for both. PRECONDITION: align is a power of two.
    /// Returns Exhausted when the aligned request does not fit, BadAlignment
    /// when `align` is zero or not a power of two. Never returns nullptr in
    /// the success case. A zero-byte request succeeds and returns an aligned
    /// pointer — it consumes only the alignment padding.
    [[nodiscard]] ALTAIR_HOT std::expected<void*, MemError>
    allocate(std::size_t bytes, std::size_t align) noexcept {
        if (!detail::is_pow2_size(align)) {
            return std::unexpected(MemError::BadAlignment);
        }
        if (base_ == nullptr) {
            return std::unexpected(MemError::Exhausted);
        }

        // Align the ADDRESS, not the offset. Aligning the offset only works if
        // the span's base is itself at least as aligned as the request — a
        // 64-byte-aligned buffer would hand back a "128-aligned" pointer that
        // is nothing of the sort. This is why allocate() is not constexpr:
        // reinterpret_cast is not permitted in a constant expression, and no
        // constexpr path can inspect a pointer's alignment.
        const auto base_addr = reinterpret_cast<std::uintptr_t>(base_);
        const auto cur = base_addr + used_;
        const auto mask = static_cast<std::uintptr_t>(align - 1);
        if (cur > static_cast<std::uintptr_t>(-1) - mask) {
            return std::unexpected(MemError::Exhausted);   // rounding would wrap
        }
        const auto aligned = (cur + mask) & ~mask;
        const std::size_t offset = static_cast<std::size_t>(aligned - base_addr);

        // NEVER `offset + bytes > cap_` — that wraps for a large `bytes`.
        if (offset > cap_ || bytes > cap_ - offset) {
            return std::unexpected(MemError::Exhausted);
        }

        used_ = offset + bytes;
        if (used_ > high_) {
            high_ = used_;
        }
        return static_cast<void*>(base_ + offset);
    }

    /// Typed convenience. Allocates n * sizeof(T) at alignof(T).
    /// PRECONDITION: T is trivially destructible — the arena never destroys.
    /// Returns Exhausted if n * sizeof(T) would overflow std::size_t.
    template <typename T>
    [[nodiscard]] ALTAIR_HOT std::expected<T*, MemError>
    allocate_n(std::size_t n) noexcept {
        static_assert(std::is_trivially_destructible_v<T>,
                      "Arena never runs destructors; T must be trivially destructible");
        if (n > static_cast<std::size_t>(-1) / sizeof(T)) {
            return std::unexpected(MemError::Exhausted);
        }
        auto r = allocate(n * sizeof(T), alignof(T));
        if (!r.has_value()) {
            return std::unexpected(r.error());
        }
        return static_cast<T*>(*r);
    }

    /// Abandon every allocation. O(1), one store. Does NOT run destructors and
    /// does NOT clear high_water().
    ALTAIR_HOT constexpr void reset() noexcept { used_ = 0; }

    /// Bytes handed out, including alignment padding. UNIT: bytes.
    [[nodiscard]] constexpr std::size_t used() const noexcept { return used_; }

    /// Total span size. UNIT: bytes.
    [[nodiscard]] constexpr std::size_t capacity() const noexcept { return cap_; }

    /// capacity() - used(). UNIT: bytes.
    [[nodiscard]] constexpr std::size_t remaining() const noexcept { return cap_ - used_; }

    /// Peak used() ever reached, across resets. UNIT: bytes.
    /// This is the sizing evidence: an arena whose high water never approaches
    /// capacity is oversized, and one that touches it is a latent Exhausted.
    [[nodiscard]] constexpr std::size_t high_water() const noexcept { return high_; }

private:
    constexpr void disown() noexcept {
        base_ = nullptr;
        cap_ = 0;
        used_ = 0;
        high_ = 0;
    }

    std::byte*  base_ = nullptr;
    std::size_t cap_ = 0;
    std::size_t used_ = 0;
    std::size_t high_ = 0;
};

} // namespace altair
