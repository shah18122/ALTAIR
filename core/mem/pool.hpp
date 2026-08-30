#pragma once

// P0-05b — fixed-size blocks from an intrusive free list.
//
// The free-list link lives INSIDE each free block, in its first word, so the
// pool costs no memory beyond the blocks themselves. That is why block_size
// must be at least sizeof(void*) and align at least alignof(void*).
//
// Does not own its memory, does not run destructors, not thread-safe.

#include <mem/arena.hpp>
#include <types/units.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// Pool — fixed-size blocks from an intrusive free list.
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
           std::size_t block_size, std::size_t align) noexcept {
        if (!detail::is_pow2_size(align) || align < alignof(void*)) {
            return std::unexpected(MemError::BadAlignment);
        }
        if (block_size < sizeof(void*)) {
            return std::unexpected(MemError::BadBlockSize);
        }
        if (base == nullptr) {
            return std::unexpected(MemError::NoCapacity);
        }

        const std::size_t stride = detail::round_up_pow2(block_size, align);
        if (stride == 0) {
            return std::unexpected(MemError::BadBlockSize);
        }

        // Align the base up, and charge the skew against the span.
        const auto raw = reinterpret_cast<std::uintptr_t>(base);
        const auto aligned = static_cast<std::uintptr_t>(
            detail::round_up_pow2(static_cast<std::size_t>(raw), align));
        const std::size_t skew = static_cast<std::size_t>(aligned - raw);
        if (skew >= bytes) {
            return std::unexpected(MemError::NoCapacity);
        }

        const std::size_t usable = bytes - skew;
        const std::size_t n = usable / stride;
        if (n == 0) {
            return std::unexpected(MemError::NoCapacity);
        }

        Pool p;
        p.base_ = reinterpret_cast<std::byte*>(aligned);
        p.stride_ = stride;
        p.cap_ = n;

        // Thread the free list in ASCENDING address order, so a fresh pool
        // hands out block 0 first: deterministic for tests, cache-friendly in
        // use. Built back-to-front because each node points at its successor.
        void* head = nullptr;
        for (std::size_t i = n; i > 0; --i) {
            std::byte* blk = p.base_ + (i - 1) * stride;
            *reinterpret_cast<void**>(blk) = head;
            head = static_cast<void*>(blk);
        }
        p.free_ = head;
        return p;
    }

    /// Take a block. UNIT: none. O(1), one pointer load and one store.
    /// Returns Exhausted when every block is out. The returned memory is
    /// uninitialised and is NOT zeroed.
    [[nodiscard]] ALTAIR_HOT std::expected<void*, MemError> acquire() noexcept {
        if (free_ == nullptr) {
            return std::unexpected(MemError::Exhausted);
        }
        void* blk = free_;
        free_ = *reinterpret_cast<void**>(blk);
        ++in_use_;
        if (in_use_ > high_) {
            high_ = in_use_;
        }
        return blk;
    }

    /// Return a block. UNIT: none. O(1).
    /// A null pointer is ignored, as free() ignores one. A pointer this pool
    /// does not own is ignored AND counted in foreign_releases() — corrupting
    /// the free list silently would be far worse than a counter.
    ALTAIR_HOT void release(void* p) noexcept {
        if (p == nullptr) {
            return;   // a silent no-op, and NOT foreign
        }
        if (!owns(p)) {
            ++foreign_;
            return;
        }
        // Push onto the head: the just-released block is the next acquired, so
        // the hottest block stays hottest.
        *reinterpret_cast<void**>(p) = free_;
        free_ = p;
        --in_use_;
    }

    /// True iff `p` points at the start of a block of this pool.
    /// UNIT: none. PRECONDITION: none; safe for any pointer value.
    [[nodiscard]] ALTAIR_HOT bool owns(const void* p) const noexcept {
        if (p == nullptr || base_ == nullptr) {
            return false;
        }
        const auto addr = reinterpret_cast<std::uintptr_t>(p);
        const auto lo = reinterpret_cast<std::uintptr_t>(base_);
        const auto hi = lo + static_cast<std::uintptr_t>(cap_ * stride_);
        if (addr < lo || addr >= hi) {
            return false;
        }
        // A mid-block pointer is NOT owned. Accepting one would splice a
        // misaligned node into the free list, and the corruption would surface
        // thousands of allocations later.
        return (addr - lo) % stride_ == 0;
    }

    /// Distance between consecutive blocks — block_size rounded up to align.
    /// UNIT: bytes.
    [[nodiscard]] constexpr std::size_t stride() const noexcept { return stride_; }

    /// Total blocks carved. UNIT: blocks.
    [[nodiscard]] constexpr std::size_t capacity() const noexcept { return cap_; }

    /// Blocks currently handed out. UNIT: blocks.
    [[nodiscard]] constexpr std::size_t in_use() const noexcept { return in_use_; }

    /// Peak in_use() ever reached. UNIT: blocks. Sizing evidence.
    [[nodiscard]] constexpr std::size_t high_water() const noexcept { return high_; }

    /// Count of release() calls with a pointer this pool does not own.
    /// UNIT: count. Non-zero is a bug upstream, and P0-09 will trip on it.
    [[nodiscard]] constexpr std::uint64_t foreign_releases() const noexcept {
        return foreign_;
    }

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
