// app/optimal_exec_main.cpp -- altair_optimal_exec: Forsyth's HJB execution.
//
// research/execution/hjb_execution.hpp on the paper's own data. Prints, for one example:
//   * the refinement ladder of E[(B_L - gamma/2)^2], E and sd against the
//     paper's Table 7.3 (the short example only -- it is the one the paper
//     tabulates);
//   * the efficient frontier over a range of gamma;
//   * every Almgren-Chriss trajectory (kT from 0 = TWAP up to 64), priced by
//     the same PDE on the same grid, so the HJB can be read against the
//     benchmark it has to match or beat.
//
// Writes nothing. Level 2 takes about 20 s, level 3 several minutes.

#include <research/execution/hjb_execution.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace {

void usage(const char* argv0) {
    std::printf(
        "usage: %s [--example short|long] [--level N] [--gamma G]\n"
        "  --example  short: Table 7.1 (one day); long: Table 8.1 (one month)\n"
        "  --level    finest refinement level, 0..4 (default 2)\n"
        "  --gamma    the frontier point to tabulate (default 200, or 200.83\n"
        "             for the long example)\n",
        argv0);
}

std::vector<double> ac_rates(const altair::HjbParams& p, std::size_t steps,
                             double kT) {
    const double T = p.horizon, dt = T / static_cast<double>(steps);
    const auto x = [&](double t) {
        return kT < 1e-9 ? 1.0 - t / T
                         : std::sinh(kT * (1.0 - t / T)) / std::sinh(kT);
    };
    std::vector<double> r(steps);
    for (std::size_t m = 0; m < steps; ++m) {
        const double t0 = dt * static_cast<double>(m);
        r[m] = -p.alpha0 * (x(t0) - x(t0 + dt)) / dt;
    }
    return r;
}

} // namespace

int main(int argc, char** argv) {
    bool long_example = false;
    unsigned level = 2;
    double gamma = 0.0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--example" && i + 1 < argc) {
            long_example = std::string_view{argv[++i]} == "long";
        } else if (a == "--level" && i + 1 < argc) {
            level = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--gamma" && i + 1 < argc) {
            gamma = std::strtod(argv[++i], nullptr);
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (level > 4) { level = 4; }
    const altair::HjbParams p = long_example ? altair::hjb_forsyth_long()
                                             : altair::hjb_forsyth_short();
    if (gamma <= 0.0) { gamma = long_example ? 200.83 : 200.0; }

    std::printf("Forsyth HJB execution, %s example, gamma %.2f\n",
                long_example ? "long (Table 8.1)" : "short (Table 7.1)", gamma);

    static constexpr double kPaper[] = {1.668460, 1.319408, 1.176402,
                                        1.094543, 1.054693};
    std::printf("\nlevel  E[(B_L-g/2)^2]   paper      E[B_L]     sd\n");
    double best_v = 0.0;
    for (unsigned k = 0; k <= level; ++k) {
        const auto s = altair::hjb_solve(p, altair::HjbGrid::level(k), gamma);
        if (!s) { std::printf("%5u  solve failed\n", k); return 1; }
        const auto& q = s->point();
        best_v = q.objective;
        if (long_example) {
            std::printf("%5u  %14.6f   %8s  %9.4f  %7.4f\n", k, q.objective,
                        "-", q.expected, q.stdev);
        } else {
            std::printf("%5u  %14.6f   %8.6f  %9.4f  %7.4f\n", k, q.objective,
                        kPaper[k], q.expected, q.stdev);
        }
    }
    std::printf("paper, finest level: E %.3f sd %.4f\n",
                long_example ? 95.6 : 99.295, long_example ? 3.47 : 0.7469);

    const auto g = altair::HjbGrid::level(level);
    std::printf("\nefficient frontier at level %u\n  gamma      E[B_L]     sd\n",
                level);
    for (double gm : {0.995 * 2 * p.s0, 1.02 * 2 * p.s0, 1.1 * 2 * p.s0,
                      1.5 * 2 * p.s0, 3.0 * 2 * p.s0}) {
        const auto s = altair::hjb_solve(p, g, gm);
        if (s) {
            std::printf("  %6.1f  %9.4f  %7.4f\n", gm, s->point().expected,
                        s->point().stdev);
        }
    }

    std::printf("\nAlmgren-Chriss on the same model and grid (gamma %.2f)\n"
                "   kT      E[B_L]     sd     E[(B_L-g/2)^2]\n", gamma);
    double best_ac = 1e300;
    for (double kT : {0.0, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0}) {
        const auto q = altair::hjb_evaluate(p, g, gamma, ac_rates(p, g.steps, kT));
        if (!q) { continue; }
        std::printf("  %4.0f  %9.4f  %7.4f  %12.6f\n", kT, q->expected, q->stdev,
                    q->objective);
        if (q->objective < best_ac) { best_ac = q->objective; }
    }
    std::printf("\nHJB %.6f vs best Almgren-Chriss %.6f: %s\n", best_v, best_ac,
                best_v <= best_ac ? "HJB no worse" : "HJB WORSE -- investigate");
    return best_v <= best_ac ? 0 : 1;
}
