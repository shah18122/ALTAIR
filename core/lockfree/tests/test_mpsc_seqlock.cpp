// P0-06b acceptance tests for core/lockfree/mpsc_ring.hpp and seqlock.hpp.
// Plain main() (Catch2 still unavailable).
//
// SAME CAVEAT AS P0-06a: x86 is TSO and no TSAN/ASAN runs here, so these tests
// exercise the ALGORITHMS, not the memory orderings.

#include <lockfree/mpsc_ring.hpp>
#include <lockfree/seqlock.hpp>

#include <atomic>
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

struct Big {
    std::uint64_t a, b, c, d;
};

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

    check(no_duplicates, "no value was seen twice");
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

void test_seqlock_basic_store_load()
{
    SeqlockSnapshot<Big> s;
    check(s.version() == 0, "version is zero before the first store");

    s.store(Big{1, 2, 3, 4});
    Big out{};
    check(s.try_load(out), "try_load succeeds when quiescent");
    check(out.a == 1 && out.b == 2 && out.c == 3 && out.d == 4, "all fields round-trip");
    check(s.load().a == 1, "load() returns the value");

    s.store(Big{5, 6, 7, 8});
    check(s.load().d == 8, "a second store is visible");
}

void test_seqlock_version_protocol()
{
    SeqlockSnapshot<std::uint64_t> s;
    check(s.version() == 0, "starts at zero");
    s.store(42);
    check(s.version() == 2, "advances by exactly 2");
    s.store(43);
    check(s.version() == 4, "and again");
    check(s.version() % 2 == 0, "even when quiescent");

    const auto v0 = s.version();
    s.store(44);
    check(s.version() != v0, "a reader can detect an update from the version alone");
}

void test_seqlock_concurrent_reader_never_tears()
{
    // The amount of EVIDENCE must not depend on the scheduler. Each reader
    // runs until it has accumulated a fixed number of SUCCESSFUL tear-checked
    // loads; the writer runs until both readers are satisfied. A first version
    // let the readers free-run for a fixed number of writes and asserted a
    // floor on successes — that was flaky (56k..206k depending on machine
    // load) because it asserted on a scheduling outcome rather than on the
    // seqlock. This construction yields exactly kTargetPerReader * 2 checked
    // reads on every run, on any box.
    constexpr std::uint64_t kTargetPerReader = 100'000;
    // Each reader must also observe the value CHANGE this many times, or it
    // has not raced the writer and its clean result means nothing.
    constexpr std::uint64_t kMinChanges = 1'000;
    // A cap so a genuinely broken or starved seqlock fails loudly instead of
    // hanging forever.
    constexpr std::uint64_t kMaxAttempts = 200'000'000;

    static SeqlockSnapshot<Big> s;
    std::atomic<int> readers_done{0};
    std::atomic<std::uint64_t> tears{0};
    std::atomic<std::uint64_t> successes{0};
    std::atomic<std::uint64_t> retries{0};
    std::atomic<std::uint64_t> spin_sink{0};
    std::atomic<bool> reader_gave_up{false};
    // The readers must not sample before the writer has published a valid
    // tuple. A default-constructed SeqlockSnapshot<Big> holds {0,0,0,0}, which
    // FAILS the {n,n+1,n+2,n+3} pattern — without this barrier a reader that
    // outruns the writer's first store reports 200'000 "tears" that are really
    // just the initial state. That is a false POSITIVE, the worst kind: it
    // would have had us hunting a memory-ordering bug that does not exist.
    std::atomic<bool> writer_ready{false};
    // Proof the readers sampled a MOVING value, not a frozen one.
    std::atomic<std::uint64_t> seen_min{~0ull};
    std::atomic<std::uint64_t> seen_max{0};

    std::thread writer([&] {
        std::uint64_t local_spin = 0;
        std::uint64_t n = 0;
        s.store(Big{n, n + 1, n + 2, n + 3});      // publish before anyone reads
        ++n;
        writer_ready.store(true, std::memory_order_release);

        while (readers_done.load(std::memory_order_acquire) < 2) {
            const std::uint64_t u = n++;
            // Always a CONSISTENT tuple. A torn read breaks the pattern.
            s.store(Big{u, u + 1, u + 2, u + 3});

            // A gap between writes. Without one the writer holds an odd
            // sequence for most of its ~1.6 ns cycle and readers retry
            // ~99.98% of the time. Real market data is not a 1.6 ns firehose.
            for (int k = 0; k < 200; ++k) {
                local_spin += static_cast<std::uint64_t>(k);
            }
        }
        spin_sink.store(local_spin, std::memory_order_relaxed);
    });

    auto reader = [&] {
        while (!writer_ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        std::uint64_t local_tears = 0, local_ok = 0, local_retry = 0;
        std::uint64_t lo = ~0ull, hi = 0;
        std::uint64_t changes = 0, prev = ~0ull;

        // Terminate on BOTH conditions, so the evidence requirement is part of
        // the loop rather than an assertion afterwards. Requiring only
        // kTargetPerReader successes let a descheduled writer leave the readers
        // blasting through 200'000 reads of one frozen tuple — which satisfies
        // the pattern trivially and proves nothing. Asserting on that after the
        // fact just made the test flaky (1 run in 14).
        while (local_ok < kTargetPerReader || changes < kMinChanges) {
            if (local_ok + local_retry > kMaxAttempts) {
                reader_gave_up.store(true, std::memory_order_relaxed);
                break;
            }
            Big g{};
            if (s.try_load(g)) {
                ++local_ok;
                if (g.b != g.a + 1 || g.c != g.a + 2 || g.d != g.a + 3) {
                    ++local_tears;
                }
                if (g.a != prev) { ++changes; prev = g.a; }
                if (g.a < lo) { lo = g.a; }
                if (g.a > hi) { hi = g.a; }
            } else {
                ++local_retry;
            }
        }
        tears.fetch_add(local_tears, std::memory_order_relaxed);
        successes.fetch_add(local_ok, std::memory_order_relaxed);
        retries.fetch_add(local_retry, std::memory_order_relaxed);
        if (lo < seen_min.load(std::memory_order_relaxed)) {
            seen_min.store(lo, std::memory_order_relaxed);
        }
        if (hi > seen_max.load(std::memory_order_relaxed)) {
            seen_max.store(hi, std::memory_order_relaxed);
        }
        readers_done.fetch_add(1, std::memory_order_release);
    };

    std::thread r1(reader);
    std::thread r2(reader);

    r1.join();
    r2.join();
    writer.join();

    const auto ok = successes.load();
    check(tears.load() == 0, "NO TORN READ in 200'000 checked concurrent loads");
    check(!reader_gave_up.load(), "no reader hit the attempt cap");
    // AT LEAST, not exactly. The reader loop continues while EITHER condition
    // is unmet, so a reader that has its 100'000 successes but still needs
    // value-changes keeps sampling and overshoots. "exactly" was wrong and
    // failed about 1 run in 36 under load — the loop was right, the assertion
    // was not.
    check(ok >= 2 * kTargetPerReader,
          "at least 200'000 checked reads — the evidence floor is guaranteed");
    // Without this the test could pass against a FROZEN value: 200'000 reads of
    // one never-changing tuple satisfy the pattern trivially and prove nothing.
    check(seen_max.load() > seen_min.load(),
          "the readers sampled a MOVING value, not a frozen one");

    const auto rt = retries.load();
    const double rate = (ok + rt) > 0
        ? 100.0 * static_cast<double>(rt) / static_cast<double>(ok + rt) : 0.0;
    std::printf("        %llu successful loads, %llu retries (%.3f%% retry rate)\n",
                static_cast<unsigned long long>(ok),
                static_cast<unsigned long long>(rt), rate);
    if (rt == 0) {
        std::printf("        NOTE: zero retries — the readers may never have\n"
                    "        overlapped a write. Reported, not asserted.\n");
    }
    (void)spin_sink.load(std::memory_order_relaxed);
}

namespace {

void report_throughput()
{
    std::printf("\nthroughput — batch-timed, single thread\n");
    constexpr int kOps = 1'000'000;

    {
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

    {
        SeqlockSnapshot<Big> s;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kOps; ++i) {
            const auto u = static_cast<std::uint64_t>(i);
            s.store(Big{u, u + 1, u + 2, u + 3});
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count()
                        / static_cast<double>(kOps);
        std::printf("  Seqlock<Big>::store          %6.2f ns/op   (budget < 20)  %s\n",
                    ns, ns < 20.0 ? "OK" : "OVER");

        Big g{};
        std::uint64_t sink = 0;
        const auto t2 = std::chrono::steady_clock::now();
        for (int i = 0; i < kOps; ++i) {
            (void)s.try_load(g);
            sink += g.a;
        }
        const auto t3 = std::chrono::steady_clock::now();
        const double ns2 = std::chrono::duration<double, std::nano>(t3 - t2).count()
                         / static_cast<double>(kOps);
        std::printf("  Seqlock<Big>::try_load       %6.2f ns/op   (budget < 20)  %s\n",
                    ns2, ns2 < 20.0 ? "OK" : "OVER");
        if (sink == 0xFFFFFFFFFFFFFFFFull) { std::printf(" (unreachable)\n"); }
    }
}

} // namespace

int main()
{
    std::printf("altair core/lockfree mpsc_ring + seqlock tests\n");
    test_mpsc_layout_and_traits();
    test_mpsc_single_thread_fifo();
    test_mpsc_fill_and_drain();
    test_mpsc_multi_producer_stress();
    test_seqlock_basic_store_load();
    test_seqlock_version_protocol();
    test_seqlock_concurrent_reader_never_tears();

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
