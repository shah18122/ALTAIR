// P0-06a acceptance tests for core/lockfree/spsc_ring.hpp.
// Plain main() (Catch2 still unavailable).
//
// NOTE ON WHAT THESE TESTS CAN AND CANNOT PROVE. x86 is TSO, so acquire and
// release compile to plain mov. A wrong memory ordering in the ring is
// INVISIBLE here and would surface only on ARM. Neither TSAN nor ASAN runs on
// this box (the root CMakeLists guards both with AND NOT MSVC). The stress test
// below is evidence about the algorithm's LOGIC, not about its ordering.

#include <lockfree/spsc_ring.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

} // namespace

using namespace altair;

void test_spsc_layout_and_traits()
{
    using R = SpscRing<std::uint64_t, 8>;

    // The padding is a performance contract, not decoration. If a careless
    // reorder puts tail_ and head_ on one line, false sharing costs ~10x and
    // no functional test would notice.
    check(sizeof(R) >= 2 * kCacheLine + 8 * sizeof(std::uint64_t),
          "ring is large enough for two padded index lines plus storage");
    check(alignof(R) >= kCacheLine, "ring is cache-line aligned");
    check(kCacheLine == 64, "kCacheLine is 64");

    check(R::capacity() == 8, "capacity() == 8");
    check(!std::is_copy_constructible_v<R>, "ring is not copy constructible");
    check(!std::is_move_constructible_v<R>, "ring is not move constructible");
    check(!std::is_copy_assignable_v<R>, "ring is not copy assignable");

    R r;
    check(r.empty_approx(), "a fresh ring is empty");
    check(!r.full_approx(), "a fresh ring is not full");
    check(r.size_approx() == 0, "size == 0");
    check(r.pushed() == 0 && r.popped() == 0, "counters start at zero");

    std::printf("        sizeof(SpscRing<u64,8>) = %zu, alignof = %zu\n",
                sizeof(R), alignof(R));
}

void test_spsc_single_thread_fifo()
{
    SpscRing<int, 4> r;
    check(r.try_push(1) && r.try_push(2) && r.try_push(3), "three pushes succeed");
    check(r.size_approx() == 3, "size == 3");

    int v = 0;
    check(r.try_pop(v) && v == 1, "FIFO: first out is first in");
    check(r.try_pop(v) && v == 2, "FIFO: second");
    check(r.try_pop(v) && v == 3, "FIFO: third");
    check(!r.try_pop(v), "empty ring rejects pop");
    check(r.size_approx() == 0, "size back to 0");
}

void test_spsc_fill_and_drain()
{
    SpscRing<int, 4> r;

    // ALL FOUR slots usable — an implementation that sacrifices one fails here.
    check(r.try_push(10) && r.try_push(20) && r.try_push(30) && r.try_push(40),
          "all 4 slots are usable");
    check(r.size_approx() == 4, "size == capacity");
    check(r.full_approx(), "ring reports full");
    check(!r.try_push(50), "a full ring rejects the push");
    check(r.size_approx() == 4, "a rejected push changes nothing");
    check(r.pushed() == 4, "pushed() counts only successes");

    int v = 0;
    check(r.try_pop(v) && v == 10,
          "the OLDEST element survived — full rejects, it does not drop-oldest");
    check(r.try_push(50), "one slot freed, one push fits");
    check(r.size_approx() == 4, "back to full");
}

void test_spsc_wraparound()
{
    SpscRing<int, 4> r;
    int v = 0;
    bool ok = true;
    for (int i = 0; i < 1000; ++i) {
        if (!r.try_push(i)) { ok = false; break; }
        if (!r.try_pop(v) || v != i) { ok = false; break; }
    }
    check(ok, "1000 push/pop cycles through a 4-slot ring (250 full wraps)");
    check(r.size_approx() == 0, "ring drained");
    check(r.pushed() == 1000 && r.popped() == 1000, "counters agree");

    // Keep the ring partly full across the wrap boundary.
    SpscRing<int, 4> s;
    check(s.try_push(1) && s.try_push(2), "seed two stragglers");
    bool ok2 = true;
    for (int i = 0; i < 1000; ++i) {
        if (!s.try_push(i)) { ok2 = false; break; }
        if (!s.try_pop(v)) { ok2 = false; break; }
    }
    check(ok2, "1000 cycles with the ring never empty");
    check(s.size_approx() == 2, "the two stragglers are still queued");
}

void test_spsc_conservation()
{
    // pushed() - popped() == size_approx(), at every quiescent point.
    // Nothing is created or destroyed in the ring.
    SpscRing<int, 8> r;
    int v = 0;
    bool ok = true;
    bool never_negative = true;
    bool never_over = true;

    for (int i = 0; i < 1000; ++i) {
        if (i % 3 != 0) { (void)r.try_push(i); }
        if (i % 5 == 0) { (void)r.try_pop(v); }
        if (r.pushed() - r.popped() != r.size_approx()) { ok = false; }
        if (r.popped() > r.pushed()) { never_negative = false; }
        if (r.size_approx() > r.capacity()) { never_over = false; }
    }

    check(ok, "conservation: pushed - popped == size, at every step");
    check(never_negative, "never popped more than pushed");
    check(never_over, "never exceeded capacity");
}

void test_spsc_pop_leaves_out_untouched()
{
    SpscRing<int, 4> r;
    int v = 0xBEEF;
    check(!r.try_pop(v), "pop from an empty ring fails");
    check(v == 0xBEEF, "a failed pop does not scribble on out");
}

void test_spsc_two_thread_stress()
{
    constexpr int kN = 1'000'000;
    static SpscRing<std::uint64_t, 1024> r;

    std::atomic<bool> sequence_ok{true};

    const auto t0 = std::chrono::steady_clock::now();

    std::thread producer([&] {
        for (int i = 0; i < kN; ++i) {
            const auto v = static_cast<std::uint64_t>(i);
            while (!r.try_push(v)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        std::uint64_t v = 0;
        for (int i = 0; i < kN; ++i) {
            while (!r.try_pop(v)) {
                std::this_thread::yield();
            }
            // Verify the FULL SEQUENCE, not just the count: a matched
            // drop-and-duplicate would pass a count-only check.
            if (v != static_cast<std::uint64_t>(i)) {
                sequence_ok.store(false, std::memory_order_relaxed);
            }
        }
    });

    producer.join();
    consumer.join();
    const auto t1 = std::chrono::steady_clock::now();

    check(sequence_ok.load(), "every element arrived exactly once, in order");
    check(r.pushed() == static_cast<std::uint64_t>(kN), "pushed == 1e6");
    check(r.popped() == static_cast<std::uint64_t>(kN), "popped == 1e6");
    check(r.size_approx() == 0, "ring drained");

    const double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("        %d elements in %.3f s = %.1f M elem/s (2 threads)\n",
                kN, sec, static_cast<double>(kN) / sec / 1e6);
}

namespace {

/// Batch-timed throughput. try_push/try_pop are ~2 ns, below the RDTSCP floor,
/// so this uses the P0-05b batching technique, not the P0-03 subtraction one.
void report_throughput()
{
    std::printf("\nthroughput — batch-timed, single thread\n");

    constexpr int kOps = 1'000'000;
    SpscRing<std::uint64_t, 1024> r;
    std::uint64_t sink = 0;
    std::uint64_t v = 0;

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i) {
        (void)r.try_push(static_cast<std::uint64_t>(i));
        (void)r.try_pop(v);
        sink += v;
    }
    const auto t1 = std::chrono::steady_clock::now();

    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count()
                    / static_cast<double>(kOps);
    std::printf("  try_push + try_pop           %6.2f ns/pair (budget < 20)  %s\n",
                ns, ns < 20.0 ? "OK" : "OVER");

    if (sink == 0xFFFFFFFFFFFFFFFFull) {   // never true; keeps the loop alive
        std::printf("  (unreachable)\n");
    }
}

} // namespace

int main()
{
    std::printf("altair core/lockfree spsc_ring tests\n");
    test_spsc_layout_and_traits();
    test_spsc_single_thread_fifo();
    test_spsc_fill_and_drain();
    test_spsc_wraparound();
    test_spsc_conservation();
    test_spsc_pop_leaves_out_untouched();
    test_spsc_two_thread_stress();

    report_throughput();

    std::printf("\n  NOTE: memory orderings are NOT validated by these tests.\n"
                "  x86 is TSO (acquire/release compile to plain mov) and no\n"
                "  TSAN/ASAN is available here. See the ledger's carried debt.\n");

    if (failures == 0) {
        std::printf("\nPASS\n");
    } else {
        std::printf("\nFAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
