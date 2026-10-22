// oms/tests/test_alpha_execution.cpp -- Lai (2026), execution with an alpha
// signal.
//
// The closed form is checked against the equation it claims to solve, then
// the rule is run on simulated prices with an Ornstein-Uhlenbeck signal
// against plain Almgren-Chriss, same paths, same random numbers. The
// benchmark is the paper's own objective: the signal-aware rule must not
// lose to the rule that ignores the signal.

#include <oms/alpha_execution.hpp>

#include <cmath>
#include <cstdio>
#include <random>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

/// Eq. 9 integrated backwards from B(T) = 0 by RK4, in tau = T - t:
/// dB/dtau = 1 - (A/eta + phi) B with A/eta = kappa coth(kappa tau).
double b_by_ode(const altair::AlphaExecParams& p, double tau_end) {
    const int n = 200000;
    const double h = tau_end / n;
    const auto rhs = [&](double tau, double b) {
        // A/eta blows up like 1/tau at tau = 0, but B ~ tau there, so the
        // product is finite; start one tiny step in.
        return 1.0 - (altair::alpha_exec_urgency(p, tau) + p.phi) * b;
    };
    // Near tau = 0, B' = 1 - B/tau + O(1) gives B = tau/2.
    double tau = 1e-9, b = 0.5 * tau;
    for (int i = 0; i < n; ++i) {
        const double k1 = rhs(tau, b);
        const double k2 = rhs(tau + 0.5 * h, b + 0.5 * h * k1);
        const double k3 = rhs(tau + 0.5 * h, b + 0.5 * h * k2);
        const double k4 = rhs(tau + h, b + h * k3);
        b += h * (k1 + 2 * k2 + 2 * k3 + k4) / 6.0;
        tau += h;
    }
    return b;
}

struct Run { double j = 0, j2 = 0, cash = 0; };

} // namespace

int main() {
    using altair::AlphaExecParams;
    using altair::alpha_exec_b;
    using altair::alpha_exec_rate;
    using altair::alpha_exec_urgency;

    std::printf("Lai (2026) alpha-signal execution\n");

    // ---- 1. B(tau) SOLVES EQ. 9 -------------------------------------------
    {
        struct Case { double eta, lambda, sigma, phi; const char* name; };
        const Case cases[] = {
            {0.01, 1.0, 0.1, 5.0, "kappa 1, phi 5"},
            {0.01, 1.0, 0.1, 1.0, "phi = kappa (the removable 0/0)"},
            {0.01, 1.0, 0.1, 0.0, "phi 0 (a persistent drift)"},
            {0.01, 0.0, 0.1, 3.0, "lambda 0 (the TWAP limit)"},
            {0.01, 400.0, 0.1, 2.0, "kappa 20 (strongly risk-averse)"},
        };
        bool ok = true;
        for (const Case& c : cases) {
            AlphaExecParams p{c.eta, c.lambda, c.sigma, c.phi};
            for (double tau : {0.05, 0.5, 1.0}) {
                const double closed = alpha_exec_b(p, tau);
                const double ode = b_by_ode(p, tau);
                const double err = std::fabs(closed - ode)
                                 / std::max(1e-12, std::fabs(ode));
                if (err > 1e-6) {
                    std::printf("    %s tau %.2f: closed %.9f ode %.9f\n",
                                c.name, tau, closed, ode);
                    ok = false;
                }
            }
        }
        check(ok, "the closed-form B(tau) matches a direct RK4 solution of "
                  "eq. 9 to 1e-6, across kappa, phi = kappa, phi = 0, "
                  "lambda = 0");
        AlphaExecParams p{0.01, 1.0, 0.1, 5.0};
        check(alpha_exec_b(p, 0.0) == 0.0 && alpha_exec_b(p, 1e-6) > 0.0,
              "B(0) = 0, as eq. 9's terminal condition requires, and B > 0 "
              "before it");

        // The paper's printed eq. 10 integrand, sinh(k tau)/sinh(k(tau-u)) in
        // the u = T - s variable, near s = T. It is unbounded: the printed
        // B would be infinite everywhere.
        const double k = p.kappa(), tau = 1.0;
        const double near = std::sinh(k * tau) / std::sinh(k * 1e-6);
        check(near > 1e5, "and the ratio as PRINTED in eq. 10 is unbounded "
                          "near s = T (1/(T-s)), which is why it is inverted "
                          "here");
    }

    // ---- 2. THE LIMITS -----------------------------------------------------
    {
        AlphaExecParams p{0.01, 1.0, 0.1, 5.0};
        const double tau = 0.7, x = 0.4;
        check(std::fabs(alpha_exec_rate(p, tau, x, 0.0)
                        - p.kappa() / std::tanh(p.kappa() * tau) * x) < 1e-15,
              "with no signal the rate is Almgren-Chriss, kappa coth(kappa "
              "tau) x");
        AlphaExecParams rn{0.01, 0.0, 0.1, 5.0};
        check(std::fabs(alpha_exec_urgency(rn, tau) * x - x / tau) < 1e-15,
              "and with lambda = 0 it is TWAP, x / tau");
        check(alpha_exec_rate(p, tau, x, 0.5) > alpha_exec_rate(p, tau, x, 0.0)
                  && alpha_exec_rate(p, tau, x, -0.5) < alpha_exec_rate(p, tau, x, 0.0),
              "a rising-price signal buys faster, a falling one slower");
        check(alpha_exec_rate(p, tau, 0.01, -50.0, true) == 0.0
                  && alpha_exec_rate(p, tau, 0.01, -50.0, false) < 0.0,
              "the buy-only clamp (eq. 12) refuses to sell to chase alpha");
    }

    // ---- 3. THE BENCHMARK: SAME PATHS, SIGNAL vs NO SIGNAL ----------------
    //
    // One day, X = 1, eta = 0.01, lambda sigma^2 = 0.01 (kappa = 1). The
    // signal has phi = 5 per day and a stationary sd of 0.05 price units
    // per day, so ignoring it costs about as much as the impact does.
    {
        const AlphaExecParams p{0.01, 1.0, 0.1, 5.0};
        const double T = 1.0, X = 1.0, sd_f = 0.05;
        const double nu = sd_f * std::sqrt(2.0 * p.phi);
        const int steps = 400, paths = 20000;
        const double dt = T / steps;
        std::mt19937_64 rng(20260301);
        std::normal_distribution<double> N;

        Run ac, sig, clamp;
        double diff = 0, diff2 = 0;
        for (int n = 0; n < paths; ++n) {
            double f = sd_f * N(rng);
            const double s0 = 100.0;
            double s = s0;
            double xa = X, xs = X, xc = X;
            double ja = 0, js = 0, jc = 0, ca = 0, cs = 0, cc = 0;
            for (int k = 0; k < steps; ++k) {
                const double tau = T - k * dt;
                const bool last = k == steps - 1;
                const double va = last ? xa / dt : alpha_exec_rate(p, tau, xa, 0.0);
                const double vs = last ? xs / dt
                    : alpha_exec_rate(p, tau, xs, f, false);
                const double vc = last ? xc / dt
                    : std::min(alpha_exec_rate(p, tau, xc, f, true), xc / dt);
                const auto step = [&](double v, double& x, double& j, double& c) {
                    j += (p.eta * v * v + p.lambda * p.sigma * p.sigma * x * x
                          + f * x) * dt;
                    c += ((s - s0) + p.eta * v) * v * dt;   // cash above S0 X
                    x -= v * dt;
                };
                step(va, xa, ja, ca);
                step(vs, xs, js, cs);
                step(vc, xc, jc, cc);
                // Permanent impact is the same total for every schedule and
                // is left out of the price so the three see one path.
                s += f * dt + p.sigma * std::sqrt(dt) * N(rng);
                f += -p.phi * f * dt + nu * std::sqrt(dt) * N(rng);
            }
            ac.j += ja; ac.cash += ca;
            sig.j += js; sig.cash += cs;
            clamp.j += jc; clamp.cash += cc;
            diff += ja - js;
            diff2 += (ja - js) * (ja - js);
        }
        const double m = 1.0 / paths;
        const double md = diff * m;
        const double se = std::sqrt((diff2 * m - md * md) * m);
        std::printf("    E[J]   AC %.6f  signal %.6f  signal, buy-only %.6f\n",
                    ac.j * m, sig.j * m, clamp.j * m);
        std::printf("    E[cash above S0 X]  AC %.6f  signal %.6f  buy-only "
                    "%.6f\n", ac.cash * m, sig.cash * m, clamp.cash * m);
        std::printf("    AC - signal: %.6f +- %.6f (1 se)\n", md, se);
        check(md > 3.0 * se,
              "on the paper's objective the signal-aware rule beats "
              "Almgren-Chriss by more than three standard errors");
        check(clamp.j * m < ac.j * m,
              "and so does the buy-only version, which never sells");
        check(sig.cash * m < ac.cash * m,
              "the cash actually paid is lower too, not just the objective");
    }

    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
