// oms/tests/test_execution_state.cpp -- the VRC Execution State Model.
//
// Each formula against a number worked by hand (the paper's own where it
// gives one), then the opportunistic slicer against TWAP on simulated days
// whose liquidity drifts between regimes, costed by the paper's eq. 13.

#include <research/execution/execution_state.hpp>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

bool near(double a, double b, double tol = 1e-12) { return std::fabs(a - b) <= tol; }

} // namespace

int main() {
    using namespace altair;
    std::printf("VRC execution state model\n");

    // ---- formulas ----------------------------------------------------------
    // Section 5.1 / eq. 14: eta 0.6, sigma 0.4%, Q/V = 2.9% -> 0.041%.
    check(near(esm_sqrt_impact(0.6, 0.004, 0.029, 1.0), 0.6 * 0.004 * std::sqrt(0.029))
              && std::fabs(esm_sqrt_impact(0.6, 0.004, 0.029, 1.0) - 0.00041) < 0.00001,
          "eq. 1 reproduces the paper's worked 0.041% impact");

    const EsmSpreadCurve curve{1.0, 4.0, 3.0, 5.0, 5.0, 1.0};
    check(near(curve.at(0.0), 1.0 + 3.0 + 2.0 * std::exp(-5.0))
              && curve.at(0.5) < curve.at(0.0) && curve.at(0.5) < curve.at(1.0),
          "eq. 9 is U-shaped: wide at the open and the close, tight mid-session");

    const EsmLevel bids[] = {{99.95, 100}, {99.90, 200}, {99.80, 400}};
    check(near(esm_effective_depth(bids, 100.0, 10.0), 300.0),
          "eq. 10 counts only the levels within c bps of the mid (10 bps: "
          "99.95 and 99.90)");
    check(near(esm_vol_ratio(0.006, 0.004), 1.5) && esm_vol_ratio(0.01, 0.0) == 0.0,
          "eq. 11 is the ratio to the time-of-day median");

    // All inputs at 1 and OFI at 0 scores 0.30 + 0.25 + 0.25 = 0.80. With a
    // standardised OFI, E|OFI| is about 0.8, so an average day is about
    // 0.96: "1 is average" holds for the score only roughly.
    check(near(esm_score(EsmInputs{}), 0.80),
          "eq. 12: inputs at their averages and balanced flow score 0.80");
    check(near(esm_score(EsmInputs{2.0, 0.5, 1.5, -1.0}),
               0.30 * 2.0 + 0.25 * 1.5 + 0.25 * 1.5 + 0.20 * 1.0),
          "eq. 12 with the depth term inverted and |OFI|");

    check(esm_regime(0.79) == EsmRegime::Liquid && esm_regime(0.8) == EsmRegime::Normal
              && esm_regime(1.2) == EsmRegime::Thin && esm_regime(1.6) == EsmRegime::Stressed,
          "Table 1 regimes, lower bounds inclusive");
    check(esm_size_fraction(EsmRegime::Liquid) == 1.0
              && esm_size_fraction(EsmRegime::Normal) == 0.9
              && esm_size_fraction(EsmRegime::Thin) == 0.7
              && esm_size_fraction(EsmRegime::Stressed) == 0.4,
          "Table 1 size scaling 100/90/70/40%");

    EsmCostParams cp;
    cp.xi = 0.001;
    const double c_in = esm_cost(100, 10000, 0.0006, 0.01, 500, EsmRegime::Normal, cp);
    const double c_walk = esm_cost(1000, 10000, 0.0006, 0.01, 500, EsmRegime::Normal, cp);
    check(near(c_in, 0.0003 + 0.6 * 0.01 * 0.1)
              && near(c_walk, 0.0003 + 0.6 * 0.01 * std::sqrt(0.1) + 0.001 * 500.0 * 500.0 / 500.0),
          "eq. 13: half spread + regime impact, plus book-walking only past "
          "the depth");

    check(near(esm_passive_benefit(0.002, 0.5, 0.001), 0.0005 - 0.0005),
          "eq. 15 at the break-even point is zero");
    check(!esm_passive_preferred(0.002, 0.5, 0.001)
              && esm_passive_preferred(0.002, 0.51, 0.001),
          "eq. 16: post only when P(fill) beats dp / (dp + S/2)");

    check(near(esm_rebalance_freq(12.0, 1.0, 0.1, 1.0, 1.0), 12.0 * std::sqrt(0.01 / 12.0))
              && near(esm_rebalance_freq(12.0, 1e6, 0.1, 1.0, 1.0), 12.0),
          "eq. 19: slower than the signal when impact decays slowly, never faster");
    check(near(esm_option_cost(0.01, 50.0, 0.02, 1000.0, 2000.0),
               0.25 + 0.02 * 1000.0 / (50.0 * 2000.0)),
          "eq. 20: IV half-spread times vega, plus surface impact");
    check(near(esm_event_spread(0.01, 3.0, 0.1, 0.0), 0.04),
          "eq. 22: at the event the spread is (1 + psi) times normal");

    check(near(esm_exit_adjusted(100.0, 0.0050, 0.0010, 0.0020, 0.0010), 100.0 * 0.0020 / 0.0030),
          "eq. 25: a dear exit shrinks the position");
    check(esm_abstain(0.0010, 0.0007, 0.0007) && !esm_abstain(0.0020, 0.0007, 0.0007),
          "eq. 29: abstain when the edge does not cover the round trip");
    check(near(esm_kelly_net(0.0030, 0.0020, 0.1), 0.1)
              && near(esm_kelly_net(0.0030, 0.0020, 0.1) * 3.0, 0.0030 / 0.01),
          "eq. 31 / section 10.1: 30 bp edge, 20 bp cost -> one third of naive Kelly");
    check(near(esm_liquidity_cap(1e6, 3.0, kEsmMaxParticipationEquity), 150000.0),
          "eq. 32 with VRC's 5% equity cap");

    check(esm_oea_child(100.0, 1.2, 10, 2.0) == 0.0,
          "eq. 33: no child when EQS >= 1 (section 11.2)");
    check(esm_oea_child(100.0, 1.5, 0, 2.0) == 100.0,
          "and at T_max the rest goes, whatever the regime");
    check(esm_oea_child(100.0, 0.5, 9, 2.0) > esm_oea_child(100.0, 0.9, 9, 2.0)
              && esm_oea_child(100.0, 0.9, 9, 2.0) > 10.0,
          "a better window takes more, and any tradeable window takes more "
          "than its TWAP share");

    // ---- benchmark: OEA vs TWAP --------------------------------------------
    //
    // 75 five-minute intervals. EQS follows an AR(1) around 1 (persistence
    // 0.9, so a bad patch lasts about an hour); the interval's spread and
    // regime come from it, and every child is costed by eq. 13 with the
    // interval's volume. Spread 6 bp on an average day, daily sigma 1.5%,
    // ADV 800,000 (the section 5.1 stock). Same days for both algorithms.
    //
    // THE RESULT IS SIZE-DEPENDENT, AND THAT IS THE FINDING. The OEA saves
    // spread by waiting for good windows and pays impact by concentrating
    // size in them (eq. 1 is concave in q, so a lumpy schedule costs more
    // impact than an even one), and half its days end in a forced finish.
    // For the small orders the paper is written for, spread dominates and
    // the OEA wins. At the paper's own 2.9%-of-ADV example it LOSES to TWAP
    // on the paper's own cost model. Both are asserted, so the boundary
    // cannot drift unnoticed.
    struct Result { double twap_bp, oea_bp, se_bp; };
    const auto simulate = [](double frac_adv, double beta) {
        const int intervals = 75, days = 20000;
        const double adv = 800000.0, q_total = frac_adv * adv;
        const double vol_interval = adv / intervals;
        std::mt19937_64 rng(20260111);
        std::normal_distribution<double> N;
        double twap = 0, oea = 0, diff = 0, diff2 = 0;
        std::vector<double> eqs(intervals);
        for (int d = 0; d < days; ++d) {
            double e = 1.0 + 0.3 * N(rng);
            for (int t = 0; t < intervals; ++t) {
                e = 1.0 + 0.9 * (e - 1.0) + 0.3 * std::sqrt(1 - 0.81) * N(rng);
                eqs[static_cast<std::size_t>(t)] = std::max(0.3, e);
            }
            const auto cost = [&](double q, double sc) {
                if (q <= 0.0) { return 0.0; }
                return q * esm_cost(q, vol_interval, 0.0006 * sc,
                                    0.015 / std::sqrt(intervals), 1e18,
                                    esm_regime(sc));
            };
            double ct = 0, co = 0, rem = q_total;
            for (int t = 0; t < intervals; ++t) {
                const double sc = eqs[static_cast<std::size_t>(t)];
                ct += cost(q_total / intervals, sc);
                const double child = esm_oea_child(
                    rem, sc, static_cast<std::size_t>(intervals - 1 - t), beta);
                co += cost(child, sc);
                rem -= child;
            }
            twap += ct;
            oea += co;
            diff += ct - co;
            diff2 += (ct - co) * (ct - co);
        }
        const double m = 1.0 / days, bp = 1e4 / q_total;
        const double md = diff * m, se = std::sqrt((diff2 * m - md * md) * m);
        return Result{twap * m * bp, oea * m * bp, se * bp};
    };
    {
        const Result small = simulate(0.0005, 2.0);
        std::printf("    0.05%% of ADV, beta 2: TWAP %.3f bp  OEA %.3f bp  "
                    "(+- %.3f)\n", small.twap_bp, small.oea_bp, small.se_bp);
        check(small.twap_bp - small.oea_bp > 3.0 * small.se_bp,
              "for a small order the opportunistic slicer beats TWAP under "
              "the paper's cost model, by more than three standard errors");
        const Result big = simulate(0.029, 2.0);
        std::printf("    2.9%% of ADV, beta 2:  TWAP %.3f bp  OEA %.3f bp  "
                    "(+- %.3f)\n", big.twap_bp, big.oea_bp, big.se_bp);
        check(big.oea_bp > big.twap_bp,
              "and at the paper's own 2.9%-of-ADV example it loses to TWAP: "
              "concentrating size costs more impact than the spread it saves");
    }

    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
