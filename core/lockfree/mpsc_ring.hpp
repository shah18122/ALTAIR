#pragma once

// P0-06b — bounded lock-free MPSC ring.
//
// Several strategy threads emit orders into one OMS thread; several decoders
// feed one aggregator. Many producers, one consumer. That needs a
// compare-exchange on the producer side, which the SPSC ring does not have.
//
// Each slot carries its own sequence number (the Vyukov bounded-queue scheme),
// so a slot is self-describing: a producer that wins the CAS but is descheduled
// before writing cannot be mistaken for a completed push.
//
// MEMORY ORDERING WARNING — see spsc_ring.hpp. x86 is TSO and no TSAN runs
// here, so a wrong ordering below is invisible on this hardware. The orderings
// are specified by the card, not discovered by testing.

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
// Using it from two consumers is undefined behaviour.
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
    MpscRing() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) {
            slots_[i].seq.store(static_cast<std::uint64_t>(i),
                                std::memory_order_relaxed);
        }
    }

    MpscRing(const MpscRing&) = delete;
    MpscRing& operator=(const MpscRing&) = delete;
    MpscRing(MpscRing&&) = delete;
    MpscRing& operator=(MpscRing&&) = delete;

    /// Enqueue one element. SAFE FROM ANY NUMBER OF PRODUCER THREADS.
    /// UNIT: none. Returns false iff the ring is full. Never blocks, never
    /// allocates. Lock-free but not wait-free: contention retries the CAS.
    /// PRECONDITION: none.
    [[nodiscard]] ALTAIR_HOT bool try_push(const T& v) noexcept {
        Slot* slot = nullptr;
        std::uint64_t pos = tail_.load(std::memory_order_relaxed);

        for (;;) {
            slot = &slots_[pos & kMask];
            const std::uint64_t seq = slot->seq.load(std::memory_order_acquire);
            // SIGNED difference. Comparing the unsigned values directly
            // mis-orders across the 64-bit wrap.
            const std::int64_t dif =
                static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos);

            if (dif == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_relaxed)) {
                    break;                 // slot claimed
                }
            } else if (dif < 0) {
                return false;              // full
            } else {
                pos = tail_.load(std::memory_order_relaxed);   // lost the race
            }
        }

        slot->data = v;
        // Release publishes the data write above.
        slot->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    /// Dequeue one element into `out`. CONSUMER THREAD ONLY.
    /// UNIT: none. Returns false iff the ring is empty; `out` is untouched then.
    /// PRECONDITION: called from exactly one thread, for the life of the ring.
    [[nodiscard]] ALTAIR_HOT bool try_pop(T& out) noexcept {
        const std::uint64_t pos = head_.load(std::memory_order_relaxed);
        Slot* slot = &slots_[pos & kMask];
        const std::uint64_t seq = slot->seq.load(std::memory_order_acquire);
        const std::int64_t dif =
            static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos + 1);

        if (dif != 0) {
            return false;                  // empty (dif > 0 cannot happen here)
        }

        head_.store(pos + 1, std::memory_order_relaxed);
        out = slot->data;
        // Re-arm the slot for the next lap around the ring.
        slot->seq.store(pos + Capacity, std::memory_order_release);
        return true;
    }

    /// Slots in the ring. UNIT: elements.
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

    /// Total successful pushes. UNIT: elements. APPROXIMATE under concurrency:
    /// a producer may have claimed a slot without having published it yet.
    [[nodiscard]] std::uint64_t pushed_approx() const noexcept {
        return tail_.load(std::memory_order_relaxed);
    }

    /// Total successful pops. UNIT: elements. Exact — there is one consumer.
    [[nodiscard]] std::uint64_t popped() const noexcept {
        return head_.load(std::memory_order_relaxed);
    }

private:
    static constexpr std::uint64_t kMask = Capacity - 1;

    struct Slot {
        std::atomic<std::uint64_t> seq;
        T data;
    };

    alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};
    alignas(kCacheLine) Slot slots_[Capacity];
};

} // namespace altair
