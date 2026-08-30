// P0-09b acceptance tests for feed/replay.hpp.
// Plain main() (Catch2 blocked on vcpkg — see LEDGER blocker #7).

#include <feed/replay.hpp>

#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

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

// The P0-02 anchor: 2026-08-28 09:15:00 IST, NSE market open.
constexpr altair::Timestamp kOpen{1787888700000000000LL};

altair::ReplayTick mk(std::uint64_t seq, altair::Timestamp ts, std::int64_t px)
{
    altair::ReplayTick t{};
    t.ts = ts;
    t.seqno = seq;
    t.token = 256265;                 // NIFTY 50, as a token — never a string
    t.reserved = 0;
    t.last = altair::Price{px};
    t.qty = altair::Qty{50};
    return t;
}

} // namespace

using namespace altair;

// ─────────────────────────────────────────────────────────────────────────
// The structural no-look-ahead guarantee, asserted the way P0-01 asserted
// that `Price * Lots` does not compile. Named concepts, because MSVC
// hard-errors on a bare negative requires-expression (verified in P0-01:
// 19.51.36256, C2678/C2679).
// ─────────────────────────────────────────────────────────────────────────
namespace probes {
template <typename R> concept has_peek  = requires(R r) { r.peek(); };
template <typename R> concept has_at    = requires(R r) { r.at(0); };
template <typename R> concept has_index = requires(R r) { r[0]; };
template <typename R> concept has_begin = requires(R r) { r.begin(); };
template <typename R> concept has_data  = requires(R r) { r.data(); };
} // namespace probes

static_assert(!probes::has_peek<altair::Replayer>,
              "a replayer that can peek can leak the future");
static_assert(!probes::has_at<altair::Replayer>,
              "at() would expose an undelivered tick");
static_assert(!probes::has_index<altair::Replayer>,
              "operator[] would expose an undelivered tick");
static_assert(!probes::has_begin<altair::Replayer>,
              "an iterator would expose the whole session");
static_assert(!probes::has_data<altair::Replayer>,
              "data() would hand out the raw array");

// next() must return BY VALUE — a reference would let a caller do pointer
// arithmetic and walk forward.
static_assert(std::is_same_v<
                  decltype(std::declval<altair::Replayer&>().next()),
                  std::expected<altair::ReplayTick, altair::ReplayError>>,
              "next() must return a ReplayTick by value");

void test_replay_tick_layout()
{
    check(sizeof(ReplayTick) == 40, "sizeof(ReplayTick) == 40");
    check(alignof(ReplayTick) >= 8, "alignof(ReplayTick) >= 8");
    check(std::is_trivially_copyable_v<ReplayTick>, "ReplayTick trivially copyable");
    std::printf("        sizeof(ReplayTick) = %zu\n", sizeof(ReplayTick));
}

void test_replay_sequential_delivery()
{
    const ReplayTick ticks[3] = {
        mk(1, kOpen, 100),
        mk(2, kOpen + duration::millis(1), 101),
        mk(3, kOpen + duration::millis(2), 102),
    };
    Replayer r{ticks, 3};

    check(r.size() == 3, "size == 3");
    check(r.delivered() == 0, "nothing delivered yet");
    check(r.remaining() == 3, "all remaining");
    check(!r.exhausted(), "not exhausted");

    check(r.next().value().last == Price{100}, "first tick");
    check(r.delivered() == 1 && r.remaining() == 2, "counters advanced");
    check(r.next().value().last == Price{101}, "second tick");
    check(r.delivered() == 2, "delivered == 2");
    check(r.next().value().seqno == 3, "third tick");
    check(r.exhausted(), "exhausted after three");

    check(r.next().error() == ReplayError::Exhausted, "a fourth is Exhausted");
    check(r.delivered() == 3, "a failed next() does not advance");
}

void test_replay_now_never_leads()
{
    const ReplayTick ticks[3] = {
        mk(1, kOpen, 100),
        mk(2, kOpen + duration::millis(1), 101),
        mk(3, kOpen + duration::millis(2), 102),
    };
    Replayer r{ticks, 3};

    check(r.now() == Timestamp::epoch(),
          "before the first tick now() is the epoch, NOT ticks[0].ts");

    const ReplayTick t0 = r.next().value();
    check(r.now() == t0.ts, "now() is exactly the tick just delivered");
    check(r.now() != ticks[1].ts, "and NOT the next one");

    const ReplayTick t1 = r.next().value();
    check(r.now() == t1.ts, "now() follows the second delivery");
    check(r.now() > t0.ts, "time advances only on delivery");

    const Timestamp a = r.now();
    const Timestamp b = r.now();
    check(a == b, "now() is stable between deliveries — it is not a clock");
}

void test_replay_no_lookahead_api()
{
    // The static_asserts at namespace scope do the work; they would have
    // failed to compile if any look-ahead accessor existed.
    check(true, "no peek() — a replayer that can peek can leak the future");
    check(true, "no at(), no operator[], no begin(), no data()");
    check(true, "next() returns by value, not by reference into the array");
}

void test_replay_rejects_non_monotonic()
{
    // seqno must STRICTLY increase.
    const ReplayTick dup[2] = { mk(1, kOpen, 100), mk(1, kOpen, 101) };
    Replayer a{dup, 2};
    check(a.next().has_value(), "first delivers");
    check(a.next().error() == ReplayError::NotMonotonic, "a repeated seqno stops it");
    check(a.delivered() == 1, "and does not advance the cursor");

    // Timestamps MAY repeat — two trades can share a nanosecond.
    const ReplayTick same_ts[2] = { mk(1, kOpen, 100), mk(2, kOpen, 101) };
    Replayer b{same_ts, 2};
    check(b.next().has_value() && b.next().has_value(),
          "an identical timestamp on a new seqno is fine");

    // But time must not go backwards.
    const ReplayTick back[2] = {
        mk(1, kOpen, 100),
        mk(2, kOpen - duration::millis(1), 101),
    };
    Replayer c{back, 2};
    check(c.next().has_value(), "first delivers");
    check(c.next().error() == ReplayError::NotMonotonic, "time going back stops it");
    check(c.delivered() == 1, "and does not advance");
}

void test_replay_validate()
{
    const ReplayTick good[3] = {
        mk(1, kOpen, 100),
        mk(2, kOpen + duration::millis(1), 101),
        mk(3, kOpen + duration::millis(2), 102),
    };
    Replayer r{good, 3};
    check(r.validate().has_value(), "a well-formed session validates");
    check(r.delivered() == 0, "validate() does not move the cursor");

    Replayer e;
    check(e.validate().error() == ReplayError::Empty, "an empty replayer is Empty");

    const ReplayTick outoforder[3] = {
        mk(1, kOpen, 100),
        mk(3, kOpen + duration::millis(1), 101),
        mk(2, kOpen + duration::millis(2), 102),
    };
    Replayer o{outoforder, 3};
    check(o.validate().error() == ReplayError::NotMonotonic, "seqno 1,3,2 is rejected");

    const ReplayTick backwards[2] = {
        mk(1, kOpen, 100),
        mk(2, kOpen - duration::millis(1), 101),
    };
    Replayer b{backwards, 2};
    check(b.validate().error() == ReplayError::NotMonotonic,
          "a backwards timestamp is rejected");
}

void test_replay_exhaustion_and_rewind()
{
    const ReplayTick ticks[3] = {
        mk(1, kOpen, 100),
        mk(2, kOpen + duration::millis(1), 101),
        mk(3, kOpen + duration::millis(2), 102),
    };
    Replayer r{ticks, 3};
    while (r.next().has_value()) { }
    check(r.exhausted() && r.remaining() == 0, "drained");

    r.rewind();
    check(r.delivered() == 0, "rewind resets the cursor");
    check(r.remaining() == 3 && !r.exhausted(), "and the counters");
    check(r.now() == Timestamp::epoch(), "rewind resets the clock too");
    check(r.next().value().seqno == 1, "the sequence restarts");

    r.rewind();
    int n = 0;
    bool same = true;
    while (auto t = r.next()) {
        ++n;
        if (t->seqno != static_cast<std::uint64_t>(n)) { same = false; }
    }
    check(n == 3 && same, "a second pass delivers exactly the same ticks");

    Replayer e;
    check(e.size() == 0 && e.exhausted(), "a default replayer is empty");
    check(e.next().error() == ReplayError::Empty, "and reports Empty, NOT Exhausted");
    check(e.now() == Timestamp::epoch(), "its clock is the epoch");

    Replayer z{nullptr, 5};
    check(z.next().error() == ReplayError::Empty, "a null pointer is Empty too");
    check(z.size() == 0, "and its size is clamped to 0");
}

void test_replay_drives_a_null_strategy()
{
    // The Phase 0 exit criterion in miniature: a session runs end to end
    // through a null strategy with the conservation ledger armed, and the
    // invariant holds at every tick.
    constexpr std::size_t kN = 1000;
    static ReplayTick session[kN];
    for (std::size_t i = 0; i < kN; ++i) {
        const std::int64_t px = 2500000 + static_cast<std::int64_t>(i % 7) * 100;
        session[i] = mk(static_cast<std::uint64_t>(i + 1),
                        kOpen + duration::millis(static_cast<std::int64_t>(i)), px);
    }

    ConservationLedger ledger{Notional{100'000'000}};
    Replayer r{session, kN};
    check(r.validate().has_value(), "the session validates");

    bool invariant_held = true;
    bool clock_is_the_tick = true;
    std::uint64_t fills = 0;

    const auto t0 = std::chrono::steady_clock::now();
    while (auto t = r.next()) {
        // The null strategy: reads the tick, trades occasionally, and reads
        // time ONLY from the replayer.
        if (t->seqno % 100 == 0) {
            if (ledger.on_fill(Qty{1}, t->last, Notional{10}).has_value()) {
                ++fills;
            }
        }
        if (!ledger.check_and_trip().has_value()) { invariant_held = false; }
        if (r.now() != t->ts) { clock_is_the_tick = false; }
    }
    const auto t1 = std::chrono::steady_clock::now();

    check(r.delivered() == kN, "every tick was delivered");
    check(invariant_held, "the conservation invariant held on EVERY tick");
    check(clock_is_the_tick, "the strategy's clock was the tick, never a wall clock");
    check(!ledger.is_breached(), "no breach latched");
    check(ledger.check().has_value(), "the books balance at the end");
    check(fills == 10, "the null strategy filled 10 times");
    check(ledger.fill_count() == 10, "and the ledger agrees");

    const double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("        %zu ticks replayed with invariants armed in %.4f s"
                " = %.2f M ticks/s\n",
                kN, sec, static_cast<double>(kN) / sec / 1e6);
}

namespace {

void report_throughput()
{
    std::printf("\nthroughput — batch-timed, single thread\n");
    constexpr std::size_t kN = 100'000;
    static ReplayTick big[kN];
    for (std::size_t i = 0; i < kN; ++i) {
        big[i] = mk(static_cast<std::uint64_t>(i + 1),
                    kOpen + duration::micros(static_cast<std::int64_t>(i)),
                    2500000);
    }

    Replayer r{big, kN};
    std::uint64_t sink = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int pass = 0; pass < 10; ++pass) {
        r.rewind();
        while (auto t = r.next()) {
            sink += t->seqno;
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count()
                    / static_cast<double>(kN * 10);
    std::printf("  Replayer::next()              %6.2f ns/call (budget < 10)  %s\n",
                ns, ns < 10.0 ? "OK" : "OVER");

    if (sink == 0xFFFFFFFFFFFFFFFFull) { std::printf("  (unreachable)\n"); }
}

} // namespace

int main()
{
    std::printf("altair feed/replay tests\n");
    test_replay_tick_layout();
    test_replay_sequential_delivery();
    test_replay_now_never_leads();
    test_replay_no_lookahead_api();
    test_replay_rejects_non_monotonic();
    test_replay_validate();
    test_replay_exhaustion_and_rewind();
    test_replay_drives_a_null_strategy();

    report_throughput();

    if (failures == 0) {
        std::printf("\nPASS\n");
    } else {
        std::printf("\nFAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
