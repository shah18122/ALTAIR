// models/curriculum.hpp -- the forecast curriculum.
//
// Smit's brief: "start with every 3 day data, ask him to forecast, take record
// of him being right or wrong, update him and then give more data, e.g. 6
// days, like this slowly and gradually."
//
// So every model walks the same doubling schedule over trading days:
//
//     stage 0   learns days [0, 3)      forecasts days [3, 6)
//     stage 1   learns days [0, 6)      forecasts days [6, 12)
//     stage k   learns days [0, W)      forecasts days [W, 2W)
//
// and every forecast is written down next to what happened. Within a stage
// the model is FROZEN: it is told nothing about the block it is forecasting
// until the block is over, and is then refitted from scratch on everything
// seen so far. That refit is the "update him" step, and it is the only way a
// model here learns anything.
//
// WHAT "RIGHT" MEANS. A model calls the direction of the next bar (next
// day's close against today's, or the next hourly close against this one). A
// call is right when the price moved that way and wrong when it moved the
// other way. A bar that closed exactly unchanged is neither, and is counted
// on its own. A call with no direction counts as wrong, as it does in
// forecast_scorecard.hpp: rounding a refusal up to a half would flatter a
// model that never commits. A model that cannot fit yet -- three rows do not
// make a boosted tree -- ABSTAINS, and that is recorded too. An abstention is
// not a coin flip and is not scored as one.
//
// WHAT "GOOD" MEANS. On an index a coin scores 50 % and "always up" scores
// the up-rate, which on NIFTY is 52-54 %. Accuracy alone therefore flatters
// every long-biased model, so each model is also put next to:
//   - a binomial test against 50 %, with a Bonferroni correction for how many
//     models and tracks were tried (twenty models on seven tracks produce a
//     few "significant" ones by luck alone);
//   - the best constant call on the same bars (direction_vs_drift);
//   - the random walk, for the PRICE it implies (forecast_scorecard.hpp);
//   - a round-trip cost, for whether acting on the calls would have paid.
//
// NO LOOK-AHEAD, BY CONSTRUCTION RATHER THAN BY CARE.
//   - The scaler is fitted on the training rows only, using models/dataset.hpp's
//     Scaler, which refuses anything else unless told the rows are a holdout.
//   - A model sees training TARGETS only through `CurriculumDesign::target`,
//     which refuses a row outside the training window.
//   - Features, lagged outcomes (what AR, ARMA, the Markov chain and the
//     momentum rule condition on) and price levels are readable only up to
//     the row being forecast. `curriculum_check_track` proves every row's
//     outcome was known by the next row's decision time. Every refused read
//     is counted in `CurriculumRun::lookahead_refusals`, and a clean run
//     reports zero.
//   - The track-record ensembles (Champion, Hedge) weight models only by
//     stages that have already finished, never by the one being forecast.

#pragma once

#include <analytics/hmm.hpp>
#include <analytics/hurst.hpp>
#include <analytics/kalman.hpp>
#include <models/attention.hpp>
#include <models/classical.hpp>
#include <models/deep_rl.hpp>
#include <models/dataset.hpp>
#include <models/forecast_scorecard.hpp>
#include <models/gbdt.hpp>
#include <models/markov.hpp>
#include <models/mlp.hpp>
#include <models/regime_rl.hpp>
#include <models/time_series.hpp>
#include <models/trainable_cnn.hpp>
#include <models/trainable_recurrent.hpp>
#include <models/transformer.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace altair {

/// Features per row that the fixed-width networks accept. A wider track is
/// refused. 64 holds a track's own features plus every other model's forecast
/// (models/curriculum_feeds.hpp).
inline constexpr std::size_t kCurriculumMaxFeatures = 64;
/// Inputs per step, and steps back, that the sequence models read.
inline constexpr std::size_t kCurriculumSeqInputs = 4;
inline constexpr std::size_t kCurriculumSeqSteps = 16;
/// Up to this many rows the SVM is models/classical.hpp's exact SMO, which is
/// quadratic per pass with no kernel cache. Above it, on EVERY row of the
/// window, the same RBF kernel is approximated by random Fourier features and
/// the SVM trained by dual coordinate descent -- the standard large-scale route.
inline constexpr std::size_t kCurriculumSvmExactRows = 1500;
/// Random Fourier features for the large-window SVM.
inline constexpr std::size_t kCurriculumSvmFeatures = 256;

enum class CurriculumError : std::uint8_t {
    /// Ragged columns, a non-positive or non-finite price, or stamps out of order.
    BadTrack,
    /// Fewer trading days than the first stage needs to learn from and forecast.
    TooFewDays,
    /// More features, or sequence inputs, than the networks accept.
    TooManyFeatures,
    /// A row's outcome was not known by the next row's decision time.
    LookAhead
};

[[nodiscard]] inline const char* curriculum_error_text(CurriculumError e) noexcept {
    switch (e) {
    case CurriculumError::BadTrack:        return "malformed track";
    case CurriculumError::TooFewDays:      return "too few trading days";
    case CurriculumError::TooManyFeatures: return "too many features";
    case CurriculumError::LookAhead:       return "an outcome is not known by the next decision";
    }
    return "unknown";
}

/// One instrument at one horizon: its rows in time order.
///
/// Row i is a decision taken at `t[i]` from `x` (features known then). Its
/// outcome is the move from `anchor[i]` (the last price known at `t[i]`) to
/// `actual[i]` (the price at the horizon, known at `t_out[i]`).
struct CurriculumTrack {
    std::string name;         ///< "NIFTY daily"
    std::string instrument;   ///< "NIFTY"
    std::string horizon;      ///< "next day"
    std::vector<std::string> feature_names;
    std::size_t p = 0;                    ///< features per row
    std::vector<double> x;                ///< rows * p, raw (unscaled)
    std::vector<std::int64_t> t;          ///< decision time, IST epoch seconds
    std::vector<std::int64_t> t_out;      ///< when the outcome is known
    std::vector<std::int32_t> day;        ///< trading-day ordinal of the decision, from 0
    std::vector<double> anchor;
    std::vector<double> actual;
    std::vector<std::size_t> seq_cols;    ///< columns the sequence models read per step
    /// A second instrument's price at each decision, for the pairs model
    /// (NIFTY against BANKNIFTY and back). Empty when the track has none.
    std::vector<double> pair;
    std::string pair_name;
    /// Rows in one trading day (1 for a daily track): the seasonal lag.
    std::size_t season = 1;
    /// Each row's place in its cycle: bar of the day intraday, weekday on a
    /// daily track. Empty when the track has none. Known at the decision.
    std::vector<std::uint16_t> slot;
    /// Round-trip cost of acting on row i's call, bp. Per row because the
    /// charges are dated (STT on futures rose on 2026-04-01). Empty when the
    /// track is not tradable.
    std::vector<double> cost_bp;
    bool tradable = true;                 ///< false for INDIA VIX: an index nobody can buy

    [[nodiscard]] std::size_t rows() const noexcept { return t.size(); }
    [[nodiscard]] double ret(std::size_t i) const noexcept { return std::log(actual[i] / anchor[i]); }
    [[nodiscard]] std::int32_t days() const noexcept { return day.empty() ? 0 : day.back() + 1; }
};

/// Refuse a track the curriculum could not score honestly.
[[nodiscard]] inline std::expected<void, CurriculumError>
curriculum_check_track(const CurriculumTrack& tr) noexcept {
    const std::size_t n = tr.rows();
    if (tr.p == 0 || tr.x.size() != n * tr.p || tr.t_out.size() != n || tr.day.size() != n
        || tr.anchor.size() != n || tr.actual.size() != n || tr.feature_names.size() != tr.p) {
        return std::unexpected(CurriculumError::BadTrack);
    }
    if (tr.p > kCurriculumMaxFeatures || tr.seq_cols.empty()
        || tr.seq_cols.size() > kCurriculumSeqInputs) {
        return std::unexpected(CurriculumError::TooManyFeatures);
    }
    for (const std::size_t c : tr.seq_cols) {
        if (c >= tr.p) { return std::unexpected(CurriculumError::BadTrack); }
    }
    for (const double v : tr.x) {
        if (!std::isfinite(v)) { return std::unexpected(CurriculumError::BadTrack); }
    }
    if (tr.tradable && tr.cost_bp.size() != n) { return std::unexpected(CurriculumError::BadTrack); }
    if (!tr.pair.empty() && tr.pair.size() != n) { return std::unexpected(CurriculumError::BadTrack); }
    for (const double v : tr.pair) {
        if (!(v > 0.0) || !std::isfinite(v)) { return std::unexpected(CurriculumError::BadTrack); }
    }
    if (tr.season == 0 || (!tr.slot.empty() && tr.slot.size() != n)) {
        return std::unexpected(CurriculumError::BadTrack);
    }
    for (const double c : tr.cost_bp) {
        if (!(c >= 0.0) || !std::isfinite(c)) { return std::unexpected(CurriculumError::BadTrack); }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!(tr.anchor[i] > 0.0) || !(tr.actual[i] > 0.0) || !std::isfinite(tr.anchor[i])
            || !std::isfinite(tr.actual[i]) || tr.t_out[i] <= tr.t[i]) {
            return std::unexpected(CurriculumError::BadTrack);
        }
        if (i == 0) {
            if (tr.day[0] != 0) { return std::unexpected(CurriculumError::BadTrack); }
            continue;
        }
        const std::int32_t step = tr.day[i] - tr.day[i - 1];
        if (tr.t[i] <= tr.t[i - 1] || step < 0 || step > 1) {
            return std::unexpected(CurriculumError::BadTrack);
        }
        if (tr.t_out[i - 1] > tr.t[i]) { return std::unexpected(CurriculumError::LookAhead); }
    }
    return {};
}

// ---------------------------------------------------------------------------
// The schedule
// ---------------------------------------------------------------------------

struct CurriculumStage {
    std::size_t index = 0;
    std::int32_t train_days = 0;   ///< days [0, train_days) are learned from
    std::int32_t test_days = 0;    ///< the next test_days days are forecast
    std::size_t train_rows = 0;    ///< rows [0, train_rows)
    std::size_t test_begin = 0;    ///< rows [test_begin, test_end)
    std::size_t test_end = 0;
};

/// Doubling windows: learn `first_days`, forecast as many again, and so on
/// until the data runs out; the last block is whatever is left.
///
/// `step_cap_days` > 0 stops the doubling once a block would exceed that many
/// days and walks on in blocks of that size (a yearly refit, say). Zero is
/// pure doubling, which is what was asked for.
[[nodiscard]] inline std::vector<CurriculumStage>
curriculum_stages(const CurriculumTrack& tr, std::int32_t first_days = 3,
                  std::int32_t step_cap_days = 0) {
    std::vector<CurriculumStage> out;
    const std::int32_t total = tr.days();
    if (first_days < 1 || total <= first_days) { return out; }
    const auto first_row_of = [&tr](std::int32_t d) {
        return static_cast<std::size_t>(
            std::lower_bound(tr.day.begin(), tr.day.end(), d) - tr.day.begin());
    };
    std::int32_t w = first_days;
    while (w < total) {
        std::int32_t step = w;
        if (step_cap_days > 0 && step > step_cap_days) { step = step_cap_days; }
        std::int32_t next = w + step;
        if (next > total) { next = total; }
        CurriculumStage s;
        s.index = out.size();
        s.train_days = w;
        s.test_days = next - w;
        s.train_rows = first_row_of(w);
        s.test_begin = s.train_rows;
        s.test_end = first_row_of(next);
        out.push_back(s);
        w = next;
    }
    return out;
}

// ---------------------------------------------------------------------------
// What a model sees in one stage
// ---------------------------------------------------------------------------

/// The standardised design for one stage, with the look-ahead guards.
///
/// `now` is the row whose decision is being taken. While a model fits, `now`
/// is the first row after the training window: the model may read any
/// feature or lagged outcome known then, and only training targets.
class CurriculumDesign {
public:
    [[nodiscard]] static std::expected<CurriculumDesign, DatasetError>
    make(const CurriculumTrack& tr, const CurriculumStage& st) {
        if (st.test_end <= st.train_rows) { return std::unexpected(DatasetError::TooFewRows); }
        return build(tr, st.train_rows, st.test_end);
    }

    /// Every row before `train_rows` is training and nothing is forecast yet:
    /// a model fitted here before the session, then asked at the decision on
    /// make()'s design with the same training rows plus today's, gives the
    /// same call as one fitted at the decision -- the scaler sees the same
    /// rows -- without fitting inside the market-data path.
    [[nodiscard]] static std::expected<CurriculumDesign, DatasetError>
    make_fit_only(const CurriculumTrack& tr, std::size_t train_rows) {
        return build(tr, train_rows, train_rows);
    }

private:
    [[nodiscard]] static std::expected<CurriculumDesign, DatasetError>
    build(const CurriculumTrack& tr, std::size_t train_rows, std::size_t rows) {
        CurriculumDesign d;
        d.tr_ = &tr;
        d.p_ = tr.p;
        d.train_ = train_rows;
        d.rows_ = rows;
        d.now_ = train_rows;
        if (d.train_ < 2 || d.rows_ < d.train_ || d.rows_ > tr.rows()) { return std::unexpected(DatasetError::TooFewRows); }
        d.z_.assign(tr.x.begin(), tr.x.begin() + static_cast<std::ptrdiff_t>(d.rows_ * d.p_));
        Matrix m{d.z_.data(), d.rows_, d.p_};
        Scaler scaler;
        if (auto ok = scaler.fit(m, 0, d.train_); !ok) { return std::unexpected(ok.error()); }
        if (auto ok = scaler.transform(m, 0, d.train_, false); !ok) { return std::unexpected(ok.error()); }
        if (d.rows_ > d.train_) {
            if (auto ok = scaler.transform(m, d.train_, d.rows_, true); !ok) { return std::unexpected(ok.error()); }
        }
        // A column constant over the training rows taught nothing and was left
        // unscaled; it is zeroed everywhere so its raw test values cannot leak
        // in as a large, unscaled input.
        d.degenerate_ = scaler.degenerate_features();
        for (std::size_t j = 0; j < d.p_ && d.degenerate_ > 0; ++j) {
            if (scaler.sd(j) > 0.0) { continue; }
            for (std::size_t i = 0; i < d.rows_; ++i) { d.z_[i * d.p_ + j] = 0.0; }
        }
        d.ret_.resize(d.rows_);
        for (std::size_t i = 0; i < d.rows_; ++i) { d.ret_[i] = tr.ret(i); }

        std::size_t ups = 0, downs = 0;
        double sum_up = 0.0, sum_down = 0.0, s = 0.0, ss = 0.0;
        for (std::size_t i = 0; i < d.train_; ++i) {
            const double r = d.ret_[i];
            s += r;
            ss += r * r;
            if (r > 0.0) { ++ups; sum_up += r; }
            else if (r < 0.0) { ++downs; sum_down += r; }
        }
        const double n = static_cast<double>(d.train_);
        d.ups_ = ups;
        d.downs_ = downs;
        d.mean_ = s / n;
        d.sd_ = std::sqrt(std::max(0.0, (ss - n * d.mean_ * d.mean_) / (n - 1.0)));
        d.mean_up_ = ups > 0 ? sum_up / static_cast<double>(ups) : 0.0;
        d.mean_down_ = downs > 0 ? sum_down / static_cast<double>(downs) : 0.0;
        return d;
    }

public:
    [[nodiscard]] const CurriculumTrack& track() const noexcept { return *tr_; }
    [[nodiscard]] std::size_t p() const noexcept { return p_; }
    [[nodiscard]] std::size_t train_rows() const noexcept { return train_; }
    [[nodiscard]] std::size_t rows() const noexcept { return rows_; }
    [[nodiscard]] std::size_t now() const noexcept { return now_; }
    [[nodiscard]] std::size_t degenerate_features() const noexcept { return degenerate_; }
    [[nodiscard]] std::size_t refusals() const noexcept { return refusals_; }

    /// The engine moves the clock. Models only ever get a const design.
    void set_now(std::size_t i) noexcept { now_ = i; }

    /// Standardised features of row i, if known at `now`; empty otherwise.
    [[nodiscard]] std::span<const double> features(std::size_t i) const noexcept {
        if (i > now_ || i >= rows_) { ++refusals_; return {}; }
        return {z_.data() + i * p_, p_};
    }

    /// The training target: log return of row j. NaN outside the training window.
    [[nodiscard]] double target(std::size_t j) const noexcept {
        if (j >= train_) { ++refusals_; return std::numeric_limits<double>::quiet_NaN(); }
        return ret_[j];
    }
    [[nodiscard]] bool up(std::size_t j) const noexcept { return target(j) > 0.0; }

    /// Realised returns of rows [0, i), all known at row i's decision.
    [[nodiscard]] std::span<const double> history(std::size_t i) const noexcept {
        if (i > now_ || i > rows_) { ++refusals_; return {}; }
        return {ret_.data(), i};
    }

    /// log(anchor) of row i: the price known at its decision.
    [[nodiscard]] double level(std::size_t i) const noexcept {
        if (i > now_ || i >= rows_) { ++refusals_; return std::numeric_limits<double>::quiet_NaN(); }
        return std::log(tr_->anchor[i]);
    }

    /// log of the paired instrument's price at row i; NaN if none or not known yet.
    [[nodiscard]] double pair_level(std::size_t i) const noexcept {
        if (i > now_ || i >= rows_) { ++refusals_; return std::numeric_limits<double>::quiet_NaN(); }
        return tr_->pair.empty() ? std::numeric_limits<double>::quiet_NaN() : std::log(tr_->pair[i]);
    }

    /// The last kCurriculumSeqSteps steps ending at row i, step-major, each
    /// step kCurriculumSeqInputs wide (unused inputs zero). Returns the step count.
    std::size_t sequence(std::size_t i, double* out) const noexcept {
        if (i > now_ || i >= rows_) { ++refusals_; return 0; }
        const std::size_t steps = i + 1 < kCurriculumSeqSteps ? i + 1 : kCurriculumSeqSteps;
        const std::size_t first = i + 1 - steps;
        const auto& cols = tr_->seq_cols;
        for (std::size_t s = 0; s < steps; ++s) {
            const double* row = z_.data() + (first + s) * p_;
            for (std::size_t c = 0; c < kCurriculumSeqInputs; ++c) {
                out[s * kCurriculumSeqInputs + c] = c < cols.size() ? row[cols[c]] : 0.0;
            }
        }
        return steps;
    }

    // Training-window statistics.
    [[nodiscard]] std::size_t train_ups() const noexcept { return ups_; }
    [[nodiscard]] std::size_t train_downs() const noexcept { return downs_; }
    /// Laplace-smoothed P(up) over the training window.
    [[nodiscard]] double up_rate() const noexcept {
        return (static_cast<double>(ups_) + 1.0) / (static_cast<double>(ups_ + downs_) + 2.0);
    }
    [[nodiscard]] double mean() const noexcept { return mean_; }
    [[nodiscard]] double sd() const noexcept { return sd_; }
    [[nodiscard]] double mean_up() const noexcept { return mean_up_; }
    [[nodiscard]] double mean_down() const noexcept { return mean_down_; }

    /// The design a hyper-parameter search runs on: the same rows [0, train),
    /// standardised with the mean and sd of rows [0, v) ONLY. A candidate fits
    /// on [0, v) and is scored on [v, train); scaled with statistics that
    /// included [v, train), the score would have seen the rows it is judged on.
    /// Its targets stop at v -- the validation targets come from this (outer)
    /// design -- and every feature of [0, train) is readable.
    ///
    /// Built once per design and shared by every model that tunes on it. Not
    /// thread-safe: a design belongs to one run. Null when [0, v) is too short.
    [[nodiscard]] const CurriculumDesign* inner(std::size_t v) const {
        if (inner_ && inner_v_ == v) { return inner_.get(); }
        CurriculumStage st;
        st.train_rows = v;
        st.test_begin = v;
        st.test_end = train_;
        auto made = make(*tr_, st);
        if (!made) { return nullptr; }
        made->set_now(train_ - 1);
        inner_ = std::make_shared<const CurriculumDesign>(std::move(*made));
        inner_v_ = v;
        return inner_.get();
    }

    /// Training rows [from, to) as one contiguous row-major block.
    [[nodiscard]] std::vector<double> block(std::size_t from, std::size_t to) const {
        std::vector<double> out;
        if (to > train_ || from >= to) { ++refusals_; return out; }
        out.assign(z_.begin() + static_cast<std::ptrdiff_t>(from * p_),
                   z_.begin() + static_cast<std::ptrdiff_t>(to * p_));
        return out;
    }

private:
    const CurriculumTrack* tr_ = nullptr;
    std::vector<double> z_;
    std::vector<double> ret_;
    std::size_t p_ = 0, train_ = 0, rows_ = 0, now_ = 0, degenerate_ = 0;
    std::size_t ups_ = 0, downs_ = 0;
    double mean_ = 0.0, sd_ = 0.0, mean_up_ = 0.0, mean_down_ = 0.0;
    mutable std::size_t refusals_ = 0;
    mutable std::shared_ptr<const CurriculumDesign> inner_;   ///< the tuning fold, once built
    mutable std::size_t inner_v_ = 0;
};

// ---------------------------------------------------------------------------
// The model contract
// ---------------------------------------------------------------------------

/// One forecast. A model fills what it has; the engine derives the rest.
struct CurriculumCall {
    bool made = false;   ///< false: the model abstained
    int dir = 0;         ///< +1 up, -1 down, 0 no direction (scored as wrong)
    /// P(up), for models that publish one. NaN for rules and margins.
    double p_up = std::numeric_limits<double>::quiet_NaN();
    /// Expected log return to the horizon. Classifiers get the plug-in
    /// p * E[r | up] + (1 - p) * E[r | down] from their training window.
    double mu = std::numeric_limits<double>::quiet_NaN();
    /// Error scale of `mu`, for the band. The training sd when the model has none.
    double sigma = std::numeric_limits<double>::quiet_NaN();
};

/// A call as the run stores it: 16 bytes instead of 40, because a 1-minute
/// track is a million rows times thirty-odd models. Float keeps seven
/// significant digits -- far below the noise in any of these numbers.
struct CurriculumStoredCall {
    float p_up = std::numeric_limits<float>::quiet_NaN();
    float mu = std::numeric_limits<float>::quiet_NaN();
    float sigma = std::numeric_limits<float>::quiet_NaN();
    std::int8_t dir = 0;
    bool made = false;

    CurriculumStoredCall() = default;
    CurriculumStoredCall(const CurriculumCall& c) noexcept   // NOLINT: implicit on purpose
        : p_up(static_cast<float>(c.p_up)), mu(static_cast<float>(c.mu)),
          sigma(static_cast<float>(c.sigma)), dir(static_cast<std::int8_t>(c.dir)), made(c.made) {}
    operator CurriculumCall() const noexcept {   // NOLINT: implicit on purpose
        CurriculumCall c;
        c.made = made;
        c.dir = dir;
        c.p_up = static_cast<double>(p_up);
        c.mu = static_cast<double>(mu);
        c.sigma = static_cast<double>(sigma);
        return c;
    }
};

class CurriculumModel {
public:
    virtual ~CurriculumModel() = default;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual std::string family() const = 0;
    /// Fit on the design's training rows. Empty when fitted; otherwise the
    /// reason the model abstains for this stage.
    [[nodiscard]] virtual std::string fit(const CurriculumDesign& d) = 0;
    [[nodiscard]] virtual CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const = 0;
    /// What this stage's fit chose, if anything ("k=15").
    [[nodiscard]] virtual std::string tuned() const { return {}; }
};

namespace curriculum_detail {

inline double nan() noexcept { return std::numeric_limits<double>::quiet_NaN(); }

inline double normal_cdf(double z) noexcept { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }

inline std::uint64_t mix(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/// An exact 0.5 is broken by the sign of the expected return. Without that,
/// an even vote or a regime whose training up-rate was exactly half would
/// hand in "no direction" -- scored as wrong -- for a whole block of rows.
inline CurriculumCall from_probability(double p, double mu = nan()) noexcept {
    CurriculumCall c;
    if (!std::isfinite(p)) { return c; }
    c.made = true;
    c.p_up = p;
    c.mu = mu;
    c.dir = p > 0.5 ? 1 : (p < 0.5 ? -1 : 0);
    if (c.dir == 0 && std::isfinite(mu)) { c.dir = mu > 0.0 ? 1 : (mu < 0.0 ? -1 : 0); }
    return c;
}

inline CurriculumCall from_return(double mu, double sigma) noexcept {
    CurriculumCall c;
    if (!std::isfinite(mu)) { return c; }
    c.made = true;
    c.mu = mu;
    c.sigma = sigma;
    c.dir = mu > 0.0 ? 1 : (mu < 0.0 ? -1 : 0);
    if (sigma > 0.0 && std::isfinite(sigma)) { c.p_up = normal_cdf(mu / sigma); }
    return c;
}

inline CurriculumCall from_direction(int dir) noexcept {
    CurriculumCall c;
    c.made = true;
    c.dir = dir;
    return c;
}

/// The last `holdout` share of the training window, for choosing a
/// hyper-parameter on data the candidate fit has not seen.
inline std::size_t inner_split(std::size_t n) noexcept { return n - n / 4; }

inline double sign_accuracy(const std::vector<double>& p, const CurriculumDesign& d, std::size_t from) {
    std::size_t right = 0, scored = 0;
    for (std::size_t k = 0; k < p.size(); ++k) {
        const double r = d.target(from + k);
        if (r == 0.0 || !std::isfinite(p[k])) { continue; }
        ++scored;
        if ((p[k] > 0.5) == (r > 0.0) && p[k] != 0.5) { ++right; }
    }
    return scored > 0 ? static_cast<double>(right) / static_cast<double>(scored) : 0.0;
}

inline std::string trim_double(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

} // namespace curriculum_detail

// ---------------------------------------------------------------------------
// Baselines
// ---------------------------------------------------------------------------

/// A fair coin, seeded per row: what luck looks like on the same bars.
class CurriculumCoin final : public CurriculumModel {
public:
    explicit CurriculumCoin(std::uint64_t seed) noexcept : seed_(seed) {}
    std::string name() const override { return "Coin flip"; }
    std::string family() const override { return "baseline"; }
    std::string fit(const CurriculumDesign&) override { return {}; }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const std::uint64_t h = curriculum_detail::mix(seed_ ^ (static_cast<std::uint64_t>(d.track().t[i])));
        return curriculum_detail::from_direction((h & 1u) != 0 ? 1 : -1);
    }

private:
    std::uint64_t seed_;
};

/// Always call the training window's majority direction.
class CurriculumMajority final : public CurriculumModel {
public:
    std::string name() const override { return "Always majority"; }
    std::string family() const override { return "baseline"; }
    std::string fit(const CurriculumDesign& d) override {
        p_ = d.up_rate();
        mu_ = d.mean();
        return {};
    }
    CurriculumCall predict(const CurriculumDesign&, std::size_t) const override {
        return curriculum_detail::from_probability(p_, mu_);
    }

private:
    double p_ = 0.5, mu_ = 0.0;
};

/// Call the last move again (momentum), or its reverse (mean reversion).
class CurriculumPersistence final : public CurriculumModel {
public:
    explicit CurriculumPersistence(bool reverse) noexcept : reverse_(reverse) {}
    std::string name() const override { return reverse_ ? "Mean reversion" : "Momentum"; }
    std::string family() const override { return "baseline"; }
    std::string fit(const CurriculumDesign&) override { return {}; }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const auto h = d.history(i);
        if (h.empty()) { return {}; }
        const double last = h.back();
        const int s = last > 0.0 ? 1 : (last < 0.0 ? -1 : 0);
        return curriculum_detail::from_direction(reverse_ ? -s : s);
    }

private:
    bool reverse_;
};

// ---------------------------------------------------------------------------
// Classical models (models/classical.hpp), tuned on the window's last quarter
// ---------------------------------------------------------------------------

class CurriculumLogistic final : public CurriculumModel {
public:
    std::string name() const override { return "Logistic regression"; }
    std::string family() const override { return "classical"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t n = d.train_rows();
        if (n < 8) { return "fewer than 8 rows"; }
        const double grid[] = {1e-3, 1e-1, 1.0};
        l2_ = grid[0];
        const std::size_t v = curriculum_detail::inner_split(n);
        if (const CurriculumDesign* di = n >= 40 ? d.inner(v) : nullptr) {
            double best = -1.0;
            for (const double l2 : grid) {
                const auto m = fit_range(*di, 0, v, l2);
                if (!m) { continue; }
                std::vector<double> p;
                for (std::size_t j = v; j < n; ++j) { p.push_back(m->probability(di->features(j))); }
                const double acc = curriculum_detail::sign_accuracy(p, d, v);
                if (acc > best) { best = acc; l2_ = l2; }
            }
        }
        auto m = fit_range(d, 0, n, l2_);
        if (!m) { return "logistic fit refused"; }
        model_ = std::move(*m);
        mu_up_ = d.mean_up();
        mu_down_ = d.mean_down();
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const double p = model_.probability(d.features(i));
        return curriculum_detail::from_probability(p, p * mu_up_ + (1.0 - p) * mu_down_);
    }
    std::string tuned() const override { return "l2=" + curriculum_detail::trim_double(l2_); }

private:
    static std::expected<LogisticRegression, ClassicalError>
    fit_range(const CurriculumDesign& d, std::size_t from, std::size_t to, double l2) {
        const auto x = d.block(from, to);
        std::vector<std::uint8_t> y;
        for (std::size_t j = from; j < to; ++j) { y.push_back(d.up(j) ? 1 : 0); }
        LogisticParams prm;
        prm.l2 = l2;
        return LogisticRegression::fit(x, to - from, d.p(), y, prm);
    }
    LogisticRegression model_{};
    double l2_ = 1e-3, mu_up_ = 0.0, mu_down_ = 0.0;
};

class CurriculumSvm final : public CurriculumModel {
public:
    explicit CurriculumSvm(std::uint64_t seed = 0x5F3u) noexcept : seed_(seed) {}
    std::string name() const override { return "SVM (RBF)"; }
    std::string family() const override { return "classical"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t n = d.train_rows();
        if (n < 8) { return "fewer than 8 rows"; }
        const double gamma = 1.0 / static_cast<double>(d.p());
        rows_ = n;
        if (n <= kCurriculumSvmExactRows) {
            const auto x = d.block(0, n);
            std::vector<std::int8_t> y;
            for (std::size_t j = 0; j < n; ++j) { y.push_back(d.up(j) ? 1 : -1); }
            SvmParams prm;
            prm.kernel = SvmKernel::Rbf;
            prm.gamma = gamma;
            prm.c = 1.0;
            prm.max_passes = 3;
            auto m = SupportVectorMachine::fit(x, n, d.p(), y, prm);
            if (!m) { return "SVM fit refused"; }
            model_ = std::move(*m);
            exact_ = true;
            return {};
        }
        exact_ = false;
        draw_features(d.p(), gamma);
        // Dual coordinate descent for the hinge-loss SVM on the features
        // (Hsieh et al. 2008), C = 1 as for the exact fit, every row, seeded order.
        constexpr double C = 1.0;
        const std::size_t D = kCurriculumSvmFeatures;
        w_.assign(D + 1, 0.0);
        std::vector<double> alpha(n, 0.0), z(D + 1);
        std::vector<std::size_t> order(n);
        for (std::size_t j = 0; j < n; ++j) { order[j] = j; }
        std::uint64_t s = seed_;
        passes_ = 0;
        for (int pass = 0; pass < 20; ++pass) {
            for (std::size_t j = n; j > 1; --j) {
                s = curriculum_detail::mix(s);
                std::swap(order[j - 1], order[s % j]);
            }
            double worst = 0.0;
            for (const std::size_t j : order) {
                embed(d.features(j), z.data());
                const double y = d.up(j) ? 1.0 : -1.0;
                double wz = 0.0, qq = 0.0;
                for (std::size_t k = 0; k <= D; ++k) { wz += w_[k] * z[k]; qq += z[k] * z[k]; }
                const double g = y * wz - 1.0;
                const double pg = alpha[j] == 0.0 ? std::min(g, 0.0) : (alpha[j] == C ? std::max(g, 0.0) : g);
                worst = std::max(worst, std::fabs(pg));
                if (pg == 0.0 || !(qq > 0.0)) { continue; }
                const double a = std::min(std::max(alpha[j] - g / qq, 0.0), C);
                const double step = (a - alpha[j]) * y;
                alpha[j] = a;
                for (std::size_t k = 0; k <= D; ++k) { w_[k] += step * z[k]; }
            }
            ++passes_;
            if (worst < 1e-3) { break; }
        }
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        double m = 0.0;
        if (exact_) {
            m = model_.margin(d.features(i));
        } else {
            std::vector<double> z(kCurriculumSvmFeatures + 1);
            embed(d.features(i), z.data());
            for (std::size_t k = 0; k < z.size(); ++k) { m += w_[k] * z[k]; }
        }
        if (!std::isfinite(m)) { return {}; }
        return curriculum_detail::from_direction(m > 0.0 ? 1 : (m < 0.0 ? -1 : 0));
    }
    std::string tuned() const override {
        return exact_ ? std::string{}
                      : std::to_string(kCurriculumSvmFeatures) + " random Fourier features, all "
                            + std::to_string(rows_) + " rows, " + std::to_string(passes_) + " passes";
    }

private:
    /// omega ~ N(0, 2 gamma I), b ~ U(0, 2 pi): E[z(x).z(y)] = exp(-gamma |x - y|^2),
    /// the kernel the exact fit uses. The last coordinate is the bias.
    void draw_features(std::size_t p, double gamma) {
        const std::size_t D = kCurriculumSvmFeatures;
        p_ = p;
        omega_.assign(D * p, 0.0);
        phase_.assign(D, 0.0);
        std::uint64_t s = seed_ ^ 0x9E3779B97F4A7C15ull;
        const auto u = [&s]() {
            s = curriculum_detail::mix(s);
            return (static_cast<double>(s >> 11) + 0.5) * (1.0 / 9007199254740992.0);
        };
        const double sd = std::sqrt(2.0 * gamma), two_pi = 6.283185307179586;
        for (double& w : omega_) { w = sd * std::sqrt(-2.0 * std::log(u())) * std::cos(two_pi * u()); }
        for (double& b : phase_) { b = two_pi * u(); }
    }
    void embed(std::span<const double> x, double* z) const {
        const std::size_t D = kCurriculumSvmFeatures;
        const double scale = std::sqrt(2.0 / static_cast<double>(D));
        for (std::size_t k = 0; k < D; ++k) {
            double a = phase_[k];
            for (std::size_t j = 0; j < p_; ++j) { a += omega_[k * p_ + j] * x[j]; }
            z[k] = scale * std::cos(a);
        }
        z[D] = 1.0;
    }
    std::uint64_t seed_;
    SupportVectorMachine model_{};
    bool exact_ = true;
    std::size_t rows_ = 0, p_ = 0;
    int passes_ = 0;
    std::vector<double> omega_, phase_, w_;
};

/// Exact k-nearest neighbours on every row of the window. Same rule as
/// models/classical.hpp's KnnModel (features standardised on the training
/// rows, Euclidean distance, ties to the earlier row, the share of the k
/// nearest that went up), searched through a k-d tree: a brute-force scan per
/// forecast is 10 ms at 1-minute scale, and there are a million forecasts.
class CurriculumKnnIndex {
public:
    void build(const CurriculumDesign& d, std::size_t from, std::size_t to) {
        p_ = d.p();
        n_ = to - from;
        mean_.assign(p_, 0.0);
        scale_.assign(p_, 0.0);
        const auto raw = d.block(from, to);
        for (std::size_t i = 0; i < n_; ++i) {
            for (std::size_t j = 0; j < p_; ++j) { mean_[j] += raw[i * p_ + j]; }
        }
        for (double& m : mean_) { m /= static_cast<double>(n_); }
        for (std::size_t i = 0; i < n_; ++i) {
            for (std::size_t j = 0; j < p_; ++j) {
                const double q = raw[i * p_ + j] - mean_[j];
                scale_[j] += q * q;
            }
        }
        for (double& v : scale_) {
            v = n_ > 1 ? std::sqrt(v / static_cast<double>(n_ - 1)) : 0.0;
            if (!(v > 1e-12)) { v = 1.0; }   // a constant feature contributes zero, as in KnnModel
        }
        std::vector<double> x(n_ * p_);
        for (std::size_t i = 0; i < n_; ++i) {
            for (std::size_t j = 0; j < p_; ++j) { x[i * p_ + j] = (raw[i * p_ + j] - mean_[j]) / scale_[j]; }
        }
        up_.resize(n_);
        for (std::size_t i = 0; i < n_; ++i) { up_[i] = d.up(from + i) ? 1 : 0; }
        order_.resize(n_);
        for (std::size_t i = 0; i < n_; ++i) { order_[i] = static_cast<std::uint32_t>(i); }
        nodes_.clear();
        (void)split(x, 0, static_cast<std::uint32_t>(n_));
        xs_.resize(n_ * p_);   // leaves stored contiguously, in tree order
        for (std::size_t i = 0; i < n_; ++i) {
            for (std::size_t j = 0; j < p_; ++j) { xs_[i * p_ + j] = x[static_cast<std::size_t>(order_[i]) * p_ + j]; }
        }
    }

    /// The k nearest training rows' up flags, nearest first; false if the query is not finite.
    bool nearest(std::span<const double> features, std::size_t k, std::vector<std::uint8_t>& out) const {
        if (features.size() != p_ || k == 0 || n_ == 0) { return false; }
        std::vector<double> q(p_), off(p_, 0.0);
        for (std::size_t j = 0; j < p_; ++j) {
            if (!std::isfinite(features[j])) { return false; }
            q[j] = (features[j] - mean_[j]) / scale_[j];
        }
        Heap h;
        h.k = std::min(k, n_);
        search(0, q.data(), off.data(), 0.0, h);
        std::sort(h.items.begin(), h.items.end());
        out.clear();
        for (const auto& [d2, row] : h.items) { out.push_back(up_[row]); }
        return true;
    }

private:
    struct Node {
        std::uint32_t lo, hi;
        std::int32_t dim = -1, left = -1, right = -1;
        double cut = 0.0;
    };
    struct Heap {
        std::size_t k = 0;
        std::vector<std::pair<double, std::uint32_t>> items;   // max-heap on (distance, row)
        double worst() const { return items.front().first; }
    };
    static constexpr std::uint32_t kLeaf = 16;

    std::int32_t split(const std::vector<double>& x, std::uint32_t lo, std::uint32_t hi) {
        const auto me = static_cast<std::int32_t>(nodes_.size());
        nodes_.push_back(Node{lo, hi});
        if (hi - lo <= kLeaf) { return me; }
        std::size_t dim = 0;
        double spread = 0.0;
        for (std::size_t j = 0; j < p_; ++j) {
            double a = std::numeric_limits<double>::infinity(), b = -a;
            for (std::uint32_t i = lo; i < hi; ++i) {
                const double v = x[static_cast<std::size_t>(order_[i]) * p_ + j];
                a = std::min(a, v);
                b = std::max(b, v);
            }
            if (b - a > spread) { spread = b - a; dim = j; }
        }
        if (!(spread > 0.0)) { return me; }   // identical points: one leaf
        const std::uint32_t mid = lo + (hi - lo) / 2;
        std::nth_element(order_.begin() + lo, order_.begin() + mid, order_.begin() + hi,
                         [&](std::uint32_t a, std::uint32_t b) {
                             return x[static_cast<std::size_t>(a) * p_ + dim] < x[static_cast<std::size_t>(b) * p_ + dim];
                         });
        const double cut = x[static_cast<std::size_t>(order_[mid]) * p_ + dim];
        const auto l = split(x, lo, mid);
        const auto r = split(x, mid, hi);
        nodes_[static_cast<std::size_t>(me)].dim = static_cast<std::int32_t>(dim);
        nodes_[static_cast<std::size_t>(me)].cut = cut;
        nodes_[static_cast<std::size_t>(me)].left = l;
        nodes_[static_cast<std::size_t>(me)].right = r;
        return me;
    }

    /// Four independent sums: one serial chain of p additions is the bottleneck.
    double distance(const double* q, const double* r) const noexcept {
        double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
        std::size_t j = 0;
        for (; j + 4 <= p_; j += 4) {
            const double a = q[j] - r[j], b = q[j + 1] - r[j + 1], c = q[j + 2] - r[j + 2], e = q[j + 3] - r[j + 3];
            s0 += a * a;
            s1 += b * b;
            s2 += c * c;
            s3 += e * e;
        }
        for (; j < p_; ++j) {
            const double a = q[j] - r[j];
            s0 += a * a;
        }
        return (s0 + s1) + (s2 + s3);
    }

    void search(std::int32_t id, const double* q, double* off, double bound, Heap& h) const {
        const Node& nd = nodes_[static_cast<std::size_t>(id)];
        if (nd.dim < 0) {
            for (std::uint32_t i = nd.lo; i < nd.hi; ++i) {
                const double d2 = distance(q, xs_.data() + static_cast<std::size_t>(i) * p_);
                const std::pair<double, std::uint32_t> item{d2, order_[i]};
                if (h.items.size() < h.k) {
                    h.items.push_back(item);
                    std::push_heap(h.items.begin(), h.items.end());
                } else if (item < h.items.front()) {   // nearer, or as near and an earlier row
                    std::pop_heap(h.items.begin(), h.items.end());
                    h.items.back() = item;
                    std::push_heap(h.items.begin(), h.items.end());
                }
            }
            return;
        }
        const auto dim = static_cast<std::size_t>(nd.dim);
        const double diff = q[dim] - nd.cut;
        search(diff < 0.0 ? nd.left : nd.right, q, off, bound, h);
        // The far side is at least this far: the query's offset to the cut in
        // this dimension replaces the one already counted for it.
        const double old = off[dim];
        const double far = bound - old * old + diff * diff;
        if (h.items.size() < h.k || far <= h.worst()) {
            off[dim] = diff;
            search(diff < 0.0 ? nd.right : nd.left, q, off, far, h);
            off[dim] = old;
        }
    }

    std::size_t p_ = 0, n_ = 0;
    std::vector<double> mean_, scale_, xs_;
    std::vector<std::uint8_t> up_;
    std::vector<std::uint32_t> order_;
    std::vector<Node> nodes_;
};

class CurriculumKnn final : public CurriculumModel {
public:
    std::string name() const override { return "k-nearest neighbours"; }
    std::string family() const override { return "classical"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t n = d.train_rows();
        if (n < 10) { return "fewer than 10 rows"; }
        const std::size_t grid[] = {5, 15, 45};
        k_ = grid[0];
        const std::size_t v = curriculum_detail::inner_split(n);
        if (const CurriculumDesign* di = n >= 40 ? d.inner(v) : nullptr) {
            // One search for the largest k answers every k on the grid: the
            // k nearest are the first k of the 45 nearest, in the same order.
            index_.build(*di, 0, v);
            std::vector<double> p[3];
            std::vector<std::uint8_t> near;
            for (std::size_t j = v; j < n; ++j) {
                const bool ok = index_.nearest(di->features(j), grid[2], near);
                for (std::size_t g = 0; g < 3; ++g) {
                    if (!ok || grid[g] > v || near.size() < grid[g]) { p[g].push_back(curriculum_detail::nan()); continue; }
                    double up = 0.0;
                    for (std::size_t m = 0; m < grid[g]; ++m) { up += near[m]; }
                    p[g].push_back(up / static_cast<double>(grid[g]));
                }
            }
            double best = -1.0;
            for (std::size_t g = 0; g < 3; ++g) {
                if (grid[g] > v) { continue; }
                const double acc = curriculum_detail::sign_accuracy(p[g], d, v);
                if (acc > best) { best = acc; k_ = grid[g]; }
            }
        }
        index_.build(d, 0, n);
        mu_up_ = d.mean_up();
        mu_down_ = d.mean_down();
        rows_ = n;
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        std::vector<std::uint8_t> near;
        if (!index_.nearest(d.features(i), k_, near) || near.empty()) { return {}; }
        double up = 0.0;
        for (const auto u : near) { up += u; }
        const double q = up / static_cast<double>(near.size());
        return curriculum_detail::from_probability(q, q * mu_up_ + (1.0 - q) * mu_down_);
    }
    std::string tuned() const override { return "k=" + std::to_string(k_) + ", all " + std::to_string(rows_) + " rows"; }

private:
    CurriculumKnnIndex index_;
    std::size_t k_ = 5, rows_ = 0;
    double mu_up_ = 0.0, mu_down_ = 0.0;
};

class CurriculumForest final : public CurriculumModel {
public:
    explicit CurriculumForest(std::uint64_t seed) noexcept : seed_(seed) {}
    std::string name() const override { return "Random forest"; }
    std::string family() const override { return "trees"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t n = d.train_rows();
        if (n < 20) { return "fewer than 20 rows"; }
        const std::size_t grid[] = {3, 6};
        depth_ = grid[0];
        const std::size_t v = curriculum_detail::inner_split(n);
        if (const CurriculumDesign* di = n >= 80 ? d.inner(v) : nullptr) {
            double best = -1.0;
            for (const std::size_t depth : grid) {
                const auto m = fit_range(*di, 0, v, depth);
                if (!m) { continue; }
                std::vector<double> p;
                for (std::size_t j = v; j < n; ++j) { p.push_back(m->predict(di->features(j))); }
                const double acc = curriculum_detail::sign_accuracy(p, d, v);
                if (acc > best) { best = acc; depth_ = depth; }
            }
        }
        auto m = fit_range(d, 0, n, depth_);
        if (!m) { return "forest fit refused"; }
        model_ = std::move(*m);
        mu_up_ = d.mean_up();
        mu_down_ = d.mean_down();
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const double p = model_.predict(d.features(i));
        return curriculum_detail::from_probability(p, p * mu_up_ + (1.0 - p) * mu_down_);
    }
    std::string tuned() const override { return "depth=" + std::to_string(depth_); }

private:
    std::expected<RandomForest, ClassicalError>
    fit_range(const CurriculumDesign& d, std::size_t from, std::size_t to, std::size_t depth) const {
        const auto x = d.block(from, to);
        std::vector<double> y;
        for (std::size_t j = from; j < to; ++j) { y.push_back(d.up(j) ? 1.0 : 0.0); }
        ForestParams prm;
        prm.trees = 48;
        prm.max_depth = depth;
        prm.min_leaf = 5;
        prm.seed = seed_;
        prm.regression = true;   // the mean of 0/1 leaves is P(up)
        return RandomForest::fit(x, to - from, d.p(), y, prm);
    }
    RandomForest model_{};
    std::uint64_t seed_;
    std::size_t depth_ = 3;
    double mu_up_ = 0.0, mu_down_ = 0.0;
};

// ---------------------------------------------------------------------------
// Regressors on the return: boosting and the MLP
// ---------------------------------------------------------------------------

class CurriculumGbdt final : public CurriculumModel {
public:
    explicit CurriculumGbdt(std::uint64_t seed) noexcept : seed_(seed) {}
    std::string name() const override { return "Gradient boosting"; }
    std::string family() const override { return "trees"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t n = d.train_rows();
        if (n < 64) { return "fewer than 64 rows"; }
        Frame f;
        f.x = d.block(0, n);
        f.rows = n;
        f.p = d.p();
        std::vector<double> y(n);
        for (std::size_t j = 0; j < n; ++j) { y[j] = d.target(j); }
        GbdtParams prm;
        prm.trees = 100;
        prm.max_depth = 3;
        prm.learning_rate = 0.05;
        prm.subsample = 0.8;
        prm.min_leaf = 20;
        prm.seed = seed_;
        auto m = fit_gbdt(f, y, prm);
        if (!m) { return "GBDT fit refused"; }
        model_ = std::move(*m);
        double ss = 0.0;
        for (std::size_t j = 0; j < n; ++j) {
            const double e = y[j] - model_.predict_row(f.x.data() + j * f.p);
            ss += e * e;
        }
        sigma_ = std::sqrt(ss / static_cast<double>(n));
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const auto x = d.features(i);
        if (x.empty()) { return {}; }
        return curriculum_detail::from_return(model_.predict_row(x.data()), sigma_);
    }

private:
    Gbdt model_{};
    std::uint64_t seed_;
    double sigma_ = 0.0;
};

class CurriculumMlp final : public CurriculumModel {
public:
    explicit CurriculumMlp(std::uint64_t seed) noexcept : seed_(seed) {}
    std::string name() const override { return "Neural net (MLP)"; }
    std::string family() const override { return "neural"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t n = d.train_rows();
        if (n < 20) { return "fewer than 20 rows"; }
        constexpr std::size_t X = kCurriculumMaxFeatures;
        std::vector<double> x(n * X, 0.0), y(n);
        std::vector<double> raw(n);
        for (std::size_t j = 0; j < n; ++j) {
            const auto f = d.features(j);
            for (std::size_t c = 0; c < f.size(); ++c) { x[j * X + c] = f[c]; }
            raw[j] = d.target(j);
        }
        scaler_ = TargetScaler{};
        if (!scaler_.fit(raw.data(), 0, n)) { return "target has no variance"; }
        for (std::size_t j = 0; j < n; ++j) { y[j] = *scaler_.transform(raw[j]); }
        Dataset ds;
        ds.x = Matrix{x.data(), n, X};
        ds.y = y.data();
        ds.rows = n;
        net_.reset(seed_);
        for (int epoch = 0; epoch < 300; ++epoch) { (void)net_.train_epoch(ds, Block{0, n}, 0.05); }
        std::vector<double> out(n);
        net_.predict(ds, Block{0, n}, out.data());
        double ss = 0.0;
        for (std::size_t j = 0; j < n; ++j) {
            const double e = raw[j] - *scaler_.inverse(out[j]);
            ss += e * e;
        }
        sigma_ = std::sqrt(ss / static_cast<double>(n));
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const auto f = d.features(i);
        if (f.empty()) { return {}; }
        constexpr std::size_t X = kCurriculumMaxFeatures;
        double x[X] = {};
        for (std::size_t c = 0; c < f.size(); ++c) { x[c] = f[c]; }
        double y = 0.0;
        Dataset ds;
        ds.x = Matrix{x, 1, X};
        ds.rows = 1;
        net_.predict(ds, Block{0, 1}, &y);
        const auto mu = scaler_.inverse(y);
        return mu ? curriculum_detail::from_return(*mu, sigma_) : CurriculumCall{};
    }

private:
    Mlp<8, kCurriculumMaxFeatures> net_{1e-6, MlpTrainer::Backprop};
    TargetScaler scaler_{};
    std::uint64_t seed_;
    double sigma_ = 0.0;
};

// ---------------------------------------------------------------------------
// Sequence models: LSTM, GRU and the causal transformer
// ---------------------------------------------------------------------------


/// `Steps` caps how far back the network reads. `budget` > 0 replaces the
/// epoch rule with that many SGD steps on the window's latest rows; no default
/// model uses it now that the transformer trains by backpropagation
/// (models/transformer.hpp's analytic_gradients) like the LSTM and GRU.
template <class Net, std::size_t Steps = kCurriculumSeqSteps>
class CurriculumSequence final : public CurriculumModel {
public:
    CurriculumSequence(std::string name, std::uint64_t seed, std::size_t budget = 0)
        : name_(std::move(name)), seed_(seed), budget_(budget) {}
    std::string name() const override { return name_; }
    std::string family() const override { return "neural"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t n = d.train_rows();
        if (n < 32) { return "fewer than 32 rows"; }
        std::vector<double> raw(n);
        for (std::size_t j = 0; j < n; ++j) { raw[j] = d.target(j); }
        scaler_ = TargetScaler{};
        if (!scaler_.fit(raw.data(), 0, n)) { return "target has no variance"; }
        net_ = std::make_unique<Net>();
        net_->reset(seed_);
        std::uint64_t s = seed_;
        double seq[kCurriculumSeqSteps * kCurriculumSeqInputs];
        if (budget_ > 0) {
            // RULE 11: visible truncation -- a fixed number of SGD steps drawn
            // from the latest 4 * budget rows; tuned() reports it.
            const std::size_t from = n > 4 * budget_ ? n - 4 * budget_ : 0;
            for (std::size_t k = 0; k < budget_; ++k) {
                s = curriculum_detail::mix(s);
                const std::size_t j = from + s % (n - from);
                const std::size_t steps = window(d, j, seq);
                const double lr = 0.01 / (1.0 + 2.0 * static_cast<double>(k) / static_cast<double>(budget_));
                (void)net_->train_step(seq, steps, *scaler_.transform(raw[j]), lr);
            }
            trained_ = "SGD steps " + std::to_string(budget_);
        } else {
            const int epochs = n <= 200 ? 30 : (n <= 2000 ? 10 : 4);
            std::vector<std::size_t> order(n);
            for (std::size_t j = 0; j < n; ++j) { order[j] = j; }
            for (int e = 0; e < epochs; ++e) {
                for (std::size_t j = n; j > 1; --j) {   // a seeded Fisher-Yates shuffle
                    s = curriculum_detail::mix(s);
                    std::swap(order[j - 1], order[s % j]);
                }
                const double lr = 0.01 / (1.0 + 0.5 * e);
                for (const std::size_t j : order) {
                    const std::size_t steps = window(d, j, seq);
                    (void)net_->train_step(seq, steps, *scaler_.transform(raw[j]), lr);
                }
            }
            trained_ = "epochs " + std::to_string(epochs);
        }
        double ss = 0.0;
        for (std::size_t j = 0; j < n; ++j) {
            const std::size_t steps = window(d, j, seq);
            const double e = raw[j] - *scaler_.inverse(net_->predict(seq, steps));
            ss += e * e;
        }
        sigma_ = std::sqrt(ss / static_cast<double>(n));
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        double seq[kCurriculumSeqSteps * kCurriculumSeqInputs];
        const std::size_t steps = window(d, i, seq);
        if (steps == 0 || !net_) { return {}; }
        const auto mu = scaler_.inverse(net_->predict(seq, steps));
        return mu ? curriculum_detail::from_return(*mu, sigma_) : CurriculumCall{};
    }
    std::string tuned() const override { return trained_; }

private:
    static_assert(Steps > 0 && Steps <= kCurriculumSeqSteps);
    /// The last `Steps` steps ending at row i, oldest first, at the front of `out`.
    static std::size_t window(const CurriculumDesign& d, std::size_t i, double* out) noexcept {
        const std::size_t got = d.sequence(i, out);
        if (got <= Steps) { return got; }
        const std::size_t skip = (got - Steps) * kCurriculumSeqInputs;
        for (std::size_t k = 0; k < Steps * kCurriculumSeqInputs; ++k) { out[k] = out[k + skip]; }
        return Steps;
    }
    std::string name_;
    std::uint64_t seed_;
    std::size_t budget_;
    std::unique_ptr<Net> net_;
    TargetScaler scaler_{};
    double sigma_ = 0.0;
    std::string trained_;
};

using CurriculumLstm = CurriculumSequence<TrainableLstm<8, kCurriculumSeqInputs, kCurriculumSeqSteps>>;
using CurriculumGru = CurriculumSequence<TrainableGru<8, kCurriculumSeqInputs, kCurriculumSeqSteps>>;
/// Eight steps back, not sixteen: attention is quadratic in steps.
inline constexpr std::size_t kCurriculumTransformerSteps = 8;
using CurriculumTransformer = CurriculumSequence<
    CausalTransformer<kCurriculumSeqInputs, 2, 8, 1, kCurriculumTransformerSteps>, kCurriculumTransformerSteps>;

// ---------------------------------------------------------------------------
// Time series: AR, ARMA, Ornstein-Uhlenbeck, Markov chain
// ---------------------------------------------------------------------------

/// AR(p) / ARMA(p, q) on the return series. Parameters are frozen for the
/// stage; the lags roll forward with the realised returns, as they would live.
class CurriculumArma final : public CurriculumModel {
public:
    CurriculumArma(std::size_t p, std::size_t q) noexcept : p_(p), q_(q) {}
    std::string name() const override {
        return q_ == 0 ? "AR(" + std::to_string(p_) + ")"
                       : "ARMA(" + std::to_string(p_) + "," + std::to_string(q_) + ")";
    }
    std::string family() const override { return "time series"; }
    std::string fit(const CurriculumDesign& d) override {
        const auto h = d.history(d.train_rows());
        if (h.size() < 30) { return "fewer than 30 returns"; }
        auto m = fit_arma(h, p_, q_);
        if (!m) { return "ARMA fit refused"; }
        model_ = std::move(*m);
        e_.clear();
        done_ = 0;
        sigma_ = std::sqrt(std::max(0.0, model_.residual_variance));
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const auto h = d.history(i);
        std::size_t lag = 0;
        for (const auto l : model_.ar_lags) { lag = std::max(lag, l); }
        for (const auto l : model_.ma_lags) { lag = std::max(lag, l); }
        if (h.size() <= lag) { return {}; }
        // Residuals by recursion with frozen coefficients, carried forward as
        // the realised returns arrive: a million-row track cannot afford a
        // full recursion per forecast. Realised returns never change, so the
        // cache stays valid while rows are asked for in order.
        if (h.size() < done_) { done_ = 0; }
        if (e_.size() < h.size()) { e_.resize(h.size(), 0.0); }
        for (std::size_t t = std::max(done_, lag); t < h.size(); ++t) {
            double v = model_.intercept;
            for (std::size_t k = 0; k < model_.ar.size(); ++k) { v += model_.ar[k] * h[t - model_.ar_lags[k]]; }
            for (std::size_t k = 0; k < model_.ma.size(); ++k) { v += model_.ma[k] * e_[t - model_.ma_lags[k]]; }
            e_[t] = h[t] - v;
        }
        done_ = h.size();
        double mu = model_.intercept;
        const std::size_t n = h.size();
        for (std::size_t k = 0; k < model_.ar.size(); ++k) { mu += model_.ar[k] * h[n - model_.ar_lags[k]]; }
        for (std::size_t k = 0; k < model_.ma.size(); ++k) { mu += model_.ma[k] * e_[n - model_.ma_lags[k]]; }
        return curriculum_detail::from_return(mu, sigma_);
    }

private:
    std::size_t p_, q_;
    LagModel model_{};
    double sigma_ = 0.0;
    mutable std::vector<double> e_;
    mutable std::size_t done_ = 0;
};

/// Ornstein-Uhlenbeck on the log price: mean reversion to the window's level.
/// On a trending index the fit is usually refused as non-stationary, and the
/// model abstains -- which is the honest answer.
class CurriculumOu final : public CurriculumModel {
public:
    std::string name() const override { return "Ornstein-Uhlenbeck"; }
    std::string family() const override { return "time series"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t n = d.train_rows();
        if (n < 20) { return "fewer than 20 prices"; }
        std::vector<double> lv(n);
        for (std::size_t j = 0; j < n; ++j) { lv[j] = d.level(j); }
        auto m = fit_ou(lv, 1.0);
        if (!m) {
            return m.error() == TimeSeriesError::NonStationary ? "not mean-reverting in this window"
                                                                : "OU fit refused";
        }
        model_ = *m;
        const double decay = std::exp(-2.0 * model_.theta);
        sigma_ = model_.sigma * std::sqrt(std::max(0.0, (1.0 - decay) / (2.0 * model_.theta)));
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const double now = d.level(i);
        if (!std::isfinite(now)) { return {}; }
        return curriculum_detail::from_return(model_.expected_next(now) - now, sigma_);
    }
    std::string tuned() const override {
        return "half-life " + curriculum_detail::trim_double(std::round(model_.half_life() * 10.0) / 10.0) + " rows";
    }

private:
    OrnsteinUhlenbeck model_{};
    double sigma_ = 0.0;
};

/// A three-state Markov chain on return terciles (models/markov.hpp).
class CurriculumMarkov final : public CurriculumModel {
public:
    std::string name() const override { return "Markov chain"; }
    std::string family() const override { return "time series"; }
    std::string fit(const CurriculumDesign& d) override {
        const auto h = d.history(d.train_rows());
        if (h.size() < 30) { return "fewer than 30 returns"; }
        std::vector<double> copy(h.begin(), h.end());
        auto b = quantile_boundaries(copy, kStates);
        if (!b) { return "returns too coarse for terciles"; }
        bounds_ = *b;
        std::vector<std::size_t> seq(h.size());
        double up[kStates] = {}, sum[kStates] = {}, cnt[kStates] = {};
        for (std::size_t j = 0; j < h.size(); ++j) {
            seq[j] = bounds_.classify(h[j]);
            cnt[seq[j]] += 1.0;
            sum[seq[j]] += h[j];
            if (h[j] > 0.0) { up[seq[j]] += 1.0; }
        }
        auto m = fit_from_states(seq, kStates);
        if (!m) { return "Markov fit refused"; }
        chain_ = *m;
        for (std::size_t s = 0; s < kStates; ++s) {
            up_[s] = (up[s] + 1.0) / (cnt[s] + 2.0);
            mean_[s] = cnt[s] > 0.0 ? sum[s] / cnt[s] : 0.0;
        }
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const auto h = d.history(i);
        if (h.empty()) { return {}; }
        const std::size_t s = bounds_.classify(h.back());
        if (chain_.row_total[s] == 0) { return {}; }
        double p = 0.0, mu = 0.0;
        for (std::size_t k = 0; k < kStates; ++k) {
            p += chain_.p[s][k] * up_[k];
            mu += chain_.p[s][k] * mean_[k];
        }
        return curriculum_detail::from_probability(p, mu);
    }

private:
    static constexpr std::size_t kStates = 3;
    StateBoundaries bounds_{};
    TransitionMatrix chain_{};
    double up_[kStates] = {}, mean_[kStates] = {};
};

/// k-means regimes (models/regime_rl.hpp): the up-rate of the regime the
/// features fall in.
class CurriculumRegimes final : public CurriculumModel {
public:
    explicit CurriculumRegimes(std::uint64_t seed) noexcept : seed_(seed) {}
    std::string name() const override { return "k-means regimes"; }
    std::string family() const override { return "regime"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t n = d.train_rows();
        if (n < kK * 5) { return "fewer than 20 rows"; }
        std::vector<std::vector<double>> x(n);
        for (std::size_t j = 0; j < n; ++j) {
            const auto f = d.features(j);
            x[j].assign(f.begin(), f.end());
        }
        auto c = kmeans(x, kK, seed_);
        if (!c) { return "k-means refused"; }
        centre_ = c->centre;
        std::vector<double> up(kK, 0.0), cnt(kK, 0.0), sum(kK, 0.0);
        for (std::size_t j = 0; j < n; ++j) {
            const std::size_t k = c->label[j];
            cnt[k] += 1.0;
            sum[k] += d.target(j);
            if (d.up(j)) { up[k] += 1.0; }
        }
        p_.assign(kK, 0.5);
        mu_.assign(kK, 0.0);
        for (std::size_t k = 0; k < kK; ++k) {
            p_[k] = (up[k] + 1.0) / (cnt[k] + 2.0);
            mu_[k] = cnt[k] > 0.0 ? sum[k] / cnt[k] : 0.0;
        }
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const auto f = d.features(i);
        if (f.empty() || centre_.empty()) { return {}; }
        std::size_t best = 0;
        double bd = std::numeric_limits<double>::infinity();
        for (std::size_t k = 0; k < centre_.size(); ++k) {
            double s = 0.0;
            for (std::size_t c = 0; c < f.size(); ++c) { s += (f[c] - centre_[k][c]) * (f[c] - centre_[k][c]); }
            if (s < bd) { bd = s; best = k; }
        }
        return curriculum_detail::from_probability(p_[best], mu_[best]);
    }

private:
    static constexpr std::size_t kK = 4;
    std::uint64_t seed_;
    std::vector<std::vector<double>> centre_;
    std::vector<double> p_, mu_;
};

// ---------------------------------------------------------------------------
// The rest of the Model Atlas that can make a next-bar call
//
// Section by section (desktop/atlas_data.hpp). Rows that cannot produce a
// forecast for one instrument's next bar -- execution, risk, option pricing,
// portfolio construction, text, fundamentals -- are listed with the reason in
// ops/forecast-curriculum.md rather than forced into a shape they do not have.
// ---------------------------------------------------------------------------

namespace curriculum_detail {

/// Solve the small dense system a x = b (n <= 64) by Gaussian elimination with
/// partial pivoting. False when singular.
inline bool solve_dense(std::vector<double> a, std::vector<double> b, std::size_t n, std::vector<double>& x) {
    for (std::size_t c = 0; c < n; ++c) {
        std::size_t piv = c;
        for (std::size_t r = c + 1; r < n; ++r) {
            if (std::fabs(a[r * n + c]) > std::fabs(a[piv * n + c])) { piv = r; }
        }
        if (!(std::fabs(a[piv * n + c]) > 1e-12)) { return false; }
        if (piv != c) {
            for (std::size_t k = 0; k < n; ++k) { std::swap(a[c * n + k], a[piv * n + k]); }
            std::swap(b[c], b[piv]);
        }
        for (std::size_t r = c + 1; r < n; ++r) {
            const double f = a[r * n + c] / a[c * n + c];
            for (std::size_t k = c; k < n; ++k) { a[r * n + k] -= f * a[c * n + k]; }
            b[r] -= f * b[c];
        }
    }
    x.assign(n, 0.0);
    for (std::size_t c = n; c-- > 0;) {
        double v = b[c];
        for (std::size_t k = c + 1; k < n; ++k) { v -= a[c * n + k] * x[k]; }
        x[c] = v / a[c * n + c];
    }
    return true;
}

/// Ridge regression of y on [x, 1] over rows [from, to); the intercept is not
/// penalised. Returns p + 1 weights, the intercept last.
inline bool ridge_fit(const CurriculumDesign& d, std::size_t from, std::size_t to, double lambda,
                      std::vector<double>& w) {
    const std::size_t p = d.p(), m = p + 1;
    std::vector<double> a(m * m, 0.0), b(m, 0.0), row(m, 1.0);
    for (std::size_t j = from; j < to; ++j) {
        const auto f = d.features(j);
        for (std::size_t c = 0; c < p; ++c) { row[c] = f[c]; }
        const double y = d.target(j);
        for (std::size_t r = 0; r < m; ++r) {
            b[r] += row[r] * y;
            for (std::size_t c = 0; c < m; ++c) { a[r * m + c] += row[r] * row[c]; }
        }
    }
    for (std::size_t c = 0; c < p; ++c) { a[c * m + c] += lambda; }
    return solve_dense(std::move(a), std::move(b), m, w);
}

inline double dot_row(const std::vector<double>& w, std::span<const double> f) noexcept {
    double v = w.back();
    for (std::size_t c = 0; c < f.size(); ++c) { v += w[c] * f[c]; }
    return v;
}

} // namespace curriculum_detail

/// Linear / ridge regression on the next return (Atlas 2).
class CurriculumRidge final : public CurriculumModel {
public:
    std::string name() const override { return "Ridge regression"; }
    std::string family() const override { return "classical"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t n = d.train_rows();
        if (n < d.p() + 5) { return "fewer rows than features"; }
        const double grid[] = {1e-3, 1e-1, 10.0};   // x n: the penalty grows with the sample
        lambda_ = grid[1];
        const std::size_t v = curriculum_detail::inner_split(n);
        if (const CurriculumDesign* di = n >= 60 ? d.inner(v) : nullptr) {
            double best = -1.0;
            for (const double g : grid) {
                std::vector<double> w;
                if (!curriculum_detail::ridge_fit(*di, 0, v, g * static_cast<double>(v), w)) { continue; }
                std::vector<double> pr;
                for (std::size_t j = v; j < n; ++j) {
                    pr.push_back(curriculum_detail::dot_row(w, di->features(j)) > 0.0 ? 1.0 : 0.0);
                }
                const double acc = curriculum_detail::sign_accuracy(pr, d, v);
                if (acc > best) { best = acc; lambda_ = g; }
            }
        }
        if (!curriculum_detail::ridge_fit(d, 0, n, lambda_ * static_cast<double>(n), w_)) { return "singular design"; }
        double ss = 0.0;
        for (std::size_t j = 0; j < n; ++j) {
            const double e = d.target(j) - curriculum_detail::dot_row(w_, d.features(j));
            ss += e * e;
        }
        sigma_ = std::sqrt(ss / static_cast<double>(n));
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const auto f = d.features(i);
        if (f.empty()) { return {}; }
        return curriculum_detail::from_return(curriculum_detail::dot_row(w_, f), sigma_);
    }
    std::string tuned() const override { return "lambda=" + curriculum_detail::trim_double(lambda_) + " x n"; }

private:
    std::vector<double> w_;
    double lambda_ = 0.1, sigma_ = 0.0;
};

/// One regression tree (Atlas 2, "Decision tree": models/gbdt.hpp with a
/// single tree at full learning rate).
class CurriculumTree final : public CurriculumModel {
public:
    std::string name() const override { return "Decision tree"; }
    std::string family() const override { return "trees"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t n = d.train_rows();
        if (n < 64) { return "fewer than 64 rows"; }
        Frame f;
        f.x = d.block(0, n);
        f.rows = n;
        f.p = d.p();
        std::vector<double> y(n);
        for (std::size_t j = 0; j < n; ++j) { y[j] = d.target(j); }
        GbdtParams prm;
        prm.trees = 1;
        prm.max_depth = 4;
        prm.learning_rate = 1.0;
        prm.subsample = 1.0;
        prm.min_leaf = n / 50 > 20 ? n / 50 : 20;
        auto m = fit_gbdt(f, y, prm);
        if (!m) { return "tree fit refused"; }
        model_ = std::move(*m);
        double ss = 0.0;
        for (std::size_t j = 0; j < n; ++j) {
            const double e = y[j] - model_.predict_row(f.x.data() + j * f.p);
            ss += e * e;
        }
        sigma_ = std::sqrt(ss / static_cast<double>(n));
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const auto x = d.features(i);
        if (x.empty()) { return {}; }
        return curriculum_detail::from_return(model_.predict_row(x.data()), sigma_);
    }

private:
    Gbdt model_{};
    double sigma_ = 0.0;
};

/// CNN (Atlas 2): models/trainable_cnn.hpp -- three layers of attention.hpp's
/// dilated causal convolution (dilations 1, 2, 4; eight channels) with the
/// kernels trained by backpropagation on up/down (cross-entropy, Adam), read
/// out at the last step. Its receptive field covers the whole sequence.
class CurriculumCnn final : public CurriculumModel {
public:
    explicit CurriculumCnn(std::uint64_t seed) noexcept : seed_(seed) {}
    std::string name() const override { return "CNN (trained, dilated causal)"; }
    std::string family() const override { return "neural"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t n = d.train_rows();
        if (n < 64) { return "fewer than 64 rows"; }
        net_ = std::make_unique<Net>();
        net_->reset(seed_);
        const int epochs = n <= 200 ? 30 : (n <= 2000 ? 10 : 4);
        std::vector<std::size_t> order(n);
        for (std::size_t j = 0; j < n; ++j) { order[j] = j; }
        std::uint64_t s = seed_;
        double seq[kCurriculumSeqSteps * kCurriculumSeqInputs];
        for (int e = 0; e < epochs; ++e) {
            for (std::size_t j = n; j > 1; --j) {   // a seeded Fisher-Yates shuffle
                s = curriculum_detail::mix(s);
                std::swap(order[j - 1], order[s % j]);
            }
            const double lr = 2e-3 / (1.0 + 0.5 * e);
            for (const std::size_t j : order) {
                const std::size_t steps = d.sequence(j, seq);
                if (steps == 0) { continue; }
                (void)net_->train_step(seq, steps, d.up(j) ? 1 : 0, lr);
            }
        }
        trained_ = "epochs " + std::to_string(epochs) + " over all " + std::to_string(n) + " rows";
        mu_up_ = d.mean_up();
        mu_down_ = d.mean_down();
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        double seq[kCurriculumSeqSteps * kCurriculumSeqInputs];
        const std::size_t steps = d.sequence(i, seq);
        if (steps == 0 || !net_) { return {}; }
        const double p = net_->probability(seq, steps);
        return curriculum_detail::from_probability(p, p * mu_up_ + (1.0 - p) * mu_down_);
    }
    std::string tuned() const override { return trained_; }

private:
    using Net = TrainableTcn<kCurriculumSeqInputs, 8, 3, 3, kCurriculumSeqSteps>;
    std::uint64_t seed_;
    std::unique_ptr<Net> net_;
    double mu_up_ = 0.0, mu_down_ = 0.0;
    std::string trained_;
};

/// Linear autoencoder (Atlas 2, models/classical.hpp: the PCA optimum) to four
/// codes, then a logistic regression on the codes.
class CurriculumAutoencoder final : public CurriculumModel {
public:
    std::string name() const override { return "Autoencoder + logistic"; }
    std::string family() const override { return "classical"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t n = d.train_rows();
        if (n < 40) { return "fewer than 40 rows"; }
        const auto x = d.block(0, n);
        // RULE 11: proven -- a bottleneck narrower than the input is what an
        // autoencoder is; on a narrow track it keeps one code fewer than inputs.
        codes_ = d.p() > kCodes ? kCodes : (d.p() > 1 ? d.p() - 1 : 1);
        auto ae = LinearAutoencoder::fit(x, n, d.p(), codes_);
        if (!ae) { return "autoencoder fit refused"; }
        ae_ = std::move(*ae);
        std::vector<double> codes;
        codes.reserve(n * codes_);
        std::vector<std::uint8_t> y(n);
        for (std::size_t j = 0; j < n; ++j) {
            const auto c = ae_.encode(d.features(j));
            if (!c) { return "encoding refused"; }
            codes.insert(codes.end(), c->begin(), c->end());
            y[j] = d.up(j) ? 1 : 0;
        }
        LogisticParams prm;
        prm.l2 = 1e-2;
        auto m = LogisticRegression::fit(codes, n, codes_, y, prm);
        if (!m) { return "readout fit refused"; }
        readout_ = std::move(*m);
        mu_up_ = d.mean_up();
        mu_down_ = d.mean_down();
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const auto c = ae_.encode(d.features(i));
        if (!c) { return {}; }
        const double p = readout_.probability(*c);
        return curriculum_detail::from_probability(p, p * mu_up_ + (1.0 - p) * mu_down_);
    }

private:
    static constexpr std::size_t kCodes = 4;
    std::size_t codes_ = kCodes;
    LinearAutoencoder ae_{};
    LogisticRegression readout_{};
    double mu_up_ = 0.0, mu_down_ = 0.0;
};

/// VAR(1) (Atlas 3, models/classical.hpp fit_var1) on the last realised return
/// and one exogenous series -- INDIA VIX's move on the index tracks.
class CurriculumVar final : public CurriculumModel {
public:
    std::string name() const override { return "VAR(1)"; }
    std::string family() const override { return "time series"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t n = d.train_rows();
        if (n < 40) { return "fewer than 40 rows"; }
        const auto h = d.history(n);
        std::vector<double> z;
        z.reserve(2 * n);
        for (std::size_t j = 1; j < n; ++j) {   // row j: the return known at j, and j's exogenous input
            z.push_back(h[j - 1]);
            z.push_back(exogenous(d, j));
        }
        z.push_back(h[n - 1]);                  // the pair known at the first test decision closes the series
        z.push_back(exogenous(d, n));
        auto m = fit_var1(z, n, 2);
        if (!m) { return "VAR fit refused"; }
        model_ = std::move(*m);
        double ss = 0.0;
        for (std::size_t j = 1; j + 1 < n; ++j) {
            const double prev[2] = {h[j - 1], exogenous(d, j)};
            const auto f = model_.predict(prev);
            if (f) { const double e = h[j] - (*f)[0]; ss += e * e; }
        }
        sigma_ = std::sqrt(ss / static_cast<double>(n));
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const auto h = d.history(i);
        if (h.empty()) { return {}; }
        const double prev[2] = {h.back(), exogenous(d, i)};
        const auto f = model_.predict(prev);
        return f ? curriculum_detail::from_return((*f)[0], sigma_) : CurriculumCall{};
    }

private:
    static double exogenous(const CurriculumDesign& d, std::size_t j) {
        const auto f = d.features(j);
        const auto& cols = d.track().seq_cols;
        return f.empty() ? 0.0 : f[cols.back()];
    }
    Var1Model model_{};
    double sigma_ = 0.0;
};

/// Kalman filter (Atlas 3, analytics/kalman.hpp) on the latent drift of the
/// returns: the one-step forecast is the filtered drift.
class CurriculumKalman final : public CurriculumModel {
public:
    std::string name() const override { return "Kalman filter (drift)"; }
    std::string family() const override { return "time series"; }
    std::string fit(const CurriculumDesign& d) override {
        const auto h = d.history(d.train_rows());
        if (h.size() < 30) { return "fewer than 30 returns"; }
        double m = 0.0, ss = 0.0;
        for (const double r : h) { m += r; }
        m /= static_cast<double>(h.size());
        for (const double r : h) { ss += (r - m) * (r - m); }
        r_ = ss / static_cast<double>(h.size());
        if (!(r_ > 0.0)) { return "returns have no variance"; }
        // q/r chosen on the window's last quarter: how fast may the drift move?
        const double grid[] = {1e-4, 1e-3, 1e-2};
        const std::size_t v = curriculum_detail::inner_split(h.size());
        double best = -1.0;
        for (const double g : grid) {
            auto k = Kalman1D::make(0.0, r_, g * r_, r_);
            if (!k) { continue; }
            std::size_t right = 0, scored = 0;
            for (std::size_t t = 0; t < h.size(); ++t) {
                if (t >= v && h[t] != 0.0 && k->state() != 0.0) {
                    ++scored;
                    if ((k->state() > 0.0) == (h[t] > 0.0)) { ++right; }
                }
                (void)k->update(h[t], 1.0);
            }
            const double acc = scored > 0 ? static_cast<double>(right) / static_cast<double>(scored) : 0.0;
            if (acc > best) { best = acc; q_ratio_ = g; }
        }
        auto made = Kalman1D::make(0.0, r_, q_ratio_ * r_, r_);
        if (!made) { return "Kalman refused"; }
        filter_ = *made;
        done_ = 0;
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const auto h = d.history(i);
        if (!filter_ || h.size() < done_) { return {}; }
        for (std::size_t t = done_; t < h.size(); ++t) { (void)filter_->update(h[t], 1.0); }
        done_ = h.size();
        return curriculum_detail::from_return(filter_->state(),
                                              std::sqrt(filter_->variance() + r_));
    }
    std::string tuned() const override { return "q/r=" + curriculum_detail::trim_double(q_ratio_); }

private:
    double r_ = 0.0, q_ratio_ = 1e-3;
    mutable std::optional<Kalman1D> filter_;
    mutable std::size_t done_ = 0;
};

/// Two-state Gaussian hidden Markov model (Atlas 3, analytics/hmm.hpp) on the
/// returns, filtered forward: P(up next) = sum over next states.
class CurriculumHmm final : public CurriculumModel {
public:
    explicit CurriculumHmm(std::uint64_t seed) noexcept : seed_(seed) {}
    std::string name() const override { return "Hidden Markov model"; }
    std::string family() const override { return "time series"; }
    std::string fit(const CurriculumDesign& d) override {
        const auto h = d.history(d.train_rows());
        if (h.size() < 100) { return "fewer than 100 returns"; }
        // Baum-Welch on every return of the window; the forward filter then
        // runs on from there.
        std::vector<double> x(h.begin(), h.end());
        auto m = fit_hmm(x, 2, seed_, 4, 60);
        if (!m) { return "HMM fit refused"; }
        hmm_ = std::move(*m);
        alpha_.assign(hmm_.k, 0.0);
        for (std::size_t s = 0; s < hmm_.k; ++s) { alpha_[s] = hmm_.pi[s]; }
        done_ = 0;
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const auto h = d.history(i);
        if (hmm_.k == 0 || h.size() < done_) { return {}; }
        const std::size_t k = hmm_.k;
        std::vector<double> next(k);
        for (std::size_t t = done_; t < h.size(); ++t) {
            double tot = 0.0;
            for (std::size_t j = 0; j < k; ++j) {
                double pr = 0.0;
                for (std::size_t s = 0; s < k; ++s) { pr += alpha_[s] * hmm_.trans(s, j); }
                next[j] = pr * detail::gauss(h[t], hmm_.mu[j], hmm_.sigma[j]);
                tot += next[j];
            }
            if (!(tot > 0.0) || !std::isfinite(tot)) { next.assign(k, 1.0 / static_cast<double>(k)); tot = 1.0; }
            for (std::size_t j = 0; j < k; ++j) { alpha_[j] = next[j] / tot; }
        }
        done_ = h.size();
        double p = 0.0, mu = 0.0, var = 0.0;
        for (std::size_t j = 0; j < k; ++j) {
            double w = 0.0;
            for (std::size_t s = 0; s < k; ++s) { w += alpha_[s] * hmm_.trans(s, j); }
            p += w * curriculum_detail::normal_cdf(hmm_.mu[j] / hmm_.sigma[j]);
            mu += w * hmm_.mu[j];
            var += w * (hmm_.sigma[j] * hmm_.sigma[j] + hmm_.mu[j] * hmm_.mu[j]);
        }
        CurriculumCall c = curriculum_detail::from_probability(p, mu);
        c.sigma = std::sqrt(std::max(0.0, var - mu * mu));
        return c;
    }

private:
    std::uint64_t seed_;
    Hmm hmm_{};
    mutable std::vector<double> alpha_;
    mutable std::size_t done_ = 0;
};

/// Hurst regime switch (Atlas 1 "Regime detection" and 3 "Hurst exponent":
/// strategies/regime.hpp's TrendDetector on analytics/hurst.hpp). On the last
/// 256 returns: persistent -> follow the last 20 returns; anti-persistent ->
/// fade them; a random walk -> no call.
class CurriculumHurst final : public CurriculumModel {
public:
    std::string name() const override { return "Hurst regime switch"; }
    std::string family() const override { return "time series"; }
    std::string fit(const CurriculumDesign& d) override {
        return d.history(d.train_rows()).size() < kWindow ? "fewer than 256 returns" : std::string{};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const auto h = d.history(i);
        if (h.size() < kWindow) { return {}; }
        // Recomputed every 16 rows: a 256-return estimate barely moves in 16.
        if (i / 16 != cached_block_ || !have_) {
            const auto est = hurst_rs(h.data() + h.size() - kWindow, kWindow);
            have_ = est.has_value();
            if (have_) { margin_ = est->std_error > 0.0 ? (est->h - 0.5) / est->std_error : 0.0; }
            cached_block_ = i / 16;
        }
        if (!have_ || std::fabs(margin_) < 2.0) { return {}; }
        double sum = 0.0;
        for (std::size_t k = h.size() - 20; k < h.size(); ++k) { sum += h[k]; }
        const int trend = sum > 0.0 ? 1 : (sum < 0.0 ? -1 : 0);
        return curriculum_detail::from_direction(margin_ > 0.0 ? trend : -trend);
    }

private:
    static constexpr std::size_t kWindow = 256;
    mutable std::size_t cached_block_ = 0;
    mutable bool have_ = false;
    mutable double margin_ = 0.0;
};

/// Seasonal autoregression (Atlas 3 "SARIMA", models/time_series.hpp's lag
/// fitter): lags 1, 2 and one trading day back.
class CurriculumSeasonalAr final : public CurriculumModel {
public:
    std::string name() const override { return "Seasonal AR (SARIMA)"; }
    std::string family() const override { return "time series"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t season = d.track().season > 2 ? d.track().season : 5;   // daily: a week
        const auto h = d.history(d.train_rows());
        if (h.size() < 4 * season + 30) { return "fewer than four seasons of returns"; }
        auto m = time_series_detail::fit_lags(h, {1, 2, season}, {}, 1, 1e-8);
        if (!m) { return "seasonal fit refused"; }
        model_ = std::move(*m);
        sigma_ = std::sqrt(std::max(0.0, model_.residual_variance));
        season_ = season;
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const auto f = model_.forecast(d.history(i));
        return f ? curriculum_detail::from_return(*f, sigma_) : CurriculumCall{};
    }
    std::string tuned() const override { return "season " + std::to_string(season_) + " rows"; }

private:
    LagModel model_{};
    double sigma_ = 0.0;
    std::size_t season_ = 0;
};

/// Deep Q-network (Atlas 10, models/deep_rl.hpp) as a contextual bandit: the
/// state is the features, the actions long and short, the reward the next
/// standardised return with the sign of the action; gamma = 0. Its greedy
/// action is the call.
class CurriculumDqn final : public CurriculumModel {
public:
    explicit CurriculumDqn(std::uint64_t seed) noexcept : seed_(seed) {}
    std::string name() const override { return "DQN (reinforcement)"; }
    std::string family() const override { return "neural"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t n = d.train_rows();
        if (n < 64 || !(d.sd() > 0.0)) { return "fewer than 64 rows"; }
        DqnConfig cfg;
        cfg.state_size = d.p();
        cfg.actions = 2;
        cfg.hidden = 16;
        cfg.replay_capacity = n;
        cfg.target_sync_updates = 4;
        cfg.learning_rate = 1e-3;
        cfg.gamma = 0.0;
        cfg.seed = seed_;
        auto a = DqnAgent::create(cfg);
        if (!a) { return "DQN refused"; }
        agent_ = std::move(*a);
        for (std::size_t j = 0; j < n; ++j) {   // the replay holds every row of the window
            const auto f = d.features(j);
            const double r = d.target(j) / d.sd();
            const bool long_side = ((j * 2654435761u) & 1u) == 0;   // both actions, deterministically
            DeepTransition t;
            t.state.assign(f.begin(), f.end());
            t.next_state = t.state;
            t.action = long_side ? 0 : 1;
            t.reward = long_side ? r : -r;
            t.terminal = true;
            (void)agent_->remember(std::move(t));
        }
        for (int e = 0; e < 8; ++e) { (void)agent_->train_replay(); }
        rows_ = n;
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        if (!agent_) { return {}; }
        const auto q = agent_->values(d.features(i));
        if (!q || q->size() != 2) { return {}; }
        const double edge = (*q)[0] - (*q)[1];
        return curriculum_detail::from_direction(edge > 0.0 ? 1 : (edge < 0.0 ? -1 : 0));
    }
    std::string tuned() const override { return "replay of all " + std::to_string(rows_) + " rows"; }

private:
    std::uint64_t seed_;
    std::optional<DqnAgent> agent_;
    std::size_t rows_ = 0;
};

/// PPO and actor-critic (Atlas 10, models/deep_rl.hpp) as a contextual
/// bandit, like the DQN: the state is the features plus a constant, the
/// actions long and short, the reward the next standardised return with the
/// action's sign. Actions are sampled from the current policy (on-policy, as
/// both methods require), in shuffled batches of 256. The policy's P(long) is
/// the forecast, so unlike the DQN these rank on the Frontier sheet.
template <class Agent>
class CurriculumPolicy final : public CurriculumModel {
public:
    CurriculumPolicy(std::string name, std::uint64_t seed) : name_(std::move(name)), seed_(seed) {}
    std::string name() const override { return name_; }
    std::string family() const override { return "neural"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t n = d.train_rows();
        if (n < 64 || !(d.sd() > 0.0)) { return "fewer than 64 rows"; }
        PolicyConfig cfg;
        cfg.state_size = d.p() + 1;
        cfg.actions = 2;
        cfg.actor_learning_rate = 1e-3;
        cfg.critic_learning_rate = 1e-3;
        cfg.gamma = 0.0;
        cfg.gae_lambda = 0.95;
        cfg.clip_ratio = 0.2;
        auto a = Agent::create(cfg);
        if (!a) { return "policy refused"; }
        agent_ = std::move(*a);
        const int epochs = n <= 200 ? 30 : (n <= 2000 ? 10 : 4);
        std::vector<std::size_t> order(n);
        for (std::size_t j = 0; j < n; ++j) { order[j] = j; }
        std::uint64_t s = seed_;
        std::vector<PolicySample> batch;
        batch.reserve(kBatch);
        for (int e = 0; e < epochs; ++e) {
            for (std::size_t j = n; j > 1; --j) {   // a seeded Fisher-Yates shuffle
                s = curriculum_detail::mix(s);
                std::swap(order[j - 1], order[s % j]);
            }
            for (std::size_t b = 0; b < n; b += kBatch) {
                batch.clear();
                // RULE 11: proven -- the last batch takes the remaining rows; every row is used.
                for (std::size_t k = b; k < std::min(n, b + kBatch); ++k) {
                    const std::size_t j = order[k];
                    PolicySample x;
                    x.state = state(d, j);
                    const auto pr = agent_->probabilities(x.state);
                    if (!pr) { return "policy refused a state"; }
                    s = curriculum_detail::mix(s);
                    const double u = static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0);
                    x.action = u < (*pr)[0] ? 0 : 1;   // 0 long, 1 short
                    x.old_probability = (*pr)[x.action];
                    const double r = d.target(j) / d.sd();
                    x.reward = x.action == 0 ? r : -r;
                    x.next_state = x.state;
                    x.terminal = true;
                    batch.push_back(std::move(x));
                }
                if constexpr (std::is_same_v<Agent, PpoAgent>) {
                    if (!agent_->update(batch, 4)) { return "PPO update refused"; }
                } else {
                    if (!agent_->update(batch)) { return "actor-critic update refused"; }
                }
            }
        }
        trained_ = "epochs " + std::to_string(epochs) + " over all " + std::to_string(n) + " rows";
        mu_up_ = d.mean_up();
        mu_down_ = d.mean_down();
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        if (!agent_) { return {}; }
        const auto pr = agent_->probabilities(state(d, i));
        if (!pr || pr->size() != 2) { return {}; }
        const double p = (*pr)[0];
        return curriculum_detail::from_probability(p, p * mu_up_ + (1.0 - p) * mu_down_);
    }
    std::string tuned() const override { return trained_; }

private:
    static constexpr std::size_t kBatch = 256;
    static std::vector<double> state(const CurriculumDesign& d, std::size_t i) {
        const auto f = d.features(i);
        std::vector<double> x(f.begin(), f.end());
        x.push_back(1.0);   // the linear policy's bias
        return x;
    }
    std::string name_;
    std::uint64_t seed_;
    std::optional<Agent> agent_;
    double mu_up_ = 0.0, mu_down_ = 0.0;
    std::string trained_;
};

/// Momentum / trend following (Atlas 1, strategies/momentum.hpp): the sign of
/// the last N returns, N chosen on the window's last quarter.
class CurriculumMomentumN final : public CurriculumModel {
public:
    std::string name() const override { return "Momentum (tuned lookback)"; }
    std::string family() const override { return "statistical"; }
    std::string fit(const CurriculumDesign& d) override {
        const auto h = d.history(d.train_rows());
        if (h.size() < 80) { return "fewer than 80 returns"; }
        const std::size_t grid[] = {5, 20, 60};
        const std::size_t v = curriculum_detail::inner_split(h.size());
        double best = -1.0;
        for (const std::size_t n : grid) {
            std::size_t right = 0, scored = 0;
            double sum = 0.0;
            for (std::size_t t = 0; t < h.size(); ++t) {
                if (t >= n && t >= v && h[t] != 0.0 && sum != 0.0) {
                    ++scored;
                    if ((sum > 0.0) == (h[t] > 0.0)) { ++right; }
                }
                sum += h[t];
                if (t >= n) { sum -= h[t - n]; }
            }
            const double acc = scored > 0 ? static_cast<double>(right) / static_cast<double>(scored) : 0.0;
            if (acc > best) { best = acc; n_ = n; }
        }
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const auto h = d.history(i);
        if (h.size() < n_) { return {}; }
        double sum = 0.0;
        for (std::size_t k = h.size() - n_; k < h.size(); ++k) { sum += h[k]; }
        return curriculum_detail::from_direction(sum > 0.0 ? 1 : (sum < 0.0 ? -1 : 0));
    }
    std::string tuned() const override { return "lookback " + std::to_string(n_); }

private:
    std::size_t n_ = 20;
};

/// Mean reversion (Atlas 1, strategies/meanrev.hpp): the log price's z-score
/// against its last 20 rows; fade it only beyond the entry band, abstain inside.
class CurriculumZScore final : public CurriculumModel {
public:
    std::string name() const override { return "Mean reversion (z-score band)"; }
    std::string family() const override { return "statistical"; }
    std::string fit(const CurriculumDesign& d) override {
        const std::size_t n = d.train_rows();
        if (n < 80) { return "fewer than 80 rows"; }
        const double grid[] = {1.5, 2.0, 2.5};
        const std::size_t v = curriculum_detail::inner_split(n);
        double best = -1.0;
        for (const double e : grid) {
            std::size_t right = 0, scored = 0;
            for (std::size_t j = v; j < n; ++j) {
                const double z = zscore(d, j);
                const double r = d.target(j);
                if (!std::isfinite(z) || std::fabs(z) < e || r == 0.0) { continue; }
                ++scored;
                if ((z < 0.0) == (r > 0.0)) { ++right; }
            }
            // A band that never fires cannot be judged: prefer the one that fired and was right.
            const double acc = scored >= 10 ? static_cast<double>(right) / static_cast<double>(scored) : 0.0;
            if (acc > best) { best = acc; entry_ = e; }
        }
        return {};
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const double z = zscore(d, i);
        if (!std::isfinite(z) || std::fabs(z) < entry_) { return {}; }
        return curriculum_detail::from_direction(z > 0.0 ? -1 : 1);
    }
    std::string tuned() const override { return "entry |z| >= " + curriculum_detail::trim_double(entry_); }

private:
    static double zscore(const CurriculumDesign& d, std::size_t i) {
        if (i < kLook) { return curriculum_detail::nan(); }
        double s = 0.0, ss = 0.0;
        for (std::size_t k = i + 1 - kLook; k <= i; ++k) { const double l = d.level(k); s += l; ss += l * l; }
        const double m = s / kLook, var = ss / kLook - m * m;
        return var > 0.0 ? (d.level(i) - m) / std::sqrt(var) : curriculum_detail::nan();
    }
    static constexpr std::size_t kLook = 20;
    double entry_ = 2.0;
};

/// Cointegration / pairs / statistical arbitrage (Atlas 1,
/// strategies/cointegration.hpp and pairs_trade.hpp): the spread of this
/// instrument's log price against its pair, hedge ratio by least squares on
/// the training window; beyond two sigma the call is that this leg reverts.
class CurriculumPairs final : public CurriculumModel {
public:
    std::string name() const override { return "Pairs (cointegration)"; }
    std::string family() const override { return "statistical"; }
    std::string fit(const CurriculumDesign& d) override {
        if (d.track().pair.empty()) { return "no paired instrument on this track"; }
        const std::size_t n = d.train_rows();
        if (n < 60) { return "fewer than 60 rows"; }
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        for (std::size_t j = 0; j < n; ++j) {
            const double x = d.pair_level(j), y = d.level(j);
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        const double nn = static_cast<double>(n), den = nn * sxx - sx * sx;
        if (!(std::fabs(den) > 1e-12)) { return "pair does not move"; }
        beta_ = (nn * sxy - sx * sy) / den;
        alpha_ = (sy - beta_ * sx) / nn;
        double s = 0.0, ss = 0.0;
        for (std::size_t j = 0; j < n; ++j) {
            const double e = d.level(j) - alpha_ - beta_ * d.pair_level(j);
            s += e; ss += e * e;
        }
        mean_ = s / nn;
        sd_ = std::sqrt(std::max(0.0, ss / nn - mean_ * mean_));
        return sd_ > 0.0 ? std::string{} : "spread has no variance";
    }
    CurriculumCall predict(const CurriculumDesign& d, std::size_t i) const override {
        const double x = d.pair_level(i), y = d.level(i);
        if (!std::isfinite(x) || !std::isfinite(y)) { return {}; }
        const double z = (y - alpha_ - beta_ * x - mean_) / sd_;
        if (std::fabs(z) < 2.0) { return {}; }
        return curriculum_detail::from_direction(z > 0.0 ? -1 : 1);
    }
    std::string tuned() const override { return "beta " + curriculum_detail::trim_double(std::round(beta_ * 100.0) / 100.0); }

private:
    double alpha_ = 0.0, beta_ = 0.0, mean_ = 0.0, sd_ = 0.0;
};

/// Every forecaster in the tree that can be pointed at a single series, plus
/// the baselines they have to beat. Ensembles are added by the engine.
[[nodiscard]] inline std::vector<std::unique_ptr<CurriculumModel>>
curriculum_default_models(std::uint64_t seed = 0xA17A1Au) {
    std::vector<std::unique_ptr<CurriculumModel>> m;
    m.push_back(std::make_unique<CurriculumCoin>(seed));
    m.push_back(std::make_unique<CurriculumMajority>());
    m.push_back(std::make_unique<CurriculumPersistence>(false));
    m.push_back(std::make_unique<CurriculumPersistence>(true));
    m.push_back(std::make_unique<CurriculumLogistic>());
    m.push_back(std::make_unique<CurriculumSvm>());
    m.push_back(std::make_unique<CurriculumKnn>());
    m.push_back(std::make_unique<CurriculumForest>(seed + 1));
    m.push_back(std::make_unique<CurriculumGbdt>(seed + 2));
    m.push_back(std::make_unique<CurriculumMlp>(seed + 3));
    m.push_back(std::make_unique<CurriculumLstm>("LSTM", seed + 4));
    m.push_back(std::make_unique<CurriculumGru>("GRU", seed + 5));
    m.push_back(std::make_unique<CurriculumTransformer>("Transformer", seed + 6));
    m.push_back(std::make_unique<CurriculumArma>(2, 0));
    m.push_back(std::make_unique<CurriculumArma>(1, 1));
    m.push_back(std::make_unique<CurriculumOu>());
    m.push_back(std::make_unique<CurriculumMarkov>());
    m.push_back(std::make_unique<CurriculumRegimes>(seed + 7));
    m.push_back(std::make_unique<CurriculumRidge>());
    m.push_back(std::make_unique<CurriculumTree>());
    m.push_back(std::make_unique<CurriculumCnn>(seed + 8));
    m.push_back(std::make_unique<CurriculumAutoencoder>());
    m.push_back(std::make_unique<CurriculumVar>());
    m.push_back(std::make_unique<CurriculumKalman>());
    m.push_back(std::make_unique<CurriculumHmm>(seed + 9));
    m.push_back(std::make_unique<CurriculumHurst>());
    m.push_back(std::make_unique<CurriculumSeasonalAr>());
    m.push_back(std::make_unique<CurriculumDqn>(seed + 10));
    m.push_back(std::make_unique<CurriculumPolicy<PpoAgent>>("PPO (reinforcement)", seed + 11));
    m.push_back(std::make_unique<CurriculumPolicy<ActorCriticAgent>>("Actor-critic (reinforcement)", seed + 12));
    m.push_back(std::make_unique<CurriculumMomentumN>());
    m.push_back(std::make_unique<CurriculumZScore>());
    m.push_back(std::make_unique<CurriculumPairs>());
    return m;
}

// ---------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------

/// The ensembles the engine adds after the base models, in this order. All of
/// them weigh the base models by FINISHED stages only:
///   Vote       the majority of the learning models (baselines excluded)
///   Champion   the model with the best record so far
///   Hedge      exponential weights on the record
///   Stack      a logistic regression on every model's past calls -- it learns
///              whom to trust, whom to invert and whom to ignore
///   Stack (confident third)  the Stack, only when it is in its most
///              confident third (threshold from its own training rows)
///   Consensus 75%  the Vote, only when three quarters of the callers agree
///   Stack (confident 10% / 2%) and Consensus 90%  the same, stricter: the
///              ex-ante answer to "how accurate can it be if it calls rarely?"
inline constexpr const char* kCurriculumEnsembles[] = {
    "Vote", "Champion", "Hedge", "Stack", "Stack (confident third)", "Consensus 75%",
    "Stack (confident 10%)", "Stack (confident 2%)", "Consensus 90%"};

struct CurriculumOptions {
    std::int32_t first_days = 3;
    std::int32_t step_cap_days = 0;
    /// Scored past forecasts a model needs before it can be Champion.
    std::size_t champion_min = 20;
    /// Scored past rows the stacking meta-model needs before it calls.
    std::size_t stack_min = 60;
    /// Consensus calls only when at least this share of the learning models,
    /// and at least `consensus_min` of them, agree.
    double consensus = 0.75;
    std::size_t consensus_min = 6;
};

/// Everything a run produced. Rows are indexed from `first_row`, the first
/// forecast (stage 0's first test row).
struct CurriculumRun {
    std::vector<std::string> models;     ///< base models, then the ensembles (kCurriculumEnsembles)
    std::vector<std::string> families;
    std::size_t base_models = 0;
    std::vector<CurriculumStage> stages;
    std::size_t first_row = 0;
    std::vector<std::uint16_t> stage_of;                  ///< [row - first_row]
    std::vector<std::vector<CurriculumStoredCall>> calls; ///< [model][row - first_row]
    std::vector<std::vector<std::string>> notes;          ///< [model][stage]
    std::vector<std::vector<double>> hedge_weight;        ///< [stage][base model]
    std::vector<int> champion;                            ///< [stage] base model, or -1
    std::vector<double> fit_seconds;                      ///< [model]
    std::size_t lookahead_refusals = 0;
    std::size_t degenerate_features = 0;                  ///< summed over stages
};

namespace curriculum_detail {

/// What the stacking meta-model reads from one call: +1 sure up, -1 sure
/// down, 0 abstained. A rule without a probability gives its direction.
inline double signed_confidence(const CurriculumCall& c) noexcept {
    if (!c.made) { return 0.0; }
    if (std::isfinite(c.p_up)) { return 2.0 * (c.p_up - 0.5); }
    return static_cast<double>(c.dir);
}

/// +1 up, -1 down, 0 unchanged.
inline int outcome(const CurriculumTrack& tr, std::size_t i) noexcept {
    const double r = tr.ret(i);
    return r > 0.0 ? 1 : (r < 0.0 ? -1 : 0);
}

inline void finish(CurriculumCall& c, const CurriculumDesign& d) noexcept {
    if (!c.made) { return; }
    if (c.dir == 0) {
        if (std::isfinite(c.p_up)) { c.dir = c.p_up > 0.5 ? 1 : (c.p_up < 0.5 ? -1 : 0); }
        else if (std::isfinite(c.mu)) { c.dir = c.mu > 0.0 ? 1 : (c.mu < 0.0 ? -1 : 0); }
    }
    if (!std::isfinite(c.mu)) {
        if (std::isfinite(c.p_up)) { c.mu = c.p_up * d.mean_up() + (1.0 - c.p_up) * d.mean_down(); }
        else { c.mu = c.dir > 0 ? d.mean_up() : (c.dir < 0 ? d.mean_down() : 0.0); }
    }
    if (!(c.sigma > 0.0) || !std::isfinite(c.sigma)) { c.sigma = d.sd(); }
}

} // namespace curriculum_detail

[[nodiscard]] inline std::expected<CurriculumRun, CurriculumError>
curriculum_run(const CurriculumTrack& tr, std::vector<std::unique_ptr<CurriculumModel>>& models,
               const CurriculumOptions& opt = {}) {
    namespace cd = curriculum_detail;
    if (auto ok = curriculum_check_track(tr); !ok) { return std::unexpected(ok.error()); }
    CurriculumRun run;
    run.stages = curriculum_stages(tr, opt.first_days, opt.step_cap_days);
    if (run.stages.empty()) { return std::unexpected(CurriculumError::TooFewDays); }

    const std::size_t base = models.size();
    run.base_models = base;
    for (const auto& m : models) {
        run.models.push_back(m->name());
        run.families.push_back(m->family());
    }
    for (const char* e : kCurriculumEnsembles) {
        run.models.emplace_back(e);
        run.families.emplace_back("ensemble");
    }
    const std::size_t total = run.models.size();
    const std::size_t vote = base, champ = base + 1, hedge = base + 2;
    const std::size_t stack = base + 3, confident = base + 4, consensus = base + 5;
    const std::size_t confident10 = base + 6, confident2 = base + 7, consensus90 = base + 8;

    run.first_row = run.stages.front().test_begin;
    const std::size_t n = tr.rows() - run.first_row;
    run.stage_of.assign(n, 0);
    run.calls.assign(total, std::vector<CurriculumStoredCall>(n));
    run.notes.assign(total, std::vector<std::string>(run.stages.size()));
    run.fit_seconds.assign(total, 0.0);

    // The record the ensembles weigh: finished stages only.
    std::vector<std::size_t> right(base, 0), wrong(base, 0);
    std::size_t rounds = 0;
    // What the stacking meta-model learns from: every base model's
    // out-of-sample call on every scored row of the finished stages.
    std::vector<double> meta_x;
    std::vector<std::uint8_t> meta_y;
    std::size_t meta_rows = 0;

    for (const CurriculumStage& st : run.stages) {
        for (std::size_t i = st.test_begin; i < st.test_end; ++i) {
            run.stage_of[i - run.first_row] = static_cast<std::uint16_t>(st.index);
        }
        auto made = CurriculumDesign::make(tr, st);
        if (!made) {
            for (std::size_t m = 0; m < total; ++m) { run.notes[m][st.index] = "abstained: scaler refused"; }
            run.hedge_weight.emplace_back(base, 0.0);
            run.champion.push_back(-1);
            continue;
        }
        CurriculumDesign& d = *made;

        // Weights from the finished stages.
        std::vector<double> w(base, 0.0);
        int best = -1;
        if (rounds > 0) {
            const double eta = std::sqrt(8.0 * std::log(static_cast<double>(base)) / static_cast<double>(rounds));
            std::vector<double> loss(base);
            double least = std::numeric_limits<double>::infinity();
            for (std::size_t m = 0; m < base; ++m) {
                // A round a model sat out costs it half, as a coin would.
                const double scored = static_cast<double>(right[m] + wrong[m]);
                loss[m] = static_cast<double>(wrong[m]) + 0.5 * (static_cast<double>(rounds) - scored);
                least = std::min(least, loss[m]);
            }
            double wsum = 0.0;
            for (std::size_t m = 0; m < base; ++m) { w[m] = std::exp(-eta * (loss[m] - least)); wsum += w[m]; }
            for (double& v : w) { v /= wsum; }
            double best_rate = -1.0;
            for (std::size_t m = 0; m < base; ++m) {
                const std::size_t scored = right[m] + wrong[m];
                if (scored < opt.champion_min) { continue; }
                const double rate = (static_cast<double>(right[m]) + 1.0) / (static_cast<double>(scored) + 2.0);
                if (rate > best_rate) { best_rate = rate; best = static_cast<int>(m); }
            }
        }
        run.hedge_weight.push_back(w);
        run.champion.push_back(best);
        run.notes[champ][st.index] = best >= 0 ? "follows " + run.models[static_cast<std::size_t>(best)]
                                               : "abstained: no track record yet";
        run.notes[hedge][st.index] = rounds > 0 ? std::string{} : "abstained: no track record yet";

        // Stacking: a logistic regression on the base models' past calls.
        std::optional<LogisticRegression> meta;
        double confident_at = std::numeric_limits<double>::infinity();
        double confident10_at = confident_at, confident2_at = confident_at;
        if (meta_rows >= opt.stack_min) {
            LogisticParams prm;
            prm.l2 = 1e-2;
            auto fitted = LogisticRegression::fit(meta_x, meta_rows, base, meta_y, prm);
            if (fitted) {
                meta = std::move(*fitted);
                // The confident third: the threshold the meta-model's own
                // training rows put a third of its calls above.
                std::vector<double> conf(meta_rows);
                for (std::size_t k = 0; k < meta_rows; ++k) {
                    conf[k] = std::fabs(meta->probability({meta_x.data() + k * base, base}) - 0.5);
                }
                std::sort(conf.begin(), conf.end());
                confident_at = conf[(2 * meta_rows) / 3];
                confident10_at = conf[(9 * meta_rows) / 10];
                confident2_at = conf[(49 * meta_rows) / 50];
                const auto wts = meta->weights();
                std::vector<std::size_t> idx(base);
                for (std::size_t m = 0; m < base; ++m) { idx[m] = m; }
                std::stable_sort(idx.begin(), idx.end(),
                                 [&wts](std::size_t a, std::size_t b) { return std::fabs(wts[a]) > std::fabs(wts[b]); });
                std::string lean;
                for (std::size_t k = 0; k < 3 && k < base; ++k) {
                    lean += (k ? ", " : "") + run.models[idx[k]] + " "
                          + (wts[idx[k]] >= 0.0 ? "+" : "") + cd::trim_double(std::round(wts[idx[k]] * 100.0) / 100.0);
                }
                run.notes[stack][st.index] = "learned from " + std::to_string(meta_rows) + " past calls; leans on " + lean;
                run.notes[confident][st.index] = "acts when |P(up) - 0.5| >= " + cd::trim_double(std::round(confident_at * 1000.0) / 1000.0);
                run.notes[confident10][st.index] = "acts when |P(up) - 0.5| >= " + cd::trim_double(std::round(confident10_at * 1000.0) / 1000.0);
                run.notes[confident2][st.index] = "acts when |P(up) - 0.5| >= " + cd::trim_double(std::round(confident2_at * 1000.0) / 1000.0);
            } else {
                run.notes[stack][st.index] = run.notes[confident][st.index] = run.notes[confident10][st.index] =
                    run.notes[confident2][st.index] = "abstained: meta fit refused";
            }
        } else {
            run.notes[stack][st.index] = run.notes[confident][st.index] = run.notes[confident10][st.index] =
                run.notes[confident2][st.index] = "abstained: fewer than " + std::to_string(opt.stack_min) + " past calls";
        }

        for (std::size_t m = 0; m < base; ++m) {
            const auto t0 = std::chrono::steady_clock::now();
            d.set_now(st.train_rows);
            const std::string why = models[m]->fit(d);
            run.notes[m][st.index] = why.empty() ? models[m]->tuned() : "abstained: " + why;
            if (why.empty()) {
                for (std::size_t i = st.test_begin; i < st.test_end; ++i) {
                    d.set_now(i);
                    CurriculumCall c = models[m]->predict(d, i);
                    cd::finish(c, d);
                    run.calls[m][i - run.first_row] = c;
                }
            }
            run.fit_seconds[m] += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }

        for (std::size_t i = st.test_begin; i < st.test_end; ++i) {
            const std::size_t r = i - run.first_row;
            // Vote: the models that learn (not the baselines), one call each.
            std::size_t ups = 0, downs = 0;
            double mus = 0.0;
            for (std::size_t m = 0; m < base; ++m) {
                const CurriculumCall c = run.calls[m][r];
                if (!c.made || run.families[m] == "baseline" || c.dir == 0) { continue; }
                (c.dir > 0 ? ups : downs) += 1;
                mus += c.mu;
            }
            if (ups + downs > 0) {
                CurriculumCall c = cd::from_probability(static_cast<double>(ups) / static_cast<double>(ups + downs),
                                                        mus / static_cast<double>(ups + downs));
                cd::finish(c, d);
                run.calls[vote][r] = c;
            }
            // Consensus: the vote, only when it is lopsided enough.
            const std::size_t callers = ups + downs;
            if (callers >= opt.consensus_min
                && static_cast<double>(std::max(ups, downs)) >= opt.consensus * static_cast<double>(callers)) {
                run.calls[consensus][r] = run.calls[vote][r];
            }
            if (callers >= opt.consensus_min
                && static_cast<double>(std::max(ups, downs)) >= 0.9 * static_cast<double>(callers)) {
                run.calls[consensus90][r] = run.calls[vote][r];
            }
            if (meta) {
                std::vector<double> sx(base);
                for (std::size_t m = 0; m < base; ++m) { sx[m] = cd::signed_confidence(run.calls[m][r]); }
                const double p = meta->probability(sx);
                CurriculumCall c = cd::from_probability(p);
                cd::finish(c, d);
                run.calls[stack][r] = c;
                if (std::fabs(p - 0.5) >= confident_at) { run.calls[confident][r] = c; }
                if (std::fabs(p - 0.5) >= confident10_at) { run.calls[confident10][r] = c; }
                if (std::fabs(p - 0.5) >= confident2_at) { run.calls[confident2][r] = c; }
            }
            if (best >= 0) { run.calls[champ][r] = run.calls[static_cast<std::size_t>(best)][r]; }
            if (rounds > 0) {
                double wu = 0.0, wall = 0.0, wmu = 0.0;
                for (std::size_t m = 0; m < base; ++m) {
                    const CurriculumCall c = run.calls[m][r];
                    if (!c.made || c.dir == 0) { continue; }
                    wall += w[m];
                    wmu += w[m] * c.mu;
                    if (c.dir > 0) { wu += w[m]; }
                }
                if (wall > 0.0) {
                    CurriculumCall c = cd::from_probability(wu / wall, wmu / wall);
                    cd::finish(c, d);
                    run.calls[hedge][r] = c;
                }
            }
        }

        // The stage is over: its outcomes join the record.
        for (std::size_t i = st.test_begin; i < st.test_end; ++i) {
            const int out = cd::outcome(tr, i);
            if (out == 0) { continue; }
            ++rounds;
            for (std::size_t m = 0; m < base; ++m) {
                const CurriculumCall c = run.calls[m][i - run.first_row];
                meta_x.push_back(cd::signed_confidence(c));
                if (!c.made) { continue; }
                (c.dir == out ? right[m] : wrong[m]) += 1;
            }
            meta_y.push_back(out > 0 ? 1 : 0);
            ++meta_rows;
        }
        run.lookahead_refusals += d.refusals();
        run.degenerate_features += d.degenerate_features();
    }
    return run;
}

// ---------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------

/// Right and wrong over a set of rows.
struct CurriculumTally {
    std::size_t forecasts = 0;   ///< calls made
    std::size_t abstained = 0;
    std::size_t flat = 0;        ///< calls on bars that closed unchanged
    std::size_t right = 0, wrong = 0;
    std::size_t no_direction = 0;   ///< calls with no direction: counted in `wrong` too
    std::size_t ups = 0;         ///< scored bars that rose, for "always up" on the same bars
    double brier_sum = 0.0;
    std::size_t brier_n = 0;
    std::size_t trades = 0, trade_wins = 0;
    double net_bp = 0.0;
    double net_bp_sq = 0.0;
    /// MAGNITUDE (models/magnitude.hpp): |move| summed over right and wrong
    /// calls, and direction x move over every directed call. Accuracy counts a
    /// 5 bp day and a 300 bp day alike; these do not.
    double abs_right = 0.0, abs_wrong = 0.0, edge = 0.0;
    std::size_t directed = 0;

    /// Share of the market's movement the calls were on the right side of.
    [[nodiscard]] double weighted_accuracy() const noexcept {
        const double t = abs_right + abs_wrong;
        return t > 0.0 ? abs_right / t : std::numeric_limits<double>::quiet_NaN();
    }
    /// Mean direction x move, bp: what the accuracy is worth before costs.
    [[nodiscard]] double gross_bp() const noexcept {
        return directed > 0 ? 1e4 * edge / static_cast<double>(directed) : std::numeric_limits<double>::quiet_NaN();
    }

    /// Mean net bp per trade over its standard error: is the P&L luck?
    [[nodiscard]] double net_t() const noexcept {
        if (trades < 2) { return std::numeric_limits<double>::quiet_NaN(); }
        const double n = static_cast<double>(trades);
        const double mean = net_bp / n;
        const double var = (net_bp_sq - n * mean * mean) / (n - 1.0);
        return var > 0.0 ? mean / std::sqrt(var / n) : std::numeric_limits<double>::quiet_NaN();
    }

    [[nodiscard]] std::size_t scored() const noexcept { return right + wrong; }
    [[nodiscard]] double accuracy() const noexcept {
        return scored() > 0 ? static_cast<double>(right) / static_cast<double>(scored())
                            : std::numeric_limits<double>::quiet_NaN();
    }
    [[nodiscard]] double brier() const noexcept {
        return brier_n > 0 ? brier_sum / static_cast<double>(brier_n) : std::numeric_limits<double>::quiet_NaN();
    }
};

/// Did acting on this call clear the round trip? Trades only when the
/// expected move exceeds the cost. Returns the net bp, or NaN for no trade.
[[nodiscard]] inline double curriculum_trade_bp(const CurriculumTrack& tr, const CurriculumCall& c,
                                                std::size_t i) noexcept {
    if (!tr.tradable || i >= tr.cost_bp.size() || !c.made || c.dir == 0
        || !(std::fabs(c.mu) * 1e4 > tr.cost_bp[i])) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return static_cast<double>(c.dir) * tr.ret(i) * 1e4 - tr.cost_bp[i];
}

[[nodiscard]] inline CurriculumTally
curriculum_tally(const CurriculumTrack& tr, const CurriculumRun& run, std::size_t model,
                 std::size_t from_row, std::size_t to_row) {
    CurriculumTally t;
    for (std::size_t i = from_row; i < to_row; ++i) {
        const CurriculumCall c = run.calls[model][i - run.first_row];
        if (!c.made) { ++t.abstained; continue; }
        ++t.forecasts;
        const int out = curriculum_detail::outcome(tr, i);
        const double net = curriculum_trade_bp(tr, c, i);
        if (std::isfinite(net)) {
            ++t.trades;
            t.net_bp += net;
            t.net_bp_sq += net * net;
            if (net > 0.0) { ++t.trade_wins; }
        }
        if (c.dir != 0) {
            ++t.directed;
            t.edge += static_cast<double>(c.dir) * tr.ret(i);
        }
        if (out == 0) { ++t.flat; continue; }
        if (out > 0) { ++t.ups; }
        (c.dir == out ? t.right : t.wrong) += 1;
        (c.dir == out ? t.abs_right : t.abs_wrong) += std::fabs(tr.ret(i));
        if (c.dir == 0) { ++t.no_direction; }
        if (std::isfinite(c.p_up)) {
            const double e = c.p_up - (out > 0 ? 1.0 : 0.0);
            t.brier_sum += e * e;
            ++t.brier_n;
        }
    }
    return t;
}

/// A model's whole record on a track, and how much it means.
struct CurriculumSummary {
    CurriculumTally all;
    double accuracy = 0.0;
    double lo95 = 0.0, hi95 = 0.0;   ///< Wilson interval
    double z_vs_half = 0.0;
    double p_vs_half = 1.0;          ///< two-sided
    double p_adjusted = 1.0;         ///< Bonferroni over `tests`
    double up_rate = 0.0;            ///< "always up" on the same scored bars
    double best_constant = 0.0;
    double z_vs_constant = 0.0;
    double p_constant_adjusted = 1.0;   ///< one-sided (better than), Bonferroni over `tests`
    bool have_price = false;
    ForecastScore price{};           ///< against the random walk
    ForecastVerdict verdict = ForecastVerdict::NotEnoughEvidence;
};

[[nodiscard]] inline CurriculumSummary
curriculum_summary(const CurriculumTrack& tr, const CurriculumRun& run, std::size_t model,
                   std::size_t tests) {
    CurriculumSummary s;
    s.all = curriculum_tally(tr, run, model, run.first_row, tr.rows());
    const std::size_t n = s.all.scored();
    if (n > 0) {
        const double nn = static_cast<double>(n);
        const double p = s.all.accuracy();
        const double z = 1.959963984540054;
        const double centre = (p + z * z / (2.0 * nn)) / (1.0 + z * z / nn);
        const double half = z * std::sqrt(p * (1.0 - p) / nn + z * z / (4.0 * nn * nn)) / (1.0 + z * z / nn);
        s.accuracy = p;
        s.lo95 = centre - half;
        s.hi95 = centre + half;
        s.z_vs_half = (p - 0.5) / std::sqrt(0.25 / nn);
        s.p_vs_half = std::erfc(std::fabs(s.z_vs_half) / std::sqrt(2.0));
        s.p_adjusted = std::min(1.0, s.p_vs_half * static_cast<double>(tests > 0 ? tests : 1));
        s.up_rate = static_cast<double>(s.all.ups) / nn;
        s.best_constant = std::max(s.up_rate, 1.0 - s.up_rate);
        s.z_vs_constant = (p - s.best_constant) / std::sqrt(0.25 / nn);
        // Held to the same bar as the coin test: corrected for every test run.
        s.p_constant_adjusted = std::min(1.0, 0.5 * std::erfc(s.z_vs_constant / std::sqrt(2.0))
                                                  * static_cast<double>(tests > 0 ? tests : 1));
    }
    std::vector<ForecastPoint> pts;
    for (std::size_t i = run.first_row; i < tr.rows(); ++i) {
        const CurriculumCall c = run.calls[model][i - run.first_row];
        if (!c.made || !std::isfinite(c.mu)) { continue; }
        ForecastPoint fp;
        fp.ts_ns = tr.t[i] * 1'000'000'000;
        fp.anchor = tr.anchor[i];
        fp.actual = tr.actual[i];
        fp.predicted = tr.anchor[i] * std::exp(c.mu);
        fp.lo = tr.anchor[i] * std::exp(c.mu - c.sigma);
        fp.hi = tr.anchor[i] * std::exp(c.mu + c.sigma);
        pts.push_back(fp);
    }
    if (auto sc = score_forecasts(pts); sc) {
        s.have_price = true;
        s.price = *sc;
        s.verdict = judge_forecast(s.price);
    }
    return s;
}

} // namespace altair
