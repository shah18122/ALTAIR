// models/markov_eval.hpp -- walk-forward evaluation of the Markov chain.
//
// P8-14.
//
// THE FIT PANE SAYS THE CHAIN CAN BE ESTIMATED. THIS ASKS WHETHER IT WORKS.
//
// P8-13 measured chi-square 298.07 against a shuffled control that does not
// reject, on 8,755 daily NIFTY returns. That is a real result and it is
// IN-SAMPLE: it says the state sequence carries serial dependence a shuffle
// destroys. It does not say a chain fitted on the past predicts the future,
// and those are different claims that a dashboard will happily conflate.
//
// CLAUDE.md: markets are not ergodic, walk-forward only, random K-fold is
// banned. So this fits on a training block, predicts the block after it, and
// reports every fold rather than only the average -- an aggregate that hides
// two good folds and eleven bad ones is the number that gets quoted.
//
// THE BOUNDARIES ARE THE LOOK-AHEAD AGAIN, ONE LEVEL UP.
//
// P8-13's trap was computing quantile cut points from the whole series. The
// same trap reappears per fold: cut points computed from all 8,755 returns and
// then "walked forward" produce a split that is walk-forward in the labels and
// contaminated in the binning. Every fold here derives its boundaries from its
// OWN training block and applies them unchanged to its test block, which is
// also what a deployed model would do -- you fit, then you run.
//
// A ZERO CELL IS THE MODEL SAYING "IMPOSSIBLE", AND IT WILL BE WRONG.
//
// Twenty-five cells fitted on two thousand days leave corner cells empty. On
// the test block those transitions happen anyway, and an unsmoothed chain
// assigns them probability zero -- log-likelihood negative infinity, one
// observation destroying the fold. Smoothing hides that, so `impossible` is
// counted separately at alpha=0 and REPORTED, and the smoothing that makes the
// score finite is an argument the caller has to pass rather than a default
// somebody inherits.
//
// AND THE BASELINE GETS THE SAME TREATMENT AS THE CHAIN.
//
// The chain is scored against the unconditional distribution of the next
// state -- "tomorrow is drawn from the usual mix, regardless of today". Both
// are estimated from the SAME training block (an unconditional taken from the
// test block would be look-ahead in the baseline, which flatters the baseline
// and so understates the chain), both are smoothed with the same alpha, and
// both are scored on exactly the same observations. A chain that cannot beat
// that baseline is not a model.
//
// WHAT THIS DOES NOT MEASURE: MONEY.
//
// A log-likelihood edge is predictability, not profitability. It says nothing
// about magnitude, nothing about whether the predictable part is larger than
// the spread, and nothing about cost. Rule 5 is not satisfied by anything in
// this file and no strategy may read these numbers as a signal.

#pragma once

#include "markov.hpp"

#include <backtest/validation.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <vector>

namespace altair {

/// What one walk-forward fold produced.
struct MarkovFoldScore {
    std::size_t index = 0;
    std::size_t train_n = 0;
    std::size_t test_n = 0;

    /// Mean log probability the CHAIN assigned to what actually happened,
    /// in nats per observation. Higher is better; all values are negative.
    double chain_logscore = 0.0;
    /// The same for the unconditional baseline, fitted on the same block.
    double baseline_logscore = 0.0;
    /// chain - baseline. THE NUMBER. Positive means knowing today's state
    /// helped. Nats per observation.
    double edge = 0.0;

    /// Test transitions the UNSMOOTHED chain gave probability exactly zero:
    /// events it called impossible that then occurred.
    std::size_t impossible = 0;
    /// Test observations whose current state had no training transitions at
    /// all. The chain has no opinion; these are excluded from BOTH scores, so
    /// the comparison stays on identical observations.
    std::size_t no_opinion = 0;
    /// Observations actually scored.
    std::size_t scored = 0;

    /// Argmax hits. The baseline's prediction is the same state every day,
    /// which is exactly why beating it is the bar.
    std::size_t chain_correct = 0;
    std::size_t baseline_correct = 0;

    // ---- THE DECOMPOSITION THAT DECIDES WHETHER ANY OF THIS IS TRADEABLE --
    //
    // A state here is a RETURN QUANTILE, so state 0 and state 4 are both large
    // moves that differ in sign. Volatility clusters -- a large move is
    // followed by a large move -- so a chain over these states will show
    // serial dependence from GARCH alone, with no directional content at all.
    // The aggregate log-score cannot tell those apart, and it is the number a
    // dashboard would print.
    //
    // So the same predictions are scored twice more, on two questions:
    //
    //   SIGN      is the next return positive?   -- directional, tradeable
    //   MAGNITUDE is |next return| above the training median? -- volatility
    //
    // Each against its own constant baseline (predict the training majority).
    // If the chain beats the baseline on magnitude and not on sign, then what
    // it has found is volatility clustering, which is real, well known, and
    // NOT a directional edge -- and saying so is the point of these fields.
    std::size_t sign_chain_correct = 0;
    std::size_t sign_base_correct = 0;
    std::size_t mag_chain_correct = 0;
    std::size_t mag_base_correct = 0;
};

/// The whole run.
struct MarkovWalkForward {
    std::vector<MarkovFoldScore> folds;

    /// Mean edge across folds, nats per observation.
    double mean_edge = 0.0;
    /// Sample standard deviation of the per-fold edge.
    double edge_sd = 0.0;
    /// mean / (sd / sqrt(k)). OPTIMISTIC -- see `folds_are_not_independent`.
    double t_stat = 0.0;
    std::size_t folds_positive = 0;

    std::size_t total_impossible = 0;
    std::size_t total_no_opinion = 0;
    std::size_t total_scored = 0;
    std::size_t chain_correct = 0;
    std::size_t baseline_correct = 0;

    /// See MarkovFoldScore for why these exist. The aggregate log-score
    /// cannot separate a directional edge from volatility clustering; these
    /// can, and the answer changes what a strategy is allowed to do with it.
    std::size_t sign_chain_correct = 0;
    std::size_t sign_base_correct = 0;
    std::size_t mag_chain_correct = 0;
    std::size_t mag_base_correct = 0;

    [[nodiscard]] double sign_chain_accuracy() const noexcept {
        return total_scored > 0 ? static_cast<double>(sign_chain_correct)
                                      / static_cast<double>(total_scored)
                                : 0.0;
    }
    [[nodiscard]] double sign_base_accuracy() const noexcept {
        return total_scored > 0 ? static_cast<double>(sign_base_correct)
                                      / static_cast<double>(total_scored)
                                : 0.0;
    }
    [[nodiscard]] double mag_chain_accuracy() const noexcept {
        return total_scored > 0 ? static_cast<double>(mag_chain_correct)
                                      / static_cast<double>(total_scored)
                                : 0.0;
    }
    [[nodiscard]] double mag_base_accuracy() const noexcept {
        return total_scored > 0 ? static_cast<double>(mag_base_correct)
                                      / static_cast<double>(total_scored)
                                : 0.0;
    }

    /// Always true. It is a named field rather than a comment because the
    /// t-statistic above is the number a reader will take away, and it is
    /// wrong in a specific direction: with an expanding window every training
    /// set contains all the earlier ones, so the fold edges share data and are
    /// positively correlated. The standard error over folds is therefore too
    /// small and |t| too large. P6-04 measured the same effect on correlated
    /// members and P8-12 applied the correction; the honest reading here is
    /// the SIGN and the fold count, not the p-value.
    bool folds_are_not_independent = true;

    [[nodiscard]] double chain_accuracy() const noexcept {
        return total_scored > 0
                   ? static_cast<double>(chain_correct)
                         / static_cast<double>(total_scored)
                   : 0.0;
    }
    [[nodiscard]] double baseline_accuracy() const noexcept {
        return total_scored > 0
                   ? static_cast<double>(baseline_correct)
                         / static_cast<double>(total_scored)
                   : 0.0;
    }
};

/// Walk-forward evaluation of a first-order chain over return states.
///
/// `alpha` is additive (Laplace) smoothing applied identically to the chain
/// and the baseline. It has NO DEFAULT: zero gives an honest -inf the moment
/// the chain calls something impossible, and any positive value is a prior
/// nobody derived. Passing it explicitly is the point.
///
/// `spec.gap` must be at least 1: the label is the NEXT state, so the last
/// training transition reaches one bar into the test block.
[[nodiscard]] inline std::expected<MarkovWalkForward, MarkovError>
markov_walk_forward(const std::vector<double>& returns, std::size_t states,
                    const WalkForwardSpec& spec, double alpha) {
    if (states < 2 || states > kMaxStates) {
        return std::unexpected(MarkovError::BadStateCount);
    }
    if (alpha < 0.0 || spec.gap < 1) {
        return std::unexpected(MarkovError::BadStateCount);
    }
    const auto k = walk_forward_count(returns.size(), spec);
    if (!k || *k == 0) {
        return std::unexpected(MarkovError::TooFewSamples);
    }

    MarkovWalkForward out;
    out.folds.reserve(*k);

    for (std::size_t fi = 0; fi < *k; ++fi) {
        const auto fold = walk_forward_fold(returns.size(), spec, fi);
        if (!fold) {
            return std::unexpected(MarkovError::TooFewSamples);
        }
        const Block& tr = fold->train;
        const Block& te = fold->test;

        // Boundaries from the TRAINING BLOCK ONLY, then held fixed. A cut
        // point recomputed from the test block would be the P8-13 bug wearing
        // a fold's clothes.
        std::vector<double> train_vals(returns.begin()
                                           + static_cast<std::ptrdiff_t>(tr.start),
                                       returns.begin()
                                           + static_cast<std::ptrdiff_t>(tr.end));
        const auto b = quantile_boundaries(train_vals, states);
        if (!b) {
            return std::unexpected(b.error());
        }

        // Counts from the training block. Transition i -> i+1, so the last
        // training return has no successor inside the block.
        std::uint32_t n[kMaxStates][kMaxStates] = {};
        std::uint32_t row_total[kMaxStates] = {};
        std::uint32_t col_total[kMaxStates] = {};
        std::uint32_t grand = 0;
        for (std::size_t i = tr.start; i + 1 < tr.end; ++i) {
            const std::size_t from = b->classify(returns[i]);
            const std::size_t to = b->classify(returns[i + 1]);
            ++n[from][to];
            ++row_total[from];
            ++col_total[to];
            ++grand;
        }

        // Conditional sign and magnitude tallies from the TRAINING block, so
        // the sign/magnitude predictors are fitted on exactly the data the
        // chain was. `mag_ref` is the training median |return|; taking it from
        // the test block would be look-ahead in the definition of "large".
        std::vector<double> abs_train;
        abs_train.reserve(tr.size());
        for (std::size_t i = tr.start; i < tr.end; ++i) {
            abs_train.push_back(std::fabs(returns[i]));
        }
        std::sort(abs_train.begin(), abs_train.end());
        const double mag_ref = abs_train.empty()
                                   ? 0.0
                                   : abs_train[abs_train.size() / 2];

        std::uint32_t up_given[kMaxStates] = {};
        std::uint32_t big_given[kMaxStates] = {};
        std::uint32_t up_total = 0;
        std::uint32_t big_total = 0;
        for (std::size_t i = tr.start; i + 1 < tr.end; ++i) {
            const std::size_t from = b->classify(returns[i]);
            if (returns[i + 1] > 0.0) { ++up_given[from]; ++up_total; }
            if (std::fabs(returns[i + 1]) > mag_ref) {
                ++big_given[from];
                ++big_total;
            }
        }
        // The constant baselines: predict the training majority, every day.
        const bool base_says_up =
            grand > 0 && up_total * 2 > grand;
        const bool base_says_big =
            grand > 0 && big_total * 2 > grand;

        MarkovFoldScore f{};
        f.index = fi;
        f.train_n = tr.size();
        f.test_n = te.size();

        // The baseline's single prediction, and it is the same every day.
        std::size_t base_argmax = 0;
        for (std::size_t j = 1; j < states; ++j) {
            if (col_total[j] > col_total[base_argmax]) {
                base_argmax = j;
            }
        }

        double chain_sum = 0.0;
        double base_sum = 0.0;
        const double aS = alpha * static_cast<double>(states);

        // The first scored transition uses the last TRAINING bar as its
        // predecessor only if the gap allows; with gap >= 1 the predecessor of
        // te.start sits inside the gap, so scoring starts at te.start and uses
        // te.start-1 -- which is a bar neither block trained on.
        for (std::size_t i = te.start; i + 1 < te.end; ++i) {
            const std::size_t from = b->classify(returns[i]);
            const std::size_t to = b->classify(returns[i + 1]);

            if (row_total[from] == 0) {
                ++f.no_opinion;   // excluded from BOTH scores
                continue;
            }
            if (n[from][to] == 0) {
                ++f.impossible;   // counted at alpha = 0, then smoothed below
            }

            const double p_chain =
                (static_cast<double>(n[from][to]) + alpha)
                / (static_cast<double>(row_total[from]) + aS);
            const double p_base =
                (static_cast<double>(col_total[to]) + alpha)
                / (static_cast<double>(grand) + aS);

            chain_sum += std::log(p_chain);
            base_sum += std::log(p_base);
            ++f.scored;

            std::size_t chain_argmax = 0;
            for (std::size_t j = 1; j < states; ++j) {
                if (n[from][j] > n[from][chain_argmax]) {
                    chain_argmax = j;
                }
            }
            if (chain_argmax == to) { ++f.chain_correct; }
            if (base_argmax == to) { ++f.baseline_correct; }

            // Sign and magnitude, on the SAME observations, against constant
            // baselines. This is the decomposition that says whether the
            // log-score edge is directional or is volatility clustering.
            const bool actually_up = returns[i + 1] > 0.0;
            const bool actually_big = std::fabs(returns[i + 1]) > mag_ref;
            const bool chain_says_up =
                up_given[from] * 2 > row_total[from];
            const bool chain_says_big =
                big_given[from] * 2 > row_total[from];
            if (chain_says_up == actually_up) { ++f.sign_chain_correct; }
            if (base_says_up == actually_up) { ++f.sign_base_correct; }
            if (chain_says_big == actually_big) { ++f.mag_chain_correct; }
            if (base_says_big == actually_big) { ++f.mag_base_correct; }
        }

        if (f.scored > 0) {
            const double d = static_cast<double>(f.scored);
            f.chain_logscore = chain_sum / d;
            f.baseline_logscore = base_sum / d;
            f.edge = f.chain_logscore - f.baseline_logscore;
        }
        out.folds.push_back(f);
    }

    // Aggregate. Every total is a sum over folds, so a fold that scored
    // nothing contributes nothing rather than a zero that drags a mean.
    double sum = 0.0;
    std::size_t counted = 0;
    for (const auto& f : out.folds) {
        out.total_impossible += f.impossible;
        out.total_no_opinion += f.no_opinion;
        out.total_scored += f.scored;
        out.chain_correct += f.chain_correct;
        out.baseline_correct += f.baseline_correct;
        out.sign_chain_correct += f.sign_chain_correct;
        out.sign_base_correct += f.sign_base_correct;
        out.mag_chain_correct += f.mag_chain_correct;
        out.mag_base_correct += f.mag_base_correct;
        if (f.scored == 0) { continue; }
        sum += f.edge;
        ++counted;
        if (f.edge > 0.0) { ++out.folds_positive; }
    }
    if (counted > 0) {
        out.mean_edge = sum / static_cast<double>(counted);
        if (counted > 1) {
            double ss = 0.0;
            for (const auto& f : out.folds) {
                if (f.scored == 0) { continue; }
                const double d = f.edge - out.mean_edge;
                ss += d * d;
            }
            out.edge_sd = std::sqrt(ss / static_cast<double>(counted - 1));
            if (out.edge_sd > 0.0) {
                out.t_stat = out.mean_edge
                           / (out.edge_sd / std::sqrt(
                                  static_cast<double>(counted)));
            }
        }
    }
    return out;
}

} // namespace altair
