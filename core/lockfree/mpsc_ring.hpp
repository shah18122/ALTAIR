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
// MEMORY ORDERING WARNING — see spsc_ring.hpp. x86 is TSO, so a wrong
// ordering below is invisible on this hardware; the TSan CI job runs the
// `concurrency` label. The orderings are specified by the card, not
// discovered by testing.
//
// TWO PROPERTIES OF THIS SCHEME THAT A CALLER MUST KNOW.
//
// 1. A PUSH IS BOUNDED. Under contention a producer retries its CAS; the old
//    loop retried forever, so a producer could spin for as long as others kept
//    winning. push() gives up after `max_attempts` and says Contended, which
//    is a different answer from Full: the caller decides whether to retry,
//    back off or count a drop, and nothing spins unboundedly inside the ring.
//
// 2. HEAD-OF-LINE: the consumer takes slots IN ORDER. A producer that has
//    claimed a slot (won the CAS) but not yet written it holds up every later
//    slot, even ones already published -- that is what keeps the ring FIFO.
//    The claim-to-publish window is one copy of T, so it is short unless the
//    producer is preempted inside it. pop() tells the two cases apart: Empty
//    (nothing claimed) versus Pending (the head is claimed, not yet
//    published), so a consumer can see a stalled producer rather than mistake
//    it for an idle ring. claim()/publish() expose the two phases.

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

    /// Why a push did not happen.
    enum class Push : std::uint8_t { Ok, Full, Contended };
    /// What the consumer found at the head.
    enum class Pop : std::uint8_t { Ok, Empty, Pending };

    /// CAS attempts a push makes before it reports Contended.
    static constexpr unsigned kDefaultAttempts = 256;

    /// A claimed slot, to be written and published by the producer that holds it.
    struct Claim {
        void* slot = nullptr;
        std::uint64_t pos = 0;
        [[nodiscard]] explicit operator bool() const noexcept { return slot != nullptr; }
    };

    /// Phase one: claim the next slot. SAFE FROM ANY NUMBER OF PRODUCER
    /// THREADS. An empty Claim with `why` Full or Contended when there is none.
    /// A claim MUST be published, promptly: until it is, the consumer waits
    /// at it (head-of-line).
    [[nodiscard]] ALTAIR_HOT Claim claim(Push* why = nullptr, unsigned max_attempts = kDefaultAttempts) noexcept {
        std::uint64_t pos = tail_.load(std::memory_order_relaxed);
        for (unsigned attempt = 0; attempt < max_attempts; ++attempt) {
            Slot* slot = &slots_[pos & kMask];
            const std::uint64_t seq = slot->seq.load(std::memory_order_acquire);
            // SIGNED difference. Comparing the unsigned values directly
            // mis-orders across the 64-bit wrap.
            const std::int64_t dif =
                static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos);

            if (dif == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_relaxed)) {
                    if (why) *why = Push::Ok;
                    return Claim{slot, pos};     // slot claimed
                }
            } else if (dif < 0) {
                if (why) *why = Push::Full;
                return {};
            } else {
                pos = tail_.load(std::memory_order_relaxed);   // lost the race
            }
        }
        if (why) *why = Push::Contended;
        return {};
    }

    /// Phase two: write the claimed slot and hand it to the consumer.
    ALTAIR_HOT void publish(Claim c, const T& v) noexcept {
        Slot* slot = static_cast<Slot*>(c.slot);
        slot->data = v;
        // Release publishes the data write above.
        slot->seq.store(c.pos + 1, std::memory_order_release);
    }

    /// Enqueue one element: claim, write, publish. SAFE FROM ANY NUMBER OF
    /// PRODUCER THREADS. Never blocks, never allocates, and gives up after
    /// `max_attempts` lost CASes (Contended), so it is bounded.
    [[nodiscard]] ALTAIR_HOT Push push(const T& v, unsigned max_attempts = kDefaultAttempts) noexcept {
        Push why = Push::Ok;
        const Claim c = claim(&why, max_attempts);
        if (!c) return why;
        publish(c, v);
        return Push::Ok;
    }

    /// push() == Ok. False when full OR contended past the default bound.
    [[nodiscard]] ALTAIR_HOT bool try_push(const T& v) noexcept { return push(v) == Push::Ok; }

    /// Dequeue one element into `out`. CONSUMER THREAD ONLY. Empty when
    /// nothing is claimed; Pending when the head slot is claimed but its
    /// producer has not published it yet (later slots wait behind it).
    /// `out` is untouched unless Ok.
    [[nodiscard]] ALTAIR_HOT Pop pop(T& out) noexcept {
        const std::uint64_t pos = head_.load(std::memory_order_relaxed);
        Slot* slot = &slots_[pos & kMask];
        const std::uint64_t seq = slot->seq.load(std::memory_order_acquire);
        const std::int64_t dif =
            static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos + 1);

        if (dif != 0) {
            // Not published. Claimed (tail moved past it) or never claimed?
            return tail_.load(std::memory_order_relaxed) > pos ? Pop::Pending : Pop::Empty;
        }

        head_.store(pos + 1, std::memory_order_relaxed);
        out = slot->data;
        // Re-arm the slot for the next lap around the ring.
        slot->seq.store(pos + Capacity, std::memory_order_release);
        return Pop::Ok;
    }

    /// pop() == Ok. CONSUMER THREAD ONLY.
    [[nodiscard]] ALTAIR_HOT bool try_pop(T& out) noexcept { return pop(out) == Pop::Ok; }

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
