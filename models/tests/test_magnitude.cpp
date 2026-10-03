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

void test_conditional_gate() {
    // No history: both magnitudes are a normal move's, sqrt(2/pi) sigmas.
    WalkForwardPayoff none;
    const auto e0 = none.estimate();
    check(std::fabs(e0.gain - 0.7978845608) < 1e-9 && std::fabs(e0.loss - 0.7978845608) < 1e-9,
          "with no resolved calls, gain and loss default to a normal move's E|z|");

    // Symmetric history: the conditional gate agrees with the old (2q-1)E|r|.
    std::mt19937 g(3);
    std::normal_distribution<double> z(0.0, 1.0);
    WalkForwardPayoff sym;
    for (int i = 0; i < 4000; ++i) sym.add(i % 5 != 0, std::fabs(z(g)));
    const auto es = sym.estimate();
    const GateValue vs = gate_value(0.58, 0.01, es, 100.0, 6.3, 0.0);
    check(std::fabs(vs.value_bp - call_value_bp(0.58, expected_abs_move(100.0), 6.3)) < 2.0,
          "when right and wrong calls move alike, it reduces to (2q-1)E|r| - cost");

    // Right on the small days, wrong on the big ones: 60 % accurate, and losing.
    WalkForwardPayoff skew;
    for (int i = 0; i < 1000; ++i) skew.add(i % 5 < 3, i % 5 < 3 ? 0.4 : 1.4);
    const auto ek = skew.estimate();
    const GateValue vk = gate_value(0.60, 0.01, ek, 100.0, 6.3, 0.0);
    check(call_value_bp(0.60, expected_abs_move(100.0), 6.3) > 0.0 && vk.value_bp < 0.0,
          "a 60 % model right on small moves and wrong on big ones: the old gate opens, the conditional one does not");
    check(ek.gain < 0.45 && ek.loss > 1.3 && ek.gain_se < 0.02, "it learned the two magnitudes, and how well it knows them");

    // Uncertainty: the same point value, opened only when it is known well enough.
    WalkForwardCalibrator thin, thick;
    for (int i = 0; i < 10; ++i) thin.add(0.75, i < 7);
    for (int i = 0; i < 2000; ++i) thick.add(0.75, i % 10 < 7);
    check(thin.calibrated_se(0.75) > 3.0 * thick.calibrated_se(0.75), "ten calls know q far less well than two thousand");
    const GateValue v_thin = gate_value(thin.calibrated(0.75), thin.calibrated_se(0.75), es, 60.0, 6.3, 1.0);
    const GateValue v_thick = gate_value(thick.calibrated(0.75), thick.calibrated_se(0.75), es, 60.0, 6.3, 1.0);
    check(v_thick.open() && !v_thin.open(),
          "a thin history keeps the gate shut where a thick one with the same edge opens it");
    check(v_thick.lower_bp < v_thick.value_bp && std::fabs(v_thick.lower_bp - (v_thick.value_bp - v_thick.se_bp)) < 1e-9,
          "the gate's bound is value - z x se");
}

} // namespace

int main() {
    std::printf("Magnitude\n");
    test_stats();
    test_calibrator();
    test_value();
    test_conditional_gate();
    std::printf("Magnitude: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
