// analytics/tests/test_action_principle.cpp -- Manhire's oscillator bound.
//
// The paper's own numbers first, then the two places it does not hold
// together, then the bound against a Gaussian on fat-tailed simulated weeks.

#include <analytics/action_principle.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
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

} // namespace

int main(int argc, char** argv) {
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

    // ---- eq. 16 is not a density -------------------------------------------
    {
        // Integral of (m / 2 pi t) exp(-m x^2 / t) over all x is sqrt(m / 4 pi t).
        const auto spy = action_bound(1.76, 0.5398, 5.0);
        double area = 0.0;
        for (double x = -60.0; x <= 60.0; x += 0.001) { area += spy->density(x) * 0.001; }
        std::printf("    eq. 16 integrates to %.4f over all moves (SPY)\n", area);
        check(std::fabs(area - std::sqrt(spy->inertia / (4.0 * std::numbers::pi * 5.0))) < 1e-6
                  && area < 0.1,
              "eq. 16 integrates to sqrt(m / 4 pi t), about 0.06, not 1");
    }

    // ---- the paper's own tests, re-run on NIFTY ----------------------------
    //
    // Weekly = non-overlapping 5-session displacements in index points, as
    // the paper does for SPY. Three windows, each fitted and then checked on
    // the months after it, including the paper's own crash (October 2008).
    // Its claims: no week beyond |A|, and erfc(X sqrt(m/2t))^2 within 0.02 of
    // the historical share of weeks with |x| >= X.
    if (argc > 1) {
        std::ifstream in(argv[1]);
        std::vector<std::string> dates;
        std::vector<double> px;
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
        const auto weeks = [&](const char* a, const char* b) {
            std::vector<double> c;
            for (std::size_t i = 0; i < px.size(); ++i)
                if (dates[i] >= a && dates[i] <= b) c.push_back(px[i]);
            std::vector<double> w;
            for (std::size_t i = 5; i < c.size(); i += 5) w.push_back(c[i] - c[i - 5]);
            return std::pair{c, w};
        };
        struct Window { const char *fa, *fb, *ta, *tb; };
        int broken = 0, windows = 0;
        double worst_gap = 0.0;
        for (const Window& w : {Window{"2002-03-01", "2008-08-31", "2008-09-01", "2009-03-31"},
                                Window{"2013-01-01", "2019-12-31", "2020-01-01", "2020-06-30"},
                                Window{"2016-01-01", "2023-12-31", "2024-01-01", "2026-12-31"}}) {
            const auto [closes, fit_w] = weeks(w.fa, w.fb);
            const auto [_, test_w] = weeks(w.ta, w.tb);
            const auto ab = altair::action_bound_from_closes(closes, 5);
            if (!ab || test_w.empty()) continue;
            ++windows;
            double worst = 0.0, in_max = 0.0;
            for (double v : test_w) if (std::fabs(v) > std::fabs(worst)) worst = v;
            for (double v : fit_w) in_max = std::max(in_max, std::fabs(v));
            broken += ab->inside(worst) ? 0 : 1;
            double sabs = 0.0;
            for (double v : fit_w) sabs += std::fabs(v);
            const double xbar = sabs / static_cast<double>(fit_w.size());
            double gap = 0.0;
            for (double k : {1.0, 2.0, 3.0}) {
                double freq = 0.0;
                for (double v : fit_w) freq += std::fabs(v) >= k * xbar;
                freq /= static_cast<double>(fit_w.size());
                gap = std::max(gap, std::fabs(ab->beyond(k * xbar) - freq));
            }
            worst_gap = std::max(worst_gap, gap);
            std::printf("    NIFTY fit %s..%s: |A| %.0f pts (largest fitted week %.0f); "
                        "worst week to %s: %.0f; erfc^2 off by up to %.2f\n",
                        w.fa, w.fb, ab->amplitude, in_max, w.tb, worst, gap);
        }
        if (windows == 3) {
            check(broken == 3,
                  "on NIFTY the 'bound' is broken in every window, the October 2008 "
                  "crash week included");
            check(worst_gap > 0.1,
                  "and erfc^2 misses the share of big weeks by more than 0.1, not "
                  "the paper's 0.02");
        } else {
            std::printf("    (NIFTY file lacks the windows: skipped)\n");
        }
    } else {
        std::printf("    (no NIFTY file given: real-data check skipped)\n");
    }

    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
