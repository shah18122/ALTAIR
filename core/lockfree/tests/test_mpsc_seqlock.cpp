// P0-06b acceptance tests for core/lockfree/mpsc_ring.hpp.
// Plain main() (Catch2 still unavailable).
//
// SAME CAVEAT AS P0-06a: x86 is TSO and no TSAN/ASAN runs here, so these tests
// exercise the algorithm, not the memory ordering.

#include <lockfree/mpsc_ring.hpp>

#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

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

void test_mpsc_layout_and_traits()
{
    using R = MpscRing<std::uint64_t, 8>;

    check(R::capacity() == 8, "capacity() == 8");
    check(alignof(R) >= kCacheLine, "ring is cache-line aligned");
    check(!std::is_copy_constructible_v<R>, "not copy constructible");
    check(!std::is_move_constructible_v<R>, "not move constructible");

    R r;
    check(r.pushed_approx() == 0, "pushed_approx starts at 0");
    check(r.popped() == 0, "popped starts at 0");

    std::printf("        sizeof(MpscRing<u64,8>) = %zu, alignof = %zu\n",
                sizeof(R), alignof(R));
}

void test_mpsc_single_thread_fifo()
{
    MpscRing<int, 4> r;
    check(r.try_push(1) && r.try_push(2) && r.try_push(3), "three pushes");

    int v = 0;
    check(r.try_pop(v) && v == 1, "FIFO first");
    check(r.try_pop(v) && v == 2, "FIFO second");
    check(r.try_pop(v) && v == 3, "FIFO third");
    check(!r.try_pop(v), "empty rejects");
}

void test_mpsc_fill_and_drain()
{
    MpscRing<int, 4> r;

    check(r.try_push(10) && r.try_push(20) && r.try_push(30) && r.try_push(40),
          "all 4 slots usable");
    check(!r.try_push(50), "full rejects");

    int v = 0;
    check(r.try_pop(v) && v == 10, "oldest survived");
    check(r.try_push(50), "slot re-armed for the next lap");
    check(r.pushed_approx() == 5 && r.popped() == 1, "counters");

    check(r.try_pop(v) && v == 20, "drain 20");
    check(r.try_pop(v) && v == 30, "drain 30");
    check(r.try_pop(v) && v == 40, "drain 40");
    check(r.try_pop(v) && v == 50, "the re-armed slot delivers across the wrap");
    check(!r.try_pop(v), "empty after full drain");
}

void test_mpsc_multi_producer_stress()
{
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 250'000;
    constexpr int kTotal = kProducers * kPerProducer;

    static MpscRing<std::uint64_t, 1024> r;

    const auto t0 = std::chrono::steady_clock::now();

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([p] {
            for (int i = 0; i < kPerProducer; ++i) {
                // Unique across the run, and the producer is recoverable.
                const auto v = static_cast<std::uint64_t>(p) * 1'000'000ull
                             + static_cast<std::uint64_t>(i);
                while (!r.try_push(v)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    int seen_count[kProducers] = {0, 0, 0, 0};
    std::int64_t last_from[kProducers] = {-1, -1, -1, -1};
    bool per_producer_ordered = true;
    std::vector<bool> seen(static_cast<std::size_t>(kProducers) * 1'000'000ull, false);
    bool no_duplicates = true;

    std::uint64_t v = 0;
    for (int n = 0; n < kTotal; ++n) {
        while (!r.try_pop(v)) {
            std::this_thread::yield();
        }
        const auto p = static_cast<std::size_t>(v / 1'000'000ull);
        const auto i = static_cast<std::int64_t>(v % 1'000'000ull);

        if (p >= static_cast<std::size_t>(kProducers) || i >= kPerProducer) {
            no_duplicates = false;
            per_producer_ordered = false;
            continue;
        }
        if (seen[static_cast<std::size_t>(v)]) { no_duplicates = false; }
        seen[static_cast<std::size_t>(v)] = true;

        // An MPSC ring does NOT promise a global order across producers, but it
        // must never reorder one producer's own pushes.
        if (i <= last_from[p]) { per_producer_ordered = false; }
        last_from[p] = i;
        ++seen_count[p];
    }

    for (auto& t : producers) { t.join(); }
    const auto t1 = std::chrono::steady_clock::now();

    check(no_duplicates, "no value was duplicated or outside its producer range");
    check(per_producer_ordered, "each producer's own values arrived in order");

    bool counts_ok = true;
    for (int p = 0; p < kProducers; ++p) {
        if (seen_count[p] != kPerProducer) { counts_ok = false; }
    }
    check(counts_ok, "each of the 4 producers contributed exactly 250'000");
    check(r.popped() == static_cast<std::uint64_t>(kTotal), "popped == 1e6");
    check(!r.try_pop(v), "ring drained");

    const double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("        %d elements, 4 producers -> 1 consumer, %.3f s = %.1f M elem/s\n",
                kTotal, sec, static_cast<double>(kTotal) / sec / 1e6);
}

void report_throughput()
{
    std::printf("\nthroughput — batch-timed, single thread\n");
    constexpr int kOps = 1'000'000;

    MpscRing<std::uint64_t, 1024> r;
    std::uint64_t sink = 0, v = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i) {
        (void)r.try_push(static_cast<std::uint64_t>(i));
        (void)r.try_pop(v);
        sink += v;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count()
                    / static_cast<double>(kOps);
    std::printf("  MpscRing push + pop          %6.2f ns/pair (budget < 40)  %s\n",
                ns, ns < 40.0 ? "OK" : "OVER");
    if (sink == 0xFFFFFFFFFFFFFFFFull) { std::printf(" (unreachable)\n"); }
}

int main()
{
    std::printf("altair core/lockfree mpsc_ring tests\n");
    test_mpsc_layout_and_traits();
    test_mpsc_single_thread_fifo();
    test_mpsc_fill_and_drain();
    test_mpsc_multi_producer_stress();

    report_throughput();

    std::printf("\n  NOTE: memory orderings are NOT validated by these tests.\n"
                "  x86 is TSO and no TSAN/ASAN is available here.\n");

    if (failures == 0) {
        std::printf("\nPASS\n");
    } else {
        std::printf("\nFAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
