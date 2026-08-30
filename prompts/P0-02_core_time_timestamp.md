# P0-02 — `core/time/timestamp.hpp`: affine time algebra and IST

> Phase 0 · Card 2 of 14 · Status: DONE (implemented by Claude, 2026-08-29 — see LEDGER)
> Depends on: P0-01 (`core/types/units.hpp`) — DONE
> Paste everything below the line into DeepSeek V4 as a single prompt.
>
> **Pre-send review done.** Every expected value in §6 was re-derived independently
> (anchor via a civil-date algorithm, not by repeating the card's own arithmetic)
> and the whole interface contract was compiled against the real P0-01 header at
> `/W4 /permissive-` on MSVC 19.51.36256: contract implementable, all
> `static_assert`s hold, all §6 assertions pass. Three defects found in the spec
> prose and fixed: item 8 was int32 overflow (UB), item 9 was uncompilable
> (`StrongInt` defines no `operator/`), and a §6 comment said "14 hours" for a
> 22-hour span. Literal coverage was added to §6 — all six `operator""` literals
> were in the contract but exercised by no test.

---

## 1. CONTEXT

You are implementing the time layer of Altair, a C++23 low-latency trading
engine for Indian equity markets.

**Timestamps live in an affine space; durations live in the associated vector
space.** An instant minus an instant is a duration. An instant plus a duration
is an instant. **An instant plus an instant is meaningless** — exactly as adding
two positions in space is meaningless — and must not compile. This is not
stylistic; it is the same dimensional discipline that makes `Price * Lots`
ill-formed in `core/types`.

All instants are nanoseconds since the Unix epoch, **UTC**. Indian markets run
on IST, which is UTC+05:30 with **no daylight saving**, so the conversion is a
constant offset — but session boundaries are IST, so IST helpers are needed.

This card is pure header, `constexpr`, no platform code and no clock reading.
Reading the TSC is P0-03; normalising broker timestamps is P0-04.

---

## 2. FILE MANIFEST

Create exactly these three files. Nothing else.

```
core/time/timestamp.hpp
core/time/CMakeLists.txt
core/time/tests/test_timestamp.cpp
```

---

## 3. INTERFACE CONTRACT

Implement **exactly** these declarations in `core/time/timestamp.hpp`, in this
order, inside `namespace altair`. Do not add, remove, rename, or re-order any
public entity.

```cpp
#pragma once

#include <types/units.hpp>

#include <cstdint>
#include <compare>
#include <limits>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// Duration — a signed span of time in nanoseconds. A vector quantity:
// durations add, subtract, negate, and scale freely.
// ─────────────────────────────────────────────────────────────────────────
struct DurationTag;
using Duration = StrongInt<DurationTag, std::int64_t>;   // nanoseconds

// ─────────────────────────────────────────────────────────────────────────
// Timestamp — an instant, nanoseconds since the Unix epoch, UTC.
// An AFFINE point. Timestamp + Timestamp does not exist, by design.
// Range is +/- 292 years around 1970, which covers every use in this system.
// ─────────────────────────────────────────────────────────────────────────
class Timestamp {
public:
    constexpr Timestamp() noexcept = default;

    /// PRECONDITION: ns is nanoseconds since 1970-01-01T00:00:00Z.
    constexpr explicit Timestamp(std::int64_t ns_since_unix_epoch) noexcept;

    [[nodiscard]] constexpr std::int64_t ns_since_epoch() const noexcept;

    [[nodiscard]] static constexpr Timestamp epoch() noexcept;  // 1970-01-01T00:00:00Z
    [[nodiscard]] static constexpr Timestamp min()   noexcept;
    [[nodiscard]] static constexpr Timestamp max()   noexcept;

    [[nodiscard]] constexpr bool is_epoch() const noexcept;

    friend constexpr auto operator<=>(Timestamp, Timestamp) noexcept = default;
    friend constexpr bool operator==(Timestamp, Timestamp) noexcept = default;

    // ── Affine algebra. Note what is absent. ──
    friend constexpr Timestamp operator+(Timestamp t, Duration d) noexcept;
    friend constexpr Timestamp operator+(Duration d, Timestamp t) noexcept;
    friend constexpr Timestamp operator-(Timestamp t, Duration d) noexcept;
    friend constexpr Duration  operator-(Timestamp a, Timestamp b) noexcept;

    constexpr Timestamp& operator+=(Duration d) noexcept;
    constexpr Timestamp& operator-=(Duration d) noexcept;

private:
    std::int64_t ns_{};
};

// ─────────────────────────────────────────────────────────────────────────
// Duration constructors.
//
// UNCHECKED by design: int64 nanoseconds spans +/- 292 years, and every span
// this system reasons about (session windows, model horizons, cooldowns,
// staleness thresholds) is under a day. Overflow requires an argument
// physically impossible in this domain.
// PRECONDITION on every one: the result must fit in int64 nanoseconds.
// ─────────────────────────────────────────────────────────────────────────
namespace duration {

[[nodiscard]] constexpr Duration nanos  (std::int64_t n) noexcept;
[[nodiscard]] constexpr Duration micros (std::int64_t n) noexcept;
[[nodiscard]] constexpr Duration millis (std::int64_t n) noexcept;
[[nodiscard]] constexpr Duration seconds(std::int64_t n) noexcept;
[[nodiscard]] constexpr Duration minutes(std::int64_t n) noexcept;
[[nodiscard]] constexpr Duration hours  (std::int64_t n) noexcept;
[[nodiscard]] constexpr Duration days   (std::int64_t n) noexcept;

/// Largest |n| accepted by each constructor without overflowing int64 ns.
inline constexpr std::int64_t kMaxMicros  = 9223372036854775LL;
inline constexpr std::int64_t kMaxMillis  = 9223372036854LL;
inline constexpr std::int64_t kMaxSeconds = 9223372036LL;
inline constexpr std::int64_t kMaxMinutes = 153722867LL;
inline constexpr std::int64_t kMaxHours   = 2562047LL;
inline constexpr std::int64_t kMaxDays    = 106751LL;

} // namespace duration

// ── Duration accessors. Truncate toward zero. ──
[[nodiscard]] constexpr std::int64_t to_nanos  (Duration d) noexcept;
[[nodiscard]] constexpr std::int64_t to_micros (Duration d) noexcept;
[[nodiscard]] constexpr std::int64_t to_millis (Duration d) noexcept;
[[nodiscard]] constexpr std::int64_t to_seconds(Duration d) noexcept;

/// Lossy. For logging and analytics only, never for ordering or comparison.
[[nodiscard]] constexpr double to_seconds_f(Duration d) noexcept;
[[nodiscard]] constexpr double to_millis_f (Duration d) noexcept;

[[nodiscard]] constexpr Duration abs_duration(Duration d) noexcept;

// ─────────────────────────────────────────────────────────────────────────
// IST — Indian Standard Time. UTC+05:30, no daylight saving, ever.
// India has observed no DST since 1945; this is a constant, not a lookup.
// ─────────────────────────────────────────────────────────────────────────

/// +05:30 expressed in nanoseconds.
inline constexpr Duration kIstOffset = Duration{19'800'000'000'000LL};

/// Nanoseconds elapsed since the most recent IST midnight at or before `t`.
/// Always in [0, 86'400'000'000'000). Uses floor semantics, so it is correct
/// for instants before the Unix epoch.
/// PRECONDITION: t.ns_since_epoch() < INT64_MAX - 19'800'000'000'000 — i.e. `t`
/// is not within 5h30m of Timestamp::max(). The IST offset is added before the
/// modulo, so instants at the very top of the range would overflow. Real market
/// instants are ~1.8e18, nowhere near the 9.2e18 ceiling.
[[nodiscard]] constexpr std::int64_t ist_ns_since_midnight(Timestamp t) noexcept;

/// The IST midnight at or before `t`, as a UTC instant.
[[nodiscard]] constexpr Timestamp ist_midnight_of(Timestamp t) noexcept;

/// The instant of `hh:mm:ss.nnn` IST on the IST day containing `t`.
/// PRECONDITION: 0 <= hh < 24, 0 <= mm < 60, 0 <= ss < 60, 0 <= ns < 1e9.
/// No validation is performed; the caller guarantees the range.
[[nodiscard]] constexpr Timestamp ist_time_of_day(Timestamp t,
                                                  int hh, int mm, int ss,
                                                  std::int64_t ns = 0) noexcept;

/// Whole IST days between the IST midnights of `a` and `b`. Sign follows b - a.
[[nodiscard]] constexpr std::int64_t ist_days_between(Timestamp a, Timestamp b) noexcept;

// ─────────────────────────────────────────────────────────────────────────
// Literals
// ─────────────────────────────────────────────────────────────────────────
namespace literals {

[[nodiscard]] constexpr Duration operator""_ns  (unsigned long long v) noexcept;
[[nodiscard]] constexpr Duration operator""_us  (unsigned long long v) noexcept;
[[nodiscard]] constexpr Duration operator""_ms  (unsigned long long v) noexcept;
[[nodiscard]] constexpr Duration operator""_s   (unsigned long long v) noexcept;
[[nodiscard]] constexpr Duration operator""_min (unsigned long long v) noexcept;
[[nodiscard]] constexpr Duration operator""_h   (unsigned long long v) noexcept;

} // namespace literals
} // namespace altair
```

---

## 4. BEHAVIOURAL SPEC

1. `Timestamp` is trivially copyable, trivially destructible, standard layout,
   and `sizeof(Timestamp) == 8`.
2. The `std::int64_t` constructor is `explicit`. `Timestamp t = 5;` must not compile.
3. **`Timestamp + Timestamp` must not compile.** Neither must `Timestamp * int`,
   `Timestamp / int`, or implicit conversion in either direction between
   `Timestamp` and `std::int64_t`, or between `Timestamp` and `Duration`.
4. `a - b` where both are `Timestamp` yields `Duration{a.ns - b.ns}`.
5. `Timestamp::min()` is `Timestamp{INT64_MIN}`, `max()` is `Timestamp{INT64_MAX}`.
6. `ist_ns_since_midnight` must use **floor** semantics on the modulo, not C++
   truncation. For `Timestamp{-19'800'000'000'001}` the shifted value is `-1`,
   and the answer is `86'399'999'999'999` — **not** `-1`. Getting this wrong is
   the same class of bug as `lots_of` truncating toward zero in P0-01.
7. `ist_midnight_of(t)` equals `t - Duration{ist_ns_since_midnight(t)}`.
8. `ist_time_of_day(t, hh, mm, ss, ns)` equals
   `ist_midnight_of(t) + Duration{sod * 1'000'000'000LL + ns}`, where `sod` is
   the seconds-of-day computed **in `std::int64_t`**:

   ```cpp
   const std::int64_t sod = static_cast<std::int64_t>(hh) * 3600
                          + static_cast<std::int64_t>(mm) * 60
                          + static_cast<std::int64_t>(ss);
   ```

   **Do not compute this in `int`.** `hh`, `mm`, `ss` are `int`, and the literal
   `1'000'000'000` is *also* `int` (it fits in 32 bits), so the obvious-looking
   `(hh*3600 + mm*60 + ss) * 1'000'000'000` evaluates entirely in 32-bit and
   overflows. For 09:15 the true product is `33'300'000'000'000` — **15,506×
   `INT_MAX`**. That is signed overflow: undefined behaviour, not a wrapped value
   you can reason about, and `-Wconversion` will not catch it. Widen to 64 bits
   *before* the multiply and suffix the literal `LL`.
9. `ist_days_between(a, b)` is
   `(ist_midnight_of(b) - ist_midnight_of(a)).raw() / 86'400'000'000'000LL`,
   exact because both operands are IST midnights, so the division never has a
   remainder to round.

   **The `.raw()` is required, not stylistic.** `Timestamp - Timestamp` yields a
   `Duration`, and `StrongInt` in P0-01 deliberately defines **no `operator/`** —
   dividing a dimensioned quantity by a bare scalar is precisely the silent unit
   loss that `core/types` exists to prevent. Unwrap explicitly, divide, return a
   plain `std::int64_t` day count. **Do not add `operator/` to
   `core/types/units.hpp`**: that file is not in your manifest, and touching it
   fails gate 3.
10. `to_seconds_f` and `to_millis_f` are the only functions here that produce a
    `double`. Their doc comments must say they are lossy and must not be used for
    ordering.
11. `abs_duration(Duration{INT64_MIN})` is undefined by negation; return
    `Duration{INT64_MAX}` in that single case rather than invoking UB, and say so
    in the doc comment.
12. Every public entity carries a doc comment stating its **units** and its
    **preconditions**. A comment that does not name the unit fails review.

---

## 5. CONSTRAINTS

- C++23. Standard library only, plus `<types/units.hpp>` from P0-01.
- Header-only. `timestamp.hpp` has no `.cpp`.
- **`timestamp.hpp` may `#include` only:** `<types/units.hpp>`, `<cstdint>`,
  `<compare>`, `<limits>`. Nothing else. In particular **no `<chrono>`** — its
  clocks are not usable on the hot path and its calendar facilities are not
  `constexpr` on every toolchain we target.
- Internal helpers go in `namespace altair::detail`, **never an anonymous
  namespace** — this header is included everywhere, and an anonymous namespace
  in a header gives internal-linkage entities to inline functions with external
  linkage, which is an ODR violation. (This was defect 1 of P0-01.)
- No exceptions, no `throw`, no dynamic allocation, no `iostream`.
- No `using namespace` at file scope in the header.
- Compiles clean at `/W4` and `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion -Wold-style-cast`.
- `core/time/CMakeLists.txt` defines INTERFACE library `altair_time`, aliases it
  `altair::time`, exports `${CMAKE_CURRENT_SOURCE_DIR}/..` as the include root so
  consumers write `#include <time/timestamp.hpp>`, links `altair_types` and
  `altair_flags`, and under `if(ALTAIR_BUILD_TESTS)` registers
  `altair_time_test` from `tests/test_timestamp.cpp` with
  `add_test(NAME time COMMAND altair_time_test)`. Mirror
  `core/types/CMakeLists.txt` exactly in structure. Catch2 is still unavailable —
  the test is a plain `int main()`.

---

## 6. ACCEPTANCE TESTS

`core/time/tests/test_timestamp.cpp`, plain `main()`, `check(bool, const char*)`
helper counting failures, returns 0 only if all pass. Use exactly these names:

```cpp
void test_timestamp_traits();
void test_timestamp_affine_algebra();
void test_duration_constructors_and_accessors();
void test_ist_offset_market_open();
void test_ist_session_boundaries();
void test_ist_floor_semantics_pre_epoch();
void test_ist_time_of_day_roundtrip();
void test_ist_days_between();
void test_duration_edge_cases();
void test_time_type_safety_static_asserts();
```

### Anchor value — derive nothing else from memory

**2026-08-28 09:15:00 IST — NSE market open — is `1787888700000000000` ns since
the Unix epoch.**

That is 2026-08-28T03:45:00Z. Derivation, so you can check it rather than trust
it: 20693 whole days from 1970-01-01 to 2026-08-28 (56 years × 365 = 20440, plus
14 leap days for 1972…2024, plus 239 days from 2026-01-01 to 2026-08-28 in a
non-leap year); 20693 × 86400 = 1'787'875'200 s; plus 03:45:00 = 13'500 s;
total 1'787'888'700 s. **Verify this yourself and state the result in
ASSUMPTIONS.** Every other IST value below is derived from this one anchor.

**test_timestamp_traits**
```
sizeof(Timestamp) == 8
std::is_trivially_copyable_v<Timestamp>
std::is_trivially_destructible_v<Timestamp>
std::is_standard_layout_v<Timestamp>
Timestamp::epoch().ns_since_epoch() == 0
Timestamp::epoch().is_epoch()
Timestamp::min().ns_since_epoch() == std::numeric_limits<std::int64_t>::min()
Timestamp::max().ns_since_epoch() == std::numeric_limits<std::int64_t>::max()
```

**test_timestamp_affine_algebra**
```
(Timestamp{1000} + duration::nanos(500)).ns_since_epoch() == 1500
(duration::nanos(500) + Timestamp{1000}).ns_since_epoch() == 1500
(Timestamp{1000} - duration::nanos(500)).ns_since_epoch() == 500
(Timestamp{1500} - Timestamp{1000}) == duration::nanos(500)
(Timestamp{1000} - Timestamp{1500}) == duration::nanos(-500)
Timestamp{1000} < Timestamp{1500}
Timestamp{1000} == Timestamp{1000}

Timestamp t{1000}; t += duration::nanos(500);  t.ns_since_epoch() == 1500
                   t -= duration::nanos(200);  t.ns_since_epoch() == 1300
```

**test_duration_constructors_and_accessors**
```
duration::nanos(1)   == Duration{1}
duration::micros(1)  == Duration{1'000}
duration::millis(1)  == Duration{1'000'000}
duration::seconds(1) == Duration{1'000'000'000}
duration::minutes(1) == Duration{60'000'000'000}
duration::hours(1)   == Duration{3'600'000'000'000}
duration::days(1)    == Duration{86'400'000'000'000}

to_nanos(duration::millis(5))   == 5'000'000
to_micros(duration::millis(5))  == 5'000
to_millis(duration::millis(5))  == 5
to_seconds(duration::millis(5)) == 0            // truncates toward zero
to_seconds(duration::millis(-5)) == 0           // truncates toward zero, not floor
to_millis_f(duration::micros(1500)) == 1.5
to_seconds_f(duration::millis(250)) == 0.25

// Literals. Every one in the contract must be exercised — an untested literal
// with a wrong multiplier is a silent unit bug everywhere it is later used.
500_ns  == duration::nanos(500)
1_us    == duration::micros(1)
1500_ms == duration::millis(1500)
1_s     == duration::seconds(1)
15_min  == duration::minutes(15)
6_h     == duration::hours(6)
(6_h + 15_min) == duration::minutes(375)     // composes to the trading session
```

**test_ist_offset_market_open**
```
constexpr Timestamp kOpen{1787888700000000000LL};   // 2026-08-28 09:15:00 IST

kIstOffset == duration::seconds(19800)
ist_ns_since_midnight(kOpen) == 33'300'000'000'000LL      // 09:15:00 = 33300 s
```

**test_ist_session_boundaries**
Derive each from `kOpen`. These are the four session instants the engine cares
about, so they are worth pinning:
```
// 09:15 open, 15:00 no-new-entries, 15:15 square-off, 15:30 close.
ist_ns_since_midnight(ist_time_of_day(kOpen,  9, 15, 0)) == 33'300'000'000'000LL
ist_ns_since_midnight(ist_time_of_day(kOpen, 15,  0, 0)) == 54'000'000'000'000LL
ist_ns_since_midnight(ist_time_of_day(kOpen, 15, 15, 0)) == 54'900'000'000'000LL
ist_ns_since_midnight(ist_time_of_day(kOpen, 15, 30, 0)) == 55'800'000'000'000LL

// The trading day is 6h15m long.
(ist_time_of_day(kOpen, 15, 30, 0) - ist_time_of_day(kOpen, 9, 15, 0))
    == duration::minutes(375)

ist_time_of_day(kOpen, 9, 15, 0) == kOpen
```

**test_ist_floor_semantics_pre_epoch**
The one that catches truncation-instead-of-floor:
```
ist_ns_since_midnight(Timestamp{-19'800'000'000'001LL}) == 86'399'999'999'999LL
ist_ns_since_midnight(Timestamp{-19'800'000'000'000LL}) == 0
ist_ns_since_midnight(Timestamp{0}) == 19'800'000'000'000LL   // epoch is 05:30 IST
ist_midnight_of(Timestamp{-19'800'000'000'001LL}).ns_since_epoch()
    == -19'800'000'000'001LL - 86'399'999'999'999LL
```

**test_ist_time_of_day_roundtrip**
```
constexpr Timestamp kOpen{1787888700000000000LL};
// Any instant in the same IST day yields the same midnight.
ist_midnight_of(kOpen) == ist_midnight_of(ist_time_of_day(kOpen, 23, 59, 59))
ist_midnight_of(kOpen) == ist_midnight_of(ist_time_of_day(kOpen, 0, 0, 0))
ist_ns_since_midnight(ist_midnight_of(kOpen)) == 0
ist_time_of_day(kOpen, 0, 0, 0, 1).ns_since_epoch()
    == ist_midnight_of(kOpen).ns_since_epoch() + 1
```

**test_ist_days_between**
```
constexpr Timestamp kOpen{1787888700000000000LL};
ist_days_between(kOpen, kOpen) == 0
ist_days_between(kOpen, kOpen + duration::days(1)) == 1
ist_days_between(kOpen + duration::days(1), kOpen) == -1
// Same IST day, 22 hours apart, still zero days.
ist_days_between(ist_time_of_day(kOpen, 1, 0, 0), ist_time_of_day(kOpen, 23, 0, 0)) == 0
```

**test_duration_edge_cases**
```
abs_duration(duration::seconds(-5)) == duration::seconds(5)
abs_duration(duration::seconds(5))  == duration::seconds(5)
abs_duration(Duration{std::numeric_limits<std::int64_t>::min()})
    == Duration{std::numeric_limits<std::int64_t>::max()}     // documented, not UB

// Largest whole hour count that still fits in int64 ns. Assert the exact value —
// "is positive" would pass against an implementation that silently wrapped.
duration::hours(duration::kMaxHours) == Duration{9'223'369'200'000'000'000LL}
duration::hours(duration::kMaxHours).raw() > 0
```

**test_time_type_safety_static_asserts**
At namespace scope. **MSVC hard-errors on a bare negative requires-expression
over these types** (verified in P0-01: 19.51.36256, C2678/C2679, and it is not
scope-dependent). Reuse the named-concept pattern from
`core/types/tests/test_units.cpp`:

```cpp
namespace time_safety_probes {
template <typename T, typename U> concept addable      = requires(T a, U b) { a + b; };
template <typename T, typename U> concept multipliable = requires(T a, U b) { a * b; };
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
```

The function body may be empty; the static_asserts do the work.

---

## 7. FORBIDDEN

- Adding a file not in the manifest, or a `.cpp` for `timestamp.hpp`.
- **Modifying `core/types/units.hpp`, or any other P0-01 file, for any reason** —
  including adding an `operator/` to `StrongInt` to make item 9 read more nicely.
  P0-01 is DONE and frozen; use `.raw()`.
- Changing any signature in the interface contract.
- Computing a nanosecond quantity in 32-bit `int` and assigning the result to an
  `std::int64_t`. Widen first, then multiply. See item 8.
- **Including `<chrono>`.**
- An anonymous namespace anywhere in the header.
- Adding `operator+(Timestamp, Timestamp)`, or any implicit conversion between
  `Timestamp`, `Duration`, and `std::int64_t`.
- Using `%` with truncation semantics where the spec requires floor. C++ `%`
  truncates toward zero; you must correct for negative operands.
- Reading any clock. This card computes; it does not observe.
- Any DST, timezone-database, or leap-second logic. IST is a constant offset and
  Unix time already excludes leap seconds.
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

Return the three files in full, then your ASSUMPTIONS section, which must
include your independent verification of the `1787888700000000000` anchor.
