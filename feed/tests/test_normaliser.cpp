// P2-04 acceptance tests for feed/normaliser.hpp.
//
// No check description here may contain the substring FAIL.

#include <feed/normaliser.hpp>

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <new>

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

namespace {

constexpr Timestamp kNow{1787888700000000000LL};      // 2026-08-28 09:15 IST
constexpr Timestamp kFloor{1787000000000000000LL};

Normaliser::Config base_cfg()
{
    Normaliser::Config c{};
    c.gate.floor = kFloor;
    c.gate.max_future_skew = duration::seconds(2);
    c.gate.require_monotonic = false;
    c.gate.enforce_session = false;
    c.active = FeedSource::Kite;
    return c;
}

Tick mk(std::uint32_t id, Timestamp ts, FeedSource src = FeedSource::Kite)
{
    Tick t{};
    t.id = static_cast<InstrumentId>(id);
    t.exchange_ts = ts;
    t.recv_ts = kNow;
    t.last = Price{2'500'000};
    t.last_qty = Qty{75};
    t.source = src;
    return t;
}

// 72 KB -- comfortably stack-safe, but file scope keeps the placement-new
// reset below well-defined and avoids re-zeroing 8192 slots per test.
Normaliser g_n{base_cfg()};

void fresh(Normaliser::Config c = base_cfg())
{
    g_n.~Normaliser();
    new (&g_n) Normaliser(c);
}

// ── 1 ────────────────────────────────────────────────────────────────────
// The trap this whole card is built around.
void monotonic_is_forced_off()
{
    std::printf("\n1 monotonic_is_forced_off\n");

    Normaliser::Config c = base_cfg();
    c.gate.require_monotonic = true;          // deliberately wrong
    fresh(c);

    check(!g_n.gate().config().require_monotonic,
          "the constructor FORCES require_monotonic off, whatever was passed");

    // The case a shared monotonic gate would have destroyed: two instruments
    // interleaved, the second reporting an EARLIER instant than the first.
    // That is completely ordinary -- they tick independently.
    Tick a = mk(1, kNow + duration::seconds(5));
    Tick b = mk(2, kNow + duration::seconds(4));
    check(g_n.submit(a, kNow + duration::seconds(6)) == Verdictum::Publish,
          "instrument 1 at +5s publishes");
    check(g_n.submit(b, kNow + duration::seconds(6)) == Verdictum::Publish,
          "instrument 2 at +4s ALSO publishes -- a shared monotonic gate would "
          "have rejected it and looked like a broken feed");
    check(g_n.stats().published == 2, "both published");
    check(g_n.stats().out_of_order == 0,
          "and neither is out of order: they are different instruments");
    check(!has_flag(b.flags, TickFlag::Stale), "nothing is flagged stale");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void future_ticks_are_dropped()
{
    std::printf("\n2 future_ticks_are_dropped\n");
    fresh();

    Tick ok = mk(1, kNow);
    check(g_n.submit(ok, kNow) == Verdictum::Publish, "a present tick publishes");
    const std::uint32_t seq_after_ok = g_n.next_seq();

    // Beyond local_now + max_future_skew. Rule 7 is absolute.
    Tick ahead = mk(1, kNow + duration::seconds(60));
    const Verdictum v = g_n.submit(ahead, kNow);
    check(v == Verdictum::DropFuture,
          "a tick from the future is DROPPED, not flagged -- rule 7 has no "
          "flag-and-continue option");
    check(g_n.stats().dropped_future == 1, "and counted");
    check(g_n.next_seq() == seq_after_ok,
          "it consumed NO sequence number: a dropped tick must not leave a gap "
          "that looks like a lost one");
    check(g_n.stats().published == 1, "still only one published");

    // Just inside the skew is fine.
    Tick edge = mk(1, kNow + duration::seconds(1));
    check(g_n.submit(edge, kNow) == Verdictum::Publish,
          "one second ahead is within the 2 s skew tolerance and publishes");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void floor_breach_flags_but_publishes()
{
    std::printf("\n3 floor_breach_flags_but_publishes\n");
    fresh();

    Tick old = mk(1, kFloor - duration::seconds(3600));
    const Verdictum v = g_n.submit(old, kNow);
    check(v == Verdictum::Publish,
          "a tick before the plausible floor still PUBLISHES");
    check(has_flag(old.flags, TickFlag::Stale), "with Stale set");
    check(g_n.stats().flagged_stale == 1, "and counted");
    check(old.last.raw() == 2'500'000 && old.last_qty.raw() == 75,
          "and its data is intact -- a trade that happened still happened, and "
          "discarding it would put a hole in the volume series to hide a "
          "clock problem");
    check(g_n.stats().published == 1, "it is in the published count");
}

// ── 4 ────────────────────────────────────────────────────────────────────
// The direct consequence of P2-02's finding: Kite's exchange_ts is
// one-second resolution, so many ticks legitimately share a second.
void equal_timestamps_are_normal()
{
    std::printf("\n4 equal_timestamps_are_normal\n");
    fresh();

    const Timestamp same = kNow;
    Tick a = mk(1, same);
    Tick b = mk(1, same);
    Tick c = mk(1, same);
    check(g_n.submit(a, kNow) == Verdictum::Publish, "first at T publishes");
    check(g_n.submit(b, kNow) == Verdictum::Publish, "second at the SAME T too");
    check(g_n.submit(c, kNow) == Verdictum::Publish, "and the third");
    check(g_n.stats().out_of_order == 0,
          "none is out of order -- equal is normal at one-second resolution, "
          "and a strict-monotonic check would have rejected two of three");
    check(!has_flag(b.flags, TickFlag::Stale)
              && !has_flag(c.flags, TickFlag::Stale),
          "and none is flagged stale");
    check(a.seq == 0 && b.seq == 1 && c.seq == 2,
          "each still gets its own sequence number");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void regression_flags_but_publishes()
{
    std::printf("\n5 regression_flags_but_publishes\n");
    fresh();

    Tick a = mk(1, kNow + duration::seconds(10));
    Tick b = mk(1, kNow + duration::seconds(9));       // strictly earlier, SAME instrument
    check(g_n.submit(a, kNow + duration::seconds(11)) == Verdictum::Publish, "first publishes");
    const Verdictum v = g_n.submit(b, kNow + duration::seconds(11));
    check(v == Verdictum::Publish, "the regressed tick also publishes");
    check(has_flag(b.flags, TickFlag::Stale), "but is flagged Stale");
    check(g_n.stats().out_of_order == 1, "and counted as out of order");
    check(!has_flag(a.flags, TickFlag::Stale), "the first is untouched");

    // The high-water mark must not move backwards, or every subsequent tick
    // between the two would be reported out of order as well.
    Tick c = mk(1, kNow + duration::seconds(10));
    check(g_n.submit(c, kNow + duration::seconds(11)) == Verdictum::Publish, "and the next");
    check(g_n.stats().out_of_order == 1,
          "a tick at the previous high-water mark is NOT a second regression: "
          "the mark did not move backwards");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void inactive_source_is_not_published()
{
    std::printf("\n6 inactive_source_is_not_published\n");
    fresh();

    Tick standby = mk(1, kNow + duration::seconds(20), FeedSource::Xts);
    const Verdictum v = g_n.submit(standby, kNow + duration::seconds(21));
    check(v == Verdictum::DropInactive, "a standby-source tick is DropInactive");
    check(g_n.stats().dropped_inactive == 1, "and counted");
    check(g_n.next_seq() == 0, "it consumes no sequence number");
    check(g_n.stats().published == 0, "and nothing was published");

    // It must not have disturbed per-instrument history either, or the standby
    // feed running ahead would make every active tick look regressed.
    Tick active = mk(1, kNow + duration::seconds(10), FeedSource::Kite);
    check(g_n.submit(active, kNow + duration::seconds(21)) == Verdictum::Publish,
          "an active tick EARLIER than the standby one still publishes");
    check(g_n.stats().out_of_order == 0,
          "and is not out of order: the standby feed did not move the "
          "instrument's high-water mark");
}

// ── 7 ────────────────────────────────────────────────────────────────────
void seq_survives_a_failover()
{
    std::printf("\n7 seq_survives_a_failover\n");
    fresh();

    std::uint32_t seen[6]{};
    for (int i = 0; i < 3; ++i) {
        Tick t = mk(1, kNow + duration::seconds(i), FeedSource::Kite);
        (void)g_n.submit(t, kNow + duration::seconds(10));
        seen[i] = t.seq;
    }

    g_n.set_active(FeedSource::Xts);
    check(g_n.active() == FeedSource::Xts, "the primary switched");

    for (int i = 0; i < 3; ++i) {
        Tick t = mk(1, kNow + duration::seconds(3 + i), FeedSource::Xts);
        (void)g_n.submit(t, kNow + duration::seconds(10));
        seen[3 + i] = t.seq;
    }

    check(g_n.stats().published == 6, "six published across the switch");
    bool contiguous = true;
    for (std::uint32_t i = 0; i < 6; ++i) {
        if (seen[i] != i) {
            contiguous = false;
        }
    }
    check(contiguous,
          "and the sequence is 0..5 -- continuous and monotonic ACROSS the "
          "failover, with no repeat and no gap, because seq never came from "
          "the feed");
    check(g_n.stats().out_of_order == 0,
          "history carried across the switch: the instrument's timeline is "
          "continuous even when the feed reporting it changes");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void depth_shares_the_path()
{
    std::printf("\n8 depth_shares_the_path\n");
    fresh();

    DepthUpdate d{};
    d.id = static_cast<InstrumentId>(1);
    d.exchange_ts = kNow;
    d.recv_ts = kNow;
    d.source = FeedSource::Kite;
    d.bid[0] = DepthLevel{Price{2'500'000}, Qty{100}, 3, 0};
    d.ask[0] = DepthLevel{Price{2'500'500}, Qty{150}, 4, 0};
    d.bid_levels = 1;
    d.ask_levels = 1;

    check(g_n.submit(d, kNow) == Verdictum::Publish, "a depth update publishes");
    check(d.seq == 0, "and takes sequence 0");

    Tick t = mk(1, kNow);
    check(g_n.submit(t, kNow) == Verdictum::Publish, "a tick follows");
    check(t.seq == 1,
          "taking sequence 1 -- ticks and depth SHARE one counter, so the two "
          "streams interleave deterministically on replay");

    DepthUpdate ahead = d;
    ahead.exchange_ts = kNow + duration::seconds(60);
    check(g_n.submit(ahead, kNow) == Verdictum::DropFuture,
          "a future depth update drops on the same guard");

    DepthUpdate old = d;
    old.exchange_ts = kFloor - duration::seconds(3600);
    old.flags = 0;
    check(g_n.submit(old, kNow) == Verdictum::Publish, "an ancient one publishes");
    check(has_flag(old.flags, TickFlag::Stale), "flagged Stale, same as a tick");
    check(mid(old).has_value() && mid(old)->raw() == 2'500'250,
          "and the book is still intact");
}

// ── session reset + backpressure ─────────────────────────────────────────
void session_and_backpressure()
{
    std::printf("\nsession reset and backpressure\n");
    fresh();

    Tick t = mk(1, kNow);
    (void)g_n.submit(t, kNow);
    g_n.note_dropped_full();
    g_n.note_dropped_full();
    check(g_n.stats().dropped_ring_full == 2,
          "the caller's ring-full reports are counted -- backpressure means a "
          "consumer is too slow, which is information");

    g_n.reset_session();
    check(g_n.next_seq() == 0, "reset_session rewinds the sequence");
    check(g_n.stats().published == 0 && g_n.stats().dropped_ring_full == 0,
          "and clears the stats");

    Tick again = mk(1, kNow - duration::seconds(5));
    check(g_n.submit(again, kNow) == Verdictum::Publish, "a new session starts clean");
    check(g_n.stats().out_of_order == 0,
          "with no history carried over from the previous one");
}

// ── gate 6: the latency budget ───────────────────────────────────────────
void benchmark()
{
    std::printf("\nnormalise latency -- ALTAIR_HOT\n");
    fresh();

    static Tick batch[256];
    for (std::size_t i = 0; i < 256; ++i) {
        batch[i] = mk(static_cast<std::uint32_t>(i % 64), kNow);
    }

    // Batch-timed. A single submit is far below clock resolution, and
    // bracketing each call would measure the harness (the P0-03 lesson).
    constexpr int kIters = 20'000;
    const auto t0 = std::chrono::steady_clock::now();
    std::size_t sink = 0;
    for (int it = 0; it < kIters; ++it) {
        for (std::size_t i = 0; i < 256; ++i) {
            Tick t = batch[i];
            sink += (g_n.submit(t, kNow) == Verdictum::Publish) ? 1u : 0u;
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double per_ns = secs * 1e9 / (static_cast<double>(kIters) * 256.0);

    std::printf("  %d x 256 submits in %.3f s -> %.1f ns each\n",
                kIters, secs, per_ns);
    check(sink == static_cast<std::size_t>(kIters) * 256,
          "every submit published -- the loop was not optimised away");
    check(per_ns < 100.0,
          "a submit costs well under 100 ns, so the normaliser is not the "
          "bottleneck beside a 70 ns decode");
}

} // namespace

int main()
{
    std::printf("altair feed normaliser tests\n");
    monotonic_is_forced_off();
    future_ticks_are_dropped();
    floor_breach_flags_but_publishes();
    equal_timestamps_are_normal();
    regression_flags_but_publishes();
    inactive_source_is_not_published();
    seq_survives_a_failover();
    depth_shares_the_path();
    session_and_backpressure();
    benchmark();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
