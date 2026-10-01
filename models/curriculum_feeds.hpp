// models/curriculum_feeds.hpp -- other models' forecasts as inputs.
//
// A finished curriculum run holds, for every row from its first test block
// on, each model's out-of-sample forecast: made at the row's decision, from a
// model fitted on finished stages only. Those forecasts are information known
// at that moment, so they can be another track's inputs -- the way the INDIA
// VIX model's forecast fed the index tracks.
//
// THE JOIN IS AS-OF, AND THAT IS THE WHOLE LOOK-AHEAD ARGUMENT. A row decided
// at t reads, from each feed, the latest value made at or before t: an
// intraday source only from the same day, a daily one at most a week old.
// Nothing made after t can reach it, and a feed built from a run inherits
// that run's guarantee that each forecast saw only its past.
// models/tests/test_curriculum_feeds.cpp changes one outcome and checks that
// no second-pass call made before it moves.

#pragma once

#include <models/band_curriculum.hpp>
#include <models/curriculum.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace altair {

/// Forecasts made by a finished run, stamped with the moment each was made.
struct ModelFeed {
    std::string source;                ///< the track it came from, for column names
    std::vector<std::string> names;    ///< one per column
    std::vector<std::int64_t> t;       ///< decision stamps, strictly ascending
    std::vector<float> v;              ///< t.size() x names.size(), row-major
    bool intraday = true;              ///< true: a value is used only on the day it was made
    std::int64_t max_age_days = 7;     ///< daily sources: the oldest value still used
    /// true: a row without a value is dropped (it precedes the feed). false:
    /// the gap reads 0 -- "no opinion" on the signed-confidence scale.
    bool required = false;

    [[nodiscard]] std::size_t cols() const noexcept { return names.size(); }
    [[nodiscard]] std::size_t rows() const noexcept { return t.size(); }
};

namespace feed_detail {

inline constexpr std::int64_t kDaySec = 86'400;

[[nodiscard]] inline std::int64_t day_of(std::int64_t stamp) noexcept {
    return stamp >= 0 ? stamp / kDaySec : -((-stamp + kDaySec - 1) / kDaySec);
}

/// Index of the latest feed row made at or before `t` that may be used at
/// `t`, or rows() when there is none.
[[nodiscard]] inline std::size_t as_of(const ModelFeed& f, std::int64_t t) noexcept {
    const auto it = std::upper_bound(f.t.begin(), f.t.end(), t);
    if (it == f.t.begin()) { return f.rows(); }
    const auto k = static_cast<std::size_t>(it - f.t.begin()) - 1;
    const std::int64_t age = day_of(t) - day_of(f.t[k]);
    if (f.intraday ? age != 0 : age > f.max_age_days) { return f.rows(); }
    return k;
}

} // namespace feed_detail

/// One column per named model (empty: every base model but the coin): its
/// signed confidence, +1 sure up, -1 sure down, 0 abstained.
[[nodiscard]] inline ModelFeed feed_from_models(const CurriculumTrack& tr, const CurriculumRun& run,
                                                std::vector<std::string> names, bool intraday) {
    if (names.empty()) {
        for (std::size_t m = 0; m < run.base_models; ++m) {
            if (run.models[m] != "Coin flip") { names.push_back(run.models[m]); }
        }
    }
    ModelFeed f;
    f.source = tr.name;
    f.intraday = intraday;
    std::vector<std::size_t> idx;
    for (const auto& n : names) {
        const auto it = std::find(run.models.begin(), run.models.end(), n);
        if (it == run.models.end()) { continue; }
        idx.push_back(static_cast<std::size_t>(it - run.models.begin()));
        f.names.push_back(n);
    }
    for (std::size_t i = run.first_row; i < tr.rows(); ++i) {
        f.t.push_back(tr.t[i]);
        for (const std::size_t m : idx) {
            const CurriculumCall c = run.calls[m][i - run.first_row];
            f.v.push_back(static_cast<float>(curriculum_detail::signed_confidence(c)));
        }
    }
    return f;
}

/// What a call says beyond its direction: the drift and the scale it forecast.
enum class FeedField : std::uint8_t { Mu, LogSigma };

/// One column per (model, field). A missing value carries the model's last
/// one forward (a drift with no call reads 0); a row before the first value
/// of every column is left out of the feed.
[[nodiscard]] inline ModelFeed feed_from_fields(const CurriculumTrack& tr, const CurriculumRun& run,
                                                const std::vector<std::pair<std::string, FeedField>>& want,
                                                bool intraday) {
    ModelFeed f;
    f.source = tr.name;
    f.intraday = intraday;
    std::vector<std::pair<std::size_t, FeedField>> idx;
    for (const auto& [n, field] : want) {
        const auto it = std::find(run.models.begin(), run.models.end(), n);
        if (it == run.models.end()) { continue; }
        idx.emplace_back(static_cast<std::size_t>(it - run.models.begin()), field);
        f.names.push_back(n + (field == FeedField::Mu ? " drift" : " log scale"));
    }
    std::vector<double> last(idx.size(), std::numeric_limits<double>::quiet_NaN());
    for (std::size_t i = run.first_row; i < tr.rows(); ++i) {
        for (std::size_t k = 0; k < idx.size(); ++k) {
            const CurriculumCall c = run.calls[idx[k].first][i - run.first_row];
            if (idx[k].second == FeedField::Mu) {
                last[k] = c.made && std::isfinite(c.mu) ? c.mu : 0.0;
            } else if (c.made && c.sigma > 0.0 && std::isfinite(c.sigma)) {
                last[k] = std::log(c.sigma);
            }
        }
        if (std::any_of(last.begin(), last.end(), [](double x) { return !std::isfinite(x); })) { continue; }
        f.t.push_back(tr.t[i]);
        for (const double x : last) { f.v.push_back(static_cast<float>(x)); }
    }
    return f;
}

/// One column per named band model: the log half-width of its calibrated 80 %
/// band. An abstention carries the last value forward.
[[nodiscard]] inline ModelFeed feed_from_bands(const CurriculumTrack& tr, const BandRun& run,
                                               const std::vector<std::string>& names, bool intraday) {
    ModelFeed f;
    f.source = tr.name;
    f.intraday = intraday;
    std::vector<std::size_t> idx;
    for (const auto& n : names) {
        const auto it = std::find(run.models.begin(), run.models.end(), n);
        if (it == run.models.end()) { continue; }
        idx.push_back(static_cast<std::size_t>(it - run.models.begin()));
        f.names.push_back(n + " log band");
    }
    std::vector<double> last(idx.size(), std::numeric_limits<double>::quiet_NaN());
    for (std::size_t i = run.first_row; i < tr.rows(); ++i) {
        for (std::size_t k = 0; k < idx.size(); ++k) {
            const float h = run.half[idx[k]][i - run.first_row];
            if (h > 0.0f && std::isfinite(h)) { last[k] = std::log(static_cast<double>(h)); }
        }
        if (std::any_of(last.begin(), last.end(), [](double x) { return !std::isfinite(x); })) { continue; }
        f.t.push_back(tr.t[i]);
        for (const double x : last) { f.v.push_back(static_cast<float>(x)); }
    }
    return f;
}

/// What attaching cost: rows dropped for a required feed, gaps read as 0.
struct FeedAttachInfo {
    std::size_t rows_in = 0, rows_out = 0;
    std::size_t before_start = 0;    ///< rows decided before `start_t`
    std::size_t no_required = 0;     ///< rows a required feed had no value for
    std::size_t gaps = 0;            ///< (row, feed) pairs read as "no opinion"
};

/// `base` with every feed's columns appended, under `name`. Rows decided
/// before `start_t` are dropped, as are rows a required feed has no value for;
/// days are renumbered so the schedule still counts whole days from zero.
[[nodiscard]] inline std::expected<CurriculumTrack, CurriculumError>
curriculum_attach_feeds(const CurriculumTrack& base, std::span<const ModelFeed* const> feeds,
                        const std::string& name, std::int64_t start_t, FeedAttachInfo& info) {
    std::size_t extra = 0;
    for (const ModelFeed* f : feeds) {
        if (f == nullptr || f->v.size() != f->rows() * f->cols()) { return std::unexpected(CurriculumError::BadTrack); }
        for (std::size_t k = 1; k < f->rows(); ++k) {
            if (f->t[k] <= f->t[k - 1]) { return std::unexpected(CurriculumError::BadTrack); }
        }
        extra += f->cols();
    }
    if (base.p + extra > kCurriculumMaxFeatures) { return std::unexpected(CurriculumError::TooManyFeatures); }

    CurriculumTrack tr;
    tr.name = name;
    tr.instrument = base.instrument;
    tr.horizon = base.horizon;
    tr.tradable = base.tradable;
    tr.feature_names = base.feature_names;
    for (const ModelFeed* f : feeds) {
        for (const auto& n : f->names) { tr.feature_names.push_back(f->source + ": " + n); }
    }
    tr.p = tr.feature_names.size();
    tr.seq_cols = base.seq_cols;
    tr.season = base.season;
    tr.pair_name = base.pair_name;

    info = FeedAttachInfo{};
    info.rows_in = base.rows();
    std::vector<std::size_t> at(feeds.size());
    std::int32_t day = -1, last_base_day = -1;
    for (std::size_t i = 0; i < base.rows(); ++i) {
        if (base.t[i] < start_t) { ++info.before_start; continue; }
        bool keep = true;
        for (std::size_t k = 0; k < feeds.size(); ++k) {
            at[k] = feed_detail::as_of(*feeds[k], base.t[i]);
            if (at[k] == feeds[k]->rows() && feeds[k]->required) { keep = false; }
        }
        if (!keep) { ++info.no_required; continue; }
        const auto row = base.x.begin() + static_cast<std::ptrdiff_t>(i * base.p);
        tr.x.insert(tr.x.end(), row, row + static_cast<std::ptrdiff_t>(base.p));
        for (std::size_t k = 0; k < feeds.size(); ++k) {
            const ModelFeed& f = *feeds[k];
            if (at[k] == f.rows()) {
                tr.x.insert(tr.x.end(), f.cols(), 0.0);
                ++info.gaps;
                continue;
            }
            for (std::size_t c = 0; c < f.cols(); ++c) { tr.x.push_back(static_cast<double>(f.v[at[k] * f.cols() + c])); }
        }
        if (base.day[i] != last_base_day) { ++day; last_base_day = base.day[i]; }
        tr.day.push_back(day);
        tr.t.push_back(base.t[i]);
        tr.t_out.push_back(base.t_out[i]);
        tr.anchor.push_back(base.anchor[i]);
        tr.actual.push_back(base.actual[i]);
        if (!base.cost_bp.empty()) { tr.cost_bp.push_back(base.cost_bp[i]); }
        if (!base.pair.empty()) { tr.pair.push_back(base.pair[i]); }
        if (!base.slot.empty()) { tr.slot.push_back(base.slot[i]); }
    }
    info.rows_out = tr.rows();
    if (const auto ok = curriculum_check_track(tr); !ok) { return std::unexpected(ok.error()); }
    return tr;
}

} // namespace altair
