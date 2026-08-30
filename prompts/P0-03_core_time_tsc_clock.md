# P0-03 — `core/time/tsc_clock`: invariant-TSC detect, calibration, drift uncertainty

> Phase 0 · Card 3 of 14 · Status: DONE (implemented by Claude, 2026-08-29 — see LEDGER)
> Depends on: P0-01 (`core/types/units.hpp`) — DONE · P0-02 (`core/time/timestamp.hpp`) — DONE
> Paste everything below the line into DeepSeek V4 as a single prompt.
>
> **Architect's note (not part of the prompt).** This is the first card with
> platform code and the first where gate 6 is live. Two decisions were made here
> rather than left open: (a) a CPU without invariant TSC is an **error**, not a
> silent fallback — the caller must opt into a degraded clock explicitly, per
> CLAUDE.md rule 9; (b) the tick→ns conversion is **Q32.32 fixed point with a
> 128-bit intermediate**, not floating point, because a naive `u64 × u64` overflows
> ~2 h into a session at 3 GHz. Both are in the contract.

---

## 1. CONTEXT

You are implementing the hardware clock layer of Altair, a C++23 low-latency
trading engine for Indian equity markets.

`std::chrono::steady_clock::now()` costs 20–30 ns on Windows and involves a
function call through the vDSO on Linux. Altair timestamps **every tick and every
decision**, so the clock read is on the hottest path there is. The x86 timestamp
counter (`RDTSC`) is a single instruction, ~15–25 cycles, and on any CPU made
since ~2008 it is *invariant*: it ticks at a constant rate regardless of
P-state, C-state, or turbo.

**A clock is a measuring instrument, so it has a calibration and an error bar**
(ROADMAP §3.2). A span measured in TSC ticks is only as accurate as the
tick→nanosecond ratio, which is *measured*, not known. This card propagates that
error. Code that reports a latency without its uncertainty is reporting a number
it cannot defend.

P0-02 gave you `Timestamp` and `Duration`. This card produces them from hardware.
Normalising *broker* timestamps is P0-04 and is not your concern.

---

## 2. FILE MANIFEST

Create exactly these three files, and modify exactly one.

```
CREATE   core/time/tsc_clock.hpp
CREATE   core/time/tsc_clock.cpp
CREATE   core/time/tests/test_tsc_clock.cpp
MODIFY   core/time/CMakeLists.txt
```

`core/time/CMakeLists.txt` currently declares `altair_time` as an INTERFACE
library because `core/time` was header-only. It gains a `.cpp` here, so it
becomes a **STATIC** library: `INTERFACE` → `STATIC`, and
`target_include_directories(... INTERFACE ...)` → `... PUBLIC ...`, likewise
`target_link_libraries`. Register the new test as `altair_tsc_clock_test` with
`add_test(NAME tsc_clock COMMAND altair_tsc_clock_test)`. Leave the existing
`altair_time_test` registration alone.

**Do not touch** `core/CMakeLists.txt`, the root `CMakeLists.txt`, `vcpkg.json`,
or anything under `core/types/`.

---

## 3. INTERFACE CONTRACT

Implement **exactly** these declarations in `core/time/tsc_clock.hpp`, in this
order, inside `namespace altair`. Do not add, remove, rename, or re-order any
public entity.

```cpp
#pragma once

#include <time/timestamp.hpp>
#include <types/units.hpp>

#include <cstdint>
#include <expected>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// Why a hardware clock could not be established.
// ─────────────────────────────────────────────────────────────────────────
enum class ClockError : std::uint8_t {
    NotX86,               // target is not x86-64; RDTSC does not exist
    NoInvariantTsc,       // TSC exists but is not invariant (rate follows P-state)
    CalibrationUnstable   // calibration samples disagreed beyond tolerance
};

// ─────────────────────────────────────────────────────────────────────────
// Which physical source is behind a clock instance. Never inferred — always
// carried, so a latency report can state what measured it.
// ─────────────────────────────────────────────────────────────────────────
enum class ClockSource : std::uint8_t {
    InvariantTsc,    // RDTSC / RDTSCP. Single instruction, ~5-10 ns.
    SteadyFallback   // std::chrono::steady_clock. Correct, ~20-30 ns, opt-in only.
};

// ─────────────────────────────────────────────────────────────────────────
// The result of calibrating a tick source against a reference clock.
// Every field states its unit. This struct IS the measurement record.
// ─────────────────────────────────────────────────────────────────────────
struct TscCalibration {
    /// Nanoseconds per tick as Q32.32 fixed point: ns = (ticks * this) >> 32.
    /// UNIT: ns/tick, scaled by 2^32. This is what the hot path uses.
    std::uint64_t ns_per_tick_q32;

    /// Point estimate. UNIT: ns/tick. ANALYTICS AND LOGGING ONLY —
    /// the hot path must use ns_per_tick_q32, never this.
    double ns_per_tick;

    /// 1-sigma standard error of ns_per_tick. UNIT: ns/tick.
    /// Zero is not a valid calibration; a real measurement always has error.
    double ns_per_tick_stderr;

    /// Tick counter reading at the anchor instant. UNIT: ticks.
    std::uint64_t anchor_ticks;

    /// The UTC instant corresponding to anchor_ticks.
    /// UNIT: nanoseconds since the Unix epoch.
    Timestamp anchor;

    /// Number of independent samples behind the estimate. UNIT: count.
    int samples;

    /// What produced these numbers.
    ClockSource source;
};

// ─────────────────────────────────────────────────────────────────────────
// CPU feature detection. Separated from the clock so it is testable on its
// own and so a caller can decide policy before paying for calibration.
// ─────────────────────────────────────────────────────────────────────────
namespace cpu {

/// True iff CPUID reports an invariant TSC (leaf 0x8000'0007, EDX bit 8).
/// Returns false on any non-x86-64 target. UNIT: none. PRECONDITION: none.
[[nodiscard]] bool has_invariant_tsc() noexcept;

/// True iff the RDTSCP instruction is available (leaf 0x8000'0001, EDX bit 27).
/// Returns false on any non-x86-64 target. UNIT: none. PRECONDITION: none.
[[nodiscard]] bool has_rdtscp() noexcept;

/// Raw tick counter read, ordered against surrounding loads and stores.
/// Uses RDTSCP where available, otherwise LFENCE followed by RDTSC.
/// UNIT: ticks. PRECONDITION: has_invariant_tsc() is true; otherwise the
/// value is still monotonic but its rate is not defined.
[[nodiscard]] ALTAIR_HOT std::uint64_t read_tsc_ordered() noexcept;

} // namespace cpu

// ─────────────────────────────────────────────────────────────────────────
// TscClock — a calibrated hardware clock.
//
// Cheap to copy (it is a calibration record and nothing else). Const-correct:
// reading the clock does not mutate it, so one instance is shared freely
// across threads without synchronisation.
// ─────────────────────────────────────────────────────────────────────────
class TscClock {
public:
    /// Detect, calibrate, and anchor against the system clock.
    /// FAILS LOUD: returns NoInvariantTsc rather than silently degrading.
    /// Costs roughly 50 ms — call once at startup, never on the hot path.
    /// PRECONDITION: none. POSTCONDITION on success: source() == InvariantTsc.
    [[nodiscard]] static std::expected<TscClock, ClockError> create() noexcept;

    /// Explicitly opt in to a steady_clock-backed clock. For CI machines,
    /// virtualised hosts, and non-x86 targets. Always succeeds.
    /// POSTCONDITION: source() == SteadyFallback, is_invariant_tsc() == false.
    [[nodiscard]] static TscClock create_fallback() noexcept;

    /// Raw tick reading from this clock's source. UNIT: ticks.
    /// Monotonic non-decreasing on a single core.
    [[nodiscard]] ALTAIR_HOT std::uint64_t now_ticks() const noexcept;

    /// Current instant. UNIT: nanoseconds since the Unix epoch, UTC.
    /// Derived from the anchor plus elapsed ticks; carries the calibration's
    /// error, which uncertainty_of() quantifies.
    [[nodiscard]] ALTAIR_HOT Timestamp now() const noexcept;

    /// Span from a previously captured tick reading to now. UNIT: nanoseconds.
    /// PRECONDITION: t0_ticks came from now_ticks() on this same clock.
    [[nodiscard]] ALTAIR_HOT Duration elapsed_since(std::uint64_t t0_ticks) const noexcept;

    /// Convert a tick count to a span. UNIT: ticks in, nanoseconds out.
    /// Uses a 128-bit intermediate, so it is exact for any int64 tick count.
    [[nodiscard]] ALTAIR_HOT Duration ticks_to_duration(std::uint64_t ticks) const noexcept;

    /// 1-sigma uncertainty of a span measured with this clock. UNIT: nanoseconds.
    /// Propagates the calibration's relative error through the multiplication
    /// and adds one tick of read quantisation:
    ///     sigma(span) = |span| * (stderr / ns_per_tick) + ns_per_tick
    /// PRECONDITION: none. Always >= 1 tick, never zero.
    [[nodiscard]] Duration uncertainty_of(Duration measured) const noexcept;

    /// The calibration record behind this clock.
    [[nodiscard]] const TscCalibration& calibration() const noexcept;

    /// What physically backs this clock.
    [[nodiscard]] ClockSource source() const noexcept;

    /// True iff this clock reads an invariant TSC. A latency benchmark must
    /// refuse to publish a number when this is false.
    [[nodiscard]] bool is_invariant_tsc() const noexcept;

private:
    explicit TscClock(const TscCalibration& cal) noexcept;

    TscCalibration cal_{};
};

} // namespace altair
```

---

## 4. BEHAVIOURAL SPEC

1. `cpu::has_invariant_tsc()` reads CPUID leaf `0x8000'0007`, EDX bit 8. It must
   first check that leaf `0x8000'0007` exists by querying leaf `0x8000'0000` and
   comparing EAX. On a non-x86-64 target it returns `false` and must still
   compile.
2. `cpu::has_rdtscp()` reads CPUID leaf `0x8000'0001`, EDX bit 27, guarded by the
   same maximum-extended-leaf check.
3. `cpu::read_tsc_ordered()` uses `RDTSCP` when `has_rdtscp()`, otherwise
   `LFENCE` then `RDTSC`. Determine which **once**, not per call — a branch on a
   cached `bool` is acceptable; a CPUID per read is not. CPUID costs ~100 ns and
   would dominate the measurement it is supposed to make.
4. `TscClock::create()` returns `ClockError::NotX86` on a non-x86-64 target, and
   `ClockError::NoInvariantTsc` when `has_invariant_tsc()` is false. It never
   returns a `SteadyFallback` clock.
5. Calibration takes **5 samples of a 10 ms window** against
   `std::chrono::steady_clock`, giving `ns_per_tick` per sample. The reported
   `ns_per_tick` is the **mean** of the samples; `ns_per_tick_stderr` is the
   sample standard deviation divided by `sqrt(5)`.
6. If the sample **relative** standard deviation exceeds `1e-3` (0.1%),
   calibration returns `ClockError::CalibrationUnstable`. That threshold catches
   a virtualised or heavily-contended host where the TSC is not trustworthy.
7. `ns_per_tick_q32` is `llround(ns_per_tick * 2^32)`. For a 3 GHz part
   `ns_per_tick` ≈ 0.3333 and `ns_per_tick_q32` ≈ 1'431'655'765.
8. `ticks_to_duration(t)` computes `(t * ns_per_tick_q32) >> 32` **using a
   128-bit intermediate**. A plain `std::uint64_t` product overflows: at 3 GHz a
   6-hour session is ~6.5e13 ticks, and `6.5e13 * 1.43e9 ≈ 9.3e22`, well past
   `UINT64_MAX` (1.8e19). Use `unsigned __int128` on GCC/Clang and `_umul128`
   from `<intrin.h>` on MSVC. Getting this wrong produces a clock that is
   correct in every test under a second and silently wrong in production.
9. `now()` is `cal_.anchor + ticks_to_duration(now_ticks() - cal_.anchor_ticks)`.
   The tick subtraction happens **before** the conversion, in unsigned
   arithmetic, so it is correct across a TSC value larger than `INT64_MAX`.
10. `uncertainty_of(d)` is
    `Duration{llround(|d| * (stderr / ns_per_tick) + ns_per_tick)}`, and is
    **never zero** — a measurement with no error bar is a lie. Clamp to a minimum
    of 1 ns.
11. `create_fallback()` builds a clock whose `now_ticks()` returns
    `steady_clock` nanoseconds directly, with `ns_per_tick == 1.0`,
    `ns_per_tick_q32 == 2^32`, `samples == 0`, and a `ns_per_tick_stderr` equal
    to the observed `steady_clock` tick period in ns (never zero — see item 10).
12. Every function marked `ALTAIR_HOT` allocates nothing, throws nothing, and
    contains no CPUID, no syscall, and no branch on anything but a cached `bool`.

---

## 5. CONSTRAINTS

- C++23. Standard library only, plus P0-01 and P0-02 headers. No new dependency;
  `vcpkg.json` is frozen.
- **`tsc_clock.hpp` may NOT include `<chrono>`.** `tsc_clock.cpp` **may** — the
  reference clock is needed for calibration, which runs once at startup and is
  not on the hot path. This is the only place in `core/time` where `<chrono>` is
  permitted, and it must not appear in a header.
- Platform intrinsics go in `tsc_clock.cpp` behind `#if defined(_MSC_VER)` /
  `#elif defined(__GNUC__)`, with a final `#else` that compiles and reports
  `NotX86`. The header must be free of platform `#if`s.
- Internal helpers go in `namespace altair::detail`, **never an anonymous
  namespace in a header**. An anonymous namespace inside `tsc_clock.cpp` is fine
  and correct — a `.cpp` is one translation unit, so there is no ODR hazard.
- No exceptions, no `throw`, no dynamic allocation anywhere in this card.
- No `using namespace` at file scope in the header.
- Compiles clean at `/W4` and `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion -Wold-style-cast`.
- Tests remain a plain `int main()`. **Catch2 and Google Benchmark are still
  unavailable** — the comment in the root `CMakeLists.txt` claiming they arrive
  with P0-03 is stale; CLAUDE.md's "no vcpkg needed until P0-05" governs. Do not
  add either, and do not edit the root `CMakeLists.txt` to correct the comment.

---

## 6. ACCEPTANCE TESTS

`core/time/tests/test_tsc_clock.cpp`, plain `main()`, `check(bool, const char*)`
helper counting failures, returns 0 only if all pass. Use exactly these names:

```cpp
void test_tsc_cpu_feature_detection();
void test_tsc_clock_creation();
void test_tsc_monotonic();
void test_tsc_calibration_sane();
void test_tsc_matches_reference_clock();
void test_tsc_uncertainty_propagates();
void test_tsc_timestamp_anchor();
void test_tsc_fallback_clock();
```

**These tests run on real hardware and must not be flaky.** Where a bound is
given it is deliberately loose; do not tighten it. A test that fails once a week
on a busy CI box is worse than no test. Every timing bound below has at least 5x
headroom over the worst case observed on a contended laptop.

**test_tsc_cpu_feature_detection**
```
// No absolute assertion — the answer is hardware-dependent. Assert consistency.
cpu::has_invariant_tsc() returns the same value on two consecutive calls
cpu::has_rdtscp()        returns the same value on two consecutive calls
cpu::read_tsc_ordered() != 0
// Print the detected features so the reviewer can see what the box reported.
```

**test_tsc_clock_creation**
```
// On a box with invariant TSC, create() succeeds and reports InvariantTsc.
// On a box without, it fails with NoInvariantTsc and NEVER returns a fallback.
auto c = TscClock::create();
if (cpu::has_invariant_tsc()):
    c.has_value()
    c->source() == ClockSource::InvariantTsc
    c->is_invariant_tsc() == true
else:
    !c.has_value()
    c.error() == ClockError::NoInvariantTsc
```

**test_tsc_monotonic**
```
// 10'000 consecutive reads, never decreasing.
auto clk = <clock under test, fallback if no TSC>;
prev = clk.now_ticks()
for 10'000 iterations:
    cur = clk.now_ticks(); cur >= prev; prev = cur
// And the same for now(), which must also never go backwards.
prev_ts = clk.now(); repeated 10'000 times, never decreasing
```

**test_tsc_calibration_sane**
```
// Only meaningful for a real TSC clock; skip (and say so) on fallback.
const auto& cal = clk.calibration();
cal.ns_per_tick > 0.01 && cal.ns_per_tick < 10.0   // 100 MHz .. 100 GHz
cal.ns_per_tick_stderr > 0.0                       // a measurement HAS error
cal.ns_per_tick_stderr < cal.ns_per_tick * 1e-3    // and it is small
cal.samples == 5
cal.ns_per_tick_q32 != 0
// Q32 round-trips to the double within half an ulp of the fixed-point grid:
|static_cast<double>(cal.ns_per_tick_q32) / 4294967296.0 - cal.ns_per_tick| < 1e-9
```

**test_tsc_matches_reference_clock**
```
// Sleep 200 ms by steady_clock; the TSC clock must agree within 2%.
// 2% is ~20x the calibration's own 0.1% tolerance - loose on purpose.
t0 = clk.now_ticks(); ref0 = steady_clock::now()
   <sleep 200 ms>
measured = clk.elapsed_since(t0); ref = steady_clock::now() - ref0
|to_millis_f(measured) - ref_ms| / ref_ms < 0.02
// And the reference span must lie inside 3 sigma of the clock's own error bar
// widened by the sleep's own imprecision - assert the uncertainty is not absurd:
to_nanos(clk.uncertainty_of(measured)) < to_nanos(measured) / 100
```

**test_tsc_uncertainty_propagates**
```
// Relative error is constant, so uncertainty must scale with the span.
u1  = clk.uncertainty_of(duration::millis(1))
u10 = clk.uncertainty_of(duration::millis(10))
to_nanos(u1)  > 0                        // never zero
to_nanos(u10) > to_nanos(u1)             // grows with the span

// Check the SCALING at 100 ms / 1000 ms, NOT at 1 ms / 10 ms.
// sigma(d) = |d| * rel + ns_per_tick. At 1 ms the constant one-tick term and
// the integer rounding of a ~1 ns result are the same order as the signal: a
// tight calibration can put sigma(1ms) at 1.49 ns (rounding DOWN to 1) and
// sigma(10ms) at 10.67 (rounding UP to 11), so a "< 11x" bound fails on
// rounding alone. That was a real 1-in-20 flake under CPU load. At 100 ms the
// 0.47 ns floor is negligible and the ratio means something.
u100  = clk.uncertainty_of(duration::millis(100))
u1000 = clk.uncertainty_of(duration::millis(1000))
to_nanos(u1000) < to_nanos(u100) * 11
to_nanos(u1000) > to_nanos(u100) * 9
// Sign-independent: uncertainty is a magnitude.
clk.uncertainty_of(duration::millis(-5)) == clk.uncertainty_of(duration::millis(5))
clk.uncertainty_of(Duration{0}) > Duration{0}     // even a zero span has a floor
```

**test_tsc_timestamp_anchor**
```
// now() must land within 100 ms of the system clock's idea of now.
// Loose because the anchor was taken up to 50 ms ago during calibration.
sys_now = <system_clock now, as ns since Unix epoch>
delta = abs_duration(clk.now() - Timestamp{sys_now})
to_millis(delta) < 100
// The anchor itself is a sane Unix instant: after 2020-01-01, before 2100-01-01.
clk.calibration().anchor.ns_since_epoch() > 1'577'836'800'000'000'000LL
clk.calibration().anchor.ns_since_epoch() < 4'102'444'800'000'000'000LL
```

**test_tsc_fallback_clock**
```
auto fb = TscClock::create_fallback();
fb.source() == ClockSource::SteadyFallback
fb.is_invariant_tsc() == false
fb.calibration().ns_per_tick == 1.0
fb.calibration().ns_per_tick_q32 == 4294967296ULL       // 2^32
fb.calibration().samples == 0
fb.calibration().ns_per_tick_stderr > 0.0               // still not zero
// A fallback clock is still a correct clock:
fb.now_ticks() > 0
to_millis(abs_duration(fb.now() - Timestamp{<system now ns>})) < 100
```

### Latency reporting — not a pass/fail assertion

At the end of `main()`, after the tests, time `now()` over 100'000 iterations and
print p50 and p99 in nanoseconds, plus which `ClockSource` produced them.

**Budget: `now()` p99 < 25 ns on an invariant TSC** (PROTOCOL §9 records
`Clock::now p99 = 18ns (budget 25ns)`). **Print it; do not assert it** — a
contended CI box will blow any timing assertion, and gate 6 is a human judgement
against ROADMAP §11, not a unit test. If `is_invariant_tsc()` is false, print
`SKIPPED (no invariant TSC)` instead of a number: a latency figure from a
fallback clock is not comparable and must not enter the ledger.

---

## 7. FORBIDDEN

- Adding a file not in the manifest. Modifying `core/CMakeLists.txt`, the root
  `CMakeLists.txt`, `vcpkg.json`, or anything under `core/types/`.
- Changing any signature in the interface contract.
- **`#include <chrono>` in `tsc_clock.hpp`.** It is permitted in the `.cpp` only.
- A platform `#if` in the header.
- Calling CPUID inside any `ALTAIR_HOT` function. Cache the feature bits once.
- Silently falling back to `steady_clock` when the TSC is unusable. `create()`
  fails; the caller opts in. (CLAUDE.md rule 9 — failing loud beats trading wrong.)
- A `ns_per_tick_stderr` of exactly zero for any clock, including the fallback.
- Computing `ticks * ns_per_tick_q32` in 64 bits. See item 8.
- Using `double` in `now()`, `now_ticks()`, `elapsed_since()`, or
  `ticks_to_duration()`. Doubles appear only in the calibration record and in
  `uncertainty_of`.
- Asserting a hard latency number in the test. Print it; the reviewer judges it.
- Adding Catch2, Google Benchmark, or any vcpkg package.
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

Return the four files in full, then your ASSUMPTIONS section, which must state
the CPU you tested on, whether it reported an invariant TSC, the calibrated
`ns_per_tick` with its standard error, and the measured `now()` p50/p99.
