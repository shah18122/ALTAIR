// Tests for models/magnitude.hpp -- magnitude-aware scoring and the value gate.

#include <models/magnitude.hpp>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

using namespace altair;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

void test_stats() {
    // Right on three small days (+10 bp), wrong on one big day (-60 bp).
    const std::vector<MagCall> c{{1, 0.001}, {1, 0.001}, {-1, -0.001}, {1, -0.006}};
    const auto s = magnitude_stats(c);
    check(s.calls == 4 && s.right == 3 && std::fabs(s.accuracy - 0.75) < 1e-12, "75 % accurate by count");
    check(std::fabs(s.weighted_accuracy - 0.3333333333) < 1e-6, "but on the right side of only a third of the movement");
    check(std::fabs(s.gross_bp - (-7.5)) < 1e-9 && s.payoff_ratio < 0.2,
          "and the gross edge is negative: the one big wrong day outweighs the three right ones");
    const std::vector<MagCall> flat{{1, 0.0}, {0, 0.01}};
    const auto f = magnitude_stats(flat);
    check(f.calls == 1 && f.right == 0 && f.wrong == 0, "a flat outcome is a call but neither right nor wrong; no call is not a call");
}

void test_calibrator() {
    WalkForwardCalibrator cal;
    check(std::fabs(cal.calibrated(0.8) - 0.5) < 1e-12, "before any outcome, every call is a coin");
    std::mt19937 g(1);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    for (int i = 0; i < 2000; ++i) {
        cal.add(0.72, u(g) < 0.70);   // the 0.70-0.75 bin: honest
        cal.add(0.92, u(g) < 0.55);   // the 0.90-0.95 bin: overconfident
    }
    check(std::fabs(cal.calibrated(0.71) - 0.70) < 0.03, "an honest probability calibrates to itself");
    check(std::fabs(cal.calibrated(0.93) - 0.55) < 0.03, "an overconfident 0.92 calibrates to what it delivered, 0.55");
    check(std::fabs(cal.calibrated(0.28) - cal.calibrated(0.72)) < 1e-12, "p_up 0.28 on a DOWN call is a 0.72 call");
    check(std::fabs(cal.calibrated(0.62) - 0.5) < 1e-12, "a bin never seen stays at a coin");
    check(std::fabs(cal.calibrated(std::nan("")) - 0.625) < 0.02, "a model without probabilities gets its running overall rate");
    check(std::fabs(cal.calibrated(1.4) - cal.calibrated(std::nan(""))) < 1e-12, "a 'probability' above 1 is treated as none, not clamped");
}

void test_value() {
    check(std::fabs(expected_abs_move(1.0) - 0.7978845608) < 1e-9, "E|r| of a normal move is sigma x sqrt(2/pi)");
    check(call_value_bp(0.55, 40.0, 6.3) < 0.0 && call_value_bp(0.55, 100.0, 6.3) > 0.0,
          "a 55 % call loses on a 40 bp day and earns on a 100 bp one: the size of the move decides");
}

} // namespace

int main() {
    std::printf("Magnitude\n");
    test_stats();
    test_calibrator();
    test_value();
    std::printf("Magnitude: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
