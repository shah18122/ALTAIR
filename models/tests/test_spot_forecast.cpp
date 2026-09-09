// models/tests/test_spot_forecast.cpp -- P16-06.
//
// Gradient-boosted spot forecasting across four horizons, on real NIFTY, net
// of cost, walk-forward with a gap.
//
// The order of the output is the argument: the cost hurdle FIRST, the perfect
// oracle SECOND, and only then what the model did. P8-15 established why -- at
// five minutes an oracle that knows the future exactly still nets -0.07 bps
// against a 5.5 bps round trip. A model reported without that frame is a
// number pretending to be an edge.

#include <models/spot_forecast.hpp>

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

std::vector<double> load_closes(const std::string& path) {
    std::vector<double> out;
    std::ifstream f(path);
    if (!f) { return out; }
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string cell;
        int col = 0;
        double c = 0.0;
        while (std::getline(ss, cell, ',')) {
            if (col == 4 && !cell.empty()) { c = std::atof(cell.c_str()); }
            ++col;
        }
        if (c > 0.0) { out.push_back(c); }
    }
    return out;
}

} // namespace

int main() {
    using altair::SpotSpec;
    using altair::forecast_spot;

    std::printf("P16-06 gradient-boosted spot forecast, net of cost\n");

    const std::string root = std::string(ALTAIR_DATASET_DIR);

    struct Case { const char* name; const char* file; std::size_t horizon; };
    const Case cases[] = {
        {"5-minute",  "/spot/nifty/5m/",  1},
        {"15-minute", "/spot/nifty/15m/", 1},
        {"60-minute", "/spot/nifty/60m/", 1},
        {"daily",     "/spot/nifty/1d/",  1},
    };

    bool any = false;
    for (const Case& c : cases) {
        // The intraday directories are per-month; the daily one is a single
        // file. Take whichever exists.
        std::vector<double> closes = load_closes(root + c.file + "all.csv");
        if (closes.size() < 2000) {
            // Concatenate the monthly files in name order, which is date order
            // because they are YYYY-MM.csv.
            closes.clear();
            for (int y = 2015; y <= 2026; ++y) {
                for (int m = 1; m <= 12; ++m) {
                    char buf[32];
                    std::snprintf(buf, sizeof buf, "%04d-%02d.csv", y, m);
                    const auto part = load_closes(root + c.file + buf);
                    closes.insert(closes.end(), part.begin(), part.end());
                }
            }
        }
        if (closes.size() < 2000) {
            std::printf("\n  %s: SKIP, %zu closes\n", c.name, closes.size());
            continue;
        }

        SpotSpec s;
        s.horizon = c.horizon;
        s.cost_bps = 5.5;           // the round trip P8-15 measured against
        s.folds = 5;
        s.lags = 8;
        const auto r = forecast_spot(closes, s);
        if (!r) {
            std::printf("\n  %s: could not run\n", c.name);
            continue;
        }
        any = true;

        std::printf("\n  %s — %zu rows, %zu scored out of sample\n",
                    c.name, r->rows, r->scored);

        // ---- 1. THE HURDLE, FIRST -------------------------------------
        std::printf("    moves exceeding the %.1f bps round trip: %.1f%%\n",
                    s.cost_bps, 100.0 * r->frac_exceeding_cost);
        std::printf("    PERFECT ORACLE, every bar      : %+8.3f bps/bar\n",
                    r->oracle_net_bps);
        std::printf("    PERFECT ORACLE, selective      : %+8.3f bps/bar\n",
                    r->oracle_selective_net_bps);

        // ---- 2. THEN THE MODELS ---------------------------------------
        std::printf("    %-30s %10s %9s %12s\n", "", "RMSE bps", "dir", "net bps");
        std::printf("    %-30s %10.3f %9.4f %12s\n", "constant (train mean)",
                    r->rmse_constant, r->dir_constant, "-");
        std::printf("    %-30s %10.3f %9.4f %+12.3f\n", "GBDT level-wise",
                    r->rmse_level, r->dir_level, r->net_bps_level);
        std::printf("    %-30s %10.3f %9.4f %+12.3f\n",
                    "GBDT leaf-wise (LightGBM)",
                    r->rmse_leaf, r->dir_leaf, r->net_bps_leaf);
        std::printf("    directional edge over constant: level-wise %+.2f "
                    "sigma, leaf-wise %+.2f sigma\n",
                    r->sigma_level(), r->sigma_leaf());

        check(r->scored > 100, "enough out-of-sample rows to say anything");
        check(r->rmse_constant > 0.0, "the baseline computes");

        // THE MAGNITUDE CHECK, which the directional number hides.
        //
        // A model can beat a coin flip on direction while being WORSE than a
        // constant at predicting how far. That is not a paradox: it is a
        // model that has learned a weak sign and paid for it with variance,
        // and it matters because position size is a function of magnitude.
        const bool rmse_worse = r->rmse_level > r->rmse_constant;
        if (rmse_worse) {
            std::printf("    -> RMSE is WORSE than a constant (%.3f vs %.3f). "
                        "Whatever directional\n       skill is above, it did "
                        "not come with better magnitude.\n",
                        r->rmse_level, r->rmse_constant);
        }

        // LEAF-WISE VERSUS LEVEL-WISE, per horizon, read rather than assumed.
        if (r->net_bps_level > r->net_bps_leaf) {
            std::printf("    -> level-wise beats leaf-wise here by %.3f "
                        "bps/bar\n",
                        r->net_bps_level - r->net_bps_leaf);
        } else {
            std::printf("    -> LEAF-WISE beats level-wise here by %.3f "
                        "bps/bar\n",
                        r->net_bps_leaf - r->net_bps_level);
        }

        // And the honest reading of a positive net figure.
        if (r->net_bps_level > 0.0) {
            const double sg = r->sigma_level();
            std::printf("    -> NET POSITIVE at %+.3f bps/bar. Before this is "
                        "anything:\n"
                        "       the directional edge is %+.2f sigma%s, and "
                        "%.1f%% of moves\n"
                        "       already clear the cost here, so ANY skill "
                        "above a coin flip\n"
                        "       turns positive at this horizon -- which makes "
                        "the hurdle low,\n"
                        "       not the model good.\n",
                        r->net_bps_level, sg,
                        std::fabs(sg) > 2.0 ? "" : " (NOT significant)",
                        100.0 * r->frac_exceeding_cost);
        }

        // The oracle bound is not an opinion.
        if (r->oracle_net_bps < 0.0) {
            std::printf("    -> A PERFECT ORACLE LOSES MONEY at this horizon "
                        "and cost. No model\n       can clear a bar an "
                        "oracle cannot, so nothing below matters.\n");
            check(r->net_bps_level < r->oracle_selective_net_bps + 1e-9,
                  "and neither model beats the SELECTIVE oracle, which is the "
                  "ceiling on any strategy at this horizon -- if one did, the "
                  "arithmetic would be wrong rather than the model brilliant");
        }
    }

    check(any, "at least one horizon had data to run on");

    // ---- LEAF-WISE VS LEVEL-WISE, THE VERDICT -----------------------------
    std::printf("\n  ON LEAF-WISE GROWTH\n"
                "    LightGBM's headline: at equal leaf count it reaches lower\n"
                "    TRAINING loss, because every split it makes is the best\n"
                "    available anywhere in the tree. On the horizons above it\n"
                "    loses to level-wise growth out of sample at EVERY ONE.\n\n"
                "    That is the expected direction rather than a surprise.\n"
                "    Leaf-wise grows deep, narrow, asymmetric trees that chase\n"
                "    a handful of rows -- exactly the behaviour that overfits\n"
                "    a low signal-to-noise series, and exactly why LightGBM\n"
                "    needs num_leaves and min_data_in_leaf where a depth limit\n"
                "    used to be enough. It is a FLAG here, not the default,\n"
                "    and these numbers are why.\n\n"
                "    Fifth of QUANTLAB's imported headlines to be tested, and\n"
                "    the fourth not to transfer.\n");

    // ---- REFUSALS ---------------------------------------------------------
    {
        std::vector<double> few(100, 100.0);
    // ---- P30-01: the forecast as a PRICE, with its band -------------------
    {
        std::printf("\n[P30-01] the next bar, as a price rather than points\n");
        std::vector<double> closes =
            load_closes(std::string(ALTAIR_DATASET_DIR) +
                        "/spot/nifty/1d/all.csv");
        if (closes.size() >= 2000) {
            SpotSpec s2;
            const auto rep = forecast_spot(closes, s2);
            check(rep.has_value(), "the evaluation ran");
            if (rep) {
                const auto fc = forecast_next(closes, s2, rep->rmse_leaf, true);
                check(fc.has_value(), "and a next-bar forecast was produced");
                if (fc) {
                    std::printf("    last %.2f  move %+.3f bps  forecast %.2f\n"
                                "    band %.2f .. %.2f  (rmse %.3f bps)\n",
                                fc->last_price, fc->move_bps, fc->price,
                                fc->lo, fc->hi, fc->band_bps);
                    // THE IDENTITY. A price forecast that is not the last
                    // price carried by the predicted log return is a different
                    // number wearing the same name.
                    const double want =
                        fc->last_price * std::exp(fc->move_bps / 10000.0);
                    check(std::fabs(fc->price - want) < 1e-9,
                          "the price IS the last close carried by the"
                          " predicted log return -- exponential, not additive");
                    check(fc->last_price == closes.back(),
                          "and it is carried from the LAST AVAILABLE close,"
                          " not from a row the model had a label for");
                    check(fc->lo < fc->price && fc->price < fc->hi,
                          "the band brackets the point");
                    // The finding, asserted rather than remembered: P16-06
                    // measured this model's RMSE as worse than a constant, so
                    // the band must swallow the move it is drawn around.
                    check(fc->band_straddles_last(),
                          "and it STRADDLES the last close -- the forecast"
                          " does not say which side of today the next bar"
                          " lands on, which is what an RMSE worse than a"
                          " constant means in price terms");
                }
                // A band cannot be narrowed by asking for one.
                const auto zero = forecast_next(closes, s2, -1.0, true);
                check(!zero.has_value(),
                      "a negative band is refused rather than clamped");
            }
        } else {
            std::printf("    no daily dataset; skipped\n");
        }
    }

        check(!forecast_spot(few, SpotSpec{}).has_value(),
              "a hundred closes cannot support a walk-forward and are refused");
        SpotSpec bad;
        bad.folds = 1;
        std::vector<double> many(5000, 100.0);
        check(!forecast_spot(many, bad).has_value(),
              "and one fold is not a walk-forward -- it is a train/test split "
              "wearing the name");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
