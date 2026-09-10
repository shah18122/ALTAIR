// P41 -- richer features, and whether the paired t actually moves.
//
// Smit asked two things: add features beyond eight lagged returns, and say
// whether the forecast gets measurably better. The second is the whole point,
// and the answer is a number that this file prints.
//
// THE FIRST CHECK IS THE ONE THAT MATTERS, AND IT CAUGHT A REAL BUG.
//
// A row's label is the sum of r[i .. i+horizon-1], so the features may read
// r[i-1] and not one element further. The first draft of append_row() used
// r[i] as its most recent feature -- the first element of the answer, handed
// to the model as an input. Nothing would have looked wrong: the walk-forward
// would still have been honest about fold boundaries, the gap would still have
// been there, and the model would simply have scored magnificently.
//
// So the row is built twice: once normally, once from a series whose r[i] has
// been detonated. If a single byte of the row changes, the features are
// reading the answer.
//
// AND THE EXPERIMENT IS A WEAK GATE, DELIBERATELY.
//
// It asserts that every configuration RUNS and produces finite numbers, and it
// PRINTS the comparison. Asserting "rich features must beat plain ones on
// NIFTY" would assert a market fact, and the honest response to that stopping
// being true is to read the number rather than edit the threshold.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <models/forecast_scorecard.hpp>
#include <models/spot_forecast.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

std::vector<double> load_closes(const std::string& dir)
{
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".csv") { files.push_back(e.path().string()); }
    }
    std::sort(files.begin(), files.end());
    std::vector<double> out;
    for (const std::string& f : files) {
        std::ifstream in(f);
        if (!in) { continue; }
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            std::istringstream ss(line);
            std::string cell;
            int col = 0;
            double close = 0.0;
            while (std::getline(ss, cell, ',')) {
                if (col == 4 && !cell.empty()) { close = std::atof(cell.c_str()); }
                ++col;
            }
            if (close > 0.0) { out.push_back(close); }
        }
    }
    return out;
}

}  // namespace

int main()
{
    std::printf("P41 richer features\n\n");

    // ---- hard rule 7, planted --------------------------------------------
    {
        std::mt19937 rng(4242u);
        std::normal_distribution<double> z(0.0, 90.0);
        std::vector<double> r(400);
        for (double& v : r) { v = z(rng); }

        altair::SpotSpec rich;
        rich.rich = true;
        constexpr std::size_t kAt = 300;

        std::vector<double> a, b;
        altair::append_row(r, kAt, rich, a);

        // PLANT: destroy the first element of the label. The row must not
        // notice.
        std::vector<double> blown = r;
        blown[kAt] = 1e6;
        altair::append_row(blown, kAt, rich, b);

        check(a.size() == altair::feature_width(rich),
              "a rich row has lags + 8 columns");
        check(a == b,
              "detonating r[i] -- the first element of the LABEL -- changes "
              "not one feature, so the row cannot be reading its own answer");

        // And the check is not vacuous: r[i-1] IS read.
        std::vector<double> c;
        std::vector<double> prev = r;
        prev[kAt - 1] = 1e6;
        altair::append_row(prev, kAt, rich, c);
        check(a != c,
              "while detonating r[i-1] does change it, so the row is really "
              "looking at the past rather than at nothing");

        // The plain path must be unchanged from what it always was.
        altair::SpotSpec plain;
        std::vector<double> p;
        altair::append_row(r, kAt, plain, p);
        bool same_as_before = p.size() == plain.lags;
        for (std::size_t k = 0; k < plain.lags && same_as_before; ++k) {
            same_as_before = p[k] == r[kAt - plain.lags + k];
        }
        check(same_as_before,
              "and with rich off the row is byte-for-byte the eight lagged "
              "returns it has always been");
    }

    // ---- the experiment ---------------------------------------------------
    const std::string dir = std::string(ALTAIR_DATASET_DIR) + "/spot/nifty/1d";
    const std::vector<double> closes = load_closes(dir);
    std::printf("\n  %zu NIFTY daily closes\n", closes.size());
    if (closes.size() < 2500) {
        std::printf("\n  NIFTY DAILY HISTORY UNAVAILABLE -- nothing measured.\n");
        return 1;
    }

    std::vector<std::int64_t> stamps;          // not needed for scoring
    altair::BacktestSpec bt;
    bt.warmup = 1500;
    bt.refit_every = 100;
    bt.max_points = 0;

    struct Run { const char* name; bool rich; bool vol; };
    const Run runs[] = {
        {"8 lagged returns (as shipped)", false, false},
        {"+ vol-scaled label",            false, true},
        {"rich features",                 true,  false},
        {"rich + vol-scaled label",       true,  true},
    };

    std::printf("\n  %-32s %8s %8s %9s %8s  %s\n", "configuration", "RMSE",
                "naive", "paired t", "dir-sig", "verdict");

    bool all_ran = true, all_finite = true;
    for (const Run& r : runs) {
        altair::SpotSpec spec;
        spec.horizon = 1;
        spec.lags = 8;
        spec.folds = 5;
        spec.cost_bps = 0.0;
        spec.rich = r.rich;
        spec.vol_scaled_label = r.vol;

        const auto probe = altair::backtest_forecasts(closes, stamps, spec, bt,
                                                      1.0, false);
        if (!probe) { all_ran = false; std::printf("    %-32s did not run\n", r.name); continue; }
        const auto ps = altair::score_forecasts(*probe);
        if (!ps) { all_ran = false; continue; }

        const auto pts = altair::backtest_forecasts(closes, stamps, spec, bt,
                                                    ps->rmse_model_bps, false);
        if (!pts) { all_ran = false; continue; }
        const auto sc = altair::score_forecasts(*pts);
        if (!sc) { all_ran = false; continue; }
        const auto v = altair::judge_forecast(*sc);

        if (!std::isfinite(sc->t_stat) || !std::isfinite(sc->rmse_model_bps)) {
            all_finite = false;
        }
        std::printf("    %-32s %8.2f %8.2f %9.2f %8.2f  %s\n", r.name,
                    sc->rmse_model_bps, sc->rmse_naive_bps, sc->t_stat,
                    sc->direction_vs_drift, altair::verdict_text(v));
    }

    check(all_ran, "every configuration runs on the real series");
    check(all_finite, "and every statistic is finite");

    std::printf("\n  paired t is the model against a RANDOM WALK. Positive "
                "means closer;\n  |t| > 2 over 200+ bars is the threshold the "
                "verdict uses. dir-sig is\n  the directional edge over the "
                "ALWAYS-UP null, in standard errors.\n");

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : "THERE WERE FAILURES");
    return failures == 0 ? 0 : 1;
}
