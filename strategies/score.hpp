// strategies/score.hpp -- the score engine: regime-conditional,
// multi-timeframe, correlation-aware.
//
// P6-04. Detection only; oms/ decides what to do with a score.
//
// TEN CORRELATED SIGNALS ARE NOT TEN SIGNALS.
//
// This is the card. Averaging n standardised signals and treating the result
// as having n independent observations behind it is the single most common way
// a scoring system manufactures confidence it has not earned.
//
// For n unit-variance signals with average pairwise correlation rho,
//
//     Var(mean) = (1 + (n-1)*rho) / n
//
// so the standard error is sqrt(1 + (n-1)*rho) times the independent case, and
// the EFFECTIVE number of signals is
//
//     n_eff = n / (1 + (n-1)*rho)
//
// Measured, on an ordinary ten-member EMA momentum stack with periods from 5
// to 165 over ONE price series -- the correlation computed from the data, not
// assumed:
//
//     mean pairwise correlation          0.781
//     effective independent signals      1.25    of ten
//     naive standard error understates   2.83x
//
// A ten-indicator panel carrying one and a quarter signals' worth of
// information. A reading the naive engine calls 2.83 sigma is really 1.00 --
// and the trade is sized on the conviction, so an engine that assumes
// independence does not merely misreport, it sizes on the misreport.
//
// MULTI-TIMEFRAME MAKES THIS STRUCTURAL, NOT ACCIDENTAL.
//
// A 5-minute and a 15-minute moving average of the same series share their
// data by construction. They are not two views of the market; they are one
// series filtered twice. The test measures the correlation between EMA signals
// at neighbouring scales on real-shaped data and feeds the measurement in,
// rather than assuming a number -- because the whole point is that the
// correlation is large and is not guessed.
//
// SIZE ON THE LOWER CONFIDENCE BOUND (ROADMAP section 3).
//
// The score is a measurement, so it has an error bar, so what a position is
// sized on is the lower bound of the edge and not its point estimate. A score
// whose interval straddles zero is not a signal. `fires()` enforces that and
// there is no accessor that returns the point estimate alone.
//
// EVERY SCORE IS NET OF COST BEFORE IT EXISTS (rule 5).
//
// The engine converts a standardised score into an expected move in paise
// using a calibrated scale, subtracts the round-trip cost, and only then
// produces a number. There is no gross field. A strategy cannot read a
// pre-cost score because one is never constructed.
//
// AN INCOMPLETE REGIME DOES NOT FALL BACK.
//
// A model conditioned on regime and a model not conditioned on regime are
// different models trained on different samples. When the regime is not fully
// decided the engine refuses, rather than quietly evaluating the
// unconditioned version and reporting it as though it were the conditioned
// one.

#pragma once

#include <risk/cost.hpp>
#include <strategies/regime.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

inline constexpr std::size_t kMaxSignals = 32;

enum class ScoreError : std::uint8_t {
    /// Fewer than one ready signal.
    NoSignals,
    /// The regime is not fully decided.
    RegimeIncomplete,
    /// No weights were supplied for this regime.
    NoWeights,
    /// The correlation was not supplied, or is outside [0, 1).
    NoCorrelation,
    /// The score-to-paise scale was not calibrated.
    NoScale,
    /// The cost calculator refused.
    CostUnavailable
};

/// One standardised input.
///
/// `z` is DIMENSIONLESS and centred, because an RSI in [0, 100] and a z-score
/// in [-3, 3] cannot be averaged -- the average is whatever the larger range
/// says it is. Standardising is the caller's job and the type name says so.
struct Signal {
    double z = 0.0;
    /// The signal's own standard error, in the same dimensionless units.
    double std_error = 1.0;
    bool ready = false;
};

/// Weights for one regime.
///
/// A separate set per regime is the entire point of "regime-conditional": the
/// same indicator carries different information in a trending market and a
/// mean-reverting one, frequently with the opposite sign.
struct RegimeWeights {
    double w[kMaxSignals] = {};
    std::size_t n = 0;
    /// Average pairwise correlation among these signals, MEASURED and passed
    /// in. There is no default: assuming zero is the bug this file exists
    /// around, and assuming anything else is a different unfounded guess.
    double mean_correlation = -1.0;
    /// Expected move, in paise, per unit of score. Calibrated from history;
    /// without it a score cannot be compared against a cost and rule 5 cannot
    /// be satisfied.
    double paise_per_score = 0.0;
    /// How many standard errors below the point estimate to size on.
    double confidence_k = 0.0;
};

/// The effective number of independent signals.
///
/// n / (1 + (n-1)*rho). Exposed on its own so it can be checked against the
/// closed form rather than trusted inside a larger calculation.
[[nodiscard]] inline double effective_signals(std::size_t n,
                                              double rho) noexcept {
    if (n == 0) { return 0.0; }
    const double d = 1.0 + (static_cast<double>(n) - 1.0) * rho;
    if (!(d > 0.0)) { return 0.0; }
    return static_cast<double>(n) / d;
}

/// How much a naive independent-signals standard error understates the truth.
/// sqrt(1 + (n-1)*rho).
[[nodiscard]] inline double correlation_inflation(std::size_t n,
                                                  double rho) noexcept {
    if (n < 2) { return 1.0; }
    const double d = 1.0 + (static_cast<double>(n) - 1.0) * rho;
    return d > 0.0 ? std::sqrt(d) : 1.0;
}

/// A score, its uncertainty, and the number to act on.
///
/// There is no `gross` and no bare point estimate of edge. `expected_net` is
/// already net of cost; `lower_bound` is what a size is derived from.
struct Score {
    /// The correlation-adjusted weighted mean of the standardised signals.
    double score = 0.0;
    /// Its standard error, WITH the correlation inflation applied.
    double std_error = 0.0;
    /// What the naive independent-signals calculation would have claimed.
    /// Carried for comparison; never the basis for a decision.
    double naive_std_error = 0.0;
    double n_effective = 0.0;
    std::size_t n_signals = 0;

    /// Expected edge in paise, NET of the round-trip cost.
    Notional expected_net{};
    /// expected_net minus k standard errors, in paise. THE number to size on.
    Notional lower_bound{};
    MarketRegime regime{};

    /// A signal exists only when its lower bound clears zero. ROADMAP section
    /// 3: a signal whose error bar straddles zero is not a signal.
    [[nodiscard]] bool fires() const noexcept {
        return regime.complete() && lower_bound.raw() > 0;
    }
    /// Direction, which is only meaningful when it fires.
    [[nodiscard]] int direction() const noexcept {
        if (!fires()) { return 0; }
        return score > 0.0 ? 1 : -1;
    }
};

/// Combine signals into one regime-conditional, cost-netted score.
///
/// `round_trip` is the full cost of getting in and out, from P3-09. It is a
/// parameter and not something this computes, because the cost depends on the
/// instrument and the schedule and this file has no business knowing either.
[[nodiscard]] inline std::expected<Score, ScoreError>
combine(const Signal* signals, std::size_t n, const RegimeWeights& w,
        const MarketRegime& regime, Notional round_trip) noexcept {
    if (!regime.complete()) {
        return std::unexpected(ScoreError::RegimeIncomplete);
    }
    if (w.n == 0 || w.n != n || n > kMaxSignals) {
        return std::unexpected(ScoreError::NoWeights);
    }
    if (!(w.mean_correlation >= 0.0) || !(w.mean_correlation < 1.0)) {
        return std::unexpected(ScoreError::NoCorrelation);
    }
    if (!(w.paise_per_score > 0.0) || !(w.confidence_k > 0.0)) {
        return std::unexpected(ScoreError::NoScale);
    }

    // Only READY signals count, and their weights are renormalised over the
    // ready set. Treating an unready signal as a zero would be treating "no
    // opinion" as "neutral opinion", and a neutral opinion drags the score
    // toward zero exactly as a real one would.
    double sw = 0.0, acc = 0.0, var_acc = 0.0;
    std::size_t ready = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!signals[i].ready) { continue; }
        const double a = w.w[i] < 0.0 ? -w.w[i] : w.w[i];
        sw += a;
        acc += w.w[i] * signals[i].z;
        const double c = w.w[i] * signals[i].std_error;
        var_acc += c * c;
        ++ready;
    }
    if (ready == 0 || !(sw > 0.0)) {
        return std::unexpected(ScoreError::NoSignals);
    }

    Score out{};
    out.regime = regime;
    out.n_signals = ready;
    out.score = acc / sw;
    // The INDEPENDENT-signals standard error: what a naive engine reports.
    out.naive_std_error = std::sqrt(var_acc) / sw;
    // And the truth, inflated by the correlation among the signals.
    out.std_error = out.naive_std_error
                  * correlation_inflation(ready, w.mean_correlation);
    out.n_effective = effective_signals(ready, w.mean_correlation);

    // Rule 5: the cost comes off before the number exists.
    //
    // Both fields are MAGNITUDES of edge available in the direction the score
    // points, which is what makes them comparable across longs and shorts.
    // `direction()` carries the sign; keeping it out of the edge fields is
    // what stops a large negative score reading as a large positive edge.
    const double gross = std::fabs(out.score) * w.paise_per_score;
    const double err = out.std_error * w.paise_per_score;
    const double net = gross - static_cast<double>(round_trip.raw());
    const double lower = net - w.confidence_k * err;

    const auto to_paise = [](double x) {
        return Notional{static_cast<std::int64_t>(x >= 0.0 ? x + 0.5 : x - 0.5)};
    };
    out.expected_net = to_paise(net);
    out.lower_bound = to_paise(lower);
    return out;
}

/// The naive score an engine that ignored correlation would have produced.
///
/// Same inputs, same arithmetic, `mean_correlation` forced to zero. Exists so
/// the difference can be measured on real data instead of argued from the
/// formula.
[[nodiscard]] inline std::expected<Score, ScoreError>
combine_assuming_independence(const Signal* signals, std::size_t n,
                              const RegimeWeights& w,
                              const MarketRegime& regime,
                              Notional round_trip) noexcept {
    RegimeWeights naive = w;
    naive.mean_correlation = 0.0;
    return combine(signals, n, naive, regime, round_trip);
}

/// Average pairwise correlation of a set of series, MEASURED.
///
/// The number `RegimeWeights::mean_correlation` wants. Provided here so a
/// caller has no excuse to guess it: the whole card is that the correlation is
/// large, and the only way to know how large is to compute it.
[[nodiscard]] inline std::expected<double, ScoreError>
mean_pairwise_correlation(const double* const* series, std::size_t k,
                          std::size_t len) noexcept {
    if (k < 2 || len < 3) { return std::unexpected(ScoreError::NoSignals); }
    double total = 0.0;
    std::size_t pairs = 0;
    for (std::size_t a = 0; a < k; ++a) {
        for (std::size_t b = a + 1; b < k; ++b) {
            double ma = 0.0, mb = 0.0;
            for (std::size_t i = 0; i < len; ++i) {
                ma += series[a][i];
                mb += series[b][i];
            }
            ma /= static_cast<double>(len);
            mb /= static_cast<double>(len);
            double saa = 0.0, sbb = 0.0, sab = 0.0;
            for (std::size_t i = 0; i < len; ++i) {
                const double da = series[a][i] - ma;
                const double db = series[b][i] - mb;
                saa += da * da;
                sbb += db * db;
                sab += da * db;
            }
            if (saa > 0.0 && sbb > 0.0) {
                total += sab / std::sqrt(saa * sbb);
                ++pairs;
            }
        }
    }
    if (pairs == 0) { return std::unexpected(ScoreError::NoSignals); }
    return total / static_cast<double>(pairs);
}

} // namespace altair
