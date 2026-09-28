#pragma once

// CX02-C1 — publish a value to many readers WITHOUT a data race.
//
// Replaces the P0-06b seqlock for payloads that are not a set of atomics.
// Findings C02-002, C02-003 and the card-level C21-003; the amendment of record
// is prompts/cx02/CORRECTIONS.md.
//
// WHAT WAS WRONG WITH THE SEQLOCK.
//
// The writer assigned the payload as a plain object between an odd and an
// even sequence number, and the reader COPIED it as a plain object before
// checking the numbers. The check threw a torn copy away, but the concurrent
// read and write had already happened -- undefined behaviour for any
// non-atomic T, whatever the sequence numbers said afterwards. The "required"
// atomic_thread_fence is an MSVC STL facility with no GCC/Clang counterpart,
// and the reader's retry loop was unbounded by specification.
//
// THE PROTOCOL.
//
// kSlots = kReaders + 2 copies of T. The writer only ever writes a slot that no
// reader can be copying; a reader only ever copies a slot the writer cannot be
// writing. No byte is read while it is being written, so there is nothing for
// a sequence check to detect.
//
//   Reader  1. claim a hold (CAS kFree -> kClaimed); none free -> NoHoldFree
//           2. p = published                          (seq_cst load)
//           3. hold = p                               (seq_cst store)
//           4. if published == p: copy slot p, release, return its generation
//              else retry from 2, at most kSlotReadRetries, then Contended
//   Writer  1. pub = published                        (seq_cst load)
//           2. read every hold                        (seq_cst loads)
//           3. write a slot that is neither pub nor held
//           4. published = that slot                  (seq_cst store)
//
// WHY THE WRITER CANNOT WRITE A SLOT BEING COPIED. Suppose the writer writes
// slot w while a reader copies w. The reader stored hold = w (R3) and then saw
// published == w (R4). The writer read published == pub != w (W1), and nothing
// else changes published before the writer's own W4. So R4 happened before W1:
// R3 < R4 < W1 < W2. The reader has not released its hold -- it is still
// copying -- so W2 read hold == w and the writer excluded w. Contradiction.
// Sequential consistency on exactly these four operations is what the proof
// needs, which is why they are seq_cst and not acquire/release.
//
// WHY A FREE SLOT ALWAYS EXISTS. At most kReaders holds each name one slot,
// plus the published slot: kReaders + 1 < kReaders + 2 = kSlots. NoFreeSlot is
// therefore unreachable. It is still returned rather than assumed (rule 11).
//
// WHY THE READ BOUND IS SAFE. The seqlock card forbade a retry cap because a
// capped seqlock read would have to return a possibly-torn value. Here a
// failed read has copied nothing: it returns Contended and leaves `out`
// untouched, which a caller can act on (C02-003).
//
// EVIDENCE LIMITS, STATED. The tests force the interleavings above
// deterministically through the Hooks parameter, and run a multi-threaded
// stress for invariant violations. Neither is race-detector evidence: TSan
// needs Linux, which this box does not have, and ASan does not detect races.

#include <lockfree/spsc_ring.hpp>   // for kCacheLine
#include <types/units.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <type_traits>

namespace altair {

enum class SlotError : std::uint8_t {
    /// Every hold was in use: more readers were mid-read at once than the
    /// object was sized for. Nothing was copied; retry later.
    NoHoldFree,
    /// The published slot moved kSlotReadRetries times in a row while this
    /// reader tried to pin it. Nothing was copied.
    Contended,
    /// No slot was free to write. Proven unreachable -- see the header.
    NoFreeSlot
};

/// RULE 11: refused past this -- a reader whose pin loses to a new publish
/// this many times in a row returns Contended instead of spinning.
inline constexpr int kSlotReadRetries = 64;

/// Test seam. Production code uses this, and its empty inline function
/// compiles to nothing.
struct NoSlotHooks {
    /// Called after the reader loaded the published slot and BEFORE it stores
    /// that value in its hold. The publisher may recycle the observed slot in
    /// this window; the reader must then detect a changed publication before
    /// copying.
    static void before_hold(std::size_t /*slot*/) noexcept {}
    /// Called by a reader after it stored its hold and BEFORE it re-checks
    /// the published slot -- the window the proof is about.
    static void after_hold(std::size_t /*slot*/) noexcept {}
    /// Called by the writer after it chose a slot and BEFORE it writes it --
    /// the OTHER half of the same window (R-AB-022). Without a seam here no
    /// forced interleaving can put a reader's pin between the writer's busy
    /// scan and its write, which is exactly what the proof turns on.
    static void before_write(std::size_t /*slot*/) noexcept {}
};

template <typename T, std::size_t kReaders, typename Hooks = NoSlotHooks>
class SnapshotSlots {
    static_assert(kReaders >= 1, "at least one reader");
    static_assert(kReaders + 2 < 0xFFFF'FFFEu, "slot indices must not collide with hold markers");
    static_assert(std::is_nothrow_copy_assignable_v<T>,
                  "copies happen in noexcept publish/read");
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "slots are default-constructed up front, allocation-free");

public:
    static constexpr std::size_t kSlots = kReaders + 2;

    SnapshotSlots() noexcept {
        for (auto& h : holds_) {
            h.store(kFree, std::memory_order_relaxed);
        }
    }

    SnapshotSlots(const SnapshotSlots&) = delete;
    SnapshotSlots& operator=(const SnapshotSlots&) = delete;

    /// Publish a new value. WRITER THREAD ONLY -- exactly one, for the life of
    /// the object. Never blocks. Returns the new generation, starting at 1.
    [[nodiscard]] std::expected<std::uint64_t, SlotError>
    publish(const T& v) noexcept {
        const std::uint32_t pub = published_.load(std::memory_order_seq_cst);
        std::array<bool, kSlots> busy{};
        if (pub < kSlots) { busy[pub] = true; }
        for (const auto& h : holds_) {
            const std::uint32_t s = h.load(std::memory_order_seq_cst);
            if (s < kSlots) { busy[s] = true; }
        }
        std::size_t w = kSlots;
        for (std::size_t i = 0; i < kSlots; ++i) {
            if (!busy[i]) {
                w = i;
                break;
            }
        }
        if (w == kSlots) {
            return std::unexpected(SlotError::NoFreeSlot);
        }
        // The writer's window: the slot is chosen and not yet written.
        Hooks::before_write(w);
        const std::uint64_t gen = writer_gen_ + 1;
        slots_[w].value = v;
        slots_[w].gen = gen;
        writer_gen_ = gen;
        last_written_ = w;
        published_.store(static_cast<std::uint32_t>(w), std::memory_order_seq_cst);
        generation_.store(gen, std::memory_order_seq_cst);
        return gen;
    }

    /// Copy the published value into `out`. SAFE FROM ANY NUMBER OF READER
    /// THREADS up to kReaders at once. Returns the copied value's generation,
    /// or 0 if nothing has been published. On 0 or on an error `out` is
    /// UNTOUCHED.
    [[nodiscard]] ALTAIR_HOT std::expected<std::uint64_t, SlotError>
    read(T& out) const noexcept {
        std::size_t h = kReaders;
        for (std::size_t i = 0; i < kReaders; ++i) {
            std::uint32_t expect = kFree;
            if (holds_[i].compare_exchange_strong(expect, kClaimed,
                                                  std::memory_order_seq_cst)) {
                h = i;
                break;
            }
        }
        if (h == kReaders) {
            return std::unexpected(SlotError::NoHoldFree);
        }
        for (int attempt = 0; attempt < kSlotReadRetries; ++attempt) {
            const std::uint32_t p = published_.load(std::memory_order_seq_cst);
            if (p >= kSlots) {
                holds_[h].store(kFree, std::memory_order_seq_cst);
                return std::uint64_t{0};
            }
            Hooks::before_hold(p);
            holds_[h].store(p, std::memory_order_seq_cst);
            Hooks::after_hold(p);
            if (published_.load(std::memory_order_seq_cst) == p) {
                out = slots_[p].value;
                const std::uint64_t g = slots_[p].gen;
                holds_[h].store(kFree, std::memory_order_seq_cst);
                return g;
            }
        }
        holds_[h].store(kFree, std::memory_order_seq_cst);
        return std::unexpected(SlotError::Contended);
    }

    /// An observational published-generation count. Zero before the first
    /// publish. A concurrent call may briefly see the previous count while a
    /// publish is completing; use `read()`'s return value when generation must
    /// be paired coherently with a copied payload.
    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }

    /// For tests: the slot the last publish wrote. WRITER THREAD ONLY.
    [[nodiscard]] std::size_t last_written() const noexcept { return last_written_; }

    /// For tests: holds not claimed by any reader. All of them, whenever no
    /// read is in flight -- a read that returned without releasing would leak.
    [[nodiscard]] std::size_t holds_free() const noexcept {
        std::size_t n = 0;
        for (const auto& h : holds_) {
            if (h.load(std::memory_order_seq_cst) == kFree) { ++n; }
        }
        return n;
    }

private:
    static constexpr std::uint32_t kFree = 0xFFFF'FFFFu;
    static constexpr std::uint32_t kClaimed = 0xFFFF'FFFEu;

    struct Slot {
        T value{};
        std::uint64_t gen = 0;
    };

    alignas(kCacheLine) std::atomic<std::uint32_t> published_{kFree};
    std::atomic<std::uint64_t> generation_{0};
    alignas(kCacheLine) mutable std::array<std::atomic<std::uint32_t>, kReaders> holds_{};
    // Writer-only state.
    std::uint64_t writer_gen_ = 0;
    std::size_t last_written_ = 0;
    alignas(kCacheLine) std::array<Slot, kSlots> slots_{};
};

} // namespace altair
