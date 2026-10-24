// analytics/tests/test_quantum_walk.cpp -- Orrell's quantum walk option model:
// the walk's known properties, the pricer against Black-Scholes, and the
// shape of the smile the paper's market model implies.

#include <research/models/quantum_walk.hpp>

#include <cmath>
#include <cstdio>
#include <numbers>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

} // namespace

int main() {
    using namespace altair::qwalk;
    std::printf("Orrell quantum walk\n");

    const auto c = classical_walk(100);
    const auto q = quantum_walk(100);
    double sq = 0, sc = 0;
    for (double p : q.p) sq += p;
    for (double p : c.p) sc += p;
    check(std::fabs(sq - 1.0) < 1e-12 && std::fabs(sc - 1.0) < 1e-12, "both walks conserve probability");
    check(std::fabs(c.variance() - 100.0) < 1e-9, "classical: variance n");
    std::printf("    Hadamard walk, 100 steps: mean %.4f, variance %.1f (paper: (1 - 1/sqrt2) n^2 = %.1f)\n", q.mean(), q.variance(),
                (1.0 - 1.0 / std::numbers::sqrt2) * 1e4);
    check(std::fabs(q.mean()) < 1e-9 && std::fabs(q.variance() / ((1.0 - 1.0 / std::numbers::sqrt2) * 1e4) - 1.0) < 0.02,
          "balanced Hadamard walk: symmetric, variance (1 - 1/sqrt2) n^2 -- quadratic in time");
    check(quantum_walk(100, 50.0 * std::numbers::pi / 180.0).mean() > 1.0, "a 50 degree start leans up (Figure 1, lower)");
    const auto dq = quantum_walk(100, std::numbers::pi / 4.0, 1.0, 400, 7);
    std::printf("    with decoherence sigma 1 (400 runs): variance %.1f\n", dq.variance());
    check(dq.variance() < 0.3 * q.variance(), "decoherence pulls it back towards the classical walk (Figure 2)");

    const double r = 0.02, t = 0.5, sigma = 0.20;
    const auto c400 = classical_walk(400);
    check(std::fabs(call_from_walk(c400, 1.0, sigma, t, r) / bs_call(1.0, sigma, t, r) - 1.0) < 0.01,
          "eq. 1 on the classical walk is Black-Scholes, within 1 %");

    // Figure 3: classical, quantum and the market compromise, as implied vols by strike.
    const auto qb = quantum_walk(100, 50.0 * std::numbers::pi / 180.0);
    std::printf("    strike  classical IV  quantum IV (45)  market IV (50)\n");
    double q_atm = 0, q_lo = 0, q_hi = 0, pq_atm = 0, pc_atm = 0;
    for (const double k : {0.85, 0.90, 0.95, 1.00, 1.05, 1.10, 1.15}) {
        const double pc = call_from_walk(c, k, sigma, t, r), pq = call_from_walk(q, k, sigma, t, r);
        const double pm = market_price(pc, call_from_walk(qb, k, sigma, t, r));
        const double iq = bs_implied(pq, k, t, r);
        std::printf("    %5.2f   %9.2f %%   %11.2f %%   %10.2f %%\n", k, 100 * bs_implied(pc, k, t, r), 100 * iq, 100 * bs_implied(pm, k, t, r));
        if (k == 1.00) { q_atm = iq; pq_atm = pq; pc_atm = pc; }
        if (k == 0.85) q_lo = iq;
        if (k == 1.15) q_hi = iq;
    }
    check(pq_atm > pc_atm, "the quantum price is higher at the money (the paper's main difference)");
    check(q_atm > q_lo + 0.03 && q_atm > q_hi + 0.03,
          "so the quantum walk's implied vol is a frown, peaked at the money -- NSE's is a smile");

    std::printf("Orrell quantum walk: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
