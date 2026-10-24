// oms/tests/test_hjb_execution.cpp -- Forsyth's HJB optimal execution.
//
// Three things are checked, in the order they would fail:
//   1. the PDE prices a schedule whose answer is known in closed form (TWAP
//      on a geometric Brownian price) -- if this is wrong nothing else means
//      anything;
//   2. the HJB reproduces the paper's own Table 7.3 trend on its Table 7.1
//      data, and the frontier bends the right way;
//   3. on the same model and the same grid, the HJB is never beaten by any
//      Almgren-Chriss trajectory -- the benchmark it has to match or exceed.
//
// Level 2 (17 s) is too slow for a sanitizer job; altair_optimal_exec prints
// the full ladder against Table 7.3.

#include <research/execution/hjb_execution.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

/// E and sd of B_L for a TWAP sale of alpha0 on dS = sigma S dZ (eta = r =
/// kappa_p = 0): B_L = f(-alpha0/T) * (alpha0/T) * int_0^T S dt.
struct Moments { double mean, sd; };
Moments twap_closed_form(const altair::HjbParams& p) {
    const double T = p.horizon, s2 = p.sigma * p.sigma;
    const double f = p.f(-p.alpha0 / T);
    const double I = (std::exp(s2 * T) - 1.0) / (s2 * s2) - T / s2 - 0.5 * T * T;
    const double var = p.s0 * p.s0 * 2.0 / (T * T) * I;
    return Moments{p.alpha0 * p.s0 * f, p.alpha0 * f * std::sqrt(var)};
}

/// The Almgren-Chriss trajectory x(t) = sinh(k(T-t))/sinh(kT) as a rate per
/// forward step. kT = 0 is TWAP.
std::vector<double> ac_rates(const altair::HjbParams& p, std::size_t steps,
                             double kT) {
    const double T = p.horizon, dt = T / static_cast<double>(steps);
    const auto x = [&](double t) {
        return kT < 1e-9 ? 1.0 - t / T
                         : std::sinh(kT * (1.0 - t / T)) / std::sinh(kT);
    };
    std::vector<double> r(steps);
    for (std::size_t m = 0; m < steps; ++m) {
        const double t0 = dt * static_cast<double>(m), t1 = t0 + dt;
        r[m] = -p.alpha0 * (x(t0) - x(t1)) / dt;
    }
    return r;
}

} // namespace

int main() {
    using altair::HjbGrid;
    using altair::HjbParams;
    using altair::hjb_evaluate;
    using altair::hjb_forsyth_short;
    using altair::hjb_solve;

    std::printf("Forsyth HJB optimal execution\n");

    // ---- 0. REFUSALS ------------------------------------------------------
    {
        HjbParams bad = hjb_forsyth_short();
        bad.sigma = 0.0;
        check(!hjb_solve(bad, HjbGrid::level(0), 200.0).has_value(),
              "zero volatility is refused");
        check(!hjb_solve(hjb_forsyth_short(), HjbGrid::level(0), 0.0).has_value(),
              "a non-positive gamma is refused");
        HjbParams no_sell = hjb_forsyth_short();
        no_sell.v_min = 0.0;
        check(!hjb_solve(no_sell, HjbGrid::level(0), 200.0).has_value(),
              "a sell program with no selling rate is refused");
        check(!hjb_evaluate(hjb_forsyth_short(), HjbGrid::level(0), 200.0,
                            std::vector<double>(3, -1.0)).has_value(),
              "a schedule with the wrong number of steps is refused");
    }

    // ---- 1. THE IMPACT FUNCTION -------------------------------------------
    {
        HjbParams p = altair::hjb_forsyth_long();
        check(p.f(0.0) == 1.0, "f(0) = 1: not trading costs nothing");
        const double v = -12.0;
        const double want = (1.0 - p.kappa_s)
                          * std::exp(-p.kappa_t * std::pow(12.0, p.beta));
        check(std::fabs(p.f(v) - want) < 1e-15,
              "f(v) = [1 + kappa_s sgn v] exp(kappa_t sgn(v) |v|^beta), eq. 2.10");
        check(p.f(-24.0) < p.f(-12.0) && p.f(24.0) > p.f(12.0),
              "selling faster gets a worse price, buying faster pays more");
        const HjbParams s = hjb_forsyth_short();
        check(std::fabs(s.liquidation(-100.0, 100.0, 1.0)
                        - (-100.0 + 100.0 * std::exp(-2.0))) < 1e-12,
              "the terminal dump of one share over 1e-6 years sells at "
              "exp(-2) of the price: the penalty for not finishing");
    }

    // ---- 2. A SCHEDULE WITH A KNOWN ANSWER --------------------------------
    //
    // TWAP has a closed-form mean and variance. With sigma -> 0 it has no
    // variance at all; the first version of the solver, which interpolated
    // V directly, reported a standard deviation of 7 here.
    {
        HjbParams p = hjb_forsyth_short();
        p.sigma = 1e-4;
        const auto g0 = HjbGrid::level(0);
        const auto q = hjb_evaluate(p, g0, 200.0, ac_rates(p, g0.steps, 0.0));
        check(q.has_value(), "the TWAP schedule evaluates");
        if (q) {
            std::printf("    sigma 1e-4 TWAP: E %.4f sd %.5f\n", q->expected,
                        q->stdev);
            check(q->stdev < 0.01, "a riskless TWAP has (almost) no variance");
        }

        p.sigma = 1.0;
        const Moments m = twap_closed_form(p);
        double prev_err = 1e9;
        bool shrinking = true;
        double sd1 = 0.0;
        for (unsigned k = 0; k <= 1; ++k) {
            const auto g = HjbGrid::level(k);
            const auto t = hjb_evaluate(p, g, 200.0, ac_rates(p, g.steps, 0.0));
            if (!t) { shrinking = false; break; }
            const double err = std::fabs(t->expected - m.mean);
            std::printf("    sigma 1 TWAP level %u: E %.4f sd %.4f  "
                        "(closed form E %.4f sd %.4f)\n",
                        k, t->expected, t->stdev, m.mean, m.sd);
            shrinking = shrinking && err < prev_err;
            prev_err = err;
            sd1 = t->stdev;
        }
        check(shrinking, "the TWAP mean converges to the closed form as the "
                         "grid is refined");
        check(std::fabs(sd1 - m.sd) / m.sd < 0.03,
              "and the TWAP standard deviation is within 3% of it at level 1");
    }

    // ---- 3. TABLE 7.1 / 7.3 -----------------------------------------------
    //
    // Paper: E[(B_L - 100)^2] = 1.668, 1.319, 1.176, 1.095, 1.055 at levels
    // 0..4, and at the finest level E = 99.295, sd = 0.7469.
    const HjbParams p = hjb_forsyth_short();
    const auto g1 = HjbGrid::level(1);
    double v1 = 0.0;
    {
        const auto s0 = hjb_solve(p, HjbGrid::level(0), 200.0);
        const auto s1 = hjb_solve(p, g1, 200.0);
        check(s0.has_value() && s1.has_value(), "the HJB solves at levels 0, 1");
        if (s0 && s1) {
            const auto& a = s0->point();
            const auto& b = s1->point();
            v1 = b.objective;
            std::printf("    level 0: V %.4f E %.4f sd %.4f   (paper V 1.668)\n",
                        a.objective, a.expected, a.stdev);
            std::printf("    level 1: V %.4f E %.4f sd %.4f   (paper V 1.319)\n",
                        b.objective, b.expected, b.stdev);
            check(b.objective < a.objective,
                  "V falls under refinement, as in Table 7.3");
            check(std::fabs(b.objective - 1.319) / 1.319 < 0.15,
                  "level 1 is within 15% of the paper's 1.319");
            check(std::fabs(b.expected - 99.295) < 0.3
                      && std::fabs(b.stdev - 0.7469) < 0.15,
                  "and E, sd are already near the paper's 99.295, 0.7469");
            check(std::fabs(b.objective
                            - ((b.expected - 100.0) * (b.expected - 100.0)
                               + b.stdev * b.stdev)) < 1e-9,
                  "V = (E - gamma/2)^2 + sd^2: mean and variance agree with "
                  "the objective");
        }
    }

    // ---- 4. THE FRONTIER BENDS THE RIGHT WAY ------------------------------
    //
    // gamma/2 is the target the quadratic aims at, so it has to sit above the
    // best attainable gain (about 100 here). Below it the pre-commitment
    // problem deliberately throws money away to land on the target -- at
    // gamma = 150 it sells for 75 with almost no spread -- which is the known
    // inefficient branch of the mean-variance frontier, not a point on it.
    {
        const double gammas[] = {200.0, 250.0, 400.0};
        double prev_e = -1e9, prev_sd = -1.0;
        bool ok = true;
        for (double gm : gammas) {
            const auto s = hjb_solve(p, g1, gm);
            if (!s) { ok = false; break; }
            std::printf("    gamma %5.0f: E %.4f sd %.4f\n", gm,
                        s->point().expected, s->point().stdev);
            ok = ok && s->point().expected > prev_e && s->point().stdev > prev_sd;
            prev_e = s->point().expected;
            prev_sd = s->point().stdev;
        }
        check(ok, "a larger gamma buys expected gain with risk: E and sd both "
                  "rise along the frontier");
    }

    // ---- 5. THE BENCHMARK: NEVER WORSE THAN ALMGREN-CHRISS ----------------
    //
    // Every static AC trajectory (kT from 0, which is TWAP, up to strongly
    // front-loaded) is evaluated by the SAME PDE on the SAME grid, so the
    // only difference is the strategy. The HJB minimises E[(B_L - gamma/2)^2]
    // over all strategies, the static ones included, so it must not lose.
    {
        double best_ac = 1e18, best_kt = 0.0;
        for (double kT : {0.0, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0}) {
            const auto q = hjb_evaluate(p, g1, 200.0, ac_rates(p, g1.steps, kT));
            if (!q) { continue; }
            std::printf("    AC kT %4.0f: E %.4f sd %.4f  V %.4f\n", kT,
                        q->expected, q->stdev, q->objective);
            if (q->objective < best_ac) { best_ac = q->objective; best_kt = kT; }
        }
        std::printf("    HJB V %.4f vs best AC V %.4f (kT %.0f)\n", v1, best_ac,
                    best_kt);
        check(v1 > 0.0 && v1 <= best_ac,
              "the HJB objective is no worse than the best Almgren-Chriss "
              "trajectory on the same model and grid");
    }

    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
