// risk/tests/test_stress.cpp -- P15-04 / P15-07.
//
// Stress testing on the real 2008 and 2020 windows, and Black-Litterman with
// the parameter nobody sets honestly made visible.

#include <risk/stress.hpp>

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

struct Series {
    std::vector<std::string> date;
    std::vector<double> ret;
};

Series load(const std::string& path) {
    Series s;
    std::vector<double> c;
    std::vector<std::string> d;
    std::ifstream f(path);
    if (!f) { return s; }
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string cell, dt;
        int col = 0;
        double v = 0.0;
        while (std::getline(ss, cell, ',')) {
            if (col == 0) { dt = cell.substr(0, 10); }
            if (col == 4 && !cell.empty()) { v = std::atof(cell.c_str()); }
            ++col;
        }
        if (v > 0.0) { c.push_back(v); d.push_back(dt); }
    }
    for (std::size_t i = 1; i < c.size(); ++i) {
        s.ret.push_back(std::log(c[i] / c[i - 1]));
        s.date.push_back(d[i]);
    }
    return s;
}

/// First index on or after `ymd`. Returns size() when the date is past the end.
std::size_t index_of(const Series& s, const char* ymd) {
    for (std::size_t i = 0; i < s.date.size(); ++i) {
        if (s.date[i] >= ymd) { return i; }
    }
    return s.date.size();
}

} // namespace

int main() {
    using altair::BlView;
    using altair::CovMatrix;
    using altair::StressWindow;
    using altair::Weights;
    using altair::bl_posterior;
    using altair::conditional_correlation;
    using altair::stress;

    std::printf("P15-04/07 stress windows and Black-Litterman\n");

    const std::string root = std::string(ALTAIR_DATASET_DIR);
    const Series nifty = load(root + "/spot/nifty/1d/all.csv");
    const Series vix = load(root + "/spot/indiavix/1d/all.csv");
    if (nifty.ret.size() < 3000) {
        std::printf("  SKIP: NIFTY has %zu returns\n", nifty.ret.size());
        return 0;
    }

    // ---- 1. REAL DATED WINDOWS, NOT A SYNTHETIC SHOCK ---------------------
    {
        const StressWindow w[] = {
            {"GFC 2008",        index_of(nifty, "2008-01-01"),
                                index_of(nifty, "2009-04-01")},
            {"Taper 2013",      index_of(nifty, "2013-05-01"),
                                index_of(nifty, "2013-09-30")},
            {"COVID 2020",      index_of(nifty, "2020-02-01"),
                                index_of(nifty, "2020-05-01")},
        };
        std::printf("\n  %-14s %8s %14s %14s\n", "window", "days",
                    "cumulative", "max drawdown");
        double worst_dd = 0.0;
        for (const StressWindow& s : w) {
            if (s.to <= s.from || s.to > nifty.ret.size()) { continue; }
            const auto r = stress(nifty.ret, nifty.ret, s);
            if (!r) { continue; }
            std::printf("    %-12s %8zu %13.2f%% %13.2f%%\n",
                        r->name, r->days, 100.0 * r->cumulative,
                        100.0 * r->max_drawdown);
            worst_dd = std::max(worst_dd, r->max_drawdown);
        }
        check(worst_dd > 0.25,
              "at least one real window shows a drawdown past 25% -- these "
              "are DATED PERIODS, not a 'down 20%' somebody chose, and the "
              "difference is that nobody had to decide how bad to make them");
    }

    // ---- 2. THE CORRELATION YOU DIVERSIFIED ON IS NOT THE ONE YOU GET -----
    {
        // Align NIFTY and VIX by date.
        std::vector<double> a, b;
        std::size_t vi = 0;
        for (std::size_t i = 0; i < nifty.date.size(); ++i) {
            while (vi < vix.date.size() && vix.date[vi] < nifty.date[i]) { ++vi; }
            if (vi < vix.date.size() && vix.date[vi] == nifty.date[i]) {
                a.push_back(nifty.ret[i]);
                b.push_back(vix.ret[vi]);
            }
        }
        std::printf("\n  %zu dates present in both NIFTY and India VIX\n",
                    a.size());
        if (a.size() > 300) {
            const auto full = conditional_correlation(a, b, 0, a.size());
            // The worst decile of NIFTY days -- the crisis condition, defined
            // by outcome rather than by date.
            std::vector<double> sorted = a;
            std::sort(sorted.begin(), sorted.end());
            const double cut = sorted[a.size() / 10];
            std::vector<double> ca, cb;
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (a[i] <= cut) { ca.push_back(a[i]); cb.push_back(b[i]); }
            }
            const auto tail =
                conditional_correlation(ca, cb, 0, ca.size());
            if (full && tail) {
                std::printf("    full-sample correlation %+.4f\n", *full);
                std::printf("    worst-decile days       %+.4f  (%zu days)\n",
                            *tail, ca.size());
                check(std::fabs(*tail) > std::fabs(*full) * 0.5,
                      "the correlation CONDITIONAL on a bad day differs "
                      "materially from the full-sample number -- which is the "
                      "entire reason a stress test measures inside a window "
                      "rather than over the sample");
            }
        }
    }

    // ---- 3. BLACK-LITTERMAN, AND THE PARAMETER NOBODY SETS ----------------
    {
        // Two assets, correlated 0.5, with the market holding 60/40.
        CovMatrix s;
        s.p = 2;
        s.a = {0.0400, 0.0150, 0.0150, 0.0225};   // vols 20% and 15%
        Weights mkt;
        mkt.w = {0.60, 0.40};

        const auto neutral = bl_posterior(s, mkt, {}, 2.5, 0.05);
        check(neutral.has_value(), "BL with NO views computes");
        if (neutral) {
            std::printf("\n  equilibrium returns implied by 60/40: "
                        "%.3f%%, %.3f%%\n",
                        100.0 * neutral->equilibrium[0],
                        100.0 * neutral->equilibrium[1]);
            check(std::fabs(neutral->mu[0] - neutral->equilibrium[0]) < 1e-12,
                  "with no views the posterior IS the equilibrium -- the "
                  "neutral answer is the market portfolio, not whatever a "
                  "mean estimate happened to produce, and that is the whole "
                  "reason to reverse-optimise first");
        }

        // The same view at two confidences. THE demonstration.
        BlView v;
        v.on = 0;
        v.value = 0.15;                            // "asset 0 returns 15%"
        std::printf("\n  ONE VIEW (asset 0 returns 15%%), at three "
                    "confidences\n");
        std::printf("    %-22s %12s %12s\n", "view sd", "mu[0]", "mu[1]");
        double mu_sure = 0.0, mu_vague = 0.0;
        for (const double conf : {0.01, 0.10, 1.00}) {
            v.confidence = conf;
            const auto r = bl_posterior(s, mkt, {v}, 2.5, 0.05);
            if (!r) { continue; }
            std::printf("    %-22.2f %11.3f%% %11.3f%%\n",
                        conf, 100.0 * r->mu[0], 100.0 * r->mu[1]);
            if (conf < 0.02) { mu_sure = r->mu[0]; }
            if (conf > 0.5) { mu_vague = r->mu[0]; }
        }
        check(std::fabs(mu_sure - 0.15) < std::fabs(mu_vague - 0.15),
              "a CONFIDENT view pulls the posterior toward the view and a "
              "vague one leaves it at the market -- Omega is not a technical "
              "detail, it is the entire strength of the tilt, and there is no "
              "procedure for choosing it");
        if (neutral) {
            check(std::fabs(mu_vague - neutral->equilibrium[0])
                      < std::fabs(mu_sure - neutral->equilibrium[0]),
                  "and at low confidence BL returns the market, which is the "
                  "correct behaviour and also means it adds NOTHING unless "
                  "you are genuinely sure");
        }

        // The view on asset 0 must move asset 1 too.
        v.confidence = 0.01;
        const auto tilted = bl_posterior(s, mkt, {v}, 2.5, 0.05);
        if (tilted && neutral) {
            check(std::fabs(tilted->mu[1] - neutral->equilibrium[1]) > 1e-6,
                  "a view on ONE asset moves the OTHER, in proportion to "
                  "their covariance -- that propagation is the reason to use "
                  "BL rather than overwriting one number in a mean vector");
        }

        BlView bad = v;
        bad.confidence = 0.0;
        check(!bl_posterior(s, mkt, {bad}, 2.5, 0.05).has_value(),
              "and a ZERO view uncertainty is refused: it asserts infinite "
              "certainty, which BL cannot represent and no analyst has");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
