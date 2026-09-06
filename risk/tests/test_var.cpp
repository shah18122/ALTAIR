// risk/tests/test_var.cpp -- P15-01 / P15-02.
//
// Two findings, one arithmetic and one empirical:
//
//   1. VaR IS NOT SUBADDITIVE. A constructed counterexample where the model
//      says diversification increased risk. This is why Basel moved the
//      trading book to Expected Shortfall, and it is almost never demonstrated
//      because it is easier to state than to build.
//
//   2. ON REAL NIFTY, THE GAUSSIAN NUMBER IS TOO SMALL AT THE TAIL. P11Q-10
//      measured excess kurtosis 11.34 on this series, so this is expected --
//      and expected is not the same as measured, which is why it is measured.

#include <risk/var.hpp>

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

std::vector<double> load_returns(const std::string& path) {
    std::vector<double> closes, out;
    std::ifstream f(path);
    if (!f) { return out; }
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string cell;
        int col = 0;
        double c = 0.0;
        while (std::getline(ss, cell, ',')) {
            if (col == 4 && !cell.empty()) { c = std::atof(cell.c_str()); }
            ++col;
        }
        if (c > 0.0) { closes.push_back(c); }
    }
    for (std::size_t i = 1; i < closes.size(); ++i) {
        out.push_back(std::log(closes[i] / closes[i - 1]));
    }
    return out;
}

} // namespace

int main() {
    using altair::historical_var;
    using altair::monte_carlo_var;
    using altair::parametric_var;

    std::printf("P15-01/02 Value at Risk and Expected Shortfall\n");

    // ---- 1. VaR IS NOT SUBADDITIVE ----------------------------------------
    //
    // Two independent defaultable bonds. Each pays +1 with probability 0.98
    // and loses 100 with probability 0.02. At 97.5% confidence, each ALONE has
    // its 2.5% quantile in the profitable region, so VaR is negative -- the
    // model reports no risk at all. Held together, the chance that at least
    // one defaults is 1 - 0.98^2 = 3.96%, which is now inside the tail, and
    // the portfolio VaR is a real loss.
    //
    // Diversification is supposed to reduce risk. VaR says it created it.
    {
        std::vector<double> a, b, both;
        // 1000 outcomes, exact proportions, so the quantile is unambiguous.
        for (int i = 0; i < 1000; ++i) {
            const bool da = i < 20;                    // 2% default for A
            const bool db = (i % 50) == 7;             // 2% for B, disjoint
            a.push_back(da ? -100.0 : 1.0);
            b.push_back(db ? -100.0 : 1.0);
            both.push_back(((da ? -100.0 : 1.0) + (db ? -100.0 : 1.0)) / 2.0);
        }
        const auto va = historical_var(a, 0.975);
        const auto vb = historical_var(b, 0.975);
        const auto vab = historical_var(both, 0.975);
        check(va && vb && vab, "all three portfolios price");
        if (va && vb && vab) {
            std::printf("    VaR(A) = %.2f, VaR(B) = %.2f, sum = %.2f\n",
                        va->var, vb->var, va->var + vb->var);
            std::printf("    VaR(A+B, equally weighted) = %.2f\n", vab->var);
            check(vab->var > va->var + vb->var,
                  "VaR(A+B) EXCEEDS VaR(A) + VaR(B): the model says combining "
                  "two independent positions INCREASED risk. That is not a "
                  "numerical artefact, it is a property of quantiles, and it "
                  "is why Basel moved the trading book to Expected Shortfall");

            std::printf("    ES(A) = %.2f, ES(B) = %.2f, sum = %.2f;"
                        "  ES(A+B) = %.2f\n",
                        va->expected_shortfall, vb->expected_shortfall,
                        va->expected_shortfall + vb->expected_shortfall,
                        vab->expected_shortfall);
            check(vab->expected_shortfall
                      <= va->expected_shortfall + vb->expected_shortfall + 1e-9,
                  "Expected Shortfall is SUBADDITIVE on the same portfolios, "
                  "so it says what a risk measure has to say: diversifying "
                  "did not make things worse");
        }
    }

    // ---- 2. THE REAL SERIES -----------------------------------------------
    const std::string path =
        std::string(ALTAIR_DATASET_DIR) + "/spot/nifty/1d/all.csv";
    const auto r = load_returns(path);
    if (r.size() < 2000) {
        std::printf("  SKIP: %s has %zu returns\n", path.c_str(), r.size());
        std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
        return failures == 0 ? 0 : 1;
    }
    std::printf("\n  %zu daily NIFTY log returns\n", r.size());

    for (const double conf : {0.95, 0.99}) {
        const auto h = historical_var(r, conf);
        const auto p = parametric_var(r.data(), r.size(), conf);
        const auto m = monte_carlo_var(r, conf, 200000, 1, 0xBEEF);
        if (!h || !p || !m) { continue; }

        std::printf("\n  %.0f%% one-day, as a percentage of capital\n",
                    100.0 * conf);
        std::printf("    %-22s %10s %10s %8s\n", "", "VaR", "ES", "tail n");
        std::printf("    %-22s %9.3f%% %9.3f%% %8zu\n", "historical",
                    100.0 * h->var, 100.0 * h->expected_shortfall, h->tail_n);
        std::printf("    %-22s %9.3f%% %9.3f%% %8s\n", "parametric (normal)",
                    100.0 * p->var, 100.0 * p->expected_shortfall, "n/a");
        std::printf("    %-22s %9.3f%% %9.3f%% %8zu\n", "Monte Carlo (resampled)",
                    100.0 * m->var, 100.0 * m->expected_shortfall, m->tail_n);

        const double gap = h->var / p->var;
        std::printf("    historical / parametric = %.2fx\n", gap);

        check(h->expected_shortfall > h->var,
              "ES exceeds VaR, as it must -- it is the mean of the losses "
              "BEYOND the quantile, and VaR says nothing about their size");

        if (conf >= 0.99) {
            check(gap > 1.05,
                  "at 99% the historical loss is materially LARGER than the "
                  "Gaussian one: the normal cannot represent a series with "
                  "excess kurtosis 11.34, and it errs SMALL -- the direction "
                  "that sizes you bigger than the market allows");
        }
        check(m->var > h->var * 0.8 && m->var < h->var * 1.25,
              "and the Monte Carlo number brackets the historical one, "
              "because it resamples the SAME empirical tail rather than "
              "drawing from a fitted normal -- doing that would make it a "
              "slow parametric VaR wearing a costume");
    }

    // ---- 3. WHAT HISTORICAL VaR STRUCTURALLY CANNOT SEE --------------------
    //
    // Its blind spot, demonstrated rather than described: on a quiet window it
    // reports that the worst day it has ever seen is the worst day possible.
    {
        std::vector<double> calm(r.end() - 250, r.end());
        const auto h_calm = historical_var(calm, 0.99);
        const auto h_all = historical_var(r, 0.99);
        if (h_calm && h_all) {
            std::printf("\n  99%% VaR on the LAST 250 DAYS: %.3f%%"
                        "   on all %zu: %.3f%%\n",
                        100.0 * h_calm->var, r.size(), 100.0 * h_all->var);
            std::printf("    tail observations: %zu against %zu\n",
                        h_calm->tail_n, h_all->tail_n);
            check(h_calm->tail_n <= 2,
                  "a 99% VaR from 250 days rests on TWO observations, and an "
                  "ES computed from two numbers is a number rather than an "
                  "estimate -- which is why tail_n is returned and not left "
                  "for the reader to work out");
        }
    }

    // ---- 4. REFUSALS ------------------------------------------------------
    {
        std::vector<double> few(10, 0.01);
        check(!historical_var(few, 0.99).has_value(),
              "ten observations cannot support a 99% quantile and are refused");
        check(!historical_var(r, 1.0).has_value(),
              "100% confidence is refused -- there is no quantile beyond the "
              "worst observation, and returning it would claim certainty");
        check(!historical_var(r, 0.4).has_value(),
              "and a confidence below 50% is refused rather than silently "
              "reporting a gain as a risk number");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
