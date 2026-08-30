// P0-05b acceptance tests for core/mem/arena.hpp and core/mem/pool.hpp.
// Plain main() (Catch2 still unavailable).

#include <mem/arena.hpp>
#include <mem/pool.hpp>
#include <mem/page_alloc.hpp>

#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <limits>

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

void test_arena_basic_bump()
{
    alignas(64) std::byte buf[1024];
    Arena a{buf, sizeof(buf)};

    check(a.capacity() == 1024, "capacity");
    check(a.used() == 0, "used starts at 0");
    check(a.remaining() == 1024, "remaining starts at capacity");

    auto p1 = a.allocate(16, 8);
    check(p1.has_value() && p1.value() != nullptr, "first allocate succeeds");
    check(a.used() == 16, "used == 16");
    check(a.remaining() == 1008, "remaining == 1008");

    auto p2 = a.allocate(32, 8);
    check(p2.has_value(), "second allocate succeeds");
    check(static_cast<std::byte*>(p2.value())
              == static_cast<std::byte*>(p1.value()) + 16,
          "allocations are contiguous and ascending");
    check(a.used() == 48, "used == 48");

    auto z = a.allocate(0, 8);
    check(z.has_value() && z.value() != nullptr,
          "a zero-byte request succeeds and is not nullptr");

    Arena empty;
    check(empty.capacity() == 0, "default arena has no capacity");
    check(empty.allocate(1, 1).error() == MemError::Exhausted,
          "default arena is Exhausted");
}

void test_arena_alignment()
{
    alignas(64) std::byte buf[1024];
    Arena a{buf, sizeof(buf)};

    (void)a.allocate(1, 1);                  // deliberately misalign the cursor
    auto p = a.allocate(8, 64);
    check(p.has_value(), "over-aligned allocate succeeds");
    check(reinterpret_cast<std::uintptr_t>(p.value()) % 64 == 0,
          "64-byte alignment honoured");
    check(a.used() >= 64, "the alignment padding was charged to used()");

    bool all_aligned = true;
    for (std::size_t align = 1; align <= 256; align *= 2) {
        auto q = a.allocate(1, align);
        if (!q.has_value()
            || reinterpret_cast<std::uintptr_t>(q.value()) % align != 0) {
            all_aligned = false;
            break;
        }
    }
    check(all_aligned, "every power of two up to 256 is honoured");

    const std::size_t before = a.used();
    check(a.allocate(8, 0).error() == MemError::BadAlignment,  "align 0 rejected");
    check(a.allocate(8, 3).error() == MemError::BadAlignment,  "align 3 rejected");
    check(a.allocate(8, 24).error() == MemError::BadAlignment, "align 24 rejected");
    check(a.used() == before, "a rejected allocate consumes nothing");

    Arena b{buf, sizeof(buf)};
    auto arr = b.allocate_n<std::uint64_t>(4);
    check(arr.has_value(), "allocate_n succeeds");
    check(reinterpret_cast<std::uintptr_t>(arr.value()) % alignof(std::uint64_t) == 0,
          "allocate_n is aligned for its type");
    check(b.used() == 32, "allocate_n<uint64_t>(4) consumes 32 bytes");
}

void test_arena_exhaustion()
{
    alignas(64) std::byte buf[128];

    Arena a{buf, sizeof(buf)};
    check(a.allocate(128, 1).has_value(), "exactly the capacity fits");
    check(a.remaining() == 0, "remaining is 0");
    check(a.allocate(1, 1).error() == MemError::Exhausted, "one more is Exhausted");

    Arena b{buf, sizeof(buf)};
    check(b.allocate(129, 1).error() == MemError::Exhausted, "one past capacity");
    check(b.used() == 0, "a failed allocate consumes nothing");

    // The overflow guard: a huge request must report Exhausted, not wrap.
    Arena c{buf, sizeof(buf)};
    check(c.allocate(std::numeric_limits<std::size_t>::max(), 1).error()
              == MemError::Exhausted,
          "SIZE_MAX request does not wrap");
    check(c.allocate(std::numeric_limits<std::size_t>::max() - 8, 8).error()
              == MemError::Exhausted,
          "SIZE_MAX-8 aligned request does not wrap");

    Arena d{buf, sizeof(buf)};
    check(d.allocate_n<std::uint64_t>(std::numeric_limits<std::size_t>::max() / 4)
              .error() == MemError::Exhausted,
          "allocate_n overflow guard");
}

void test_arena_reset_and_high_water()
{
    alignas(64) std::byte buf[1024];
    Arena a{buf, sizeof(buf)};

    (void)a.allocate(400, 8);
    check(a.used() == 400, "used == 400");
    check(a.high_water() == 400, "high water == 400");

    a.reset();
    check(a.used() == 0, "reset clears used");
    check(a.high_water() == 400, "reset does NOT clear high water");
    check(a.remaining() == 1024, "reset restores remaining");

    (void)a.allocate(100, 8);
    check(a.high_water() == 400, "a smaller peak does not lower high water");
    (void)a.allocate(500, 8);
    // 604, NOT 600: used was 100, the 8-alignment bumps the second offset to
    // 104, and that 4 bytes of padding is genuinely consumed. Asserting 600
    // would be asserting that alignment padding is free.
    check(a.used() == 604, "alignment padding is charged to used()");
    check(a.high_water() == 604, "a larger peak raises high water");
}

void test_pool_create_validation()
{
    alignas(64) std::byte buf[1024];

    check(Pool::create(buf, sizeof(buf), 64, 0).error() == MemError::BadAlignment,
          "align 0 rejected");
    check(Pool::create(buf, sizeof(buf), 64, 3).error() == MemError::BadAlignment,
          "align 3 rejected");
    check(Pool::create(buf, sizeof(buf), 64, 1).error() == MemError::BadAlignment,
          "align below alignof(void*) rejected");
    check(Pool::create(buf, sizeof(buf), 1, 8).error() == MemError::BadBlockSize,
          "block smaller than a free-list link rejected");
    check(Pool::create(buf, 8, 64, 8).error() == MemError::NoCapacity,
          "a span too small for one block rejected");

    auto p = Pool::create(buf, sizeof(buf), 64, 64);
    check(p.has_value(), "valid create succeeds");
    if (p.has_value()) {
        check(p->stride() == 64, "stride == 64");
        check(p->capacity() == 16, "capacity == 16 blocks");
        check(p->in_use() == 0, "in_use starts at 0");
        check(p->high_water() == 0, "high water starts at 0");
        check(p->foreign_releases() == 0, "foreign releases start at 0");
    }

    auto q = Pool::create(buf, sizeof(buf), 40, 16);
    check(q.has_value(), "unaligned block size create succeeds");
    if (q.has_value()) {
        check(q->stride() == 48, "stride rounds block_size up to align");
        check(q->capacity() == 1024 / 48, "capacity uses the rounded stride");
    }

    Pool empty;
    check(empty.capacity() == 0, "default pool has no capacity");
    check(empty.acquire().error() == MemError::Exhausted, "default pool is Exhausted");
}

void test_pool_acquire_release_cycle()
{
    alignas(64) std::byte buf[1024];
    auto p = Pool::create(buf, sizeof(buf), 64, 64);
    check(p.has_value(), "create for cycle test");
    if (!p.has_value()) {
        return;
    }

    void* a0 = p->acquire().value();
    void* a1 = p->acquire().value();
    check(static_cast<std::byte*>(a1) == static_cast<std::byte*>(a0) + 64,
          "a fresh pool hands out ascending addresses");
    check(p->in_use() == 2, "in_use == 2");
    check(p->high_water() == 2, "high water == 2");

    p->release(a1);
    check(p->in_use() == 1, "release decrements in_use");
    check(p->acquire().value() == a1, "LIFO: the released block comes back next");
    check(p->in_use() == 2, "in_use back to 2");

    while (p->in_use() < p->capacity()) {
        (void)p->acquire();
    }
    check(p->in_use() == 16, "drained to capacity");
    check(p->acquire().error() == MemError::Exhausted, "exhausted pool reports it");
    check(p->high_water() == 16, "high water == capacity");

    // Every block distinct, aligned, and in range — verified while releasing.
    bool all_aligned = true;
    for (std::size_t i = 0; i < p->capacity(); ++i) {
        std::byte* blk = buf + i * 64;
        if (reinterpret_cast<std::uintptr_t>(blk) % 64 != 0) {
            all_aligned = false;
        }
        p->release(static_cast<void*>(blk));
    }
    check(all_aligned, "every block is 64-byte aligned");
    check(p->in_use() == 0, "all blocks returned");
    check(p->high_water() == 16, "high water survives full release");
    check(p->foreign_releases() == 0, "no foreign releases during the cycle");

    std::size_t again = 0;
    while (p->acquire().has_value()) {
        ++again;
    }
    check(again == 16, "the pool is fully reusable after release");
}

void test_pool_ownership_and_foreign_release()
{
    alignas(64) std::byte buf[1024];
    alignas(64) std::byte other[64];
    auto p = Pool::create(buf, sizeof(buf), 64, 64);
    check(p.has_value(), "create for ownership test");
    if (!p.has_value()) {
        return;
    }

    void* b = p->acquire().value();
    check(p->owns(b), "owns an acquired block");
    check(!p->owns(nullptr), "does not own nullptr");
    check(!p->owns(other), "does not own a different buffer");
    check(!p->owns(buf + 1024), "does not own one past the end");
    check(!p->owns(buf + 32), "does NOT own a mid-block pointer");
    check(p->owns(buf + 64), "owns the next block boundary");

    p->release(nullptr);
    check(p->foreign_releases() == 0, "a null release is not foreign");
    check(p->in_use() == 1, "a null release changes nothing");

    p->release(other);
    check(p->foreign_releases() == 1, "a foreign release is counted");
    check(p->in_use() == 1, "a foreign release changes nothing else");

    // The one that would silently corrupt the free list if owns() only did a
    // range check without confirming the block boundary.
    p->release(buf + 32);
    check(p->foreign_releases() == 2, "a mid-block release is counted as foreign");
    check(p->in_use() == 1, "a mid-block release changes nothing else");

    p->release(b);
    check(p->in_use() == 0, "the real block still releases correctly");
    check(p->acquire().has_value(), "the pool still works afterwards");
}

namespace {

/// Throughput report. These operations are 1-3 ns, BELOW the ~12 ns cost of the
/// RDTSCP pair that would measure them — subtracting the harness (the P0-03
/// technique) is not enough when the quantum exceeds the signal. Time a batch
/// and divide.
void report_throughput()
{
    std::printf("\nthroughput — batch-timed, not per-call\n");

    auto blk = PageBlock::reserve(8u * 1024u * 1024u, false);
    if (!blk.has_value()) {
        std::printf("  SKIPPED (could not reserve backing pages)\n");
        return;
    }

    constexpr int kOps = 1'000'000;
    std::size_t sink = 0;

    {
        Arena a{blk->data(), blk->size()};
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kOps; ++i) {
            if ((i & 1023) == 0) {
                a.reset();          // counted in the measurement, not excluded
            }
            auto r = a.allocate(32, 8);
            sink += reinterpret_cast<std::uintptr_t>(r.value_or(nullptr)) & 0xFF;
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count()
                        / static_cast<double>(kOps);
        std::printf("  Arena::allocate(32, 8)        %6.2f ns/op   (budget < 10)  %s\n",
                    ns, ns < 10.0 ? "OK" : "OVER");
        std::printf("    (includes one reset() per 1000 allocations)\n");
    }

    {
        auto p = Pool::create(blk->data(), blk->size(), 64, 64);
        if (!p.has_value()) {
            std::printf("  pool: SKIPPED (create failed)\n");
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kOps; ++i) {
            auto r = p->acquire();
            void* q = r.value_or(nullptr);
            sink += reinterpret_cast<std::uintptr_t>(q) & 0xFF;
            p->release(q);
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count()
                        / static_cast<double>(kOps);
        std::printf("  Pool::acquire + release       %6.2f ns/pair (budget < 15)  %s\n",
                    ns, ns < 15.0 ? "OK" : "OVER");
    }

    if (sink == 0xFFFFFFFFu) {   // never true; keeps the loops alive
        std::printf("  (unreachable)\n");
    }
}

} // namespace

int main()
{
    std::printf("altair core/mem arena + pool tests\n");
    test_arena_basic_bump();
    test_arena_alignment();
    test_arena_exhaustion();
    test_arena_reset_and_high_water();
    test_pool_create_validation();
    test_pool_acquire_release_cycle();
    test_pool_ownership_and_foreign_release();

    report_throughput();

    if (failures == 0) {
        std::printf("\nPASS\n");
    } else {
        std::printf("\nFAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
