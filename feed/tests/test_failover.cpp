// P2-05 acceptance tests for feed/failover.hpp.
//
// No check description here may contain the substring FAIL.

#include <feed/failover.hpp>

#include <cstdio>
#include <cstddef>
#include <cstdint>
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

constexpr Timestamp kT0{1787888700000000000LL};

Timestamp at_ms(std::int64_t ms)
{
    return Timestamp{kT0.ns_since_epoch() + ms * 1'000'000LL};
}

FailoverWatchdog::Config cfg()
{
    FailoverWatchdog::Config c{};
    c.staleness_ms = 250;
    c.failback_stable_s = 60;
    c.enabled = true;
    c.preferred = FeedSource::Kite;
    return c;
}

FailoverWatchdog g_wd{cfg()};

void fresh(FailoverWatchdog::Config c = cfg())
{
    g_wd.~FailoverWatchdog();
    new (&g_wd) FailoverWatchdog(c);
}

// ── 1 ────────────────────────────────────────────────────────────────────
// D2. The phantom-default-state bug again, in its feed-health costume.
void unheard_is_not_healthy()
{
    std::printf("\n1 unheard_is_not_healthy\n");
    fresh();

    const auto d = g_wd.poll(kT0);
    check(d.kite == FeedHealth::Unknown && d.xts == FeedHealth::Unknown,
          "a source never heard from is Unknown -- not Live and not Stale");
    check(d.action == FailoverAction::NoneHealthy,
          "so at session start NEITHER is usable, which is the truth");
    check(g_wd.active() == FeedSource::Kite, "and the primary has not moved");

    // Kite speaks; XTS has still never delivered a byte.
    g_wd.on_data(FeedSource::Kite, at_ms(10));
    const auto e = g_wd.poll(at_ms(20));
    check(e.kite == FeedHealth::Live, "Kite is Live once it has spoken");
    check(e.xts == FeedHealth::Unknown,
          "XTS stays Unknown -- treating silence-so-far as healthy would let "
          "the watchdog fail over to a feed that has never sent anything");
    check(e.action == FailoverAction::None, "nothing changes");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// The reason this card keys on recv_ts.
void staleness_is_measured_on_recv_ts()
{
    std::printf("\n2 staleness_is_measured_on_recv_ts\n");
    fresh();

    g_wd.on_data(FeedSource::Kite, at_ms(0));
    g_wd.on_data(FeedSource::Xts, at_ms(0));

    check(g_wd.poll(at_ms(249)).kite == FeedHealth::Live,
          "249 ms of silence is still Live at a 250 ms threshold");
    check(g_wd.poll(at_ms(250)).kite == FeedHealth::Live,
          "250 ms exactly is the boundary and still Live");
    check(g_wd.poll(at_ms(251)).kite == FeedHealth::Stale,
          "251 ms is Stale");

    // The point: a 250 ms threshold against a ONE-SECOND clock would fire on
    // a perfectly healthy feed. recv_ts has nanosecond resolution; Kite's
    // exchange_ts does not (P2-02).
    check(at_ms(251).ns_since_epoch() % 1'000'000'000LL != 0,
          "recv_ts carries sub-second resolution, which a 250 ms threshold "
          "needs and which Kite's one-second exchange_ts cannot provide");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void switches_away_from_a_stale_primary()
{
    std::printf("\n3 switches_away_from_a_stale_primary\n");
    fresh();

    g_wd.on_data(FeedSource::Kite, at_ms(0));
    g_wd.on_data(FeedSource::Xts, at_ms(0));
    check(g_wd.poll(at_ms(10)).action == FailoverAction::None,
          "both healthy: nothing to do");

    // Kite goes quiet; XTS keeps talking.
    g_wd.on_data(FeedSource::Xts, at_ms(300));
    const auto d = g_wd.poll(at_ms(310));
    check(d.kite == FeedHealth::Stale && d.xts == FeedHealth::Live,
          "Kite is stale, XTS is live");
    check(d.action == FailoverAction::SwitchTo && d.active == FeedSource::Xts,
          "so the primary switches to XTS");
    check(g_wd.active() == FeedSource::Xts, "and the watchdog holds it");
    check(g_wd.stats().switches == 1, "counted as one switch");

    // No stability wait on the way OUT: staying on a dead feed costs every tick.
    check(g_wd.stats().failbacks == 0,
          "and it is not a failback -- the switch away from a dead primary is "
          "immediate, with no stability wait, because every tick spent on a "
          "silent feed is lost");
}

// ── 4 ────────────────────────────────────────────────────────────────────
// D4. The asymmetry: leaving is immediate, returning is not.
void failback_waits_for_stability()
{
    std::printf("\n4 failback_waits_for_stability\n");
    fresh();

    g_wd.on_data(FeedSource::Kite, at_ms(0));
    g_wd.on_data(FeedSource::Xts, at_ms(0));
    g_wd.on_data(FeedSource::Xts, at_ms(300));
    (void)g_wd.poll(at_ms(310));
    check(g_wd.active() == FeedSource::Xts, "we are on XTS after Kite stalled");

    // Kite comes back. It must NOT immediately reclaim the primary.
    g_wd.on_data(FeedSource::Kite, at_ms(400));
    g_wd.on_data(FeedSource::Xts, at_ms(400));
    const auto soon = g_wd.poll(at_ms(410));
    check(soon.kite == FeedHealth::Live, "Kite is healthy again");
    check(soon.action == FailoverAction::None,
          "but the primary does NOT move back yet -- a feed flapping either "
          "side of the threshold would otherwise drag the primary with it, "
          "and every switch costs a subscription round trip");

    // Keep both alive past the 60 s stability window.
    for (std::int64_t t = 500; t <= 61'000; t += 100) {
        g_wd.on_data(FeedSource::Kite, at_ms(t));
        g_wd.on_data(FeedSource::Xts, at_ms(t));
    }
    const auto back = g_wd.poll(at_ms(61'050));
    check(back.action == FailoverAction::SwitchTo
              && back.active == FeedSource::Kite,
          "after 60 s of continuous health it fails BACK to the preferred "
          "source");
    check(g_wd.stats().failbacks == 1, "counted as a failback");
    check(g_wd.stats().switches == 2, "and as the second switch overall");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void both_stale_is_reported_not_papered_over()
{
    std::printf("\n5 both_stale_is_reported_not_papered_over\n");
    fresh();

    g_wd.on_data(FeedSource::Kite, at_ms(0));
    g_wd.on_data(FeedSource::Xts, at_ms(0));

    const auto d = g_wd.poll(at_ms(1000));
    check(d.kite == FeedHealth::Stale && d.xts == FeedHealth::Stale,
          "both sources have gone quiet");
    check(d.action == FailoverAction::NoneHealthy,
          "which is reported as NoneHealthy");
    check(g_wd.active() == FeedSource::Kite,
          "and the primary does NOT move -- switching to an equally dead feed "
          "would look like a fix and change nothing");
    check(g_wd.stats().none_healthy >= 1, "counted, so it can be alarmed on");
    check(g_wd.stats().switches == 0, "with no switch recorded");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void disabled_still_reports()
{
    std::printf("\n6 disabled_still_reports\n");
    auto c = cfg();
    c.enabled = false;
    fresh(c);

    g_wd.on_data(FeedSource::Kite, at_ms(0));
    g_wd.on_data(FeedSource::Xts, at_ms(0));
    g_wd.on_data(FeedSource::Xts, at_ms(300));

    const auto d = g_wd.poll(at_ms(310));
    check(d.kite == FeedHealth::Stale && d.xts == FeedHealth::Live,
          "health is still computed with failover disabled");
    check(d.action == FailoverAction::None, "but no switch is issued");
    check(g_wd.active() == FeedSource::Kite, "the primary stays put");
    check(g_wd.stats().suppressed == 1,
          "and the suppression is COUNTED, so an operator can see what the "
          "watchdog would have done");
}

// ── 7 ────────────────────────────────────────────────────────────────────
void out_of_order_arrival_does_not_age_a_source()
{
    std::printf("\n7 out_of_order_arrival_does_not_age_a_source\n");
    fresh();

    g_wd.on_data(FeedSource::Kite, at_ms(500));
    g_wd.on_data(FeedSource::Xts, at_ms(500));
    // A late straggler carrying an older recv_ts.
    g_wd.on_data(FeedSource::Kite, at_ms(100));

    const auto d = g_wd.poll(at_ms(600));
    check(d.kite == FeedHealth::Live,
          "a single out-of-order arrival does not lower the high-water mark, "
          "so the source does not suddenly look stale");
    check(d.action == FailoverAction::None, "and no spurious switch happens");
}

// ── 8 ────────────────────────────────────────────────────────────────────
// The whole point: a switch must not disturb the sequence.
Normaliser g_norm{[] {
    Normaliser::Config c{};
    c.gate.floor = Timestamp{1787000000000000000LL};
    c.gate.max_future_skew = duration::seconds(2);
    c.active = FeedSource::Kite;
    return c;
}()};

void applying_a_switch_keeps_the_sequence()
{
    std::printf("\n8 applying_a_switch_keeps_the_sequence\n");
    fresh();
    g_norm.reset_session();

    auto tick = [](FeedSource s) {
        Tick t{};
        t.id = static_cast<InstrumentId>(1);
        t.exchange_ts = kT0;
        t.recv_ts = kT0;
        t.source = s;
        return t;
    };

    std::uint32_t seen[4]{};
    for (int i = 0; i < 2; ++i) {
        Tick t = tick(FeedSource::Kite);
        (void)g_norm.submit(t, kT0);
        seen[i] = t.seq;
    }

    g_wd.on_data(FeedSource::Kite, at_ms(0));
    g_wd.on_data(FeedSource::Xts, at_ms(0));
    g_wd.on_data(FeedSource::Xts, at_ms(300));
    const auto d = g_wd.poll(at_ms(310));
    check(d.action == FailoverAction::SwitchTo, "the watchdog decides to switch");

    g_wd.apply(d, g_norm);
    check(g_norm.active() == FeedSource::Xts,
          "apply() moves the normaliser's active source");

    for (int i = 0; i < 2; ++i) {
        Tick t = tick(FeedSource::Xts);
        (void)g_norm.submit(t, kT0);
        seen[2 + i] = t.seq;
    }

    bool contiguous = true;
    for (std::uint32_t i = 0; i < 4; ++i) {
        if (seen[i] != i) {
            contiguous = false;
        }
    }
    check(contiguous,
          "and the sequence runs 0..3 unbroken ACROSS the switch -- no repeat, "
          "no gap, because seq never came from the feed");
    check(g_norm.stats().published == 4, "four ticks published in total");
}

} // namespace

int main()
{
    std::printf("altair feed failover tests\n");
    unheard_is_not_healthy();
    staleness_is_measured_on_recv_ts();
    switches_away_from_a_stale_primary();
    failback_waits_for_stability();
    both_stale_is_reported_not_papered_over();
    disabled_still_reports();
    out_of_order_arrival_does_not_age_a_source();
    applying_a_switch_keeps_the_sequence();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
