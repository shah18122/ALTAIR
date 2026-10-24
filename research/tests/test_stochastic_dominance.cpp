// analytics/tests/test_stochastic_dominance.cpp -- Olmo & Sanso-Navarro's
// conditional stochastic dominance test: known orderings first, then a
// conditional one, then NIFTY against BANKNIFTY.
//
// Optional arguments: NIFTY and BANKNIFTY daily CSVs. Without them the
// real-data section is skipped.

#include <research/models/stochastic_dominance.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

std::map<std::string, double> read_closes(const char* path) {
    std::map<std::string, double> m;
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string f[5];
        for (auto& v : f) std::getline(ss, v, ',');
        if (!f[4].empty()) m[f[0].substr(0, 10)] = std::stod(f[4]);
    }
    return m;
}

} // namespace

int main(int argc, char** argv) {
    using namespace altair;
    std::printf("Conditional stochastic dominance\n");
    std::mt19937_64 rng(1932225);
    std::normal_distribution<double> N;

    // ---- unconditional orderings that are known ----------------------------
    {
        std::vector<double> hi(400), lo(400), calm(400), wild(400);
        for (std::size_t t = 0; t < 400; ++t) {
            const double z = N(rng), u = N(rng);
            hi[t] = 0.5 + z; lo[t] = u;            // a shift: first order
            calm[t] = 0.5 * N(rng); wild[t] = N(rng);  // a spread: second order only
        }
        const auto ab = stochastic_dominance(hi, lo, 1), ba = stochastic_dominance(lo, hi, 1);
        std::printf("    shift, order 1: 'higher dominates' p %.2f, 'lower dominates' p %.3f\n",
                    ab.p_value, ba.p_value);
        check(ab.p_value > 0.1 && ba.p_value < 0.01,
              "order 1: a mean shift is found, and the reverse claim rejected");
        const auto c1 = stochastic_dominance(calm, wild, 1);
        const auto c2 = stochastic_dominance(calm, wild, 2);
        const auto w2 = stochastic_dominance(wild, calm, 2);
        std::printf("    spread: calm>wild order 1 p %.3f, order 2 p %.2f; wild>calm order 2 p %.3f\n",
                    c1.p_value, c2.p_value, w2.p_value);
        check(c1.p_value < 0.05 && c2.p_value > 0.1 && w2.p_value < 0.05,
              "same mean, half the spread: not first-order better, but every "
              "risk-averse investor prefers it (order 2)");
    }

    // ---- conditional: the ordering flips with the state ---------------------
    {
        const std::size_t n = 1200;
        std::vector<std::vector<double>> cond(n);
        std::vector<double> a(n), b(n), xs(n);
        for (std::size_t t = 0; t < n; ++t) {
            const double x = N(rng);
            cond[t] = {x};
            xs[t] = x;
            a[t] = 0.8 * x + 0.5 * N(rng);   // A does well when x is high
            b[t] = 0.5 * N(rng);
        }
        const std::vector<double> h{sd_silverman_bandwidth(xs)};
        const auto w_hi = sd_kernel_weights(cond, {1.0}, h);
        const auto w_lo = sd_kernel_weights(cond, {-1.0}, h);
        const auto hi_ab = stochastic_dominance(a, b, 1, w_hi), hi_ba = stochastic_dominance(b, a, 1, w_hi);
        const auto lo_ab = stochastic_dominance(a, b, 1, w_lo), lo_ba = stochastic_dominance(b, a, 1, w_lo);
        const auto un = stochastic_dominance(a, b, 1);
        const auto un_ba = stochastic_dominance(b, a, 1);
        std::printf("    x = +1: A>B p %.2f, B>A p %.3f | x = -1: A>B p %.3f, B>A p %.2f | "
                    "unconditional A>B p %.3f (n_eff at x=1: %.0f)\n",
                    hi_ab.p_value, hi_ba.p_value, lo_ab.p_value, lo_ba.p_value, un.p_value,
                    hi_ab.n_eff);
        check(hi_ab.p_value > 0.1 && hi_ba.p_value < 0.05 && lo_ab.p_value < 0.05
                  && lo_ba.p_value > 0.1,
              "conditioning on yesterday's state finds A better when x is high and "
              "B better when it is low");
        check(un.p_value < 0.05 && un_ba.p_value < 0.05,
              "which the unconditional test cannot say: there neither dominates");
    }

    // ---- NIFTY against BANKNIFTY --------------------------------------------
    //
    // Daily returns 2010-2026, conditioned on NIFTY's realised volatility over
    // the previous 20 sessions (known at the prior close), at a calm level
    // (its 20th percentile) and a stressed one (90th). Reported, not assumed.
    if (argc > 2) {
        const auto nf = read_closes(argv[1]), bn = read_closes(argv[2]);
        std::vector<std::string> days;
        for (const auto& [d, v] : nf) if (d >= "2010-01-01" && bn.count(d)) days.push_back(d);
        std::vector<double> rn, rb, vol;
        std::vector<std::vector<double>> cond;
        std::vector<double> past;
        for (std::size_t i = 1; i < days.size(); ++i) {
            const double a = std::log(nf.at(days[i]) / nf.at(days[i - 1]));
            const double b = std::log(bn.at(days[i]) / bn.at(days[i - 1]));
            if (past.size() >= 20) {
                double s2 = 0.0;
                for (std::size_t k = past.size() - 20; k < past.size(); ++k) s2 += past[k] * past[k];
                const double v = std::sqrt(s2 / 20.0 * 252.0);
                rn.push_back(a); rb.push_back(b); vol.push_back(v); cond.push_back({v});
            }
            past.push_back(a);
        }
        if (rn.size() > 1000) {
            std::vector<double> sorted = vol;
            std::sort(sorted.begin(), sorted.end());
            const double calm = sorted[sorted.size() / 5], stress = sorted[sorted.size() * 9 / 10];
            const std::vector<double> h{sd_silverman_bandwidth(vol)};
            for (const auto& [label, level] : {std::pair{"calm", calm}, std::pair{"stressed", stress}}) {
                const auto w = sd_kernel_weights(cond, {level}, h);
                const auto n2 = stochastic_dominance(rn, rb, 2, w, 100, 300);
                const auto b2 = stochastic_dominance(rb, rn, 2, w, 100, 300);
                std::printf("    %s (NIFTY 20d vol %.0f %%): NIFTY>BANKNIFTY order 2 p %.2f, "
                            "BANKNIFTY>NIFTY p %.2f (n_eff %.0f)\n",
                            label, 100.0 * level, n2.p_value, b2.p_value, n2.n_eff);
            }
            const auto u2 = stochastic_dominance(rn, rb, 2, {}, 100, 300);
            const auto v2 = stochastic_dominance(rb, rn, 2, {}, 100, 300);
            std::printf("    unconditional, %zu days to %s: NIFTY>BANKNIFTY order 2 p %.2f, reverse p %.2f\n",
                        rn.size(), days.back().c_str(), u2.p_value, v2.p_value);
            check(std::isfinite(u2.stat) && std::isfinite(v2.stat),
                  "the test runs on real index returns (the p-values above are the result)");
        } else {
            std::printf("    (index files too short: skipped)\n");
        }
    } else {
        std::printf("    (no index files given: real-data check skipped)\n");
    }

    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
