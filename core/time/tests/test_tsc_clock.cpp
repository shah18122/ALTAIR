// P0-03 acceptance tests for core/time/tsc_clock.hpp.
// Plain main() (Catch2 not available until P0-05 brings vcpkg in).
//
// These run on real hardware. Every timing bound is deliberately loose — a test
// that fails once a week on a busy box is worse than no test.

#include <time/tsc_clock.hpp>
#include <time/timestamp.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <thread>
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

void skip(const char* what)
{
    std::printf("  skip: %s\n", what);
}

std::int64_t system_now_ns()
{
    const auto d = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
}

/// The clock under test: the real TSC clock where the CPU allows it, otherwise
/// an explicit fallback. Never a silent substitution — the tests say which.
const altair::TscClock& clock_under_test()
{
    static const altair::TscClock c = [] {
        auto r = altair::TscClock::create();
        return r.has_value() ? *r : altair::TscClock::create_fallback();
    }();
    return c;
}

} // namespace

using namespace altair;

void test_tsc_cpu_feature_detection()
{
    const bool inv1 = cpu::has_invariant_tsc();
    const bool inv2 = cpu::has_invariant_tsc();
    const bool rp1  = cpu::has_rdtscp();
    const bool rp2  = cpu::has_rdtscp();

    check(inv1 == inv2, "has_invariant_tsc() is stable across calls");
    check(rp1 == rp2,   "has_rdtscp() is stable across calls");
    check(cpu::read_tsc_ordered() != 0, "read_tsc_ordered() != 0");

    std::printf("        detected: invariant_tsc=%s rdtscp=%s\n",
                inv1 ? "yes" : "no", rp1 ? "yes" : "no");
}

void test_tsc_clock_creation()
{
    auto c = TscClock::create();

#if defined(__APPLE__)
    check(c.has_value(), "create() succeeds with the macOS monotonic clock");
    if (c.has_value()) {
        check(c->source() == ClockSource::AppleMonotonic,
              "source() == AppleMonotonic on macOS");
        check(!c->is_invariant_tsc(), "Apple monotonic source is not labelled TSC");
    }
#else
    if (cpu::has_invariant_tsc()) {
        check(c.has_value(), "create() succeeds on an invariant-TSC CPU");
        if (c.has_value()) {
            check(c->source() == ClockSource::InvariantTsc, "source() == InvariantTsc");
            check(c->is_invariant_tsc(), "is_invariant_tsc() == true");
        }
    } else {
        check(!c.has_value(), "create() fails without invariant TSC");
        if (!c.has_value()) {
            check(c.error() == ClockError::NoInvariantTsc,
                  "error() == NoInvariantTsc, never a silent fallback");
        }
    }
#endif
}

void test_tsc_monotonic()
{
    const auto& clk = clock_under_test();

    std::uint64_t prev = clk.now_ticks();
    bool ticks_ok = true;
    for (int i = 0; i < 10'000; ++i) {
        const std::uint64_t cur = clk.now_ticks();
        if (cur < prev) { ticks_ok = false; break; }
        prev = cur;
    }
    check(ticks_ok, "now_ticks() never decreases over 10'000 reads");

    Timestamp prev_ts = clk.now();
    bool ts_ok = true;
    for (int i = 0; i < 10'000; ++i) {
        const Timestamp cur = clk.now();
        if (cur < prev_ts) { ts_ok = false; break; }
        prev_ts = cur;
    }
    check(ts_ok, "now() never goes backwards over 10'000 reads");
}

void test_tsc_calibration_sane()
{
    const auto& clk = clock_under_test();

    if (!clk.is_invariant_tsc()) {
        skip("calibration sanity (no invariant TSC on this box)");
        return;
    }

    const auto& cal = clk.calibration();
    check(cal.ns_per_tick > 0.01 && cal.ns_per_tick < 10.0,
          "ns_per_tick in [0.01, 10) — 100 MHz to 100 GHz");
    check(cal.ns_per_tick_stderr > 0.0,
          "ns_per_tick_stderr > 0 — a measurement HAS error");
    check(cal.ns_per_tick_stderr < cal.ns_per_tick * 1e-3,
          "ns_per_tick_stderr < 0.1% of the estimate");
    check(cal.samples == 5, "samples == 5");
    check(cal.ns_per_tick_q32 != 0, "ns_per_tick_q32 != 0");

    const double q32_as_double = static_cast<double>(cal.ns_per_tick_q32) / 4294967296.0;
    check(q32_as_double - cal.ns_per_tick < 1e-9 &&
          cal.ns_per_tick - q32_as_double < 1e-9,
          "Q32.32 round-trips to the double estimate");

    std::printf("        ns_per_tick = %.9f +/- %.9f  (%.4f GHz), q32 = %llu\n",
                cal.ns_per_tick, cal.ns_per_tick_stderr,
                1.0 / cal.ns_per_tick,
                static_cast<unsigned long long>(cal.ns_per_tick_q32));
}

void test_tsc_matches_reference_clock()
{
    const auto& clk = clock_under_test();

    const std::uint64_t t0 = clk.now_ticks();
    const auto ref0 = std::chrono::steady_clock::now();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const Duration measured = clk.elapsed_since(t0);
    const auto ref1 = std::chrono::steady_clock::now();

    const double ref_ms =
        std::chrono::duration<double, std::milli>(ref1 - ref0).count();
    const double got_ms = to_millis_f(measured);
    const double rel = (got_ms > ref_ms ? got_ms - ref_ms : ref_ms - got_ms) / ref_ms;

    check(rel < 0.02, "TSC span agrees with steady_clock within 2%");
    check(to_nanos(clk.uncertainty_of(measured)) < to_nanos(measured) / 100,
          "uncertainty of a 200 ms span is under 1% of it");

    std::printf("        measured %.3f ms, reference %.3f ms, rel err %.5f%%\n",
                got_ms, ref_ms, rel * 100.0);
}

void test_tsc_uncertainty_propagates()
{
    const auto& clk = clock_under_test();

    const Duration u1  = clk.uncertainty_of(duration::millis(1));
    check(to_nanos(u1) > 0, "uncertainty of 1 ms is never zero");

    // Check GROWTH and SCALING where the proportional term dominates.
    // sigma(d) is |d|*rel + ns_per_tick, rounded to whole ns. At fixed spans
    // (1 ms / 10 ms, later 100 ms / 1000 ms) a tight calibration puts the
    // proportional term below the rounding of a ~1 ns result and the checks
    // flake: 1-in-20 under CPU load once, and again after calibration
    // endpoints were bracketed (rel fell to ~2e-8). So the span is taken from
    // the calibration: the one where |d|*rel is 100 ns. rel is floored at
    // 1e-9 by create(), so the span is at most 100 s and 10x fits in int64.
    const auto& cal = clk.calibration();
    const double rel = cal.ns_per_tick_stderr / cal.ns_per_tick;
    const std::int64_t base = std::llround(100.0 / rel);
    const Duration ua = clk.uncertainty_of(Duration{base});
    const Duration ub = clk.uncertainty_of(Duration{base * 10});
    check(to_nanos(ub) > to_nanos(ua), "uncertainty grows with the span");
    check(to_nanos(ub) < to_nanos(ua) * 11,
          "10x span gives at most ~11x error (measured where it is meaningful)");
    check(to_nanos(ub) > to_nanos(ua) * 9,
          "10x span gives at least ~9x error");

    check(clk.uncertainty_of(duration::millis(-5)) == clk.uncertainty_of(duration::millis(5)),
          "uncertainty is a magnitude, sign-independent");
    check(clk.uncertainty_of(Duration{0}) > Duration{0},
          "even a zero span carries the one-tick floor");

    std::printf("        sigma(1ms) = %lld ns; sigma(%lld ns) = %lld ns, sigma(10x) = %lld ns\n",
                static_cast<long long>(to_nanos(u1)), static_cast<long long>(base),
                static_cast<long long>(to_nanos(ua)), static_cast<long long>(to_nanos(ub)));
}

void test_tsc_timestamp_anchor()
{
    const auto& clk = clock_under_test();

    const Timestamp sys{system_now_ns()};
    const Duration delta = abs_duration(clk.now() - sys);

    check(to_millis(delta) < 100, "now() is within 100 ms of the system clock");

    const std::int64_t anchor_ns = clk.calibration().anchor.ns_since_epoch();
    check(anchor_ns > 1'577'836'800'000'000'000LL, "anchor is after 2020-01-01");
    check(anchor_ns < 4'102'444'800'000'000'000LL, "anchor is before 2100-01-01");

    std::printf("        now() - system_clock = %lld ns\n",
                static_cast<long long>(to_nanos(clk.now() - sys)));
}

void test_tsc_fallback_clock()
{
    const TscClock fb = TscClock::create_fallback();

    check(fb.source() == ClockSource::SteadyFallback, "fallback source");
    check(!fb.is_invariant_tsc(), "fallback is not an invariant TSC");
    check(fb.calibration().ns_per_tick == 1.0, "fallback ns_per_tick == 1.0");
    check(fb.calibration().ns_per_tick_q32 == (static_cast<std::uint64_t>(1) << 32),
          "fallback q32 == 2^32");
    check(fb.calibration().samples == 0, "fallback samples == 0");
    check(fb.calibration().ns_per_tick_stderr > 0.0,
          "fallback stderr > 0 — still not a lie");

    check(fb.now_ticks() > 0, "fallback now_ticks() > 0");
    const Timestamp sys{system_now_ns()};
    check(to_millis(abs_duration(fb.now() - sys)) < 100,
          "fallback now() within 100 ms of the system clock");
}

namespace {

/// Latency report. Printed, never asserted — gate 6 is a human judgement
/// against ROADMAP §11, and a contended box would make any bound flaky.
void report_latency()
{
    const auto& clk = clock_under_test();

    std::printf("\nlatency — TscClock::now()\n");
    if (!clk.is_invariant_tsc()) {
        std::printf("  SKIPPED (no invariant TSC) — a fallback-clock figure is not\n"
                    "  comparable and must not enter the ledger.\n");
        return;
    }

    constexpr int kIters = 100'000;
    // Unsigned: summing ~1.8e18 ns timestamps overflows within a few adds, and
    // signed overflow is UB (UBSan). Unsigned wrap is defined.
    std::uint64_t sink = 0;   // defeats dead-code elimination without volatile

    // Bracketing a call with two ordered TSC reads costs a full RDTSCP itself.
    // Measure that apparatus first and subtract it, or we report our own
    // instrument rather than the clock.
    std::vector<std::int64_t> base;
    std::vector<std::int64_t> pay;
    base.reserve(kIters);
    pay.reserve(kIters);

    for (int i = 0; i < 10'000; ++i) {   // warm up: page in, settle frequency
        sink += static_cast<std::uint64_t>(clk.now().ns_since_epoch());
    }

    for (int i = 0; i < kIters; ++i) {
        const std::uint64_t a = clk.now_ticks();
        const std::uint64_t b = clk.now_ticks();
        base.push_back(to_nanos(clk.ticks_to_duration(b - a)));
    }
    for (int i = 0; i < kIters; ++i) {
        const std::uint64_t a = clk.now_ticks();
        sink += static_cast<std::uint64_t>(clk.now().ns_since_epoch());
        const std::uint64_t b = clk.now_ticks();
        pay.push_back(to_nanos(clk.ticks_to_duration(b - a)));
    }

    std::sort(base.begin(), base.end());
    std::sort(pay.begin(), pay.end());
    const auto pct = [](const std::vector<std::int64_t>& v, int p) {
        return v[static_cast<std::size_t>(static_cast<long long>(v.size()) * p / 100)];
    };
    const std::int64_t b50 = pct(base, 50);
    const std::int64_t p50 = pct(pay, 50);
    const std::int64_t p99 = pct(pay, 99);
    const std::int64_t c50 = p50 - b50 > 0 ? p50 - b50 : 0;
    const std::int64_t c99 = p99 - b50 > 0 ? p99 - b50 : 0;

    std::printf("  source = InvariantTsc, n = %d, TSC = %.3f GHz\n",
                kIters, 1.0 / clk.calibration().ns_per_tick);
    std::printf("  harness overhead (back-to-back now_ticks) p50 = %lld ns\n",
                static_cast<long long>(b50));
    std::printf("  raw       p50 = %3lld ns   p99 = %3lld ns\n",
                static_cast<long long>(p50), static_cast<long long>(p99));
    std::printf("  corrected p50 = %3lld ns   p99 = %3lld ns   (budget: p99 < 25 ns)\n",
                static_cast<long long>(c50), static_cast<long long>(c99));
    std::printf("  %s\n", c99 < 25 ? "within budget" : "OVER BUDGET — review gate 6");
    if (sink == 0) {   // never true; keeps the loops alive
        std::printf("  (unreachable)\n");
    }
}

} // namespace

// C01-008: re-anchoring keeps the rate and pulls now() back to UTC.
static void test_reanchor_keeps_rate()
{
    const TscClock c = TscClock::create_fallback();
    const TscClock r = c.reanchored();
    check(r.source() == c.source(), "reanchored() keeps the clock source");
    check(r.calibration().ns_per_tick_q32 == c.calibration().ns_per_tick_q32,
          "reanchored() keeps the calibrated rate");
    check(r.calibration().anchor >= c.calibration().anchor,
          "reanchored() moves the anchor forward, never back");
    const auto drift = r.drift_from_system().raw();
    check(drift > -1'000'000'000LL && drift < 1'000'000'000LL,
          "a fresh anchor is within one second of the system clock");
}

int main()
{
    std::printf("altair core/time tsc_clock tests\n");
    test_tsc_cpu_feature_detection();
    test_tsc_clock_creation();
    test_tsc_monotonic();
    test_tsc_calibration_sane();
    test_tsc_matches_reference_clock();
    test_tsc_uncertainty_propagates();
    test_tsc_timestamp_anchor();
    test_tsc_fallback_clock();
    test_reanchor_keeps_rate();

    report_latency();

    if (failures == 0) {
        std::printf("\nPASS\n");
    } else {
        std::printf("\nFAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
