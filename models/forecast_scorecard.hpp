// models/forecast_scorecard.hpp -- is the price forecast any good?
//
// P33-01. Smit asked a different question from the one spot_forecast.hpp
// answers, and it is a better-posed one:
//
//     forecast the next bar's PRICE, then check it against what the price
//     actually did, and tell me whether the model is doing well or badly.
//
// THE BENCHMARK IS THE RANDOM WALK, AND THAT IS THE WHOLE DESIGN.
//
// spot_forecast.hpp scores a TRADING rule: it takes the sign of the forecast,
// charges a round trip, and reports basis points per bar. The right control
// there is buy-and-hold, because a long-biased rule on a drifting index
// collects the drift for free.
//
// This file scores a FORECAST, and the control is different. The null for
// "what will the price be" is not "hold the index" -- it is
//
//     next price = last price
//
// the random walk with no drift. Anybody can produce that forecast, it costs
// nothing, and on a financial series it is very hard to beat. A price
// forecaster that cannot beat it has produced nothing, however good its
// basis-point P&L looks after a cost hurdle that a daily move clears 95% of
// the time.
//
// THE COMPARISON IS PAIRED, BECAUSE SOME BARS ARE JUST HARDER.
//
// Comparing two RMSEs computed independently throws away the fact that both
// models faced the SAME bars. A quiet week is easy for both and a gap day is
// hard for both, and that shared difficulty is variance the comparison does
// not need to carry. So the statistic is the per-bar difference of squared
// errors, and its standard error is the standard error of that difference --
// the same reasoning P21-03 applied when `excess_over` replaced two
// independent means.
//
// AND THE BAND IS CHECKED AGAINST WHAT IT CLAIMS.
//
// A forecast comes with a band of one out-of-sample RMSE. Under a normal
// error that band should contain the truth about 68.3% of the time. Measuring
// how often it ACTUALLY does is the only way to know whether the band means
// anything -- a band that covers 40% is not a confidence interval, it is a
// decoration, and every size derived from it is wrong in the direction that
// costs money.
//
// NO LOOK-AHEAD, AND THE REFIT SCHEDULE IS WHY IT IS TRACTABLE.
//
// Every prediction is made by a model fitted only on bars STRICTLY BEFORE the
// one being predicted. Refitting at every step would be honest and far too
// slow, so the model is refitted every `refit_every` bars and used forward
// until the next refit -- which is still strictly causal, and is what a
// production retraining cadence looks like anyway.

#pragma once

#include <models/spot_forecast.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

namespace altair {

enum class ForecastScoreError : std::uint8_t {
    /// Fewer bars than the walk-forward needs.
    TooFewBars,
    /// A spec that cannot produce a forecast.
    BadParameter,
    /// Not one refit produced a model.
    NoModel
};

/// One forecast, and what actually happened.
struct ForecastPoint {
    /// The bar being forecast. Zero when the caller supplied no stamps.
    std::int64_t ts_ns = 0;
    /// The last close the model was given -- the anchor, and the naive
    /// forecast.
    double anchor = 0.0;
    /// The model's price for this bar.
    double predicted = 0.0;
    /// What the price actually was.
    double actual = 0.0;
    /// The band the model published around `predicted`.
    double lo = 0.0, hi = 0.0;

    /// Signed error in basis points, model and naive. Positive means the
    /// forecast was ABOVE the truth.
    [[nodiscard]] double err_bps() const noexcept {
        return anchor > 0.0 && actual > 0.0
                   ? 10'000.0 * std::log(predicted / actual) : 0.0;
    }
    [[nodiscard]] double naive_err_bps() const noexcept {
        return anchor > 0.0 && actual > 0.0
                   ? 10'000.0 * std::log(anchor / actual) : 0.0;
    }
    /// Did the forecast get the DIRECTION of the move right?
    ///
    /// A forecast exactly equal to the anchor has no direction and is counted
    /// as a miss rather than as a half -- it is a refusal to call, and
    /// rounding it up would flatter a model that never commits.
    [[nodiscard]] bool direction_right() const noexcept {
        const double p = predicted - anchor;
        const double a = actual - anchor;
        return (p > 0.0 && a > 0.0) || (p < 0.0 && a < 0.0);
    }
    [[nodiscard]] bool inside_band() const noexcept {
        return lo <= actual && actual <= hi;
    }
};

/// What the record says, and how sure it is.
struct ForecastScore {
    std::size_t n = 0;

    /// Mean absolute error, bps. The number a reader understands fastest.
    double mae_model_bps = 0.0;
    double mae_naive_bps = 0.0;
    /// Root mean squared error, bps. What the skill score is built on.
    double rmse_model_bps = 0.0;
    double rmse_naive_bps = 0.0;

    /// 1 - rmse_model/rmse_naive. Positive means better than assuming the
    /// price does not move. Zero means exactly as good as doing nothing.
    double skill = 0.0;

    /// PAIRED mean of (naive squared error - model squared error), and its
    /// standard error. Positive mean = the model is closer, on average.
    double paired_mean = 0.0;
    double paired_se = 0.0;
    /// paired_mean / paired_se. The number the verdict is taken from.
    double t_stat = 0.0;

    /// Fraction of bars whose direction the model called correctly.
    double direction_hit = 0.0;
    /// And its distance from a coin flip, in standard errors.
    ///
    /// KEPT, BUT IT IS NOT THE BENCHMARK. See `direction_vs_drift` -- on a
    /// drifting series a coin flip is not what a trivial forecaster scores.
    double direction_sigma = 0.0;

    /// What ALWAYS SAYING "UP" would have scored -- the fraction of bars that
    /// actually rose.
    double up_rate = 0.0;
    /// The best a constant-direction forecaster could do: max(up, 1-up). This
    /// is the honest directional null on a series with drift.
    double best_constant_direction = 0.0;
    /// Model hit rate minus that, in standard errors. THE directional number.
    double direction_vs_drift = 0.0;

    /// Fraction of actuals that landed inside the published band, and what
    /// the band CLAIMS to cover.
    double coverage = 0.0;
    double nominal_coverage = 0.6827;   // one sigma, normal

    /// Systematic error. A forecaster that is always 3 bps high is fixable;
    /// one that is unbiased and wide is not.
    double mean_error_bps = 0.0;
};

/// The verdict, and there are four of them for a reason.
enum class ForecastVerdict : std::uint8_t {
    /// Not enough forecasts to say anything. Ordinal zero, deliberately: an
    /// uninitialised verdict must not read as an endorsement.
    NotEnoughEvidence = 0,
    /// Measurably worse than assuming the price does not move.
    WorseThanNaive,
    /// Not distinguishable from the random walk. THE COMMON CASE, and it is
    /// not the same as "worse" -- it is "no evidence either way", which is
    /// what most honest forecast records say.
    NoBetterThanNaive,
    /// Measurably better than the random walk.
    BetterThanNaive
};

[[nodiscard]] inline const char* verdict_text(ForecastVerdict v) noexcept {
    switch (v) {
    case ForecastVerdict::NotEnoughEvidence:  return "NOT ENOUGH EVIDENCE";
    case ForecastVerdict::WorseThanNaive:     return "WORSE THAN NAIVE";
    case ForecastVerdict::NoBetterThanNaive:  return "NO BETTER THAN NAIVE";
    case ForecastVerdict::BetterThanNaive:    return "BETTER THAN NAIVE";
    }
    return "UNKNOWN";
}

/// THE DECISION MAKER.
///
/// `k` is how many standard errors the paired difference must clear. Two is
/// the usual choice and it is a PARAMETER rather than a constant here,
/// because the threshold is a risk appetite and not a fact about the data.
///
/// `min_n` guards the other failure: a t-statistic on forty forecasts is a
/// number, not evidence, and reporting BETTER off one good month is how a
/// model gets deployed on a fluke.
[[nodiscard]] inline ForecastVerdict
judge_forecast(const ForecastScore& s, double k = 2.0,
               std::size_t min_n = 200) noexcept {
    if (s.n < min_n || !(s.paired_se > 0.0)) {
        return ForecastVerdict::NotEnoughEvidence;
    }
    if (s.t_stat > k) { return ForecastVerdict::BetterThanNaive; }
    if (s.t_stat < -k) { return ForecastVerdict::WorseThanNaive; }
    return ForecastVerdict::NoBetterThanNaive;
}

/// Score a set of forecasts against what happened.
[[nodiscard]] inline std::expected<ForecastScore, ForecastScoreError>
score_forecasts(const std::vector<ForecastPoint>& pts) noexcept {
    if (pts.empty()) { return std::unexpected(ForecastScoreError::TooFewBars); }

    ForecastScore s{};
    s.n = pts.size();
    const double n = static_cast<double>(s.n);

    double abs_m = 0.0, abs_v = 0.0, sq_m = 0.0, sq_v = 0.0;
    double sum_err = 0.0;
    std::size_t hits = 0, inside = 0, ups = 0;
    std::vector<double> diff;
    diff.reserve(pts.size());

    for (const ForecastPoint& p : pts) {
        const double em = p.err_bps();
        const double ev = p.naive_err_bps();
        abs_m += std::fabs(em);
        abs_v += std::fabs(ev);
        sq_m += em * em;
        sq_v += ev * ev;
        sum_err += em;
        if (p.direction_right()) { ++hits; }
        if (p.actual > p.anchor) { ++ups; }
        if (p.inside_band()) { ++inside; }
        // PAIRED: the same bar, both models, differenced before averaging.
        diff.push_back(ev * ev - em * em);
    }

    s.mae_model_bps = abs_m / n;
    s.mae_naive_bps = abs_v / n;
    s.rmse_model_bps = std::sqrt(sq_m / n);
    s.rmse_naive_bps = std::sqrt(sq_v / n);
    s.skill = s.rmse_naive_bps > 0.0
                  ? 1.0 - s.rmse_model_bps / s.rmse_naive_bps : 0.0;
    s.direction_hit = static_cast<double>(hits) / n;
    s.direction_sigma = s.n > 1
        ? (s.direction_hit - 0.5) / std::sqrt(0.25 / n) : 0.0;
    s.up_rate = static_cast<double>(ups) / n;
    s.best_constant_direction = std::max(s.up_rate, 1.0 - s.up_rate);
    // Against the constant-direction forecaster, not against a coin. The
    // standard error is still that of a proportion; what changes is what the
    // model is being asked to beat.
    s.direction_vs_drift =
        s.n > 1 ? (s.direction_hit - s.best_constant_direction)
                      / std::sqrt(0.25 / n)
                : 0.0;
    s.coverage = static_cast<double>(inside) / n;
    s.mean_error_bps = sum_err / n;

    double dm = 0.0;
    for (double d : diff) { dm += d; }
    dm /= n;
    double dv = 0.0;
    for (double d : diff) { dv += (d - dm) * (d - dm); }
    s.paired_mean = dm;
    s.paired_se = s.n > 1 ? std::sqrt(dv / (n - 1.0)) / std::sqrt(n) : 0.0;
    s.t_stat = s.paired_se > 0.0 ? s.paired_mean / s.paired_se : 0.0;
    return s;
}

/// How the walk-forward is run.
struct BacktestSpec {
    /// Bars of history the FIRST model is fitted on. Everything after this is
    /// a forecast.
    std::size_t warmup = 2000;
    /// Refit this often. Every prediction still uses a model fitted only on
    /// bars strictly before the one predicted.
    std::size_t refit_every = 100;
    /// Cap on how many bars to score, taken from the END of the series. Zero
    /// means all of them.
    std::size_t max_points = 0;

    [[nodiscard]] bool valid() const noexcept {
        return warmup >= 500 && refit_every >= 1;
    }
};

/// Walk forward through `closes`, forecasting each bar from its own past.
///
/// `band_bps` is the half-width published around each forecast. It is passed
/// in rather than computed here for the reason forecast_next takes it: a band
/// derived from the training residual is narrower than the truth by exactly
/// the amount the model overfits, and this file must not be the place that
/// mistake enters.
[[nodiscard]] inline std::expected<std::vector<ForecastPoint>, ForecastScoreError>
backtest_forecasts(const std::vector<double>& closes,
                   const std::vector<std::int64_t>& stamps,
                   const SpotSpec& spec, const BacktestSpec& bt,
                   double band_bps, bool leaf_wise) {
    if (!spec.valid() || !bt.valid()) {
        return std::unexpected(ForecastScoreError::BadParameter);
    }
    if (closes.size() < bt.warmup + spec.lags + spec.horizon + 10) {
        return std::unexpected(ForecastScoreError::TooFewBars);
    }
    if (!stamps.empty() && stamps.size() != closes.size()) {
        return std::unexpected(ForecastScoreError::BadParameter);
    }

    // The whole log-return series, in bps. Building it once is safe: a
    // return at index j is closes[j+1]/closes[j], and every use below is
    // bounded by the bar being predicted.
    std::vector<double> r;
    r.reserve(closes.size());
    for (std::size_t i = 1; i < closes.size(); ++i) {
        if (!(closes[i] > 0.0) || !(closes[i - 1] > 0.0)) {
            r.push_back(0.0);
        } else {
            r.push_back(10'000.0 * std::log(closes[i] / closes[i - 1]));
        }
    }

    std::size_t first = bt.warmup;
    if (bt.max_points > 0 && closes.size() - first > bt.max_points) {
        first = closes.size() - bt.max_points;
    }
    if (first <= spec.lags + 1) { return std::unexpected(ForecastScoreError::TooFewBars); }

    std::vector<ForecastPoint> out;
    out.reserve(closes.size() - first);
    std::size_t fits = 0;

    for (std::size_t block = first; block + spec.horizon < closes.size();
         block += bt.refit_every) {

        // ── FIT ON [0, block) AND NOTHING ELSE ──────────────────────────
        //
        // Row i uses returns r[i-lags .. i) and is labelled with the forward
        // window starting at r[i], so a row is only admissible when its whole
        // label window closed strictly before `block`.
        //
        // P41. THE SAME BUILDER spot_forecast.hpp USES, and that is not a
        // tidy-up. There were FOUR copies of this construction -- two here,
        // two there -- agreeing only because each was four lines long. The
        // Forecast page takes its RECORD from this function and its PRICE
        // from forecast_next; the moment the two builders differed, the
        // record would have described a different model from the number
        // printed beside it, and nothing would have looked wrong.
        Frame f;
        f.p = feature_width(spec);
        std::vector<double> y;
        const std::size_t train_end = block > 1 ? block - 1 : 0;   // in r-space
        for (std::size_t i = first_row_index(spec);
             i + spec.horizon <= train_end; ++i) {
            append_row(r, i, spec, f.x);
            double fwd = 0.0;
            for (std::size_t h = 0; h < spec.horizon; ++h) { fwd += r[i + h]; }
            y.push_back(fwd / label_scale(r, i, spec));
            ++f.rows;
        }
        if (f.rows < 500) { continue; }

        GbdtParams pr;
        pr.trees = 120;
        pr.max_depth = leaf_wise ? 12 : 4;
        pr.learning_rate = 0.05;
        pr.seed = 0xB0057u;
        pr.leaf_wise = leaf_wise;
        pr.max_leaves = 16;

        const auto model = fit_gbdt(f, y, pr);
        if (!model) { continue; }
        ++fits;

        // ── PREDICT EACH BAR FROM ITS OWN FEATURES ──────────────────────
        //
        // THE FIT IS HELD ACROSS THE BLOCK. THE PREDICTION IS NOT, AND THE
        // FIRST VERSION OF THIS FUNCTION GOT THAT WRONG.
        //
        // It called forecast_next once per block and applied the single move
        // it returned to all 250 bars that followed. Refitting rarely is a
        // legitimate production cadence and costs almost nothing in accuracy;
        // reusing one PREDICTION for 250 bars is simply a different and wrong
        // thing, because the features move every bar even when the model does
        // not.
        //
        // It was caught by the control in test 2: a series with phi = -0.45,
        // which a lagged-return model must be able to forecast, came back
        // WORSE THAN NAIVE at t = -12.99. On that series the correct call
        // alternates sign every bar, and a held prediction is not merely
        // stale -- it is anti-correlated with the truth. A judge validated
        // only against a random walk would have shown nothing.
        //
        // Fitting is the expensive part and it still happens once per block.
        // Predicting is one tree traversal.
        const std::size_t stop =
            std::min(block + bt.refit_every, closes.size() - spec.horizon + 1);
        std::vector<double> row;
        row.reserve(feature_width(spec));
        for (std::size_t i = block; i < stop; ++i) {
            // The bar being predicted is closes[i]; its anchor is closes[i-1].
            // The features are the returns ending at r[i-2] -- every return
            // fully observed BEFORE closes[i-1] was the last close. r[j] is
            // the move from closes[j] to closes[j+1], so the newest usable
            // return is r[i-2], and append_row's label index is therefore
            // i-1: it reads up to r[i-2] and no further.
            if (i < first_row_index(spec) + 1) { continue; }
            const std::size_t r_end = i - 1;          // label index
            if (r_end < first_row_index(spec)) { continue; }
            row.clear();
            append_row(r, r_end, spec, row);
            // AND THE SCALE COMES BACK OFF. Trained on return/volatility, so
            // the raw output is in those units.
            const double move = model->predict_row(row.data())
                                * label_scale(r, r_end, spec);

            ForecastPoint p{};
            p.anchor = closes[i - 1];
            p.predicted = p.anchor * std::exp(move / 10'000.0);
            p.lo = p.anchor * std::exp((move - band_bps) / 10'000.0);
            p.hi = p.anchor * std::exp((move + band_bps) / 10'000.0);
            p.actual = closes[i + spec.horizon - 1];
            if (!stamps.empty() && i + spec.horizon - 1 < stamps.size()) {
                p.ts_ns = stamps[i + spec.horizon - 1];
            }
            if (p.anchor > 0.0 && p.actual > 0.0) { out.push_back(p); }
        }
    }
    if (fits == 0) { return std::unexpected(ForecastScoreError::NoModel); }
    if (out.empty()) { return std::unexpected(ForecastScoreError::TooFewBars); }
    return out;
}

} // namespace altair
