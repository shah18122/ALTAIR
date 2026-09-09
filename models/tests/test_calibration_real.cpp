// P35-02b. Conformal calibration on the REAL NIFTY series.
//
// test_calibration.cpp proves the machinery is correct on data whose answer is
// known by construction. This one asks the only question that decides whether
// the method earns its place in the product: does it help on NIFTY?
//
// It is deliberately a WEAK gate and a LOUD report. The assertions are limited
// to things that must hold for any sane run -- the walk-forward produced
// forecasts, every number is finite, the buckets add up -- because a test that
// asserts "conformal must beat the RMSE band on NIFTY daily closes" would be
// asserting a market fact, and the day that stops being true the correct
// response is to read the number, not to see a red test and edit the
// threshold until it passes.
//
// The numbers it prints are the deliverable. They belong in the LEDGER row.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <models/calibration.hpp>
#include <models/forecast_scorecard.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

struct Bars {
    std::vector<double> close;
    std::vector<std::int64_t> ts_ns;
};

/// Every CSV in a directory, name order, concatenated. Same shape as
/// test_horizon_eval.cpp's loader, which is the established pattern here.
Bars load_dir(const std::string& dir)
{
    Bars b;
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".csv") { files.push_back(e.path().string()); }
    }
    std::sort(files.begin(), files.end());

    for (const std::string& f : files) {
        std::ifstream in(f);
        if (!in) { continue; }
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            std::istringstream ss(line);
            std::string cell, ts;
            int col = 0;
            double close = 0.0;
            while (std::getline(ss, cell, ',')) {
                if (col == 0) { ts = cell; }
                if (col == 4 && !cell.empty()) { close = std::atof(cell.c_str()); }
                ++col;
            }
            if (close > 0.0 && ts.size() >= 10) {
                // Day resolution is enough here -- nothing below reads the
                // stamp except to keep the points ordered.
                const std::int64_t day =
                    std::atoll(ts.substr(0, 4).c_str()) * 10000
                    + std::atoll(ts.substr(5, 2).c_str()) * 100
                    + std::atoll(ts.substr(8, 2).c_str());
                b.close.push_back(close);
                b.ts_ns.push_back(day);
            }
        }
    }
    return b;
}

void report(const char* what, const altair::CalibrationReport& r)
{
    std::printf("    %-22s  covered %.4f   reg-MAE %5.2f pp   "
                "reg-max %5.2f pp\n",
                what, r.coverage, r.reg_mae_pp, r.reg_maxdev_pp);
}

}  // namespace

int main()
{
    std::printf("P35-02b conformal calibration on the real NIFTY series\n\n");

    const std::string root = ALTAIR_DATASET_DIR;
    const std::string dir = root + "/spot/nifty/1d";

    const Bars bars = load_dir(dir);
    std::printf("  %zu daily closes from %s\n", bars.close.size(), dir.c_str());
    if (bars.close.size() < 2500) {
        // NOT a pass. dataset/ is gitignored and regenerable, so a machine
        // without it must say so plainly rather than printing "all checks
        // passed" over an empty run -- that is how an absent dataset becomes
        // a green build.
        std::printf("\n  NIFTY DAILY HISTORY UNAVAILABLE -- nothing was "
                    "measured.\n  Fetch it with altair_kite_update --go.\n");
        return 1;
    }

    // The same forecaster the Live Forecast page runs.
    altair::SpotSpec spec;
    spec.horizon = 1;
    spec.lags = 8;
    spec.folds = 5;
    spec.cost_bps = 0.0;

    altair::BacktestSpec bt;
    bt.warmup = 1500;
    bt.refit_every = 100;
    bt.max_points = 0;          // every bar we can score, not the last 1500

    // Two passes, as the page does: one to learn the realised RMSE, one to
    // publish a band the model has actually earned.
    const auto probe = altair::backtest_forecasts(bars.close, bars.ts_ns, spec,
                                                  bt, 1.0, false);
    check(probe.has_value(), "the walk-forward runs on the real series");
    if (!probe) { return 1; }

    const auto probe_score = altair::score_forecasts(*probe);
    check(probe_score.has_value(), "and it scores");
    if (!probe_score) { return 1; }

    const double band = probe_score->rmse_model_bps;
    const auto pts = altair::backtest_forecasts(bars.close, bars.ts_ns, spec,
                                                bt, band, false);
    check(pts.has_value(), "the second pass runs with the earned band");
    if (!pts) { return 1; }

    std::printf("\n  %zu out-of-sample forecasts, band = %.2f bps "
                "(one realised RMSE)\n", pts->size(), band);

    // The RMSE band claims one sigma under a normal error.
    const double alpha = 1.0 - 0.6827;

    const auto before = altair::score_calibration(*pts, alpha, 20);
    check(before.has_value(), "the published band scores");
    if (!before) { return 1; }

    std::printf("\n  TARGET COVERAGE %.4f\n\n", 1.0 - alpha);
    report("RMSE band (as published)", *before);

    struct Run { const char* name; altair::ConformalMode mode; double lambda; };
    const Run runs[] = {
        {"conformal SWC",          altair::ConformalMode::Sliding,        0.0},
        {"conformal TWC 250-bar",  altair::ConformalMode::TimeWeighted,   0.004},
        {"conformal TWC 50-bar",   altair::ConformalMode::TimeWeighted,   0.020},
        {"conformal TWC 25-bar",   altair::ConformalMode::TimeWeighted,   0.040},
        {"conformal RWC 50-bar",   altair::ConformalMode::RegimeWeighted, 0.020},
    };

    double best = before->reg_mae_pp;
    const char* best_name = "RMSE band (as published)";
    bool all_finite = true;

    for (const Run& r : runs) {
        altair::CalibrationSpec cal;
        cal.conformal.alpha = alpha;
        cal.conformal.mode = r.mode;
        cal.conformal.lambda = r.lambda;
        cal.conformal.bandwidth = 0.5;
        cal.conformal.n_min = 25.0;
        cal.warmup = 250;
        cal.vol_window = 20;

        const auto got = altair::calibrate_band(*pts, cal);
        if (!got) {
            std::printf("    %-22s  did not calibrate\n", r.name);
            continue;
        }
        report(r.name, got->second);
        if (!std::isfinite(got->second.coverage)
            || !std::isfinite(got->second.reg_mae_pp)) {
            all_finite = false;
        }
        if (got->second.reg_mae_pp < best) {
            best = got->second.reg_mae_pp;
            best_name = r.name;
        }
    }

    check(all_finite, "every calibrated report is finite");
    std::printf("\n  BEST PER-REGIME CALIBRATION: %s (reg-MAE %.2f pp)\n",
                best_name, best);

    // The per-quintile detail for the published band, which is the thing a
    // reader of the page needs to see.
    std::printf("\n  THE PUBLISHED BAND, BY VOLATILITY QUINTILE\n");
    for (std::size_t q = 0; q < altair::kVolBuckets; ++q) {
        if (before->by_vol[q].n == 0) { continue; }
        std::printf("    q%zu  n=%-5zu realised vol %7.1f bps   covered %.4f\n",
                    q, before->by_vol[q].n, before->by_vol[q].mean_vol_bps,
                    1.0 - before->by_vol[q].exceedance);
    }

    std::size_t bucketed = 0;
    for (const auto& b : before->by_vol) { bucketed += b.n; }
    check(bucketed + 20 == pts->size(),
          "every forecast past the volatility window is bucketed");

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : "THERE WERE FAILURES");
    return failures == 0 ? 0 : 1;
}
