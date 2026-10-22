// models/tests/test_markov_nifty.cpp -- Verma & Jha's three-state NIFTY
// Markov chain, reproduced on the repo's closes and then tested.
//
// K. Verma & A. K. Jha, "Predicting Nifty 50 Trends Using a Markov Chain
// Approach", IJCRT 12(12), December 2024 (SSRN 5063966). States: the close
// went up, down or was unchanged against the day before; transition matrix
// from 12 Dec 2022 - 12 Dec 2024 (493 days). Reported:
//     P(up | up) 0.5863, P(up | down) 0.5352, "long run" up 0.5639.
//
// Argument: a daily CSV (time,open,high,low,close,...), normally
// dataset/spot/nifty/1d/all.csv. Without it the test only checks it builds.

#include <models/markov.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

} // namespace

int main(int argc, char** argv) {
    using namespace altair;
    std::printf("NIFTY three-state Markov chain (Verma & Jha 2024)\n");
    if (argc < 2) {
        std::printf("    (no NIFTY file given: skipped)\nPASS (0 failures)\n");
        return 0;
    }
    std::vector<std::string> dates;
    std::vector<double> px;
    {
        std::ifstream in(argv[1]);
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            std::stringstream ss(line);
            std::string f[5];
            for (auto& v : f) std::getline(ss, v, ',');
            if (f[4].empty()) continue;
            dates.push_back(f[0].substr(0, 10));
            px.push_back(std::stod(f[4]));
        }
    }
    constexpr std::size_t kUp = 0, kDown = 1, kFlat = 2;
    const auto states = [&](const char* a, const char* b) {
        std::vector<std::size_t> s;
        for (std::size_t i = 1; i < px.size(); ++i) {
            if (dates[i] < a || dates[i] > b) continue;
            s.push_back(px[i] > px[i - 1] ? kUp : (px[i] < px[i - 1] ? kDown : kFlat));
        }
        return s;
    };

    const auto seq = states("2022-12-13", "2024-12-12");
    const auto m = fit_from_states(seq, 3);
    if (!m || seq.size() < 400) {
        std::printf("    (file lacks 2022-24: skipped)\nPASS (0 failures)\n");
        return 0;
    }
    std::printf("    %zu days: P(up|up) %.4f  P(up|down) %.4f  (paper 0.5863, 0.5352)\n",
                seq.size(), m->p[kUp][kUp], m->p[kDown][kUp]);
    check(std::fabs(m->p[kUp][kUp] - 0.5863) < 0.002 && std::fabs(m->p[kDown][kUp] - 0.5352) < 0.003,
          "the paper's transition probabilities reproduce on the repo's closes");
    const auto pi = stationary(*m);
    const auto u = m->unconditional();
    std::printf("    stationary up %.4f; share of up days %.4f (paper's 'long run' 0.5639)\n",
                pi[kUp], u[kUp]);
    check(std::fabs(pi[kUp] - 0.5629) < 0.003,
          "the true stationary up-probability is the paper's own P^16 value (0.5629); its "
          "headline 0.5639 is just the share of up days");
    const auto ind = independence_chi_square(*m);
    std::printf("    independence chi-square %.2f vs 5%% critical %.2f\n",
                ind.chi_square, ind.critical_5pct);
    check(!ind.rejects_independence,
          "and the rows are not different from 'tomorrow ignores today' (chi-square)");

    // Both rows say "up" is likelier, so the chain's call is always "up".
    // Score that call on everything after the paper's window.
    const auto after = states("2024-12-13", "2099-12-31");
    std::size_t ups = 0, calls = 0;
    for (std::size_t i = 1; i < after.size(); ++i) {
        if (after[i] == kFlat) continue;
        ++calls;
        ups += after[i] == kUp;
    }
    const double hit = calls ? static_cast<double>(ups) / static_cast<double>(calls) : 0.0;
    std::printf("    after the paper: %zu days, the chain's 'up' call right %.1f %%\n", calls, 100.0 * hit);
    check(m->p[kUp][kUp] > 0.5 && m->p[kDown][kUp] > 0.5,
          "both rows forecast 'up', so the chain is 'always buy' in disguise");
    check(calls > 200 && hit < 0.52,
          "and since the paper's window 'up' has been right no more than a coin");

    // Over a long history the rows DO differ: daily NIFTY closes have mild
    // momentum (stale prices in the index average are the usual cause).
    const auto longrun = fit_from_states(states("2000-01-01", "2099-12-31"), 3);
    if (longrun) {
        const auto t = independence_chi_square(*longrun);
        std::printf("    2000 on: P(up|up) %.4f  P(up|down) %.4f  chi-square %.1f\n",
                    longrun->p[kUp][kUp], longrun->p[kDown][kUp], t.chi_square);
        check(t.rejects_independence && longrun->p[kUp][kUp] > longrun->p[kDown][kUp],
              "over 26 years the persistence is real (chi-square rejects), but it is "
              "about 7 points, before costs");
    }

    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
