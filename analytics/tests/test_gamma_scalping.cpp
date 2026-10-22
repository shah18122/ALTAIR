// analytics/tests/test_gamma_scalping.cpp -- the delta-hedged P&L formulas
// against a Monte Carlo Black-76 hedge of a 21-day ATM call.

#include <analytics/gamma_scalping.hpp>
#include <analytics/greeks.hpp>

#include <cmath>
#include <cstdio>
#include <random>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

struct Run { double mean, sd, cost; };

/// Long one ATM call priced at sigma_i, short its Black-76 delta in futures,
/// rehedged n times to expiry; the forward realises sigma_r. Cost k is the
/// round-trip fraction, half paid on every futures trade. `cost` reports the
/// rebalancing trades only -- Leland's (4) prices those, not the opening
/// hedge or the unwind at expiry -- while the P&L pays them all.
Run hedge(double sigma_i, double sigma_r, int n, double k, unsigned seed) {
    using namespace altair;
    const double F0 = 100.0, K = 100.0, T = 21.0 / 365.0, dt = T / n;
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> Z;
    const int paths = 20000;
    double s1 = 0.0, s2 = 0.0, sc = 0.0;
    for (int p = 0; p < paths; ++p) {
        double F = F0;
        auto g = detail::black76_unchecked(OptionRight::Call, F, K, T, sigma_i, 0.0);
        double cash = -g.price, d = g.delta, cost = 0.5 * k * std::fabs(d) * F, rebal = 0.0;
        for (int i = 1; i <= n; ++i) {
            const double Fn = F * std::exp(-0.5 * sigma_r * sigma_r * dt
                                           + sigma_r * std::sqrt(dt) * Z(rng));
            cash -= d * (Fn - F);
            F = Fn;
            const double nd = i < n
                ? detail::black76_unchecked(OptionRight::Call, F, K, T - i * dt, sigma_i, 0.0).delta
                : 0.0;
            const double c = 0.5 * k * std::fabs(nd - d) * F;
            cost += c;
            if (i < n) { rebal += c; }
            d = nd;
        }
        const double pnl = cash + std::max(F - K, 0.0) - cost;
        s1 += pnl; s2 += pnl * pnl; sc += rebal;
    }
    const double m = s1 / paths;
    return {m, std::sqrt(s2 / paths - m * m), sc / paths};
}

} // namespace

int main() {
    using namespace altair;
    std::printf("Gamma scalping\n");
    const double T = 21.0 / 365.0;
    const double vega = detail::black76_unchecked(OptionRight::Call, 100.0, 100.0, T, 0.15, 0.0).vega;

    check(gamma_scalp_step(0.1, 100.0, 1.0, 0.1, 0.01) == 0.5 * 0.1 * (1.0 - 1.0),
          "(1) breaks even when the move is exactly the implied move");
    {
        // The article's second form: a move of exactly sigma_r sqrt(dt).
        const double G = 0.05, S = 100.0, dt = 1.0 / 365.0, sr = 0.20, si = 0.15;
        const double a = gamma_scalp_step(G, S, S * sr * std::sqrt(dt), si, dt);
        const double b = 0.5 * G * S * S * dt * (sr * sr - si * si);
        check(std::fabs(a - b) < 1e-12 && a > 0.0
                  && gamma_scalp_step(G, S, S * 0.10 * std::sqrt(dt), si, dt) < 0.0,
              "(1) = 1/2 Gamma S^2 dt (sigma_r^2 - sigma_i^2): wins only when sigma_r > sigma_i");
    }

    {
        const Run fair = hedge(0.15, 0.15, 84, 0.0, 1);
        const Run rich = hedge(0.15, 0.20, 84, 0.0, 2);
        const double se = rich.sd / std::sqrt(20000.0);
        std::printf("    sigma_r = sigma_i: mean %.4f; sigma_r 20 vs 15: mean %.4f, "
                    "formula %.4f\n", fair.mean, rich.mean, gamma_scalp_expected(vega, 0.20, 0.15));
        check(std::fabs(fair.mean) < 4.0 * fair.sd / std::sqrt(20000.0),
              "(2) no edge when realised equals implied");
        check(std::fabs(rich.mean - gamma_scalp_expected(vega, 0.20, 0.15)) < 4.0 * se,
              "(2) long gamma earns vega x (sigma_r - sigma_i), within 4 standard errors");
        // Path dependence: hedged 336 times at sigma_r = sigma_i the spread
        // is just hedging error; with sigma_r = 20 % it stays large however
        // often you hedge, because the P&L depends on WHERE the moves came
        // (near the strike, high gamma) and not only on how big they were.
        const Run fine_fair = hedge(0.15, 0.15, 336, 0.0, 4);
        const Run fine_rich = hedge(0.15, 0.20, 336, 0.0, 5);
        std::printf("    336 hedges: sd %.4f at sigma_r = sigma_i, %.4f at sigma_r 20 %%\n",
                    fine_fair.sd, fine_rich.sd);
        check(fine_rich.sd > 2.5 * fine_fair.sd,
              "path dependence: with realised above implied, the P&L spread does not "
              "shrink with hedging frequency");
    }
    for (int n : {21, 84, 336}) {
        const Run r = hedge(0.15, 0.15, n, 0.0, 10u + static_cast<unsigned>(n));
        const double f = gamma_scalp_hedge_error(vega, 0.15, n);
        std::printf("    %3d hedges: sd %.4f, Kamal-Derman %.4f\n", n, r.sd, f);
        check(std::fabs(r.sd / f - 1.0) < 0.08,
              "(3) hedging error matches sqrt(pi/4) vega sigma / sqrt(N) within 8%");
    }
    {
        // 5 bp round trip, hedged every quarter of a session.
        const double k = 0.0005, dt = T / 84.0;
        const Run c = hedge(0.15, 0.15, 84, k, 3);
        const double leland = vega * (gamma_scalp_leland_vol(0.15, k, dt, true) - 0.15);
        std::printf("    5 bp, 84 hedges: rebalancing cost %.4f, Leland %.4f\n", c.cost, leland);
        check(std::fabs(c.cost / leland - 1.0) < 0.15,
              "(4) Leland's adjusted vol prices the hedging cost within 15%");
        check(gamma_scalp_leland_vol(0.15, k, dt, false) < 0.15
                  && gamma_scalp_leland_vol(0.15, 1.0, dt, false) == 0.0,
              "(4) and lowers a short option's effective vol, floored at zero");
    }

    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
