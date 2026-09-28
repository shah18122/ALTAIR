#pragma once

// P0-03 — invariant-TSC detection, calibration, and drift uncertainty.
//
// std::chrono::steady_clock::now() costs 20-30 ns. Altair timestamps every tick
// and every decision, so the clock read is the hottest path there is. RDTSC is
// a single instruction, and on any CPU since ~2008 it is *invariant*: constant
// rate regardless of P-state, C-state, or turbo.
//
// A clock is a measuring instrument, so it has a calibration and an error bar
// (ROADMAP §3.2). A span measured in ticks is only as accurate as the
// tick->nanosecond ratio, which is measured, not known. That error is
// propagated here. A latency reported without its uncertainty is a number
// nobody can defend.
//
// No <chrono> in this header — it is needed only to calibrate, which happens
// once at startup, in the .cpp.

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
    CalibrationUnstable,  // calibration samples disagreed beyond tolerance
    AppleClockUnavailable // mach_absolute_time timebase could not be established
};

// ─────────────────────────────────────────────────────────────────────────
// Which physical source is behind a clock instance. Never inferred — always
// carried, so a latency report can state what measured it.
// ─────────────────────────────────────────────────────────────────────────
enum class ClockSource : std::uint8_t {
    InvariantTsc,    // RDTSC / RDTSCP. Single instruction, ~5-10 ns.
    AppleMonotonic,  // mach_absolute_time on Apple Silicon / macOS.
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
    /// PRECONDITION: none. On x86 success uses InvariantTsc; on macOS success
    /// uses AppleMonotonic. Other non-x86 targets still fail explicitly.
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
    /// PRECONDITION: none. Always >= 1 ns, never zero.
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
