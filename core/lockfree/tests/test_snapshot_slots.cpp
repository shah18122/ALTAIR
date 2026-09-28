// CX02-C1 / C1c acceptance tests for core/lockfree/snapshot_slots.hpp.
//
// The P0-06b seqlock copied its payload while the writer could be assigning it
// (C02-002), and its reader spun without bound (C02-003). SnapshotSlots makes
// that copy impossible by construction; these tests force the interleavings
// the proof depends on, deterministically, through the Hooks parameter.
//
// The proof has three relevant windows:
//   * a reader, after loading the published index but before storing its hold
//     (`before_hold`, added with the stale-index reuse regression),
//   * a reader's, between storing its hold and re-checking the published slot
//     (`after_hold`), and
//   * the WRITER's, between choosing a free slot and writing it
//     (`before_write`, added by C1c after R-AB-022 pointed out that no forced
//     interleaving could reach it).
//
// Test 6 is a real multi-threaded stress. It checks an invariant, not the
// absence of a race: ASan cannot see races and TSan needs Linux, which this
// box does not have. Its payload is deliberately 4 KB -- two 8-byte words
// cannot show a partial write, and the production payload is a ~9 KB
// ConfigSnapshot (R-AB-023).
//
// No check description here may contain the substring FAIL.

#include <lockfree/snapshot_slots.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

struct Pair {
    std::uint64_t a = 0;
    std::uint64_t b = 0;     // always 2 * a when published
};

/// The stress payload: 512 words that must ALL equal the generation. A torn
/// copy shows as one word out of step, which two words could never reveal.
struct Wide {
    std::array<std::uint64_t, 512> cells{};
};

struct Hooks {
    static inline std::function<void(std::size_t)> before_hold_fn;
    static inline std::function<void(std::size_t)> after_hold_fn;
    static inline std::function<void(std::size_t)> before_write_fn;
    static void before_hold(std::size_t slot) noexcept {
        if (before_hold_fn) { before_hold_fn(slot); }
    }
    static void after_hold(std::size_t slot) noexcept {
        if (after_hold_fn) { after_hold_fn(slot); }
    }
    static void before_write(std::size_t slot) noexcept {
        if (before_write_fn) { before_write_fn(slot); }
    }
};

using altair::SlotError;
using altair::SnapshotSlots;
using altair::kSlotReadRetries;

Pair make(std::uint64_t n) { return Pair{n, 2 * n}; }

Wide wide(std::uint64_t n)
{
    Wide w;
    w.cells.fill(n);
    return w;
}

// ── 1 ────────────────────────────────────────────────────────────────────
void writer_never_writes_a_held_slot()
{
    std::printf("\n1 writer_never_writes_a_held_slot\n");
    using S = SnapshotSlots<Pair, 1, Hooks>;
    static S s;
    check(s.publish(make(1)).value_or(0) == 1, "generation 1 published");
    bool held_slot_written = false;
    bool every_publish_succeeded = true;
    int calls = 0;
    Hooks::after_hold_fn = [&](std::size_t held) {
        if (++calls > 1) { return; }        // interfere on the first pin only
        // The reader holds `held` and has not re-checked. Publish MORE times
        // than there are slots: every one must avoid the held slot.
        for (std::uint64_t n = 2; n <= 2 + 2 * S::kSlots; ++n) {
            const auto r = s.publish(make(n));
            every_publish_succeeded = every_publish_succeeded && r.has_value();
            if (s.last_written() == held) { held_slot_written = true; }
        }
    };
    Pair out{};
    const auto g = s.read(out);
    Hooks::after_hold_fn = nullptr;
    check(!held_slot_written,
          "no publish wrote the slot a reader had pinned, across 2 x kSlots"
          " publishes");
    check(every_publish_succeeded,
          "and none was refused: a free slot existed every time");
    check(g.has_value() && calls >= 2,
          "the reader saw published move, retried, and read");
    check(g.has_value() && out.b == 2 * out.a && *g == out.a,
          "and what it read is one whole publish, with its own generation");
    check(s.holds_free() == 1, "and the hold was released");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void contended_read_is_refused_not_torn()
{
    std::printf("\n2 contended_read_is_refused_not_torn\n");
    using S = SnapshotSlots<Pair, 1, Hooks>;
    static S s;
    (void)s.publish(make(1));
    std::uint64_t n = 1;
    int calls = 0;
    Hooks::after_hold_fn = [&](std::size_t) {
        ++calls;
        (void)s.publish(make(++n));         // moves published EVERY time
    };
    Pair out{12345, 999};
    const auto g = s.read(out);
    Hooks::after_hold_fn = nullptr;
    check(!g && g.error() == SlotError::Contended,
          "a reader that loses every re-check is refused with Contended");
    check(calls == kSlotReadRetries,
          "after exactly kSlotReadRetries attempts -- bounded, not a spin");
    check(out.a == 12345 && out.b == 999,
          "and `out` is UNTOUCHED -- the seqlock left it indeterminate");
    check(s.holds_free() == 1, "the hold was released on the refusal path too");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void a_free_slot_exists_with_every_hold_pinned()
{
    std::printf("\n3 a_free_slot_exists_with_every_hold_pinned\n");
    using S = SnapshotSlots<Pair, 2, Hooks>;
    static S s;
    (void)s.publish(make(1));
    int depth = 0;
    bool refused = false;
    bool wrote_pinned = false;
    std::size_t pinned_outer = S::kSlots;
    Hooks::after_hold_fn = [&](std::size_t held) {
        ++depth;
        if (depth == 1) {
            pinned_outer = held;
            // Move published so the second reader pins a DIFFERENT slot.
            (void)s.publish(make(2));
            Pair inner{};
            (void)s.read(inner);             // re-enters this hook at depth 2
        } else if (depth == 2) {
            // Both holds now name slots: the outer reader's stale one and
            // this reader's. Publish repeatedly.
            for (std::uint64_t n = 3; n < 3 + 3 * S::kSlots; ++n) {
                const auto r = s.publish(make(n));
                if (!r) { refused = true; }
                if (s.last_written() == held || s.last_written() == pinned_outer) {
                    wrote_pinned = true;
                }
            }
        }
    };
    Pair out{};
    (void)s.read(out);
    Hooks::after_hold_fn = nullptr;
    check(depth >= 2, "two readers were pinned at once");
    check(!refused,
          "with kReaders holds pinned, every publish still found a free slot --"
          " NoFreeSlot is unreachable at kSlots = kReaders + 2");
    check(!wrote_pinned, "and none of them wrote either pinned slot");
    check(s.holds_free() == 2, "both holds were released");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void too_many_concurrent_readers_are_refused()
{
    std::printf("\n4 too_many_concurrent_readers_are_refused\n");
    using S = SnapshotSlots<Pair, 1, Hooks>;
    static S s;
    (void)s.publish(make(7));
    SlotError inner_error = SlotError::NoFreeSlot;
    bool inner_refused = false;
    int calls = 0;
    Hooks::after_hold_fn = [&](std::size_t) {
        if (++calls > 1) { return; }
        Pair inner{};
        const auto r = s.read(inner);       // the only hold is taken
        inner_refused = !r;
        if (!r) { inner_error = r.error(); }
    };
    Pair out{};
    const auto g = s.read(out);
    Hooks::after_hold_fn = nullptr;
    check(inner_refused && inner_error == SlotError::NoHoldFree,
          "a reader beyond kReaders is refused with NoHoldFree, not blocked");
    check(g.has_value() && out.a == 7, "while the reader holding it reads normally");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void nothing_published_reads_zero_and_touches_nothing()
{
    std::printf("\n5 nothing_published_reads_zero_and_touches_nothing\n");
    SnapshotSlots<Pair, 3> s;
    Pair out{5, 6};
    const auto g = s.read(out);
    check(g.has_value() && *g == 0 && out.a == 5 && out.b == 6,
          "a read before any publish returns generation 0 and leaves out alone");
    check(s.generation() == 0 && s.holds_free() == 3, "nothing is held");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void stress_one_writer_three_readers()
{
    std::printf("\n6 stress_one_writer_three_readers (invariant evidence, not"
                " race detection)\n");
    static SnapshotSlots<Wide, 3> s;
    constexpr std::uint64_t kReadsEach = 200'000;
    constexpr std::uint64_t kFloor = 10'000;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> torn{0}, backwards{0}, contended{0}, reads{0};

    std::thread writer([&] {
        std::uint64_t n = 1;
        while (!stop.load(std::memory_order_acquire)) {
            (void)s.publish(wide(n++));
        }
    });
    auto reader = [&] {
        std::uint64_t last = 0;
        for (std::uint64_t i = 0; i < kReadsEach; ++i) {
            Wide p{};
            const auto g = s.read(p);
            if (!g) {
                contended.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (*g == 0) { continue; }
            reads.fetch_add(1, std::memory_order_relaxed);
            // EVERY word must belong to the same publish. A partial write
            // shows here and could not show on a two-word payload.
            for (const std::uint64_t cell : p.cells) {
                if (cell != *g) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
            if (*g < last) { backwards.fetch_add(1, std::memory_order_relaxed); }
            last = *g;
        }
    };
    std::vector<std::thread> rs;
    for (int i = 0; i < 3; ++i) { rs.emplace_back(reader); }
    for (auto& t : rs) { t.join(); }
    stop.store(true, std::memory_order_release);
    writer.join();

    std::printf("    %llu reads of a %zu-byte payload, %llu contended refusals\n",
                static_cast<unsigned long long>(reads.load()), sizeof(Wide),
                static_cast<unsigned long long>(contended.load()));
    check(reads.load() >= kFloor,
          "an evidence floor of 10,000 observed reads -- `> 0` would pass"
          " having seen almost nothing");
    check(torn.load() == 0,
          "no read ever returned a payload whose 512 words were not all one"
          " publish");
    check(backwards.load() == 0,
          "no reader ever saw its generation go backwards");
    check(s.holds_free() == 3, "every hold was released at the end");
}

// ── 7 ────────────────────────────────────────────────────────────────────
// CX02-C1c (R-AB-022). The WRITER's window: the slot is chosen and not yet
// written. Nothing could reach this interleaving before `before_write`.
void reader_pins_between_the_writers_scan_and_its_write()
{
    std::printf("\n7 reader_pins_between_the_writers_scan_and_its_write\n");
    using S = SnapshotSlots<Pair, 1, Hooks>;
    static S s;
    (void)s.publish(make(1));

    std::size_t about_to_write = S::kSlots;
    std::size_t reader_pinned = S::kSlots;
    std::uint64_t got_gen = 0;
    Pair got{};
    bool inside = false;
    Hooks::before_write_fn = [&](std::size_t w) {
        if (inside) { return; }
        inside = true;
        about_to_write = w;
        // A reader pins and copies RIGHT HERE -- after the writer chose its
        // slot and before it writes a byte of it.
        Hooks::after_hold_fn = [&](std::size_t p) { reader_pinned = p; };
        const auto g = s.read(got);
        Hooks::after_hold_fn = nullptr;
        got_gen = g.value_or(0);
        inside = false;
    };
    const auto pub = s.publish(make(2));
    Hooks::before_write_fn = nullptr;

    check(pub.has_value() && about_to_write < S::kSlots
              && reader_pinned < S::kSlots,
          "a reader pinned a slot inside the writer's window");
    check(reader_pinned != about_to_write,
          "and it is NOT the slot the writer was about to write -- the half of"
          " the proof no forced interleaving could reach before");
    check(got_gen == 1 && got.a == 1 && got.b == 2,
          "the reader copied the PREVIOUS publish whole, not a half-written"
          " slot");
    check(s.holds_free() == 1, "and released its hold");
    Pair after{};
    const auto g2 = s.read(after);
    check(g2.has_value() && *g2 == 2 && after.a == 2 && after.b == 4,
          "the writer's publish then completes and is readable");
}

// ── 8 ────────────────────────────────────────────────────────────────────
// A reader can load an index and be descheduled before publishing its hold.
// Recycle that slot, then leave it inactive before resuming: index-only
// revalidation must reject the stale observation and copy the latest slot.
void a_reader_rechecks_after_its_unheld_slot_is_recycled()
{
    std::printf("\n8 a_reader_rechecks_after_its_unheld_slot_is_recycled\n");
    using S = SnapshotSlots<Pair, 1, Hooks>;
    S s;
    const auto first = s.publish(make(1));
    if (!first) {
        check(false, "the initial value publishes before the stale-read test");
        return;
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool reader_paused = false;
    bool resume_reader = false;
    std::atomic<int> hook_calls{0};
    std::size_t stale_slot = S::kSlots;
    Pair observed{};
    std::uint64_t observed_generation = 0;
    bool read_succeeded = false;

    Hooks::before_hold_fn = [&](std::size_t slot) {
        if (hook_calls.fetch_add(1, std::memory_order_relaxed) != 0) {
            return;
        }
        std::unique_lock lock(mutex);
        stale_slot = slot;
        reader_paused = true;
        cv.notify_one();
        cv.wait(lock, [&] { return resume_reader; });
    };

    std::thread reader([&] {
        const auto result = s.read(observed);
        read_succeeded = result.has_value();
        observed_generation = result.value_or(0);
    });

    bool paused = false;
    {
        std::unique_lock lock(mutex);
        paused = cv.wait_for(lock, std::chrono::seconds(5),
                             [&] { return reader_paused; });
    }
    if (!paused) {
        {
            std::lock_guard lock(mutex);
            resume_reader = true;
        }
        cv.notify_one();
        reader.join();
        Hooks::before_hold_fn = nullptr;
        check(false, "the reader reaches the pre-hold pause point");
        return;
    }

    // Starting at slot 0, these publishes select slots 1, 0 (recycling the
    // stale observation), and 1. The final publication no longer names slot 0.
    const auto second = s.publish(make(2));
    const std::size_t second_slot = s.last_written();
    const auto third = s.publish(make(3));
    const std::size_t recycled_slot = s.last_written();
    const auto fourth = s.publish(make(4));
    const std::size_t latest_slot = s.last_written();
    const bool recycled_then_moved = first.has_value() && second.has_value()
        && third.has_value() && fourth.has_value()
        && stale_slot != second_slot && stale_slot == recycled_slot
        && stale_slot != latest_slot && *fourth == 4;

    {
        std::lock_guard lock(mutex);
        resume_reader = true;
    }
    cv.notify_one();
    reader.join();
    Hooks::before_hold_fn = nullptr;

    check(recycled_then_moved,
          "the initially observed slot was recycled, then the final publish"
          " moved away from it");
    check(read_succeeded && observed_generation == 4
              && observed.a == 4 && observed.b == 8,
          "the stale read retries and returns the latest complete value with"
          " its matching generation, never the recycled slot");
    check(hook_calls.load(std::memory_order_relaxed) >= 2,
          "the reader reaches the pre-hold window again after rejecting its"
          " stale publication");
    check(s.holds_free() == 1, "the retry path releases its hold");
}

} // namespace

int main()
{
    std::printf("CX02-C1/C1c -- SnapshotSlots\n");
    writer_never_writes_a_held_slot();
    contended_read_is_refused_not_torn();
    a_free_slot_exists_with_every_hold_pinned();
    too_many_concurrent_readers_are_refused();
    nothing_published_reads_zero_and_touches_nothing();
    stress_one_writer_three_readers();
    reader_pins_between_the_writers_scan_and_its_write();
    a_reader_rechecks_after_its_unheld_slot_is_recycled();
    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
