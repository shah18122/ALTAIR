// P0-04 acceptance tests for core/time/exchange_ts.hpp.
// Plain main() (Catch2 not available until P0-05 brings vcpkg in).

#include <time/exchange_ts.hpp>
#include <time/timestamp.hpp>

#include <cstdio>
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

// ─────────────────────────────────────────────────────────────────────────
// Compile-time proof. to_utc is constexpr precisely so the epoch arithmetic —
// the part that must never be wrong — is checked by the compiler.
// ─────────────────────────────────────────────────────────────────────────
static_assert(altair::to_utc(1787888700LL,
                  altair::SourceTimeSpec{altair::TimeUnit::Seconds,
                                         altair::Duration{0}, false}).value()
              == altair::Timestamp{1787888700000000000LL},
              "Unix seconds convert at compile time");
static_assert(altair::to_utc(1787908500LL,
                  altair::SourceTimeSpec{altair::TimeUnit::Seconds,
                                         altair::Duration{0}, true}).value()
              == altair::Timestamp{1787888700000000000LL},
              "the IST-naive correction SUBTRACTS 5h30m, at compile time");
static_assert(!altair::to_utc(9'223'372'037LL,
                  altair::SourceTimeSpec{altair::TimeUnit::Seconds,
                                         altair::Duration{0}, false}).has_value(),
              "overflow is caught at compile time");

namespace {

// The anchor, from P0-02: 2026-08-28 09:15:00 IST, NSE market open.
constexpr Timestamp kNow{1787888700000000000LL};
// 2020-01-01T00:00:00Z — the plausibility floor.
constexpr Timestamp kFloor{1'577'836'800'000'000'000LL};

constexpr SourceTimeSpec kUnixS {TimeUnit::Seconds, Duration{0}, false};
constexpr SourceTimeSpec kUnixMs{TimeUnit::Millis,  Duration{0}, false};
constexpr SourceTimeSpec kNaiveS{TimeUnit::Seconds, Duration{0}, true};

} // namespace

void test_exchange_ts_unit_scaling()
{
    check(ns_per_unit(TimeUnit::Seconds) == 1'000'000'000LL, "ns_per_unit(Seconds)");
    check(ns_per_unit(TimeUnit::Millis)  == 1'000'000LL,     "ns_per_unit(Millis)");
    check(ns_per_unit(TimeUnit::Micros)  == 1'000LL,         "ns_per_unit(Micros)");
    check(ns_per_unit(TimeUnit::Nanos)   == 1LL,             "ns_per_unit(Nanos)");

    check(to_utc(1787888700LL, kUnixS).value() == Timestamp{1787888700000000000LL},
          "Unix seconds -> ns");
    check(to_utc(1787888700000LL, kUnixMs).value() == Timestamp{1787888700000000000LL},
          "Unix millis -> ns, same instant");
    check(to_utc(0, kUnixS).value() == Timestamp::epoch(), "raw 0 is the epoch");
    check(to_utc(-1, kUnixS).value() == Timestamp{-1'000'000'000LL},
          "pre-epoch raw is legal here");
}

void test_exchange_ts_epoch_offset()
{
    // A source counting seconds from 2010-01-01.
    constexpr Duration k2010{1'262'304'000'000'000'000LL};
    constexpr SourceTimeSpec s2010{TimeUnit::Seconds, k2010, false};

    // 1787888700 - 1262304000 = 525584700 s since the 2010 epoch.
    check(to_utc(525'584'700LL, s2010).value() == Timestamp{1787888700000000000LL},
          "2010-epoch seconds resolve to the anchor");
    check(to_utc(0, s2010).value() == Timestamp{1'262'304'000'000'000'000LL},
          "raw 0 is the 2010 epoch itself");

    // A source whose epoch predates Unix (negative offset) round-trips too.
    constexpr SourceTimeSpec s1960{TimeUnit::Seconds,
                                   Duration{-315'619'200'000'000'000LL}, false};
    check(to_utc(0, s1960).value() == Timestamp{-315'619'200'000'000'000LL},
          "negative epoch offset");
}

void test_exchange_ts_ist_naive_correction()
{
    // The naive raw is 19800 s LARGER than the truth; the correction subtracts.
    check(to_utc(1787908500LL, kNaiveS).value() == Timestamp{1787888700000000000LL},
          "IST-naive raw corrects to the true instant");

    // Uncorrected, the same raw sits 5h30m in the future.
    check(to_utc(1787908500LL, kUnixS).value() - to_utc(1787908500LL, kNaiveS).value()
              == kIstOffset,
          "uncorrected reading is exactly kIstOffset ahead");

    // And a naive spec applied to an already-correct raw is 5h30m behind.
    check(to_utc(1787888700LL, kNaiveS).value()
              == Timestamp{1787888700000000000LL - 19'800'000'000'000LL},
          "naive spec on a true raw lands 5h30m in the past");
}

void test_exchange_ts_overflow_rejected()
{
    check(max_raw_for(TimeUnit::Seconds) == 9'223'372'036LL,        "max_raw_for(Seconds)");
    check(max_raw_for(TimeUnit::Millis)  == 9'223'372'036'854LL,    "max_raw_for(Millis)");
    check(max_raw_for(TimeUnit::Micros)  == 9'223'372'036'854'775LL,"max_raw_for(Micros)");
    check(max_raw_for(TimeUnit::Nanos)   == std::numeric_limits<std::int64_t>::max(),
          "max_raw_for(Nanos)");

    check(to_utc(9'223'372'037LL, kUnixS).error() == TsReject::Unconvertible,
          "one past the seconds limit is rejected");
    check(to_utc(-9'223'372'037LL, kUnixS).error() == TsReject::Unconvertible,
          "one past the negative seconds limit is rejected");
    check(to_utc(9'223'372'036LL, kUnixS).has_value(),
          "exactly at the limit converts");

    check(to_utc(std::numeric_limits<std::int64_t>::min(), kUnixS).error()
              == TsReject::Unconvertible,
          "INT64_MIN is rejected without UB");

    // Overflow via the epoch offset rather than the scaling.
    constexpr SourceTimeSpec far{TimeUnit::Nanos,
                                 Duration{std::numeric_limits<std::int64_t>::max()},
                                 false};
    check(to_utc(std::numeric_limits<std::int64_t>::max(), far).error()
              == TsReject::Unconvertible,
          "epoch-offset overflow is rejected");
}

namespace {

const Duration kSkew = duration::seconds(2);
// Monotonic OFF — for the tests that isolate floor/ceiling.
const PlausibilityGate::Config cfgFC  {kFloor, kSkew, false, false, {}};
// Monotonic ON — for the tests that exercise the watermark.
const PlausibilityGate::Config cfgMono{kFloor, kSkew, true,  false, {}};

} // namespace

void test_plausibility_floor_and_ceiling()
{
    PlausibilityGate g{cfgFC};

    check(g.admit(kNow, kNow).value() == kNow, "exactly now is admitted");
    check(g.admit(kNow + kSkew, kNow).value() == kNow + kSkew,
          "exactly at the ceiling is admitted");
    check(g.admit(kNow + kSkew + duration::nanos(1), kNow).error()
              == TsReject::AfterCeiling,
          "one ns past the ceiling is rejected (look-ahead guard)");
    check(g.admit(kFloor - duration::nanos(1), kNow).error() == TsReject::BeforeFloor,
          "one ns below the floor is rejected");
    check(g.admit(kFloor, kNow).value() == kFloor, "the floor itself is admitted");
}

void test_plausibility_monotonic()
{
    PlausibilityGate g{cfgMono};

    check(g.admit(kNow, kNow).has_value(), "first timestamp admitted");
    check(g.admit(kNow, kNow).has_value(), "an exact repeat IS admitted");
    check(g.admit(kNow - duration::nanos(1), kNow).error() == TsReject::Regressed,
          "a regression is rejected");
    check(g.last_admitted() == kNow, "a rejection does NOT move the watermark");
    check(g.admit(kNow + duration::millis(1), kNow).has_value(), "forward progress admitted");
    check(g.last_admitted() == kNow + duration::millis(1), "watermark advanced");

    // With the check off, the same regression is admitted.
    PlausibilityGate g2{cfgFC};
    check(g2.admit(kNow, kNow).has_value(), "loose gate admits the first");
    check(g2.admit(kNow - duration::seconds(1), kNow).has_value(),
          "loose gate admits a regression");
}

void test_plausibility_session_window()
{
    // 09:15:00 to 15:30:00 IST, supplied by the CALLER — no shipped constant.
    constexpr SessionWindow w{33'300'000'000'000LL, 55'800'000'000'000LL};

    check(is_in_session(kNow, w), "09:15:00 exactly is in session");
    check(is_in_session(ist_time_of_day(kNow, 15, 30, 0), w),
          "15:30:00 exactly is in session (inclusive close)");
    check(!is_in_session(ist_time_of_day(kNow, 9, 14, 59), w),
          "09:14:59 is out of session");
    check(!is_in_session(ist_time_of_day(kNow, 15, 30, 1), w),
          "15:30:01 is out of session");

    // enforce_session off (the default): a pre-open tick is a valid instant.
    const PlausibilityGate::Config off{kFloor, duration::hours(24), true, false, w};
    PlausibilityGate g_off{off};
    check(g_off.admit(ist_time_of_day(kNow, 8, 0, 0), kNow).has_value(),
          "pre-open tick admitted when the session is not enforced");

    // enforce_session on: the same tick is rejected.
    const PlausibilityGate::Config on{kFloor, duration::hours(24), true, true, w};
    PlausibilityGate g_on{on};
    check(g_on.admit(ist_time_of_day(kNow, 8, 0, 0), kNow).error()
              == TsReject::OutsideSession,
          "pre-open tick rejected when the session is enforced");
}

void test_plausibility_counters_and_order()
{
    PlausibilityGate g{cfgMono};
    (void)g.admit(kNow, kNow);                                  // admitted
    (void)g.admit(kFloor - duration::nanos(1), kNow);           // before_floor
    (void)g.admit(kNow + duration::hours(1), kNow);             // after_ceiling
    (void)g.admit(kNow - duration::nanos(1), kNow);             // regressed

    check(g.stats().admitted == 1,        "counter: admitted");
    check(g.stats().before_floor == 1,    "counter: before_floor");
    check(g.stats().after_ceiling == 1,   "counter: after_ceiling");
    check(g.stats().regressed == 1,       "counter: regressed");
    check(g.stats().outside_session == 0, "counter: outside_session untouched");

    // A timestamp that genuinely fails BOTH floor and ceiling must report the
    // FIRST check only, and move only that one counter.
    //   floor = kNow (2026), skew = 0, local_now = kFloor (2020)
    //   mid   = kFloor + 1000 days -> under the floor AND over the ceiling
    const PlausibilityGate::Config strict{kNow, duration::nanos(0), true, false, {}};
    PlausibilityGate g3{strict};
    const Timestamp mid = kFloor + duration::days(1000);
    check(mid < kNow, "precondition: mid is under the floor");
    check(mid > kFloor, "precondition: mid is over the ceiling");
    check(g3.admit(mid, kFloor).error() == TsReject::BeforeFloor,
          "first failing check wins: BeforeFloor, not AfterCeiling");
    check(g3.stats().before_floor == 1,  "only the first counter moved");
    check(g3.stats().after_ceiling == 0, "the second check never ran");

    g.reset();
    check(g.stats().admitted == 0 && g.stats().before_floor == 0, "reset clears counters");
    check(g.last_admitted() == Timestamp::epoch(), "reset clears the watermark");
}

int main()
{
    std::printf("altair core/time exchange_ts tests\n");
    test_exchange_ts_unit_scaling();
    test_exchange_ts_epoch_offset();
    test_exchange_ts_ist_naive_correction();
    test_exchange_ts_overflow_rejected();
    test_plausibility_floor_and_ceiling();
    test_plausibility_monotonic();
    test_plausibility_session_window();
    test_plausibility_counters_and_order();

    if (failures == 0) {
        std::printf("PASS\n");
    } else {
        std::printf("FAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
