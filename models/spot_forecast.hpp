// models/spot_forecast.hpp -- gradient-boosted spot forecasting, net of cost.
//
// P16-06.
//
// THE COST HURDLE COMES FIRST, BEFORE ANY MODEL RUNS.
//
// CLAUDE.md rule 5: every signal is priced net of full cost BEFORE it exists.
// P8-15 built that check and it settles more questions than a training run
// does -- at five minutes a PERFECT ORACLE nets -0.07 bps per bar against a
// 5.5 bps round trip. An oracle. Knowing the future exactly, and still losing.
//
// So this file reports, per horizon and in this order:
//
//   1. what fraction of moves even EXCEED the round-trip cost;
//   2. what a perfect oracle earns net;
//   3. only then, what the model does.
//
// A model that beats a constant on RMSE and sits below the oracle's net figure
// has not found an edge, it has found a number. Reporting (3) without (1) and
// (2) is how a 54% directional accuracy becomes a strategy.
//
// LEAF-WISE VERSUS LEVEL-WISE IS RUN AS A COMPARISON, NOT A CHOICE.
//
// Leaf-wise growth is LightGBM's headline and it reliably reaches lower
// TRAINING loss at equal leaf count. Whether that survives out of sample on a
// financial series is a different question with a different answer, and the
// honest way to answer it is to run both on identical folds and read the
// out-of-sample numbers. This project has already found four such headlines
// that did not transfer.
//
// WALK-FORWARD, WITH A GAP.
//
// Fit on a block, skip a gap the length of the forecast horizon, then score.
// Without the gap the last training label overlaps the first test window and
// the model has seen part of its own answer -- the subtlest look-ahead there
// is, because nothing in the code reaches forward and the leak is entirely in
// the label construction.

#pragma once

#include <models/gbdt.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <vector>

namespace altair {

enum class SpotError : std::uint8_t {
    TooFewSamples,
    BadParameter
};

struct SpotSpec {
    /// Bars ahead to forecast.
    std::size_t horizon = 1;
    /// ROUND-TRIP cost in basis points. Not one-way: a forecast is acted on
    /// by entering AND leaving.
    double cost_bps = 5.5;
    /// Walk-forward folds.
    std::size_t folds = 5;
    /// Lagged returns used as features.
    std::size_t lags = 8;

    /// P41. Beyond lagged returns: realised volatility at three scales,
    /// momentum at two, the absolute move, a z-score and a vol ratio.
    ///
    /// OFF BY DEFAULT so every number this project has already published
    /// stays reproducible. Turning it on changes the model, and a model that
    /// silently changed under a stored result would make the stored result a
    /// lie.
    bool rich = false;

    /// Divide the label by trailing realised volatility, and multiply the
    /// prediction back by it.
    ///
    /// THE POINT IS STATIONARITY, NOT SCALE. Trees do not care about units --
    /// a monotone rescaling of a FEATURE cannot change a single split, so
    /// standardising the inputs of a GBDT is arithmetic with no effect. The
    /// LABEL is different. A daily NIFTY return is drawn from a distribution
    /// whose width moves by a factor of four between calm and stress, so one
    /// model is being asked to fit two very different targets at once.
    /// Dividing by the volatility KNOWN AT THE TIME turns it into roughly one
    /// target, and multiplying back afterwards returns a real return.
    ///
    /// This is the honest version of "scale down, predict, upscale": it is
    /// worth doing to the thing being predicted, and pointless on the inputs
    /// of a tree.
    bool vol_scaled_label = false;

    [[nodiscard]] bool valid() const noexcept {
        return horizon >= 1 && folds >= 2 && lags >= 2 && cost_bps >= 0.0;
    }
};

/// Bars of history a row needs before it can be built.
///
/// The longest window any rich feature reads. Rows before this cannot be
/// built at all -- and are DROPPED rather than filled with a zero, because a
/// realised volatility of zero is a specific, wrong, and very confident claim.
inline constexpr std::size_t kRichWarmup = 60;

/// Feature columns a spec produces.
[[nodiscard]] inline std::size_t feature_width(const SpotSpec& s) noexcept {
    return s.rich ? s.lags + 8 : s.lags;
}

/// First index at which a row can be built.
///
/// THE LABEL'S WINDOW COUNTS TOO, and leaving it out was a real bug with a
/// spectacular symptom. vol_scaled_label divides by a 20-bar volatility, which
/// needs 19 bars of history; before that window_sd returns 0 and the floor in
/// label_scale turns it into 1.0. So with `rich` off, rows 8..18 carried
/// UNSCALED labels of roughly +/-100 bps while every later row carried a
/// standardised one of roughly +/-1 -- a handful of rows a hundred times
/// larger than the rest, which trees fit enthusiastically. Measured RMSE was
/// 625 bps against a naive 140, and it looked like the method failing rather
/// than eleven poisoned rows.
[[nodiscard]] inline std::size_t first_row_index(const SpotSpec& s) noexcept {
    std::size_t need = s.lags;
    if (s.rich && kRichWarmup > need) { need = kRichWarmup; }
    // window_sd(j, 20) with j = i-1 requires i-1 >= 19, so i >= 20.
    if (s.vol_scaled_label && need < 20) { need = 20; }
    return need;
}

namespace detail {

/// Standard deviation of r[i-w+1 .. i]. Reads NOTHING past i.
[[nodiscard]] inline double window_sd(const std::vector<double>& r,
                                      std::size_t i, std::size_t w) noexcept {
    if (w < 2 || i + 1 < w) { return 0.0; }
    const std::size_t from = i + 1 - w;
    double m = 0.0;
    for (std::size_t k = from; k <= i; ++k) { m += r[k]; }
    m /= static_cast<double>(w);
    double v = 0.0;
    for (std::size_t k = from; k <= i; ++k) {
        const double e = r[k] - m;
        v += e * e;
    }
    return std::sqrt(v / static_cast<double>(w - 1));
}

[[nodiscard]] inline double window_sum(const std::vector<double>& r,
                                       std::size_t i, std::size_t w) noexcept {
    if (i + 1 < w) { return 0.0; }
    double t = 0.0;
    for (std::size_t k = i + 1 - w; k <= i; ++k) { t += r[k]; }
    return t;
}

}  // namespace detail

/// THE ONE FEATURE BUILDER.
///
/// P41. There were two, in forecast_spot and forecast_next, and the comment
/// beside the second one said "a second feature construction here would be a
/// second thing to keep in step" -- while being exactly that. They agreed only
/// because both were four lines long. Adding a ninth column to one and not the
/// other would have made the model that was EVALUATED different from the model
/// that PREDICTS, and every score on the page would have described a different
/// thing from the number beside it.
///
/// `i` is the index at which the LABEL BEGINS, matching the loops that call
/// this: the label is the sum of r[i .. i+horizon-1]. So the last return this
/// may read is r[i-1], and r[i] is already part of the answer.
///
/// GETTING THAT OFF BY ONE WOULD BE INVISIBLE AND FATAL. The first draft of
/// this function used r[i] as the most recent feature, which puts the first
/// element of the label into the inputs. The model would have scored
/// magnificently, the walk-forward would still have been honest about fold
/// boundaries, and every number on the page would have been fiction. It is
/// checked by a test that detonates r[i] and requires the row to come back
/// bit-identical.
///
/// Appends `feature_width(spec)` values to `out`.
inline void append_row(const std::vector<double>& r, std::size_t i,
                       const SpotSpec& spec, std::vector<double>& out) {
    for (std::size_t k = 0; k < spec.lags; ++k) {
        out.push_back(r[i - spec.lags + k]);
    }
    if (!spec.rich) { return; }

    // Everything below is anchored at i-1, the last bar the model has seen.
    const std::size_t j = i - 1;
    const double rv5 = detail::window_sd(r, j, 5);
    const double rv20 = detail::window_sd(r, j, 20);
    const double rv60 = detail::window_sd(r, j, 60);
    out.push_back(rv5);
    out.push_back(rv20);
    out.push_back(rv60);
    out.push_back(detail::window_sum(r, j, 5));
    out.push_back(detail::window_sum(r, j, 20));
    out.push_back(std::fabs(r[j]));
    // A z-score and a vol ratio, both guarded: a flat window gives rv 0 and
    // the ratio is undefined. Zero is the right answer for "no information"
    // here precisely because it is the mean of a z-score, not a fabricated
    // measurement.
    out.push_back(rv20 > 1e-9 ? r[j] / rv20 : 0.0);
    out.push_back(rv20 > 1e-9 ? rv5 / rv20 : 1.0);
}

/// The volatility a vol-scaled label is divided by.
///
/// `i` is where the label begins, so this is anchored at i-1 like the features
/// -- the scale must be knowable at prediction time, and r[i] is not.
[[nodiscard]] inline double label_scale(const std::vector<double>& r,
                                        std::size_t i,
                                        const SpotSpec& spec) noexcept {
    if (!spec.vol_scaled_label || i == 0) { return 1.0; }
    const double sd = detail::window_sd(r, i - 1, 20);
    // A floor, not a clamp toward danger: dividing by a near-zero volatility
    // would manufacture an enormous label out of a quiet week.
    return sd > 1.0 ? sd : 1.0;
}

struct SpotReport {
    std::size_t rows = 0;
    std::size_t scored = 0;
    /// Fraction of horizon moves whose magnitude exceeds the round-trip cost.
    /// THE first number to read: if this is small, nothing downstream matters.
    double frac_exceeding_cost = 0.0;
    /// A perfect oracle, trading every bar, net of cost. bps per bar.
    double oracle_net_bps = 0.0;
    /// A perfect oracle trading ONLY when the move clears cost. The ceiling
    /// on any strategy at this horizon.
    double oracle_selective_net_bps = 0.0;

    /// RMSE in bps.
    double rmse_constant = 0.0;
    double rmse_level = 0.0;
    double rmse_leaf = 0.0;
    /// Directional accuracy.
    double dir_constant = 0.0;
    double dir_level = 0.0;
    double dir_leaf = 0.0;
    /// The model's own net bps per bar, trading its own signal, after cost.
    double net_bps_level = 0.0;
    double net_bps_leaf = 0.0;

    /// Directional edge over the constant, in standard errors.
    [[nodiscard]] double sigma_level() const noexcept { return sig(dir_level); }
    [[nodiscard]] double sigma_leaf() const noexcept { return sig(dir_leaf); }

private:
    [[nodiscard]] double sig(double d) const noexcept {
        if (scored < 2) { return 0.0; }
        const double se = std::sqrt(0.25 / static_cast<double>(scored));
        return se > 0.0 ? (d - dir_constant) / se : 0.0;
    }
};

/// Fit and score both growth strategies on the same walk-forward folds.
///
/// `closes` are prices; features are lagged log returns, so the model never
/// sees a level and cannot learn "the index is higher now than in 1998".
/// A forecast for the NEXT bar, as a PRICE.
///
/// P30-01. `forecast_spot` above is an EVALUATION -- it walks folds and reports
/// how well the model did. It never predicts anything forward, and everything
/// it returns is in basis points. Smit asked for a price.
///
/// A BASIS POINT IS NOT A FORECAST SOMEBODY CAN ACT ON.
///
/// "+12.4 bps" requires the reader to hold the last price in their head and do
/// the arithmetic, and the arithmetic is exponential rather than additive
/// because the label is a log return. So this returns the level: the last
/// available close carried forward by the model's own prediction.
///
/// AND IT IS RETURNED WITH A BAND, NEVER ALONE.
///
/// ROADMAP section 3: a measurement without its error is not a measurement,
/// and this project has now applied that to edge, implied vol, Hurst,
/// velocity and an information coefficient. A point forecast is the same
/// object. The band is the model's own OUT-OF-SAMPLE RMSE from the
/// walk-forward -- not the training residual, which is optimistic by exactly
/// the amount the model overfits -- so `lo` and `hi` are one standard error of
/// the thing the model actually gets wrong.
///
/// THE BAND WILL BE EMBARRASSINGLY WIDE, AND THAT IS THE POINT.
///
/// P16-06 measured this model's RMSE as WORSE than a constant at every
/// horizon. A band drawn from that RMSE swamps the point, which is the honest
/// picture: the forecast is a number, and the number is not distinguishable
/// from the last price.
struct SpotForecast {
    /// The last close the model was given. UNIT: the caller's price units.
    double last_price = 0.0;
    /// The model's prediction for the next `horizon` bars. UNIT: bps.
    double move_bps = 0.0;
    /// last_price * exp(move_bps / 10000). Same units as last_price.
    double price = 0.0;
    /// One out-of-sample RMSE, in bps and as a price distance.
    double band_bps = 0.0;
    double lo = 0.0, hi = 0.0;
    /// Rows the model was fitted on.
    std::size_t fitted_on = 0;

    // ── WHICH BAR. P32-06. ───────────────────────────────────────────────
    //
    // A FORECAST WITHOUT A TIMESTAMP IS NOT A FORECAST.
    //
    // Everything above says what the number is and how uncertain it is.
    // Nothing said WHEN, and a price level with no bar attached is not
    // actionable and not checkable: nobody can come back later and ask
    // whether it was right, because there is no row to compare it against.
    //
    // Rule 10 wants every live decision reproducible from
    // {model_hash, feature_version, config_hash, spec_version, tick_seqno}.
    // The seqno's job on this path is done by these two stamps: the last bar
    // the model SAW, and the bar it is ABOUT.
    //
    // TWO STAMPS, NOT ONE, AND THE DIFFERENCE IS THE POINT. `last_ts_ns` is
    // an observation and `for_ts_ns` is a claim. Collapsing them into "the
    // forecast time" is how a number computed at 15:29 gets read as a number
    // about 15:29, which is the direction that looks like an edge.
    //
    // Zero means the caller supplied no stamps. NOT epoch: a forecast for
    // 1970-01-01T00:00:00Z is a real timestamp and a wrong one, and it would
    // render as a date rather than as an absence.
    std::int64_t last_ts_ns = 0;   ///< the last bar the model was given
    std::int64_t for_ts_ns = 0;    ///< the bar this forecast is about
    /// Bar width in ns, carried so a reader can check for_ts - last_ts is one
    /// bar of the series it was actually fitted on rather than of whatever
    /// the page assumed.
    std::int64_t interval_ns = 0;

    [[nodiscard]] bool stamped() const noexcept {
        return last_ts_ns != 0 && for_ts_ns != 0;
    }
    /// True when the band straddles the last price -- i.e. the forecast does
    /// not say which side of today the next bar lands on. On this series it
    /// always does, and a caller that hides this is publishing a direction it
    /// does not have.
    [[nodiscard]] bool band_straddles_last() const noexcept {
        return lo <= last_price && last_price <= hi;
    }
};

/// Fit on ALL available history and predict the next bar.
///
/// `band_bps` must come from a walk-forward, so this takes the RMSE the
/// caller already measured rather than computing a training residual. Passing
/// the training residual would narrow the band by exactly the amount the model
/// overfits, which is the one direction it must never be wrong in.
/// `stamps` are the bar timestamps of `closes`, in nanoseconds, and may be
/// empty. When supplied they must be the SAME LENGTH as `closes` -- a
/// mismatched pair would label the forecast with somebody else's bar, and
/// there is no way for a reader to notice that on screen.
[[nodiscard]] inline std::expected<SpotForecast, SpotError>
forecast_next(const std::vector<double>& closes, const SpotSpec& spec,
              double oos_rmse_bps, bool leaf_wise,
              const std::vector<std::int64_t>& stamps = {}) {
    if (!spec.valid()) { return std::unexpected(SpotError::BadParameter); }
    if (closes.size() < 2000) {
        return std::unexpected(SpotError::TooFewSamples);
    }
    if (!(oos_rmse_bps >= 0.0)) {
        return std::unexpected(SpotError::BadParameter);
    }

    std::vector<double> r;
    r.reserve(closes.size());
    for (std::size_t i = 1; i < closes.size(); ++i) {
        r.push_back(10000.0 * std::log(closes[i] / closes[i - 1]));
    }

    // Same rows as forecast_spot builds, so the model being predicted from is
    // the model that was evaluated. A second feature construction here would
    // be a second thing to keep in step.
    Frame f;
    f.p = feature_width(spec);
    // TWO LABEL VECTORS, AND THE DISTINCTION MATTERS.
    //
    // `y` is what the model is FITTED on -- divided by trailing volatility
    // when vol_scaled_label is set, so the target is roughly stationary.
    // `y_bps` is the real forward return, and it is what everything is SCORED
    // against. Scoring in scaled units would make the RMSE incomparable
    // between runs and would make the net-bps figure fictional: "profit" would
    // be measured in units of a volatility that changes every bar.
    //
    // When the flag is off the two are identical and `scale` is all ones.
    std::vector<double> y, y_bps, scale;
    for (std::size_t i = first_row_index(spec); i + spec.horizon < r.size();
         ++i) {
        append_row(r, i, spec, f.x);
        double fwd = 0.0;
        for (std::size_t h = 0; h < spec.horizon; ++h) { fwd += r[i + h]; }
        const double sc = label_scale(r, i, spec);
        y.push_back(fwd / sc);
        y_bps.push_back(fwd);
        scale.push_back(sc);
        ++f.rows;
    }
    if (f.rows < 500) { return std::unexpected(SpotError::TooFewSamples); }

    GbdtParams pr;
    pr.trees = 120;
    pr.max_depth = leaf_wise ? 12 : 4;
    pr.learning_rate = 0.05;
    pr.seed = 0xB0057u;
    pr.leaf_wise = leaf_wise;
    pr.max_leaves = 16;

    const auto model = fit_gbdt(f, y, pr);
    if (!model) { return std::unexpected(SpotError::TooFewSamples); }

    // The feature row for the NEXT bar, built by THE SAME function that built
    // every training row. It is deliberately NOT a row from `f`: every row in
    // f has a label, which means every row in f is far enough from the end
    // that its forward window closed. The row we want is the one that has no
    // label yet -- so its label index is r.size(), one past the last return,
    // and append_row reads up to r.size()-1 exactly as it should.
    std::vector<double> last_row;
    last_row.reserve(feature_width(spec));
    append_row(r, r.size(), spec, last_row);

    SpotForecast out;
    out.fitted_on = f.rows;
    out.last_price = closes.back();
    // AND THE SCALE COMES BACK OFF HERE. The model was trained on
    // return/volatility, so its output is in those units and multiplying by
    // the volatility known at this bar is what turns it back into basis
    // points. Forgetting this would produce a forecast roughly a hundred
    // times too small and entirely plausible.
    out.move_bps = model->predict_row(last_row.data())
                   * label_scale(r, r.size(), spec);
    out.price = out.last_price * std::exp(out.move_bps / 10000.0);
    out.band_bps = oos_rmse_bps;
    out.lo = out.last_price * std::exp((out.move_bps - oos_rmse_bps) / 10000.0);
    out.hi = out.last_price * std::exp((out.move_bps + oos_rmse_bps) / 10000.0);

    // ── WHICH BAR THIS IS ABOUT ──────────────────────────────────────────
    //
    // REFUSED RATHER THAN GUESSED when the stamps do not line up with the
    // closes. A stamp vector of a different length cannot be aligned to the
    // prices by any rule that is not an assumption, and the consequence of
    // getting it wrong is a forecast labelled with somebody else's bar --
    // which nothing downstream and nobody reading the screen could detect.
    if (!stamps.empty()) {
        if (stamps.size() != closes.size()) {
            return std::unexpected(SpotError::BadParameter);
        }
        out.last_ts_ns = stamps.back();

        // THE BAR WIDTH IS THE MEDIAN GAP, NOT THE LAST GAP.
        //
        // The last gap on a daily series is routinely a weekend, and on an
        // intraday one it is routinely the overnight break -- so "last + last
        // gap" would put a Monday forecast on Tuesday and a 09:15 forecast on
        // the following afternoon. The median gap over the recent tail is the
        // bar width the series actually has.
        //
        // The tail rather than the whole series, because the sampling rate of
        // a series can change: NIFTY daily starts in 1990 and the modern part
        // is what a forecast for the next bar is about.
        std::vector<std::int64_t> gaps;
        // RULE 11: a WINDOW, not a truncation of the caller's data. Every
        // stamp is available; this deliberately looks at the recent tail
        // because the sampling rate of a series can change and the bar width
        // that matters is the current one. Nothing is dropped -- the gaps
        // outside the window are not wanted.
        const std::size_t look = stamps.size() < 200 ? stamps.size() : 200;
        for (std::size_t i = stamps.size() - look + 1; i < stamps.size(); ++i) {
            const std::int64_t g = stamps[i] - stamps[i - 1];
            if (g > 0) { gaps.push_back(g); }
        }
        if (!gaps.empty()) {
            std::sort(gaps.begin(), gaps.end());
            out.interval_ns = gaps[gaps.size() / 2];
            out.for_ts_ns =
                out.last_ts_ns
                + out.interval_ns * static_cast<std::int64_t>(spec.horizon);
        }
    }
    return out;
}

[[nodiscard]] inline std::expected<SpotReport, SpotError>
forecast_spot(const std::vector<double>& closes, const SpotSpec& spec) {
    if (!spec.valid()) { return std::unexpected(SpotError::BadParameter); }
    if (closes.size() < 2000) {
        return std::unexpected(SpotError::TooFewSamples);
    }
    std::vector<double> r;
    r.reserve(closes.size());
    for (std::size_t i = 1; i < closes.size(); ++i) {
        r.push_back(10000.0 * std::log(closes[i] / closes[i - 1]));  // bps
    }

    // Rows: lags features, label is the SUM of the next `horizon` returns.
    Frame f;
    f.p = feature_width(spec);
    // TWO LABEL VECTORS, AND THE DISTINCTION MATTERS.
    //
    // `y` is what the model is FITTED on -- divided by trailing volatility
    // when vol_scaled_label is set, so the target is roughly stationary.
    // `y_bps` is the real forward return, and it is what everything is SCORED
    // against. Scoring in scaled units would make the RMSE incomparable
    // between runs and would make the net-bps figure fictional: "profit" would
    // be measured in units of a volatility that changes every bar.
    //
    // When the flag is off the two are identical and `scale` is all ones.
    std::vector<double> y, y_bps, scale;
    for (std::size_t i = first_row_index(spec); i + spec.horizon < r.size();
         ++i) {
        append_row(r, i, spec, f.x);
        double fwd = 0.0;
        for (std::size_t h = 0; h < spec.horizon; ++h) { fwd += r[i + h]; }
        const double sc = label_scale(r, i, spec);
        y.push_back(fwd / sc);
        y_bps.push_back(fwd);
        scale.push_back(sc);
        ++f.rows;
    }
    if (f.rows < 500) { return std::unexpected(SpotError::TooFewSamples); }

    SpotReport out;
    out.rows = f.rows;

    // ---- 1. THE COST HURDLE, BEFORE ANY MODEL --------------------------
    std::size_t over = 0;
    double oracle = 0.0, oracle_sel = 0.0;
    for (const double v : y) {
        const double mag = std::fabs(v);
        if (mag > spec.cost_bps) {
            ++over;
            oracle_sel += mag - spec.cost_bps;
        }
        oracle += mag - spec.cost_bps;
    }
    const double n_all = static_cast<double>(y.size());
    out.frac_exceeding_cost = static_cast<double>(over) / n_all;
    out.oracle_net_bps = oracle / n_all;
    out.oracle_selective_net_bps = oracle_sel / n_all;

    // ---- 2. WALK-FORWARD, WITH A GAP -----------------------------------
    const std::size_t block = f.rows / (spec.folds + 1);
    if (block < 200) { return std::unexpected(SpotError::TooFewSamples); }

    double sse_c = 0.0, sse_l = 0.0, sse_f = 0.0;
    std::size_t hit_c = 0, hit_l = 0, hit_f = 0, scored = 0;
    double net_l = 0.0, net_f = 0.0;

    for (std::size_t fold = 1; fold <= spec.folds; ++fold) {
        const std::size_t tr_end = block * fold;
        // THE GAP. Without it the last training label overlaps the first test
        // window and the model has seen part of its own answer.
        const std::size_t te_start = tr_end + spec.horizon;
        const std::size_t te_end = std::min(te_start + block, f.rows);
        if (te_start >= te_end || tr_end < 300) { continue; }

        Frame tr;
        tr.p = f.p;
        tr.rows = tr_end;
        tr.x.assign(f.x.begin(),
                    f.x.begin() + static_cast<long>(tr_end * f.p));
        const std::vector<double> ytr(y.begin(),
                                      y.begin() + static_cast<long>(tr_end));

        // THE CONSTANT BENCHMARK IS IN BPS, not in scaled units. It is
        // supposed to be "predict the average move and never change your
        // mind"; a constant in scaled space un-scales to a prediction that
        // moves with volatility, which is a different and better forecaster
        // and would quietly raise the bar the model is measured against.
        double mean = 0.0;
        for (std::size_t k = 0; k < tr_end; ++k) { mean += y_bps[k]; }
        mean /= static_cast<double>(tr_end);
        std::size_t up = 0;
        for (std::size_t k = 0; k < tr_end; ++k) {
            if (y_bps[k] > 0.0) { ++up; }
        }
        const bool const_up = up * 2 > tr_end;

        GbdtParams lvl;
        lvl.trees = 120;
        lvl.max_depth = 4;
        lvl.learning_rate = 0.05;
        lvl.seed = 0xB0057u + fold;

        GbdtParams leaf = lvl;
        leaf.leaf_wise = true;
        leaf.max_leaves = 16;
        leaf.max_depth = 12;          // leaf-wise needs room to be deep

        const auto m_l = fit_gbdt(tr, ytr, lvl);
        const auto m_f = fit_gbdt(tr, ytr, leaf);
        if (!m_l || !m_f) { continue; }

        for (std::size_t i = te_start; i < te_end; ++i) {
            // Back into basis points before anything is measured.
            const double sc = scale[i];
            const double truth = y_bps[i];
            const double pl = m_l->predict_row(&f.x[i * f.p]) * sc;
            const double pf = m_f->predict_row(&f.x[i * f.p]) * sc;
            sse_c += (mean - truth) * (mean - truth);
            sse_l += (pl - truth) * (pl - truth);
            sse_f += (pf - truth) * (pf - truth);
            if (const_up == (truth > 0.0)) { ++hit_c; }
            if ((pl > 0.0) == (truth > 0.0)) { ++hit_l; }
            if ((pf > 0.0) == (truth > 0.0)) { ++hit_f; }
            // NET of cost, trading the sign of the forecast every bar.
            net_l += (pl > 0.0 ? truth : -truth) - spec.cost_bps;
            net_f += (pf > 0.0 ? truth : -truth) - spec.cost_bps;
            ++scored;
        }
    }
    if (scored < 100) { return std::unexpected(SpotError::TooFewSamples); }

    const double ns = static_cast<double>(scored);
    out.scored = scored;
    out.rmse_constant = std::sqrt(sse_c / ns);
    out.rmse_level = std::sqrt(sse_l / ns);
    out.rmse_leaf = std::sqrt(sse_f / ns);
    out.dir_constant = static_cast<double>(hit_c) / ns;
    out.dir_level = static_cast<double>(hit_l) / ns;
    out.dir_leaf = static_cast<double>(hit_f) / ns;
    out.net_bps_level = net_l / ns;
    out.net_bps_leaf = net_f / ns;
    return out;
}

} // namespace altair
