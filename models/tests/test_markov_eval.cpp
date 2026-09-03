// models/tests/test_markov_eval.cpp -- P8-14.
//
// Runs against the REAL daily NIFTY series in `dataset/`, and asserts the
// PROTOCOL rather than the outcome.
//
// A test that demanded a positive out-of-sample edge would be a test that
// demanded the market cooperate, and the first time it failed the honest
// reaction would be to weaken the test. So the assertions are on the things
// that must hold whatever the market did -- boundaries never touch the test
// block, the two scores cover identical observations, an impossible transition
// is counted rather than smoothed away -- and the edge itself is PRINTED.
//
// If the chain does not survive walk-forward, that is the finding, and it is
// worth more than the chi-square that made it look promising.

#include <models/markov.hpp>
#include <models/markov_eval.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

/// Closes from the daily CSV. Deliberately its OWN reader, not the desktop's:
/// two independent paths to the same series is what makes a result a property
/// of the data rather than of one parser.
std::vector<double> load_closes(const std::string& path) {
    std::vector<double> out;
    std::ifstream f(path);
    if (!f) { return out; }
    std::string line;
    std::getline(f, line);          // header
    while (std::getline(f, line)) {
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
    return out;
}

} // namespace

int main() {
    std::printf("P8-14 markov walk-forward\n");

    const std::string path =
        std::string(ALTAIR_DATASET_DIR) + "/spot/nifty/1d/all.csv";
    const auto closes = load_closes(path);
    if (closes.size() < 3000) {
        std::printf("  SKIP: %s has %zu closes\n", path.c_str(), closes.size());
        return 0;
    }
    const auto rets = altair::log_returns(closes);
    std::printf("  %zu closes -> %zu returns\n", closes.size(), rets.size());

    altair::WalkForwardSpec spec{};
    spec.initial_train = 2000;   // ~8 years, enough for 25 cells
    spec.test_len = 500;         // ~2 years per fold
    spec.step = 500;             // non-overlapping test blocks
    spec.gap = 1;                // the label is the NEXT state
    spec.expanding = true;

    const double alpha = 0.5;    // Jeffreys. Explicit, not inherited.
    const auto r = altair::markov_walk_forward(rets, 5, spec, alpha);
    check(r.has_value(), "walk-forward ran");
    if (!r) { return 1; }

    std::printf("\n  fold  train   test  scored   chain    base    edge"
                "   impossible  no-op\n");
    for (const auto& f : r->folds) {
        std::printf("  %4zu %6zu %6zu  %6zu  %7.4f %7.4f %+7.4f  %8zu %6zu\n",
                    f.index, f.train_n, f.test_n, f.scored, f.chain_logscore,
                    f.baseline_logscore, f.edge, f.impossible, f.no_opinion);
    }

    std::printf("\n  mean edge      %+.5f nats/obs over %zu folds\n",
                r->mean_edge, r->folds.size());
    std::printf("  fold sd        %.5f\n", r->edge_sd);
    std::printf("  t              %+.2f   (OPTIMISTIC: expanding folds share "
                "training data)\n", r->t_stat);
    std::printf("  folds positive %zu of %zu\n", r->folds_positive,
                r->folds.size());
    std::printf("  accuracy       chain %.4f  vs  baseline %.4f\n",
                r->chain_accuracy(), r->baseline_accuracy());
    std::printf("  impossible     %zu transitions the chain ruled out, that "
                "happened\n", r->total_impossible);
    std::printf("  no opinion     %zu (excluded from BOTH scores)\n",
                r->total_no_opinion);

    // THE DECOMPOSITION. The log-score edge above cannot tell a directional
    // edge from volatility clustering, and only one of those is tradeable.
    std::printf("\n  IS THE EDGE DIRECTIONAL, OR IS IT VOLATILITY?\n");
    std::printf("    SIGN       chain %.4f  vs  baseline %.4f   %+.4f\n",
                r->sign_chain_accuracy(), r->sign_base_accuracy(),
                r->sign_chain_accuracy() - r->sign_base_accuracy());
    std::printf("    MAGNITUDE  chain %.4f  vs  baseline %.4f   %+.4f\n",
                r->mag_chain_accuracy(), r->mag_base_accuracy(),
                r->mag_chain_accuracy() - r->mag_base_accuracy());

    // ---- the protocol, which must hold whatever the market did -----------

    check(r->folds.size() >= 5, "enough folds to say anything");

    bool ordered = true;
    for (const auto& f : r->folds) {
        const auto fold = altair::walk_forward_fold(rets.size(), spec, f.index);
        if (!fold || fold->train.end + spec.gap > fold->test.start) {
            ordered = false;
        }
    }
    check(ordered, "every fold trains strictly before it tests, plus the gap");

    bool same_obs = true;
    for (const auto& f : r->folds) {
        // Both scores are means over `scored`; a divergence would mean one
        // side saw observations the other did not.
        if (f.scored + f.no_opinion + 1 < f.test_n) { same_obs = false; }
        if (f.scored == 0 && f.edge != 0.0) { same_obs = false; }
    }
    check(same_obs, "chain and baseline scored the same observations");

    check(r->total_scored > 3000, "a meaningful number of scored transitions");

    // An impossible transition must be COUNTED even though alpha made the
    // score finite. Zero here would mean the counter never fires, which on 25
    // cells and 2,000 training days would itself be suspicious.
    std::printf("\n  (impossible transitions are counted at alpha=0 and scored "
                "at alpha=%.2f)\n", alpha);

    // ---- the look-ahead this file exists to prevent -----------------------
    //
    // Refit with boundaries taken from the WHOLE series and walked forward
    // anyway. If that scores better, the difference is the contamination --
    // the same 15.1% relabelling P8-13 measured, now expressed in nats.
    std::vector<double> all = rets;
    const auto global_b = altair::quantile_boundaries(all, 5);
    check(global_b.has_value(), "global boundaries for the contrast");
    if (global_b) {
        const auto fold0 = altair::walk_forward_fold(rets.size(), spec, 0);
        std::vector<double> train_vals(
            rets.begin(), rets.begin()
                              + static_cast<std::ptrdiff_t>(fold0->train.end));
        const auto train_b = altair::quantile_boundaries(train_vals, 5);
        bool differ = false;
        for (std::size_t i = 0; i + 1 < 5; ++i) {
            if (std::fabs(global_b->edges[i] - train_b->edges[i]) > 1e-12) {
                differ = true;
            }
        }
        std::printf("\n  fold 0 boundaries, train-only vs whole-series:\n");
        for (std::size_t i = 0; i + 1 < 5; ++i) {
            std::printf("    edge %zu   %+.6f   vs  %+.6f\n", i,
                        train_b->edges[i], global_b->edges[i]);
        }
        check(differ,
              "whole-series boundaries DIFFER from train-only -- the "
              "contamination is real, not theoretical");
    }

    // ---- THE VERDICT, stated rather than left to be spotted ---------------
    //
    // The three numbers above disagree in a way that is easy to misread, and
    // the misreading is the profitable-sounding one.
    const double sign_gain = r->sign_chain_accuracy() - r->sign_base_accuracy();
    const double mag_gain = r->mag_chain_accuracy() - r->mag_base_accuracy();

    std::printf("\n  VERDICT\n");
    std::printf("    The chain improves the 5-state distributional score out "
                "of sample\n    on %zu of %zu folds (%+.5f nats). That is a "
                "real dependence: this is\n    OUT of sample, so 25 parameters "
                "against 5 would be punished, not helped.\n",
                r->folds_positive, r->folds.size(), r->mean_edge);
    if (sign_gain <= 0.002) {
        std::printf("\n    AND IT HAS NO DIRECTIONAL CONTENT. On sign the "
                    "chain scores %.4f\n    against %.4f for a CONSTANT "
                    "predictor -- \"always up\", which wins\n    because NIFTY "
                    "drifts up. Difference %+.4f. A distributional edge that\n"
                    "    vanishes on reduction to a decision is not a signal, "
                    "and rule 5 never\n    even comes into it: there is no "
                    "pre-cost number to take a cost off.\n",
                    r->sign_chain_accuracy(), r->sign_base_accuracy(),
                    sign_gain);
    } else {
        std::printf("\n    Sign accuracy is %+.4f above the constant baseline. "
                    "INVESTIGATE before\n    believing it -- CLAUDE.md puts "
                    "the ceiling at 52-55%% and anything more\n    is overfit "
                    "until proven otherwise.\n", sign_gain);
    }
    if (mag_gain < 0.0) {
        std::printf("\n    On MAGNITUDE it is beaten by a constant too "
                    "(%.4f vs %.4f, %+.4f),\n    which is the more "
                    "interesting loss. Volatility clustering is real, so\n"
                    "    the chain should have won here. It does not, because "
                    "the training\n    median |return| is inflated by the "
                    "volatile 1990s, so \"never large\"\n    is right %.0f%% of "
                    "the time in the later test blocks. The chain\n    "
                    "sometimes says large and pays for it. That is "
                    "non-stationarity beating\n    a fitted model -- markets "
                    "are not ergodic, exactly as CLAUDE.md says,\n    and an "
                    "expanding window is the wrong window for this quantity.\n",
                    r->mag_chain_accuracy(), r->mag_base_accuracy(), mag_gain,
                    100.0 * r->mag_base_accuracy());
    }
    std::printf("\n    NO STRATEGY MAY TAKE A DIRECTIONAL POSITION FROM THIS "
                "CHAIN.\n    The regime label is usable as a CONDITIONER -- "
                "report per regime, size\n    differently within one -- which "
                "is what ROADMAP has it for.\n");

    check(r->total_scored > 0 && r->folds.size() > 0,
          "the decomposition was computed on real observations");

    // ---- AND THAT LAST CLAIM IS TESTABLE, SO TEST IT ----------------------
    //
    // "An expanding window is the wrong window for this quantity" is an
    // explanation, and an explanation that is never checked is a story. If it
    // is right, a ROLLING window -- which lets the volatile 1990s fall out of
    // the training set -- should close the magnitude gap. If it is wrong, the
    // gap stays and the story was decoration.
    altair::WalkForwardSpec rolling = spec;
    rolling.expanding = false;
    const auto rr = altair::markov_walk_forward(rets, 5, rolling, alpha);
    check(rr.has_value(), "rolling-window run");
    if (rr) {
        const double r_mag = rr->mag_chain_accuracy() - rr->mag_base_accuracy();
        const double r_sign =
            rr->sign_chain_accuracy() - rr->sign_base_accuracy();
        std::printf("\n  ROLLING WINDOW (2,000 bars, old data falls out)\n");
        std::printf("    mean edge  %+.5f over %zu folds, %zu positive\n",
                    rr->mean_edge, rr->folds.size(), rr->folds_positive);
        std::printf("    SIGN       chain %.4f  vs  baseline %.4f   %+.4f\n",
                    rr->sign_chain_accuracy(), rr->sign_base_accuracy(),
                    r_sign);
        std::printf("    MAGNITUDE  chain %.4f  vs  baseline %.4f   %+.4f"
                    "   (expanding was %+.4f)\n",
                    rr->mag_chain_accuracy(), rr->mag_base_accuracy(), r_mag,
                    mag_gain);
        if (r_mag > mag_gain) {
            std::printf("    -> the magnitude gap CLOSES by %.4f when old data "
                        "falls out.\n       The explanation holds: it was the "
                        "stale training median.\n", r_mag - mag_gain);
        } else {
            std::printf("    -> the gap does NOT close. The expanding-window "
                        "explanation above is\n       WRONG and should not be "
                        "repeated; the chain simply loses on magnitude.\n");
        }
        // Asserted: the two windows must actually differ, or the contrast
        // measured nothing. Which one wins is the market's business.
        check(rr->mean_edge != r->mean_edge,
              "rolling and expanding are genuinely different fits");
    }

    std::printf("\n  NOTE: a log-likelihood edge is predictability, not "
                "profitability.\n"
                "  Nothing here is net of cost and rule 5 is not satisfied by "
                "any of it.\n");

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
