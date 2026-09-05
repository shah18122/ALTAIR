// core/testing/latency_gate.hpp -- a latency budget is a claim about the
// SHIPPED binary.
//
// P12-07.
//
// A BUDGET ASSERTED IN AN UNOPTIMISED BUILD MEASURES THE WRONG PROGRAM.
//
// `ctest --preset debug` failed three tests -- kite_decoder, normaliser and
// l2_book -- and all three failures were latency assertions. The decoder
// measured 7692 ns against a 3 us budget it clears comfortably at -O2. Nothing
// was wrong with the code. The assertion was checking a binary that is never
// shipped, in a configuration whose entire purpose is to be slow and
// inspectable.
//
// There are two wrong fixes and it is worth naming them, because both are
// easier than the right one:
//
//   * RAISE THE BUDGET so debug passes. Now the gate cannot fail in the
//     optimised build either, and gate 6 is decoration.
//   * DELETE THE TEST FROM THE DEBUG PRESET. Now the debug run is silently
//     doing less than the release run, and the difference is in a CMake file
//     nobody reads while debugging.
//
// So the measurement always RUNS -- it is a useful number in any build, and
// running it also keeps the benchmark code compiling under the debug
// preset's stricter iterator checks -- and only the COMPARISON is gated.
//
// AND IT MUST NOT SKIP QUIETLY.
//
// A skipped check that prints nothing is a check that passes, which is the
// failure this repository has hit in six other places wearing six other
// costumes: absence read as success. So the unmeasured case prints the number
// it measured, the budget it did not enforce, and the ratio between them, and
// says in the same line that this is NOT a gate 6 measurement.
//
// NDEBUG IS THE TEST, AND IT IS THE RIGHT ONE.
//
// Not a bespoke CMake define. `CMAKE_BUILD_TYPE=Debug` does not define NDEBUG
// and `RelWithDebInfo` and `Release` both do, so the split falls exactly on
// "was the optimiser asked to try" with no new plumbing to drift out of sync
// with the presets. The asan and tsan presets are Debug builds and are covered
// by the same test, which is correct twice over: they are unoptimised AND
// instrumented, so their timings describe neither the shipped binary nor
// anything else.

#pragma once

#include <cstdio>

namespace altair::testing {

/// True only where the optimiser was asked to do its job.
[[nodiscard]] constexpr bool latency_gate_active() noexcept {
#ifdef NDEBUG
    return true;
#else
    return false;
#endif
}

[[nodiscard]] constexpr const char* build_flavour() noexcept {
#ifdef NDEBUG
    return "optimised";
#else
    return "UNOPTIMISED";
#endif
}

/// Report a budget that was measured but deliberately not enforced.
///
/// Prints the number, the budget, and how far over it landed. A reader must be
/// able to tell at a glance that the check did not run AND what it would have
/// said, because "no output" and "passed" look identical in a test log.
inline void latency_not_measured(const char* what, double measured_ns,
                                 double budget_ns) {
    std::printf("  SKIP: %s\n"
                "        %.0f ns measured against a %.0f ns budget (%.1fx) -- "
                "NOT a gate 6 result:\n"
                "        this is an %s build, so the figure describes a binary "
                "that never ships.\n",
                what, measured_ns, budget_ns,
                budget_ns > 0.0 ? measured_ns / budget_ns : 0.0,
                build_flavour());
}

} // namespace altair::testing
