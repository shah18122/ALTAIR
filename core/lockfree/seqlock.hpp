#pragma once

// P0-06b — seqlock snapshot: one writer publishes a value, many readers sample.
//
// Readers must never see a half-written struct, and must never block the
// writer. A mutex would let a slow reader stall the writer; a seqlock cannot.
//
// The trade is explicit: READERS MAY FAIL AND RETRY, WRITERS NEVER BLOCK. That
// is the correct trade for market data, where a reader that missed the last
// update wants the NEXT one, not a stale one it waited for.
//
// MEMORY ORDERING WARNING — see spsc_ring.hpp. The odd/even version protocol
// is exactly the kind of algorithm whose ordering bugs are invisible on x86.

#include <lockfree/spsc_ring.hpp>   // for kCacheLine
#include <types/units.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// SeqlockSnapshot — one writer, many readers.
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
    ALTAIR_HOT void store(const T& v) noexcept {
        const std::uint64_t s = seq_.load(std::memory_order_relaxed);

        // Odd: a write is in flight.
        seq_.store(s + 1, std::memory_order_release);
        // The fence stops the value write below from being hoisted above the
        // odd marker.
        std::atomic_thread_fence(std::memory_order_release);

        value_ = v;

        // And stops it from sinking past the even marker.
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);
    }

    /// Sample the value. SAFE FROM ANY NUMBER OF READER THREADS.
    /// UNIT: none. Returns false iff a write was in flight, in which case
    /// `out` holds an INDETERMINATE value and must not be used. It is
    /// deliberately not left unchanged: promising that would cost a copy on
    /// every failed read, on the path where failure is cheapest.
    /// PRECONDITION: none.
    [[nodiscard]] ALTAIR_HOT bool try_load(T& out) const noexcept {
        const std::uint64_t s0 = seq_.load(std::memory_order_acquire);
        if ((s0 & 1u) != 0u) {
            return false;              // a write is in flight
        }

        out = value_;

        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint64_t s1 = seq_.load(std::memory_order_acquire);

        // A differing s1 means the value was rewritten under us, so the copy
        // may be torn.
        return s0 == s1;
    }

    /// Sample the value, retrying until it succeeds. SAFE FROM ANY READER.
    /// UNIT: none. Spins; on a single-core box a writer in a tight loop could
    /// starve this, so prefer try_load() on a latency-critical path.
    /// There is deliberately NO retry cap — a capped retry would have to return
    /// a possibly-torn value, which is worse than spinning.
    /// PRECONDITION: none.
    [[nodiscard]] ALTAIR_HOT T load() const noexcept {
        T out{};
        while (!try_load(out)) {
            // spin
        }
        return out;
    }

    /// The current version counter. UNIT: writes x 2.
    /// EVEN when stable, ODD while a write is in flight. Zero before the first
    /// store(). Readers can use a change in this value to detect an update.
    [[nodiscard]] std::uint64_t version() const noexcept {
        return seq_.load(std::memory_order_acquire);
    }

private:
    alignas(kCacheLine) std::atomic<std::uint64_t> seq_{0};
    alignas(kCacheLine) T value_{};
};

} // namespace altair
