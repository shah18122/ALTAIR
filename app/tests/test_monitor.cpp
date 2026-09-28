// app/tests/test_monitor.cpp -- P12-03.
//
// The failure this card exists for: a dead feed passes every threshold.
//
// Drops 0, latency 0 ns, residual 0 paise. Every limit satisfied, every light
// green, and no tick has arrived in forty minutes. So the tests below are not
// "does the threshold compare correctly" -- that part is arithmetic. They are
// about the two states a threshold cannot represent: NEVER REPORTED, and
// STOPPED REPORTING.

#include <app/monitor.hpp>

#include <cstdio>
#include <cmath>
#include <limits>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

/// The gauges an Altair process actually runs on, in the order the card names
/// them: latency, drops, drift, PnL, invariants.
enum Gauge : std::size_t {
    kTickLatencyP99 = 0,   ///< us, Above
    kFeedDrops,            ///< count in window, Above
    kModelDriftPsi,        ///< PSI vs the training distribution, Above
    kSessionPnlPaise,      ///< Below -- a LOSS is the bad direction
    kConservationResidual, ///< paise, Above; anything but 0 trips the switch
    kFillRate,             ///< Below -- fills / intents
    kGaugeCount
};

altair::Monitor<kGaugeCount> make_monitor() {
    using altair::Duration;
    using altair::GaugeSpec;
    using altair::Sense;
    using altair::duration::seconds;

    std::array<GaugeSpec, kGaugeCount> s{};
    // Latency: ROADMAP section 3 -- book imbalance decays in 10-200 ms, so a
    // p99 in the hundreds of us is the budget, not the mean.
    s[kTickLatencyP99]       = {"tick.latency.p99.us", 200.0, 1000.0,
                                Sense::Above, seconds(5)};
    s[kFeedDrops]            = {"feed.drops", 1.0, 10.0, Sense::Above,
                                seconds(5)};
    s[kModelDriftPsi]        = {"model.drift.psi", 0.10, 0.25, Sense::Above,
                                seconds(300)};
    // PnL is money, and it crosses into this file as a double ONCE, for
    // display. The ledger it came from is integer paise (rule 3).
    s[kSessionPnlPaise]      = {"pnl.session.paise", -500000.0, -2000000.0,
                                Sense::Below, seconds(60)};
    s[kConservationResidual] = {"invariant.residual.paise", 1.0, 1.0,
                                Sense::Above, seconds(5)};
    s[kFillRate]             = {"oms.fill_rate", 0.80, 0.50, Sense::Below,
                                seconds(60)};
    return altair::Monitor<kGaugeCount>{s};
}

/// Every gauge at a value that clears its threshold comfortably.
void observe_healthy(altair::Monitor<kGaugeCount>& m, altair::Timestamp t) {
    m.observe(kTickLatencyP99, 45.0, t);
    m.observe(kFeedDrops, 0.0, t);
    m.observe(kModelDriftPsi, 0.03, t);
    m.observe(kSessionPnlPaise, 125000.0, t);
    m.observe(kConservationResidual, 0.0, t);
    m.observe(kFillRate, 0.97, t);
}

} // namespace

int main() {
    using altair::Health;
    using altair::LatencyHistogram;
    using altair::Timestamp;
    using altair::health_text;
    using altair::duration::minutes;
    using altair::duration::seconds;

    std::printf("P12-03 monitoring and alerting\n");

    const Timestamp t0{1'788'393'600'000'000'000LL};   // an arbitrary wall time

    // ---- 1. A MONITOR THAT WAS NEVER WIRED UP IS NOT HEALTHY --------------
    //
    // The failure at t=0. Every threshold is satisfied because nothing has
    // been compared to anything, and a monitor that reports Ok here passes the
    // one check it needed to fail: did anyone connect?
    {
        auto m = make_monitor();
        const auto w = m.worst(t0);
        check(w.health == Health::Unobserved,
              "a monitor with nothing wired to it reports UNOBSERVED, not ok "
              "-- every threshold IS satisfied, which is the problem");
        check(m.health(kFeedDrops, t0) == Health::Unobserved,
              "and per-gauge too: a drops counter nobody feeds is not a drops "
              "counter reading zero");

        auto partial = make_monitor();
        partial.observe(kTickLatencyP99, 45.0, t0);
        partial.observe(kModelDriftPsi, 0.01, t0);
        partial.observe(kSessionPnlPaise, 0.0, t0);
        partial.observe(kConservationResidual, 0.0, t0);
        partial.observe(kFillRate, 0.97, t0);
        const auto mixed = partial.worst(t0);
        check(mixed.health == Health::Unobserved && mixed.index == kFeedDrops,
              "one unwired gauge outranks every healthy gauge in the summary");
    }

    // ---- 2. THE DEAD FEED -------------------------------------------------
    //
    // The card. A process healthy at t0, then the exchange connection dies.
    // Nothing reports anything. Every stored value is still the good one.
    {
        auto m = make_monitor();
        observe_healthy(m, t0);
        check(m.worst(t0).health == Health::Ok,
              "healthy while everything is reporting");

        const Timestamp t1 = t0 + minutes(40);
        const auto w = m.worst(t1);
        check(w.health == Health::Stale,
              "forty minutes later, with every STORED value still perfect and "
              "every threshold still satisfied, the board is STALE");
        // And the values really are still fine -- so a threshold-only monitor
        // would report green here. That is the whole finding, asserted.
        check(m.state(kFeedDrops).value == 0.0
                  && m.state(kConservationResidual).value == 0.0
                  && m.state(kTickLatencyP99).value == 45.0,
              "the stored numbers are unchanged and all pass: a threshold-only "
              "monitor is GREEN on a feed that died forty minutes ago");
        std::printf("        worst = %s (%s)\n", health_text(w.health), w.name);
    }

    // ---- 3. ZERO IS A VALUE. ABSENCE IS NOT. ------------------------------
    //
    // Both cases hold the number 0.0 for feed drops. They must not produce the
    // same answer.
    {
        auto reported = make_monitor();
        reported.observe(kFeedDrops, 0.0, t0);
        auto silent = make_monitor();

        check(reported.health(kFeedDrops, t0) == Health::Ok
                  && silent.health(kFeedDrops, t0) == Health::Unobserved,
              "a reported 0 and an unreported 0 are the same number and "
              "different facts -- ok versus UNOBSERVED");
    }

    // ---- 4. STALENESS OUTRANKS A BREACH -----------------------------------
    //
    // Ordering, not taste. A breached threshold is the system telling you
    // something bad. A stale gauge is the system having stopped telling you
    // anything, and you do not know your exposure.
    {
        auto m = make_monitor();
        observe_healthy(m, t0);
        m.observe(kModelDriftPsi, 0.40, t0);           // hard drift breach
        check(m.worst(t0).health == Health::Critical
                  && m.worst(t0).index == kModelDriftPsi,
              "a drift breach reports CRITICAL and names the gauge");

        // Now let the FEED go stale while drift keeps reporting its breach.
        const Timestamp t1 = t0 + seconds(30);
        m.observe(kModelDriftPsi, 0.40, t1);
        const auto w = m.worst(t1);
        check(w.health == Health::Stale,
              "with a live CRITICAL and a silent gauge, the silent one wins: "
              "you can act on a breach, you cannot act on a blank");
        std::printf("        worst = %s (%s)\n", health_text(w.health), w.name);
    }

    // ---- 5. BOTH SENSES ---------------------------------------------------
    //
    // Half these signals are bad when large and half when small. A monitor
    // that only knows Above cannot watch fill rate or PnL at all, and the way
    // it fails is by reporting them healthy forever.
    {
        auto m = make_monitor();
        observe_healthy(m, t0);
        m.observe(kFillRate, 0.45, t0);                // 45% of intents filled
        check(m.health(kFillRate, t0) == Health::Critical,
              "fill rate is CRITICAL when it falls, not when it rises");
        m.observe(kFillRate, 0.97, t0);
        m.observe(kSessionPnlPaise, -2500000.0, t0);   // -Rs 25,000
        check(m.health(kSessionPnlPaise, t0) == Health::Critical,
              "and a Rs 25,000 session loss is CRITICAL while a Rs 25,000 gain "
              "is not -- the same magnitude, opposite sense");
    }

    // ---- 5b. INVALID NUMBERS ARE NOT HEALTHY -----------------------------
    {
        auto m = make_monitor();
        m.observe(kTickLatencyP99,
                  std::numeric_limits<double>::quiet_NaN(), t0);
        check(m.health(kTickLatencyP99, t0) == Health::Critical,
              "NaN cannot slip through both threshold comparisons as Ok");

        std::array<altair::GaugeSpec, 1> bad_specs{{
            {"bad.order", 10.0, 5.0, altair::Sense::Above, seconds(60)}}};
        altair::Monitor<1> bad_thresholds{bad_specs};
        bad_thresholds.observe(0, 0.0, t0);
        check(bad_thresholds.health(0, t0) == Health::Critical,
              "misordered warning/critical thresholds are not reported as Ok");
    }

    // ---- 6. THE MEAN IS THE NUMBER THAT LOOKS FINE ------------------------
    //
    // 985 ticks at 40 us and 15 at 3 ms. The mean clears a 200 us budget with
    // room to spare; one tick in 67 misses the window it was trading on.
    {
        LatencyHistogram h;
        for (int i = 0; i < 985; ++i) { h.record(40'000); }
        for (int i = 0; i < 15;  ++i) { h.record(3'000'000); }

        const double mean_us = h.mean_ns() / 1000.0;
        const double p99_us = static_cast<double>(h.percentile(0.99)) / 1000.0;
        const double p50_us = static_cast<double>(h.percentile(0.50)) / 1000.0;
        std::printf("        p50 %.1f us   mean %.1f us   p99 %.0f us"
                    "   max %.0f us\n",
                    p50_us, mean_us, p99_us,
                    static_cast<double>(h.max_ns()) / 1000.0);

        check(mean_us < 200.0 && p99_us > 200.0,
              "the mean PASSES a 200 us budget and the p99 FAILS it by 15x -- "
              "one number says ship it, the other says the tail is not "
              "trading on the signal it thinks it is");
        check(p50_us < 100.0,
              "and the median passes too, so a two-number dashboard of "
              "median and mean still shows nothing");
    }

    // ---- 7. THE HISTOGRAM ROUNDS UP -------------------------------------
    //
    // A latency figure that rounds DOWN is a latency figure that clears a
    // budget it did not clear.
    {
        LatencyHistogram h;
        h.record(199'999);                             // just inside 200 us
        const std::int64_t p = h.percentile(1.0);
        check(p >= 199'999,
              "a single sample reports at least its own value, never less");

        // The two properties that actually matter, swept over five decades of
        // real latencies (1 ns to 100 ms):
        //
        //   a) every sample lands at or below its own bucket's upper edge, so
        //      a reported percentile never UNDERSTATES the latency;
        //   b) buckets do not go backwards, and every bucket the mapping can
        //      actually REACH has a strictly larger edge than the last one.
        //
        // The reachability qualifier is not a weakening. The first two octaves
        // cannot be quartered -- {1} and {2,3} have no four parts -- so a few
        // indices below 8 are unreachable and tie with their neighbour. They
        // never take a count, so they cannot affect a percentile. Asserting
        // strict monotonicity over ALL 256 would be asserting something the
        // integers do not permit, and the fix for a failing assertion like
        // that is usually to weaken it until it passes.
        bool never_understates = true;
        bool index_monotone = true;
        bool reachable_strict = true;
        std::size_t prev_idx = 0;
        std::int64_t prev_edge = 0;
        for (std::int64_t ns = 1; ns <= 100'000'000; ns += 1 + ns / 64) {
            const std::size_t idx = LatencyHistogram::index_of(ns);
            const std::int64_t edge = LatencyHistogram::upper_edge(idx);
            if (edge < ns) { never_understates = false; }
            if (idx < prev_idx) { index_monotone = false; }
            if (idx != prev_idx && edge <= prev_edge) { reachable_strict = false; }
            prev_idx = idx;
            prev_edge = edge;
        }
        check(never_understates,
              "over five decades of latencies, no sample exceeds its own "
              "bucket edge -- a percentile can round up, never down");
        check(index_monotone && reachable_strict,
              "and buckets never go backwards: every reachable bucket has a "
              "strictly larger edge than the one before it");
        check(LatencyHistogram::index_of(0) == 0
                  && LatencyHistogram::index_of(-5) == 0
                  && LatencyHistogram::index_of(1'000'000'000'000LL)
                         < LatencyHistogram::kBuckets,
              "a backwards clock is recorded rather than discarded, and a "
              "1000-second sample stays in range instead of running off the "
              "end of the array");
    }

    // ---- 8. COUNTERS AND RUNNING MEAN DO NOT WRAP ------------------------
    {
        std::uint64_t counter = std::numeric_limits<std::uint64_t>::max() - 1;
        const bool reached_limit = altair::monitor_detail::increment_saturating(counter);
        const bool stopped_at_limit =
            !altair::monitor_detail::increment_saturating(counter)
            && counter == std::numeric_limits<std::uint64_t>::max();
        check(reached_limit && stopped_at_limit,
              "diagnostic counters stop at UINT64_MAX instead of wrapping");

        LatencyHistogram h;
        const auto largest = std::numeric_limits<std::int64_t>::max();
        h.record(largest);
        h.record(largest);
        h.record(-1);  // included in the sample count as a zero-duration fact
        check(h.count() == 3 && h.max_ns() == largest
                  && std::isfinite(h.mean_ns())
                  && h.mean_ns() > 6.0e18 && h.mean_ns() < 6.2e18,
              "near-INT64_MAX samples keep the online mean finite without a signed-sum overflow");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
