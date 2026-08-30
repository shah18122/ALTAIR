# P0-04 — `core/time/exchange_ts`: per-source epoch normalisation + plausibility gate

> Phase 0 · Card 4 of 14 · Status: DONE (implemented by Claude, 2026-08-29 — see LEDGER)
> Depends on: P0-01 (`core/types/units.hpp`) · P0-02 (`core/time/timestamp.hpp`) — both DONE
> Paste everything below the line into DeepSeek V4 as a single prompt.
>
> **CONFIRMED 2026-08-31 against Zerodha's own client** (`gokiteconnect`, read
> for protocol facts only — nothing vendored, no code copied). Decision (a)
> below was right, and for a sharper reason than anticipated: **Kite has two
> time formats with different semantics.**
>
> | Source | Wire form | Spec |
> |---|---|---|
> | Binary ticker | `uint32` big-endian **seconds since the Unix epoch, genuine UTC** — `ticker.go` builds it with `time.Unix(...)` | `{Seconds, Duration{0}, ist_naive = false}` |
> | REST / CSV | zoneless `"2006-01-02 15:04:05"`, parsed `ParseInLocation(..., Asia/Kolkata)` | effectively `ist_naive = true` |
>
> Hardcoding one answer per broker would have been wrong for whichever half it
> did not cover. Full-mode offsets: `exchange_timestamp` at `b[60:64]`,
> `last_trade_time` at `b[44:48]`; index-full puts the timestamp at `b[28:32]`.
> Those are `uint32` seconds, so the field survives to 2106 — but anything
> reading it into an `int32` breaks in 2038, which `to_utc(std::int64_t, ...)`
> already prevents.
>
> **Architect's note (not part of the prompt).** This is the card where gate 7
> has teeth: a wrong answer admits a corrupt timestamp into the pipeline, and
> every downstream decision inherits it. Three decisions were made here rather
> than left open: (a) **no broker's epoch or unit is hardcoded** — they are
> per-source configuration, because getting one wrong silently shifts every tick
> from that feed; (b) **no `kNseEquitySession` constant ships** — session times
> change (muhurat, special sessions) and CLAUDE.md rule 1 bans baked-in market
> parameters; (c) `admit()` does **not** check the session by default — a
> pre-open tick has a perfectly valid timestamp, and conflating "implausible"
> with "outside trading hours" would drop legitimate data.

---

## 1. CONTEXT

You are implementing broker-timestamp normalisation for Altair, a C++23
low-latency trading engine for Indian equity markets.

Every feed states time differently. One sends seconds since the Unix epoch,
another milliseconds, another seconds since a vendor-specific epoch. Some encode
**IST wall-clock as though it were UTC** — a 5 h 30 m error that looks entirely
plausible, because the number is a valid timestamp for a real instant. It is
simply the wrong instant, and it is in the *future*, which is the one direction
that silently injects look-ahead into a backtest.

CLAUDE.md rule 7: **strategies read time off the tick, never off a wall clock.**
That makes this file the single place where a bad clock can poison everything
downstream. Rule 9: **failing loud beats trading wrong** — an implausible
timestamp is rejected and counted, never clamped, never guessed.

P0-02 gave you `Timestamp`, `Duration`, and `kIstOffset`. P0-03 gave you a local
hardware clock to compare against. This card converts raw broker integers into
`Timestamp`, and decides which ones to believe.

---

## 2. FILE MANIFEST

Create exactly these two files, and modify exactly one.

```
CREATE   core/time/exchange_ts.hpp
CREATE   core/time/tests/test_exchange_ts.cpp
MODIFY   core/time/CMakeLists.txt
```

Header-only — there is no `.cpp`. Everything here is integer arithmetic and must
be `constexpr`, so the epoch conversions can be proven at compile time.

In `core/time/CMakeLists.txt`, add an `altair_exchange_ts_test` executable from
`tests/test_exchange_ts.cpp` with
`add_test(NAME exchange_ts COMMAND altair_exchange_ts_test)`. `altair_time` is
already a STATIC library (P0-03) — do not change its type, and leave the
`altair_time_test` and `altair_tsc_clock_test` registrations alone.

**Do not touch** `core/CMakeLists.txt`, the root `CMakeLists.txt`, `vcpkg.json`,
`core/types/`, or any other file in `core/time/`.

---

## 3. INTERFACE CONTRACT

Implement **exactly** these declarations in `core/time/exchange_ts.hpp`, in this
order, inside `namespace altair`. Do not add, remove, rename, or re-order any
public entity.

```cpp
#pragma once

#include <time/timestamp.hpp>
#include <types/units.hpp>

#include <cstdint>
#include <expected>
#include <limits>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// The unit a source states its raw integer timestamp in.
// ─────────────────────────────────────────────────────────────────────────
enum class TimeUnit : std::uint8_t { Seconds, Millis, Micros, Nanos };

// ─────────────────────────────────────────────────────────────────────────
// Why a raw timestamp was not believed. Never silently corrected.
// ─────────────────────────────────────────────────────────────────────────
enum class TsReject : std::uint8_t {
    Unconvertible,    // scaling to int64 nanoseconds would overflow
    BeforeFloor,      // earlier than the configured plausible floor
    AfterCeiling,     // later than local_now + max_future_skew (LOOK-AHEAD guard)
    Regressed,        // earlier than the last admitted timestamp from this source
    OutsideSession    // a valid instant, but not inside the trading window
};

// ─────────────────────────────────────────────────────────────────────────
// How one feed states time. LOADED FROM CONFIG, never hardcoded per broker —
// an epoch or unit guessed wrong shifts every tick from that source by a
// constant, which looks like a working feed until it is reconciled.
// ─────────────────────────────────────────────────────────────────────────
struct SourceTimeSpec {
    /// The unit of the raw integer.
    TimeUnit unit;

    /// (source epoch) - (Unix epoch). UNIT: nanoseconds.
    /// Zero for a Unix-epoch source. Positive for a later epoch: a source
    /// counting from 2010-01-01 has +1'262'304'000'000'000'000 ns.
    Duration epoch_offset;

    /// True iff the raw value encodes IST wall-clock as though it were UTC.
    /// Such a value is kIstOffset (5 h 30 m) LARGER than the truth, i.e. it
    /// stamps every tick 19800 s into the future. Corrected by subtraction.
    bool ist_naive;
};

/// Nanoseconds in one unit of `u`. UNIT: ns/unit. PRECONDITION: none.
[[nodiscard]] constexpr std::int64_t ns_per_unit(TimeUnit u) noexcept;

/// Largest |raw| that can be scaled to int64 nanoseconds without overflow,
/// for unit `u`. UNIT: units of `u`. PRECONDITION: none.
[[nodiscard]] constexpr std::int64_t max_raw_for(TimeUnit u) noexcept;

/// Convert one raw broker integer to a UTC instant.
/// UNIT: raw is in spec.unit since spec's epoch; result is ns since the Unix
/// epoch, UTC. Total: no input of any int64 value invokes UB.
/// PRECONDITION: none. Returns Unconvertible rather than wrapping.
[[nodiscard]] ALTAIR_HOT constexpr std::expected<Timestamp, TsReject>
to_utc(std::int64_t raw, SourceTimeSpec spec) noexcept;

// ─────────────────────────────────────────────────────────────────────────
// A trading window, as offsets from IST midnight.
//
// NO CONSTANT IS SHIPPED FOR NSE OR BSE. Session times change — muhurat
// sessions, special sessions, exchange notices — and CLAUDE.md rule 1 bans
// baked-in market parameters. The caller fills this from config.
// ─────────────────────────────────────────────────────────────────────────
struct SessionWindow {
    /// Inclusive start. UNIT: ns since IST midnight, [0, 86'400'000'000'000).
    std::int64_t open_ns;
    /// Inclusive end. UNIT: ns since IST midnight, [0, 86'400'000'000'000).
    std::int64_t close_ns;
};

/// True iff `t` falls inside `w` on its own IST day, bounds inclusive.
/// UNIT: none. PRECONDITION: w.open_ns <= w.close_ns, both in range.
[[nodiscard]] constexpr bool is_in_session(Timestamp t, SessionWindow w) noexcept;

// ─────────────────────────────────────────────────────────────────────────
// PlausibilityGate — one per feed source. NOT thread-safe: it carries the
// last-admitted watermark, so it belongs to exactly one decoder thread.
// ─────────────────────────────────────────────────────────────────────────
class PlausibilityGate {
public:
    struct Config {
        /// Reject anything earlier than this. UNIT: ns since the Unix epoch.
        Timestamp floor;

        /// Tolerance for a source clock running ahead of ours.
        /// UNIT: nanoseconds. Anything beyond local_now + this is rejected —
        /// this is the look-ahead guard, not a nicety.
        Duration max_future_skew;

        /// Reject a timestamp earlier than the last admitted one.
        bool require_monotonic;

        /// Also require the timestamp to fall inside `session`.
        /// Default OFF: a pre-open tick is a valid instant.
        bool enforce_session;

        /// Only consulted when enforce_session is true.
        SessionWindow session;
    };

    struct Stats {
        std::uint64_t admitted;
        std::uint64_t before_floor;
        std::uint64_t after_ceiling;
        std::uint64_t regressed;
        std::uint64_t outside_session;
    };

    constexpr explicit PlausibilityGate(Config cfg) noexcept;

    /// Decide whether to believe `exchange_ts`, given our own clock reading.
    /// UNIT: both arguments are ns since the Unix epoch, UTC.
    /// Checks run in a fixed order — floor, ceiling, monotonic, session — and
    /// the FIRST failure is reported. Exactly one counter moves per call.
    /// PRECONDITION: local_now comes from a clock this process trusts.
    [[nodiscard]] ALTAIR_HOT constexpr std::expected<Timestamp, TsReject>
    admit(Timestamp exchange_ts, Timestamp local_now) noexcept;

    /// Running tally. Never reset implicitly — a feed's reject rate is a
    /// health signal that P9 drift detection consumes.
    [[nodiscard]] constexpr const Stats& stats() const noexcept;

    [[nodiscard]] constexpr const Config& config() const noexcept;

    /// The last admitted timestamp, or the epoch if nothing has been admitted.
    [[nodiscard]] constexpr Timestamp last_admitted() const noexcept;

    /// Clear the watermark and the counters. For session rollover and tests.
    constexpr void reset() noexcept;

private:
    Config cfg_{};
    Stats stats_{};
    Timestamp last_{};
};

} // namespace altair
```

---

## 4. BEHAVIOURAL SPEC

1. `ns_per_unit` returns `1'000'000'000` for `Seconds`, `1'000'000` for
   `Millis`, `1'000` for `Micros`, `1` for `Nanos`.
2. `max_raw_for(u)` is `INT64_MAX / ns_per_unit(u)`: `9'223'372'036` for
   `Seconds`, `9'223'372'036'854` for `Millis`, `9'223'372'036'854'775` for
   `Micros`, `INT64_MAX` for `Nanos`. These match `duration::kMax*` from P0-02 —
   the same limits, reached from the other direction.
3. `to_utc` performs, **in exactly this order**:
   a. reject with `Unconvertible` if `|raw| > max_raw_for(spec.unit)`;
   b. `scaled = raw * ns_per_unit(spec.unit)`;
   c. add `spec.epoch_offset`, rejecting `Unconvertible` on int64 overflow;
   d. if `spec.ist_naive`, **subtract** `kIstOffset`, rejecting `Unconvertible`
      on int64 overflow.
   The order matters: the IST correction applies to the instant, after the
   source epoch has been resolved.
4. `to_utc` is **total** — no int64 input, including `INT64_MIN`, may invoke
   undefined behaviour. Check before every operation, never after.
5. The IST-naive correction is a **subtraction**. A naive encoder takes IST
   wall-clock 09:15:00 and writes the integer for 09:15:00 UTC, which is
   `19800` s *larger* than the true instant. Adding would double the error to
   11 h and stamp the tick further into the future. For the 2026-08-28 market
   open the naive raw is `1'787'908'500` s and the truth is `1'787'888'700` s.
6. `is_in_session(t, w)` compares `ist_ns_since_midnight(t)` against
   `[w.open_ns, w.close_ns]`, **both bounds inclusive**. A tick stamped exactly
   at the close belongs to the session.
7. `admit` runs its checks in this fixed order and returns on the first failure:
   `BeforeFloor` → `AfterCeiling` → `Regressed` → `OutsideSession`.
8. The ceiling is `local_now + cfg.max_future_skew`, compared with `>`. A
   timestamp exactly at the ceiling is admitted; one nanosecond past is not.
9. `Regressed` is only checked when `cfg.require_monotonic`. The comparison is
   `exchange_ts < last_admitted()`; an exact repeat of the last timestamp is
   **admitted**, because two trades can share a millisecond.
10. `OutsideSession` is only checked when `cfg.enforce_session`.
11. Exactly one `Stats` counter is incremented per `admit` call. On success,
    `admitted` increments and the watermark advances to `exchange_ts`. On any
    rejection the watermark is **unchanged** — a rejected timestamp must not
    poison the monotonic check for the next one.
12. `reset()` zeroes every counter and sets the watermark back to
    `Timestamp::epoch()`. `Config` is untouched.

---

## 5. CONSTRAINTS

- C++23. Standard library only, plus P0-01 and P0-02 headers. No new dependency.
- **Header-only, and every function `constexpr`.** The acceptance tests prove
  the epoch arithmetic with `static_assert`, which is only possible if the whole
  conversion path is usable in a constant expression.
- **No `<chrono>`.** No clock is read here — `local_now` is passed in. A
  function in this file that reads a clock would make the backtest and the live
  path diverge, which CLAUDE.md rule 6 forbids outright.
- Internal helpers go in `namespace altair::detail`, **never an anonymous
  namespace** — this is a header. Name any overflow helper distinctly (e.g.
  `add_overflows_i64`) so it cannot collide with `detail::mul_overflows` from
  P0-01, which is a different function with a different signature.
- No exceptions, no `throw`, no dynamic allocation, no `iostream`.
- No `using namespace` at file scope in the header.
- Compiles clean at `/W4` and `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion -Wold-style-cast`.
- Tests are a plain `int main()`. Catch2 is still unavailable.

---

## 6. ACCEPTANCE TESTS

`core/time/tests/test_exchange_ts.cpp`, plain `main()`, `check(bool, const char*)`
helper counting failures, returns 0 only if all pass. Use exactly these names:

```cpp
void test_exchange_ts_unit_scaling();
void test_exchange_ts_epoch_offset();
void test_exchange_ts_ist_naive_correction();
void test_exchange_ts_overflow_rejected();
void test_plausibility_floor_and_ceiling();
void test_plausibility_monotonic();
void test_plausibility_session_window();
void test_plausibility_counters_and_order();
```

### Anchor values — all derived, all verified

The anchor is P0-02's: **2026-08-28 09:15:00 IST = `1787888700` s =
`1787888700000000000` ns** since the Unix epoch. Every constant below was
re-derived with a civil-date algorithm, not from memory:

| Instant | Unix seconds |
|---|---|
| 1980-01-01T00:00:00Z | `315532800` |
| 2010-01-01T00:00:00Z | `1262304000` |
| 2020-01-01T00:00:00Z | `1577836800` |
| 2026-08-28 09:15 IST | `1787888700` |
| same, IST-naive raw   | `1787908500` |

**test_exchange_ts_unit_scaling**
```
ns_per_unit(TimeUnit::Seconds) == 1'000'000'000
ns_per_unit(TimeUnit::Millis)  == 1'000'000
ns_per_unit(TimeUnit::Micros)  == 1'000
ns_per_unit(TimeUnit::Nanos)   == 1

constexpr SourceTimeSpec unix_s{TimeUnit::Seconds, Duration{0}, false};
constexpr SourceTimeSpec unix_ms{TimeUnit::Millis, Duration{0}, false};

to_utc(1787888700LL,    unix_s ).value() == Timestamp{1787888700000000000LL}
to_utc(1787888700000LL, unix_ms).value() == Timestamp{1787888700000000000LL}
to_utc(0, unix_s).value() == Timestamp::epoch()
to_utc(-1, unix_s).value() == Timestamp{-1'000'000'000LL}     // pre-epoch is legal here
```

**test_exchange_ts_epoch_offset**
```
// A source counting seconds from 2010-01-01.
constexpr Duration k2010{1'262'304'000'000'000'000LL};
constexpr SourceTimeSpec s2010{TimeUnit::Seconds, k2010, false};

// 1787888700 - 1262304000 = 525584700 s since the 2010 epoch.
to_utc(525'584'700LL, s2010).value() == Timestamp{1787888700000000000LL}
to_utc(0, s2010).value() == Timestamp{1'262'304'000'000'000'000LL}   // the 2010 epoch itself

// A source with an epoch BEFORE Unix (negative offset) round-trips too.
constexpr SourceTimeSpec s1960{TimeUnit::Seconds, Duration{-315'619'200'000'000'000LL}, false};
to_utc(0, s1960).value() == Timestamp{-315'619'200'000'000'000LL}
```

**test_exchange_ts_ist_naive_correction**
The one that catches the 5h30m trap:
```
constexpr SourceTimeSpec naive{TimeUnit::Seconds, Duration{0}, true};
constexpr SourceTimeSpec plain{TimeUnit::Seconds, Duration{0}, false};

// The naive raw is 19800 s LARGER than the truth; correction subtracts.
to_utc(1787908500LL, naive).value() == Timestamp{1787888700000000000LL}
// Uncorrected, the same raw is 5h30m in the future - assert the gap explicitly.
to_utc(1787908500LL, plain).value() - to_utc(1787908500LL, naive).value()
    == kIstOffset
// And the naive reading of the TRUE raw is 5h30m in the past.
to_utc(1787888700LL, naive).value() == Timestamp{1787888700000000000LL - 19'800'000'000'000LL}
```

**test_exchange_ts_overflow_rejected**
```
max_raw_for(TimeUnit::Seconds) == 9'223'372'036LL
max_raw_for(TimeUnit::Millis)  == 9'223'372'036'854LL
max_raw_for(TimeUnit::Micros)  == 9'223'372'036'854'775LL
max_raw_for(TimeUnit::Nanos)   == std::numeric_limits<std::int64_t>::max()

constexpr SourceTimeSpec unix_s{TimeUnit::Seconds, Duration{0}, false};
to_utc(9'223'372'037LL,  unix_s).error() == TsReject::Unconvertible
to_utc(-9'223'372'037LL, unix_s).error() == TsReject::Unconvertible
to_utc(9'223'372'036LL,  unix_s).has_value()          // exactly at the limit is fine

// INT64_MIN must not invoke UB anywhere on the path.
to_utc(std::numeric_limits<std::int64_t>::min(), unix_s).error() == TsReject::Unconvertible

// Overflow via the epoch offset, not the scaling.
constexpr SourceTimeSpec far{TimeUnit::Nanos,
                             Duration{std::numeric_limits<std::int64_t>::max()}, false};
to_utc(std::numeric_limits<std::int64_t>::max(), far).error() == TsReject::Unconvertible
```

Shared by the three gate tests:
```
constexpr Timestamp kFloor{1'577'836'800'000'000'000LL};   // 2020-01-01
constexpr Timestamp kNow  {1787888700000000000LL};         // the anchor
const Duration kSkew = duration::seconds(2);

// Monotonic OFF - for the tests that isolate floor/ceiling.
PlausibilityGate::Config cfgFC  {kFloor, kSkew, false, false, {}};
// Monotonic ON - for the tests that exercise the watermark.
PlausibilityGate::Config cfgMono{kFloor, kSkew, true,  false, {}};
```

**test_plausibility_floor_and_ceiling**
Uses `cfgFC`. Monotonic must be **off** here: the sequence below walks backwards
in time on purpose, and a monotonic gate would report `Regressed` on the last
line instead of the intended result.
```
PlausibilityGate g{cfgFC};

g.admit(kNow, kNow).value() == kNow                                  // exactly now
g.admit(kNow + kSkew, kNow).value() == kNow + kSkew                  // exactly at the ceiling
g.admit(kNow + kSkew + duration::nanos(1), kNow).error()
    == TsReject::AfterCeiling                                        // 1 ns past it
g.admit(kFloor - duration::nanos(1), kNow).error() == TsReject::BeforeFloor
g.admit(kFloor, kNow).value() == kFloor                              // the floor itself is fine
```

**test_plausibility_monotonic**
```
PlausibilityGate g{cfgMono};
g.admit(kNow, kNow).has_value()
g.admit(kNow, kNow).has_value()                // an exact repeat IS admitted
g.admit(kNow - duration::nanos(1), kNow).error() == TsReject::Regressed
g.last_admitted() == kNow                      // a rejection must NOT move the watermark
g.admit(kNow + duration::millis(1), kNow).has_value()
g.last_admitted() == kNow + duration::millis(1)

// With the check off, the same regression is admitted.
PlausibilityGate g2{cfgFC};
g2.admit(kNow, kNow).has_value()
g2.admit(kNow - duration::seconds(1), kNow).has_value()
```

**test_plausibility_session_window**
```
// 09:15:00 to 15:30:00 IST, supplied by the CALLER - no shipped constant.
constexpr SessionWindow w{33'300'000'000'000LL, 55'800'000'000'000LL};

is_in_session(Timestamp{1787888700000000000LL}, w) == true    // 09:15:00 exactly
is_in_session(ist_time_of_day(Timestamp{1787888700000000000LL}, 15, 30, 0), w) == true
is_in_session(ist_time_of_day(Timestamp{1787888700000000000LL},  9, 14, 59), w) == false
is_in_session(ist_time_of_day(Timestamp{1787888700000000000LL}, 15, 30, 1), w) == false

// enforce_session off (the default): a pre-open tick is admitted.
PlausibilityGate::Config off{kFloor, duration::hours(24), true, false, w};
PlausibilityGate{off}.admit(ist_time_of_day(kNow, 8, 0, 0), kNow).has_value()

// enforce_session on: the same tick is rejected.
PlausibilityGate::Config on{kFloor, duration::hours(24), true, true, w};
PlausibilityGate{on}.admit(ist_time_of_day(kNow, 8, 0, 0), kNow).error()
    == TsReject::OutsideSession
```

**test_plausibility_counters_and_order**
```
PlausibilityGate g{cfgMono};
g.admit(kNow, kNow);                                    // admitted, watermark = kNow
g.admit(kFloor - duration::nanos(1), kNow);             // before_floor
g.admit(kNow + duration::hours(1), kNow);               // after_ceiling
g.admit(kNow - duration::nanos(1), kNow);               // regressed

g.stats().admitted == 1
g.stats().before_floor == 1
g.stats().after_ceiling == 1
g.stats().regressed == 1
g.stats().outside_session == 0

// A timestamp that genuinely fails BOTH floor and ceiling must report the
// FIRST check only, and move only that one counter. Construct it properly:
// put the floor ABOVE local_now, so a mid value is under the floor AND over
// the ceiling at the same time.
//   floor = kNow (2026-08-28), skew = 0, local_now = kFloor (2020-01-01)
//   mid   = kFloor + 1000 days  ->  under the floor, over the ceiling
PlausibilityGate::Config strict{kNow, duration::nanos(0), true, false, {}};
PlausibilityGate g3{strict};
const Timestamp mid = kFloor + duration::days(1000);
g3.admit(mid, kFloor).error() == TsReject::BeforeFloor    // not AfterCeiling
g3.stats().before_floor  == 1
g3.stats().after_ceiling == 0                             // the second check never ran

g.reset();
g.stats().admitted == 0 && g.stats().before_floor == 0
g.last_admitted() == Timestamp::epoch()
```

### Compile-time proof

At namespace scope, because `to_utc` is `constexpr` and the epoch arithmetic is
the part that must never be wrong:

```cpp
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
```

---

## 7. FORBIDDEN

- Adding a file not in the manifest. Modifying `core/CMakeLists.txt`, the root
  `CMakeLists.txt`, `vcpkg.json`, `core/types/`, or any other `core/time/` file.
- Changing any signature in the interface contract.
- **Shipping a named constant for any exchange's session times, epoch, or unit.**
  No `kNseSession`, no `kKiteEpoch`, no `kXtsEpoch`. They are configuration.
  (CLAUDE.md rule 1.)
- **Reading any clock.** `local_now` is a parameter. A clock read here would make
  replay and live diverge (rule 6) and would make `admit` untestable.
- Including `<chrono>`.
- Clamping, saturating, or "correcting" an implausible timestamp. Reject it and
  count it. (Rule 9.)
- Advancing the monotonic watermark on a rejected timestamp.
- **Adding `kIstOffset` for an `ist_naive` source instead of subtracting it.**
  That doubles the error to 11 h. See spec item 5.
- Signed overflow anywhere on the `to_utc` path, including for `INT64_MIN`.
- An anonymous namespace in the header, or a helper in `altair::detail` whose
  name collides with P0-01's `mul_overflows`.
- Writing a test that asserts whatever your implementation happens to produce.
  The expected values in §6 are the specification.

---

## 8. RULES

```
RULES — violating any of these fails review:
1. Produce complete files. No "...", no "rest unchanged", no placeholder bodies.
2. Do not create, rename, or delete any file not in the File Manifest.
3. Do not change any signature in the Interface Contract. If you believe a
   signature is wrong, implement it as specified AND add a comment block at the
   top of the file titled "CONTRACT OBJECTION" explaining why. Do not act on it.
4. Do not add any third-party dependency. Only what vcpkg.json already lists.
5. No exceptions on the hot path. Return std::expected<T, Error>.
6. No dynamic allocation inside any function marked ALTAIR_HOT.
7. No `using namespace` at file scope in a header.
8. Every public function gets a doc comment stating units and preconditions.
9. Write the acceptance tests exactly as named. Do not rename or merge them.
10. If a requirement is ambiguous, implement the most conservative reading and
    list the ambiguity under "ASSUMPTIONS" at the end of your response.
```

Return the three files in full, then your ASSUMPTIONS section, which must state
your independent check that `1787908500 - 19800 == 1787888700`, and confirm the
direction of the IST-naive correction in your own words.
