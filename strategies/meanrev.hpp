// strategies/meanrev.hpp -- z-score fade, and the cost hurdle a fade has to
// clear before it is allowed to exist.
//
// P21-02. Depends on `strategies/momentum.hpp` for `StrategyResult` and
// `evaluate` -- the two strategies report in the same vocabulary on purpose,
// because the whole point of building both is that they are comparable.
//
// THE OTHER HALF OF THE STAGE 3 HOLE.
//
// Momentum says the last move continues. Mean reversion says it reverses.
// They cannot both be right at the same horizon, and the interesting fact is
// that at DIFFERENT horizons they routinely both are: intraday index returns
// mean-revert (bid-ask bounce and liquidity provision), multi-day trends
// persist. A tree that has one and not the other cannot see that, which is
// the second reason to build both rather than picking the better-sounding one.
//
// HYSTERESIS IS NOT A REFINEMENT. IT IS THE STRATEGY'S ONLY DEFENCE.
//
// A naive fade -- long whenever z < 0, short whenever z > 0 -- crosses zero
// constantly and trades on every crossing. Its turnover approaches 2.0 per
// bar, which at 5.5 bps round trip is a hurdle of 11 bps PER BAR. Nothing
// earns that.
//
// So the rule has two thresholds, not one: enter at |z| > entry_z, and hold
// until |z| < exit_z. The dead band between them is what stops the position
// flickering, and `MeanRevSpec::valid()` REFUSES a spec where exit_z >=
// entry_z, because that spec is the naive fade wearing two parameters.
//
// PRICE THE HURDLE BEFORE THE SIGNAL, NOT AFTER.
//
// Hard rule 5 -- every signal is priced net of full cost BEFORE it exists.
// `required_gross_bps()` below is that rule made callable: given a turnover
// and a cost, it returns the gross edge per bar the strategy must produce
// merely to break even. Call it with the turnover a spec WILL generate and
// you know whether the idea is worth testing before you test it.
//
// THE CONTROL IS A RANDOM WALK, AND IT MUST LOSE.
//
// Every result in this project ships beside the dumb thing it has to beat:
// 1/N against the optimisers, TWAP against Almgren-Chriss, the ten-line
// heuristic against Q-learning -- and the dumb thing has won every time so
// far. Here the control is the SAME RULE ON NOISE. On a random walk a fade
// has no gross edge and still pays every basis point of its turnover, so its
// net is reliably negative. A fade that does not lose money on a random walk
// is not detecting mean reversion; it is detecting a bug.

#pragma once

#include "strategies/momentum.hpp"

#include <algorithm>
#include <cmath>
#include <expected>
#include <vector>

namespace altair {

/// Gross return per bar, in bps, needed just to break even at this turnover.
///
/// This is hard rule 5 in one line. A strategy that turns over 0.4 of its book
/// per bar at 5.5 bps round trip needs 2.2 bps per bar BEFORE it has made a
/// rupee -- roughly 550 bps a year on 250 bars, which is a number worth
/// looking at before writing the strategy rather than after.
[[nodiscard]] constexpr double required_gross_bps(double turnover,
                                                  double cost_bps) noexcept {
    return turnover * cost_bps;
}

/// The fraction of bars that must be directionally right, at a given average
/// win and loss size, for the strategy to clear its own cost.
///
/// Reported because a Sharpe is abstract and a hit rate is not: "you need to
/// be right 53.8% of the time" is a claim a person can weigh against the
/// 52-55% ceiling in the reality checks. Returns NaN when the payoff is
/// degenerate, rather than a plausible-looking number.
[[nodiscard]] inline double breakeven_hit_rate(double avg_win_bps,
                                               double avg_loss_bps,
                                               double turnover,
                                               double cost_bps) noexcept {
    const double denom = avg_win_bps + avg_loss_bps;
    if (!(denom > 0.0)) { return std::nan(""); }
    return (avg_loss_bps + required_gross_bps(turnover, cost_bps)) / denom;
}

struct MeanRevSpec {
    /// Bars in the rolling mean and standard deviation.
    std::size_t window = 60;
    /// Enter when |z| exceeds this.
    double entry_z = 1.5;
    /// Exit when |z| falls below this. STRICTLY between 0 and entry_z.
    ///
    /// Both ends of that range are load-bearing and both were found the hard
    /// way. exit_z >= entry_z is the naive fade wearing two parameters, and
    /// exit_z <= 0 is worse: |z| < 0 is UNSATISFIABLE, so the position enters
    /// once and is never closed. On 36 years of daily NIFTY that spec scored
    /// +4.84 bps/bar with a turnover of 0.0001 and a t of +3.02, and it was
    /// briefly printed in a mean-reversion table as though it were a
    /// mean-reversion result. It was the index's own drift, held for 36 years
    /// by an accident of the first entry's sign.
    double exit_z = 0.5;
    /// Cap on |position|.
    double max_position = 1.0;
    /// Scale the position with the size of the dislocation rather than taking
    /// a flat +-1. Off by default: the flat version is the plainer rule and a
    /// plain rule's failure is attributable to the idea.
    bool scale_with_z = false;

    [[nodiscard]] bool valid() const noexcept {
        return window >= 10 && entry_z > 0.0 && exit_z > 0.0
            && exit_z < entry_z && max_position > 0.0;
    }
};

/// Rolling z-score of the cumulative log price against its own trailing mean.
///
/// `level[i]` is the cumulative sum of returns through i -- a price in log
/// space. The z-score at i uses level[i-window..i-1] only, so the value
/// returned for index i is knowable strictly before bar i is traded.
///
/// Returns NaN where the window has no variation. That is deliberate and it
/// is the "absence is not zero" rule again: a flat window has no z-score, and
/// a z of 0.0 would be read as "at fair value, do nothing" when the truth is
/// "this cannot be measured". The caller must test for NaN.
[[nodiscard]] inline std::vector<double>
rolling_z(const std::vector<double>& level, std::size_t window) {
    std::vector<double> z(level.size(), std::nan(""));
    if (window < 2 || level.size() <= window) { return z; }
    for (std::size_t i = window; i < level.size(); ++i) {
        double mu = 0.0;
        for (std::size_t k = i - window; k < i; ++k) { mu += level[k]; }
        mu /= static_cast<double>(window);
        double s2 = 0.0;
        for (std::size_t k = i - window; k < i; ++k) {
            const double d = level[k] - mu;
            s2 += d * d;
        }
        const double sd = std::sqrt(s2 / static_cast<double>(window - 1));
        // RELATIVE degeneracy test. An absolute `sd > 0` lets through a window
        // of 500 identical values whose sd computes to 9e-18 rather than
        // exactly zero -- the hole found in the HMM guard in P14-03, and the
        // same floating-point hole would be here.
        const double scale = std::max(std::fabs(mu), 1e-12);
        if (!(sd > 1e-9 * scale)) { continue; }
        z[i] = (level[i - 1] - mu) / sd;
    }
    return z;
}

/// Cumulative log level from a return series, so the z-score has something to
/// be a z-score OF. Returns are in bps; the level is in bps too.
[[nodiscard]] inline std::vector<double>
level_from_returns(const std::vector<double>& r) {
    std::vector<double> level(r.size(), 0.0);
    double c = 0.0;
    for (std::size_t i = 0; i < r.size(); ++i) {
        c += r[i];
        level[i] = c;
    }
    return level;
}

/// Position series for a z-score fade with a dead band.
///
/// State machine, three states: flat, long, short. Entry needs |z| > entry_z;
/// exit needs |z| < exit_z. Between the two the position is HELD, which is
/// the entire mechanism by which this strategy is affordable.
[[nodiscard]] inline std::expected<std::vector<double>, StratError>
meanrev_positions(const std::vector<double>& r, const MeanRevSpec& spec) {
    if (!spec.valid()) { return std::unexpected(StratError::BadParameter); }
    if (r.size() < spec.window + 100) {
        return std::unexpected(StratError::TooFewSamples);
    }
    const std::vector<double> level = level_from_returns(r);
    const std::vector<double> z = rolling_z(level, spec.window);

    std::vector<double> pos(r.size(), 0.0);
    double held = 0.0;
    bool any = false;
    for (std::size_t i = spec.window; i < r.size(); ++i) {
        const double zi = z[i];
        if (std::isnan(zi)) {
            // Cannot measure. Flatten rather than hold blind -- rule 9, an
            // ambiguity blocks rather than guesses, and holding a position
            // whose thesis is unmeasurable is a guess.
            held = 0.0;
            pos[i] = 0.0;
            continue;
        }
        any = true;
        if (held == 0.0) {
            if (zi > spec.entry_z) {
                held = -1.0;    // stretched high -> fade short
            } else if (zi < -spec.entry_z) {
                held = 1.0;
            }
        } else if (std::fabs(zi) < spec.exit_z) {
            held = 0.0;
        }
        double p = held * spec.max_position;
        if (spec.scale_with_z && held != 0.0) {
            const double stretch = std::fabs(zi) / spec.entry_z;
            p = held * spec.max_position * std::min(stretch, 2.0);
        }
        pos[i] = p;
    }
    if (!any) { return std::unexpected(StratError::Degenerate); }
    return pos;
}

} // namespace altair
