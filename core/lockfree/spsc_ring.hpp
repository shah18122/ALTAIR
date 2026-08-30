#pragma once

// P0-06a — bounded lock-free SPSC ring: the feed -> strategy hot path.
//
// This is the queue between the feed decoder thread and the strategy thread,
// crossed by every tick. A mutex here would cost a syscall on contention and
// put a scheduler decision on the critical path.
//
// Two threads, one producing and one consuming, need no mutual exclusion: the
// producer owns the write index, the consumer owns the read index, and a single
// release/acquire pair publishes each element. What they must NOT do is share a
// cache line — if head and tail land on the same 64 bytes, every push
// invalidates the consumer's copy and every pop invalidates the producer's.
// That is false sharing, it can cost 10x, and it is invisible to every
// single-threaded test. The padding below is load-bearing.
//
// MEMORY ORDERING WARNING. x86 is TSO: acquire and release compile to plain
// mov, so a wrong ordering annotation here is invisible on this hardware and
// would surface only on ARM. Neither TSAN nor ASAN runs on the dev box. The
// orderings are specified by the card, not discovered by testing — do not
// "simplify" one to relaxed or "harden" one to seq_cst.

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
    [[nodiscard]] ALTAIR_HOT bool try_push(const T& v) noexcept {
        // Relaxed: the producer owns tail_, so reading its own index needs no
        // synchronisation.
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);

        if (tail - cached_head_ == Capacity) {
            // Looks full. Only now pay to read the consumer's cache line.
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail - cached_head_ == Capacity) {
                return false;
            }
        }

        slots_[tail & (Capacity - 1)] = v;

        // Release publishes the slot write above. Reversing these two lines
        // exposes an unwritten slot to the consumer.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Dequeue one element into `out`. CONSUMER THREAD ONLY.
    /// UNIT: none. Returns false iff the ring is empty; `out` is untouched then.
    /// PRECONDITION: called from exactly one thread, for the life of the ring.
    [[nodiscard]] ALTAIR_HOT bool try_pop(T& out) noexcept {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);

        if (head == cached_tail_) {
            // Looks empty. Only now pay to read the producer's cache line.
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head == cached_tail_) {
                return false;   // `out` deliberately untouched
            }
        }

        out = slots_[head & (Capacity - 1)];

        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /// Elements currently queued. UNIT: elements.
    /// APPROXIMATE when called concurrently — it is a snapshot of two indices
    /// read at different instants, and is exact only when the other side is
    /// quiescent. Safe from either thread. Never use it to decide whether a
    /// subsequent push or pop will succeed; check that call's return value.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        return static_cast<std::size_t>(pushed() - popped());
    }

    /// size_approx() == 0. Same approximation caveat.
    [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }

    /// size_approx() == capacity(). Same approximation caveat.
    [[nodiscard]] bool full_approx() const noexcept { return size_approx() == Capacity; }

    /// Slots in the ring. UNIT: elements. All of them are usable.
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

    /// Total successful pushes since construction. UNIT: elements.
    /// Monotonic. Safe from either thread.
    [[nodiscard]] std::uint64_t pushed() const noexcept {
        return tail_.load(std::memory_order_relaxed);
    }

    /// Total successful pops since construction. UNIT: elements.
    /// Monotonic. Safe from either thread.
    [[nodiscard]] std::uint64_t popped() const noexcept {
        return head_.load(std::memory_order_relaxed);
    }

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
