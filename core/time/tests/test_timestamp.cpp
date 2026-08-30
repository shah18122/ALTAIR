// P0-02 acceptance tests for core/time/timestamp.hpp.
// Plain main() (Catch2 not available until P0-03).

#include <time/timestamp.hpp>

#include <cstdio>
#include <cstdint>
#include <limits>
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
using namespace altair::literals;

// ─────────────────────────────────────────────────────────────────────────
// Static type-safety assertions. These do the work at compile time.
//
// MSVC hard-errors on a bare negative requires-expression over these types
// (verified in P0-01: 19.51.36256, C2678/C2679, and it is not scope-dependent).
// The standard-conforming workaround is to wrap each negative check in a named
// concept, which MSVC does evaluate correctly. Semantics are identical.
// ─────────────────────────────────────────────────────────────────────────
namespace time_safety_probes {

template <typename T, typename U>
concept addable = requires(T a, U b) { a + b; };

template <typename T, typename U>
concept multipliable = requires(T a, U b) { a * b; };

} // namespace time_safety_probes

static_assert(!time_safety_probes::addable<altair::Timestamp, altair::Timestamp>,
              "Timestamp + Timestamp is meaningless and must not compile");
static_assert(!time_safety_probes::multipliable<altair::Timestamp, std::int64_t>,
              "Timestamp * scalar is meaningless and must not compile");
static_assert(!std::is_convertible_v<std::int64_t, altair::Timestamp>,
              "int64 must not implicitly convert to Timestamp");
static_assert(!std::is_convertible_v<altair::Timestamp, std::int64_t>,
              "Timestamp must not implicitly convert to int64");
static_assert(!std::is_convertible_v<altair::Timestamp, altair::Duration>,
              "Timestamp must not implicitly convert to Duration");
static_assert(!std::is_convertible_v<altair::Duration, altair::Timestamp>,
              "Duration must not implicitly convert to Timestamp");

// Positive cases — these MUST compile and be constant-evaluable:
static_assert(time_safety_probes::addable<altair::Timestamp, altair::Duration>);
static_assert(time_safety_probes::addable<altair::Duration, altair::Duration>);
static_assert(altair::ist_ns_since_midnight(altair::Timestamp{1787888700000000000LL})
              == 33'300'000'000'000LL,
              "IST market open, evaluated at compile time");
static_assert(altair::ist_ns_since_midnight(altair::Timestamp{-19'800'000'000'001LL})
              == 86'399'999'999'999LL,
              "floor semantics, evaluated at compile time");

void test_timestamp_traits()
{
    check(sizeof(Timestamp) == 8, "sizeof(Timestamp) == 8");
    check(std::is_trivially_copyable_v<Timestamp>, "Timestamp trivially copyable");
    check(std::is_trivially_destructible_v<Timestamp>, "Timestamp trivially destructible");
    check(std::is_standard_layout_v<Timestamp>, "Timestamp standard layout");
    check(Timestamp::epoch().ns_since_epoch() == 0, "epoch() == 0 ns");
    check(Timestamp::epoch().is_epoch(), "epoch().is_epoch()");
    check(Timestamp::min().ns_since_epoch() == std::numeric_limits<std::int64_t>::min(),
          "min() == INT64_MIN");
    check(Timestamp::max().ns_since_epoch() == std::numeric_limits<std::int64_t>::max(),
          "max() == INT64_MAX");
}

void test_timestamp_affine_algebra()
{
    check((Timestamp{1000} + duration::nanos(500)).ns_since_epoch() == 1500,
          "instant + span == instant");
    check((duration::nanos(500) + Timestamp{1000}).ns_since_epoch() == 1500,
          "span + instant == instant");
    check((Timestamp{1000} - duration::nanos(500)).ns_since_epoch() == 500,
          "instant - span == instant");
    check((Timestamp{1500} - Timestamp{1000}) == duration::nanos(500),
          "instant - instant == span");
    check((Timestamp{1000} - Timestamp{1500}) == duration::nanos(-500),
          "instant - instant, negative span");
    check(Timestamp{1000} < Timestamp{1500}, "1000 < 1500");
    check(Timestamp{1000} == Timestamp{1000}, "1000 == 1000");

    Timestamp t{1000};
    t += duration::nanos(500);
    check(t.ns_since_epoch() == 1500, "operator+= advances");
    t -= duration::nanos(200);
    check(t.ns_since_epoch() == 1300, "operator-= rewinds");
}

void test_duration_constructors_and_accessors()
{
    check(duration::nanos(1)   == Duration{1},                  "nanos(1)");
    check(duration::micros(1)  == Duration{1'000},              "micros(1) == 1e3 ns");
    check(duration::millis(1)  == Duration{1'000'000},          "millis(1) == 1e6 ns");
    check(duration::seconds(1) == Duration{1'000'000'000},      "seconds(1) == 1e9 ns");
    check(duration::minutes(1) == Duration{60'000'000'000},     "minutes(1) == 6e10 ns");
    check(duration::hours(1)   == Duration{3'600'000'000'000},  "hours(1) == 3.6e12 ns");
    check(duration::days(1)    == Duration{86'400'000'000'000}, "days(1) == 8.64e13 ns");

    check(to_nanos(duration::millis(5))   == 5'000'000, "to_nanos(millis(5))");
    check(to_micros(duration::millis(5))  == 5'000,     "to_micros(millis(5))");
    check(to_millis(duration::millis(5))  == 5,         "to_millis(millis(5))");
    check(to_seconds(duration::millis(5)) == 0,         "to_seconds truncates toward zero");
    check(to_seconds(duration::millis(-5)) == 0,        "to_seconds(-5ms) == 0, not floor");
    check(to_millis_f(duration::micros(1500)) == 1.5,   "to_millis_f(micros(1500)) == 1.5");
    check(to_seconds_f(duration::millis(250)) == 0.25,  "to_seconds_f(millis(250)) == 0.25");

    // Literals — the same spans, written the way config and tests will write them.
    check(500_ns  == duration::nanos(500),   "literal _ns");
    check(1_us    == duration::micros(1),    "literal _us");
    check(1500_ms == duration::millis(1500), "literal _ms");
    check(1_s     == duration::seconds(1),   "literal _s");
    check(15_min  == duration::minutes(15),  "literal _min");
    check(6_h     == duration::hours(6),     "literal _h");
    check((6_h + 15_min) == duration::minutes(375),
          "6h15m composes to the 375-minute session");
}

void test_ist_offset_market_open()
{
    constexpr Timestamp kOpen{1787888700000000000LL};   // 2026-08-28 09:15:00 IST

    check(kIstOffset == duration::seconds(19800), "kIstOffset == +05:30");
    check(ist_ns_since_midnight(kOpen) == 33'300'000'000'000LL,
          "market open is 09:15:00 IST == 33300 s");
}

void test_ist_session_boundaries()
{
    constexpr Timestamp kOpen{1787888700000000000LL};

    // 09:15 open, 15:00 no-new-entries, 15:15 square-off, 15:30 close.
    check(ist_ns_since_midnight(ist_time_of_day(kOpen,  9, 15, 0)) == 33'300'000'000'000LL,
          "09:15 open");
    check(ist_ns_since_midnight(ist_time_of_day(kOpen, 15,  0, 0)) == 54'000'000'000'000LL,
          "15:00 no-new-entries");
    check(ist_ns_since_midnight(ist_time_of_day(kOpen, 15, 15, 0)) == 54'900'000'000'000LL,
          "15:15 square-off");
    check(ist_ns_since_midnight(ist_time_of_day(kOpen, 15, 30, 0)) == 55'800'000'000'000LL,
          "15:30 close");

    // The trading day is 6h15m long.
    check((ist_time_of_day(kOpen, 15, 30, 0) - ist_time_of_day(kOpen, 9, 15, 0))
              == duration::minutes(375),
          "session length == 375 min");

    check(ist_time_of_day(kOpen, 9, 15, 0) == kOpen, "09:15 on kOpen's day IS kOpen");
}

void test_ist_floor_semantics_pre_epoch()
{
    check(ist_ns_since_midnight(Timestamp{-19'800'000'000'001LL}) == 86'399'999'999'999LL,
          "shifted == -1 floors to 86399999999999, not -1");
    check(ist_ns_since_midnight(Timestamp{-19'800'000'000'000LL}) == 0,
          "shifted == 0 gives 0");
    check(ist_ns_since_midnight(Timestamp{0}) == 19'800'000'000'000LL,
          "Unix epoch is 05:30 IST");
    check(ist_midnight_of(Timestamp{-19'800'000'000'001LL}).ns_since_epoch()
              == -19'800'000'000'001LL - 86'399'999'999'999LL,
          "ist_midnight_of pre-epoch");
}

void test_ist_time_of_day_roundtrip()
{
    constexpr Timestamp kOpen{1787888700000000000LL};

    // Any instant in the same IST day yields the same midnight.
    check(ist_midnight_of(kOpen) == ist_midnight_of(ist_time_of_day(kOpen, 23, 59, 59)),
          "23:59:59 is the same IST day");
    check(ist_midnight_of(kOpen) == ist_midnight_of(ist_time_of_day(kOpen, 0, 0, 0)),
          "00:00:00 is the same IST day");
    check(ist_ns_since_midnight(ist_midnight_of(kOpen)) == 0,
          "midnight is 0 ns since midnight");
    check(ist_time_of_day(kOpen, 0, 0, 0, 1).ns_since_epoch()
              == ist_midnight_of(kOpen).ns_since_epoch() + 1,
          "the ns argument is added verbatim");
}

void test_ist_days_between()
{
    constexpr Timestamp kOpen{1787888700000000000LL};

    check(ist_days_between(kOpen, kOpen) == 0, "same instant == 0 days");
    check(ist_days_between(kOpen, kOpen + duration::days(1)) == 1, "+1 day");
    check(ist_days_between(kOpen + duration::days(1), kOpen) == -1, "-1 day");
    // Same IST day, 22 hours apart, still zero days.
    check(ist_days_between(ist_time_of_day(kOpen, 1, 0, 0),
                           ist_time_of_day(kOpen, 23, 0, 0)) == 0,
          "01:00 -> 23:00 same IST day == 0 days");
}

void test_duration_edge_cases()
{
    check(abs_duration(duration::seconds(-5)) == duration::seconds(5), "abs(-5s) == 5s");
    check(abs_duration(duration::seconds(5))  == duration::seconds(5), "abs(5s) == 5s");
    check(abs_duration(Duration{std::numeric_limits<std::int64_t>::min()})
              == Duration{std::numeric_limits<std::int64_t>::max()},
          "abs(INT64_MIN) == INT64_MAX, documented, not UB");

    // Largest whole hour count that still fits in int64 ns. Assert the exact
    // value — "is positive" would pass against an implementation that wrapped.
    check(duration::hours(duration::kMaxHours) == Duration{9'223'369'200'000'000'000LL},
          "hours(kMaxHours) exact value");
    check(duration::hours(duration::kMaxHours).raw() > 0,
          "hours(kMaxHours) is positive");
}

void test_time_type_safety_static_asserts()
{
    // The static_asserts at namespace scope do the work.
}

int main()
{
    std::printf("altair core/time tests\n");
    test_timestamp_traits();
    test_timestamp_affine_algebra();
    test_duration_constructors_and_accessors();
    test_ist_offset_market_open();
    test_ist_session_boundaries();
    test_ist_floor_semantics_pre_epoch();
    test_ist_time_of_day_roundtrip();
    test_ist_days_between();
    test_duration_edge_cases();
    test_time_type_safety_static_asserts();

    if (failures == 0) {
        std::printf("PASS\n");
    } else {
        std::printf("FAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
