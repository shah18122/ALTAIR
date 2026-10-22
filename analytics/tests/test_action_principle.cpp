// analytics/tests/test_action_principle.cpp -- Manhire's oscillator bound.
//
// The paper's own numbers first, then the two places it does not hold
// together, then the bound against a Gaussian on fat-tailed simulated weeks.

#include <analytics/action_principle.hpp>

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

} // namespace

int main() {
    using altair::action_bound;
    std::printf("Manhire action-principle bound\n");

    // ---- the paper's SPY and DIA numbers -----------------------------------
    {
        const auto spy = action_bound(1.76, 0.5398, 5.0);
        check(spy.has_value(), "SPY: the bound exists");
        if (spy) {
            std::printf("    SPY |A| %.2f  m %.4f\n", spy->amplitude, spy->inertia);
            check(std::fabs(spy->amplitude - 22.11) < 0.01,
                  "SPY |A| = 1.76 / (2 x 0.5398 - 1) = 22.11 points (eq. 6)");
            check(std::fabs(spy->inertia - 0.2019) < 0.0001,
                  "SPY m = 2 x 5 x (pi / 22.11)^2 = 0.2019 (eq. 12)");
            check(spy->inside(-21.84) && !spy->inside(-22.5),
                  "the October 2008 crash week (-21.84) is inside the bound");
            check(std::fabs(spy->phase(spy->amplitude) - std::numbers::pi) < 1e-12,
                  "phi = pi X / |A|: the bound itself sits at phi = pi");
        }
        // DIA: the paper gives only |A| = 20.25 and m = 0.2407.
        const double m_dia = 2.0 * 5.0 * std::pow(std::numbers::pi / 20.25, 2);
        check(std::fabs(m_dia - 0.2407) < 0.0001, "DIA m = 0.2407 from |A| = 20.25");
    }

    // ---- where it does not hold together -----------------------------------
    {
        check(!action_bound(1.76, 0.5, 5.0).has_value()
                  && !action_bound(1.76, 0.48, 5.0).has_value(),
              "p-bar <= 0.5 gives no bound (eq. 6 divides by 2p - 1), and is refused");
        const auto a = action_bound(1.76, 0.54, 5.0), b = action_bound(1.76, 0.53, 5.0);
        check(a && b && b->amplitude / a->amplitude > 1.3,
              "and near one half it is fragile: p-bar 0.53 instead of 0.54 "
              "raises the 'limit' by a third");
        const auto spy = action_bound(1.76, 0.5398, 5.0);
        const double x = 0.48 * 22.11 / std::numbers::pi;   // phi = 0.48
        const double total = spy->within(x) + spy->beyond(x);
        std::printf("    at phi 0.48: within %.3f + beyond %.3f = %.3f\n",
                    spy->within(x), spy->beyond(x), total);
        check(total < 0.55,
              "eq. 17 as published is not a distribution: erf^2 + erfc^2 is "
              "about 0.50 at phi = 0.48, not 1");
    }

    // ---- against a Gaussian, on fat tails ----------------------------------
    //
    // Weekly moves drawn from a Student t with 4 degrees of freedom (index
    // returns have tails about that heavy), a small upward drift so p-bar is
    // above one half, 30 years of weeks fitted, 300 years scored. The tail
    // used for the oscillator is 1 - within() (see the header). Reported, and
    // asserted only for what both models must do.
    {
        std::mt19937_64 rng(2017);
        std::student_t_distribution<double> t4(4.0);
        const auto draw = [&] { return 0.25 + 1.6 * t4(rng); };
        std::vector<double> fit(1560), score(15600);
        for (double& v : fit) v = draw();
        for (double& v : score) v = draw();
        double sabs = 0, s2 = 0, s1 = 0;
        std::size_t up = 0;
        for (double v : fit) { sabs += std::fabs(v); s1 += v; s2 += v * v; up += v > 0; }
        const double n = static_cast<double>(fit.size());
        const auto ab = action_bound(sabs / n, static_cast<double>(up) / n, 5.0);
        const double mean = s1 / n, sd = std::sqrt(s2 / n - mean * mean);
        check(ab.has_value(), "a drifting fat-tailed market gives a bound");
        if (ab) {
            std::printf("    fitted |A| %.1f (sd %.2f, largest scored move %.1f)\n",
                        ab->amplitude, sd, [&] { double m = 0; for (double v : score) m = std::max(m, std::fabs(v)); return m; }());
            double err_osc = 0, err_gau = 0;
            for (double k : {1.0, 2.0, 3.0, 4.0}) {
                const double X = k * sd;
                double emp = 0;
                for (double v : score) emp += std::fabs(v) >= X;
                emp /= static_cast<double>(score.size());
                const double osc = 1.0 - ab->within(X);
                const double gau = std::erfc(X / (sd * std::sqrt(2.0)));
                std::printf("    P(|x| >= %.0f sd): data %.4f  oscillator %.4f  gaussian %.4f\n",
                            k, emp, osc, gau);
                err_osc += std::fabs(osc - emp);
                err_gau += std::fabs(gau - emp);
            }
            std::printf("    total tail error: oscillator %.4f  gaussian %.4f\n", err_osc, err_gau);
            check(std::isfinite(err_osc) && std::isfinite(err_gau),
                  "both tail models score (which is better is reported, not assumed)");
        }
    }

    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
