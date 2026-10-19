// app/forecast_leak_check.hpp -- proof, not care, that no forecast saw its
// future.
//
// HOW A LEAK GETS IN. A backtest leaks when a decision at time T uses
// something not known at T. In a forecast pipeline it comes in by these
// doors, and every one of them has been seen in real research code:
//   1. A feature computed with later data: a centred or forward window, a
//      rolling statistic that includes the current bar's outcome, a
//      same-day close used at 10:15, a series joined by date to an
//      intraday row (the day's VIX close at 10:15).
//   2. Normalisation over the whole sample: a scaler, a z-score or a PCA
//      fitted on every row, so test rows shape the training inputs.
//   3. Targets that overlap the split: a label whose horizon reaches past
//      the end of the training window into the block being forecast.
//   4. Selection on the future: keeping only days that turned out complete,
//      only stocks that still exist, only the model that did best.
//   5. Tuning on the test block: a hyper-parameter, a threshold or a
//      "top x %" cut-off chosen with the rows it is then scored on.
//   6. Stacking on in-sample predictions: an ensemble or a second-pass
//      model fed a first model's fitted values rather than the forecasts it
//      made before each outcome.
//   7. A back door in the code: a model handed the whole track, or a
//      revised history (bars corrected after the fact) treated as what was
//      known then.
//
// THE TEST. A forecast made at T is a function of what was known at T. So
// give the pipeline a different world after T, and it must say exactly the
// same things up to T:
//   builders  rebuild every track from the bars known at the cut (later bars
//             removed), and again from bars whose prices after the cut were
//             all moved; every row decided by the cut must come out bit for
//             bit the same -- its outcome too, where that was known by then.
//   models    rerun the whole curriculum -- every model, every ensemble,
//             every 80 % band -- on the rows up to the cut, with the cut
//             row's own outcome changed; every call up to and including the
//             cut row must be bit for bit the same.
// A difference names the first row and field that moved: by definition, a
// leak. This catches doors 1-7 whatever their cause, including ones nobody
// has thought of yet, because it tests the property instead of the code.

#pragma once

#include <app/data_audit.hpp>
#include <app/forecast_tracks.hpp>
#include <models/band_curriculum.hpp>
#include <models/curriculum.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace altair::forecast_leak {

namespace da = altair::data_audit;
namespace ft = altair::forecast_tracks;
using Bars = std::vector<da::AuditBar>;

/// When a bar is complete and its prices known: a daily bar at the 15:30
/// close; an intraday bar at its end, the session's last bar at 15:30.
[[nodiscard]] inline std::int64_t known_at(const da::AuditBar& b, int tf) noexcept {
    const std::int64_t close = da::audit_day(b.t) * ft::kDaySec + ft::kCloseSec;
    if (tf == da::kDailyTf) { return close; }
    const std::int64_t end = b.t + static_cast<std::int64_t>(tf) * 60;
    return b.t < close && end > close ? close : end;   // RULE 11: proven -- only the session's last bar runs past 15:30
}

/// The bars known by `cut`.
[[nodiscard]] inline Bars known_by(const Bars& bars, int tf, std::int64_t cut) {
    Bars out;
    for (const auto& b : bars) {
        if (known_at(b, tf) <= cut) { out.push_back(b); }
    }
    return out;
}

/// The same bars with every price after `cut` moved, each bar by its own
/// factor: a different future, the same past.
[[nodiscard]] inline Bars other_future(const Bars& bars, int tf, std::int64_t cut) {
    Bars out = bars;
    for (std::size_t k = 0; k < out.size(); ++k) {
        if (known_at(out[k], tf) <= cut) { continue; }
        const double f = std::exp(0.03 * std::sin(1.3 * static_cast<double>(k) + 0.7));
        out[k].o *= f;
        out[k].h *= f;
        out[k].l *= f;
        out[k].c *= f;
    }
    return out;
}

/// How a builder's inputs are transformed: identity, known by the cut, or
/// with another future after it.
struct Transform {
    std::function<Bars(const Bars&, int)> bars;
    std::function<ft::VixForecast(const ft::VixForecast&)> vix_forecast;
};

[[nodiscard]] inline Transform identity() {
    return {[](const Bars& b, int) { return b; }, [](const ft::VixForecast& f) { return f; }};
}
[[nodiscard]] inline Transform truncate_at(std::int64_t cut) {
    return {[cut](const Bars& b, int tf) { return known_by(b, tf, cut); },
            [cut](const ft::VixForecast& f) {
                ft::VixForecast out;
                for (const auto& [day, p] : f) {
                    if (day * ft::kDaySec + ft::kCloseSec <= cut) { out.emplace(day, p); }
                }
                return out;
            }};
}
[[nodiscard]] inline Transform perturb_after(std::int64_t cut) {
    return {[cut](const Bars& b, int tf) { return other_future(b, tf, cut); },
            [cut](const ft::VixForecast& f) {
                ft::VixForecast out = f;
                for (auto& [day, p] : out) {
                    if (day * ft::kDaySec + ft::kCloseSec > cut) { p = 1.0 - p; }
                }
                return out;
            }};
}

/// What a comparison found: items compared, items that differed, the first.
struct Diff {
    std::size_t compared = 0, differ = 0;
    std::string first;
    void note(const std::string& what) {
        if (differ++ == 0) { first = what; }
    }
    [[nodiscard]] bool clean() const noexcept { return differ == 0; }
};

namespace detail {

[[nodiscard]] inline bool same(double a, double b) noexcept { return a == b || (std::isnan(a) && std::isnan(b)); }
[[nodiscard]] inline bool same(float a, float b) noexcept { return a == b || (std::isnan(a) && std::isnan(b)); }

/// Rows of `t` chosen by `keep`, in order.
template <class Keep>
[[nodiscard]] std::vector<std::size_t> rows_where(const CurriculumTrack& t, Keep keep) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < t.rows(); ++i) {
        if (keep(i)) { out.push_back(i); }
    }
    return out;
}

} // namespace detail

/// Rows of two builds of one track, field by field. `rows_a` and `rows_b`
/// pick the rows to set side by side; `outcome_known(i)` (on `a`) says
/// whether row i's outcome is compared too.
template <class Known>
[[nodiscard]] Diff compare_rows(const CurriculumTrack& a, const std::vector<std::size_t>& rows_a, const CurriculumTrack& b,
                                const std::vector<std::size_t>& rows_b, Known outcome_known) {
    Diff d;
    if (a.p != b.p || a.feature_names != b.feature_names) {
        d.note("the feature set differs");
        return d;
    }
    if (rows_a.size() != rows_b.size()) {
        d.note(std::to_string(rows_a.size()) + " rows against " + std::to_string(rows_b.size()));
    }
    const std::size_t n = std::min(rows_a.size(), rows_b.size());
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t i = rows_a[k], j = rows_b[k];
        ++d.compared;
        const std::string at = "row decided " + da::format_audit_time(a.t[i], false) + ": ";
        if (a.t[i] != b.t[j]) { d.note(at + "decision time"); continue; }
        if (a.t_out[i] != b.t_out[j]) { d.note(at + "outcome time"); continue; }
        if (a.day[i] != b.day[j]) { d.note(at + "day ordinal"); continue; }
        if (!detail::same(a.anchor[i], b.anchor[j])) { d.note(at + "price at the decision"); continue; }
        bool ok = true;
        for (std::size_t c = 0; c < a.p && ok; ++c) {
            if (!detail::same(a.x[i * a.p + c], b.x[j * b.p + c])) {
                d.note(at + "feature '" + a.feature_names[c] + "'");
                ok = false;
            }
        }
        if (!ok) { continue; }
        if (!a.slot.empty() && (b.slot.size() <= j || a.slot[i] != b.slot[j])) { d.note(at + "slot"); continue; }
        if (!a.pair.empty() && (b.pair.size() <= j || !detail::same(a.pair[i], b.pair[j]))) { d.note(at + "paired price"); continue; }
        if (!a.cost_bp.empty() && (b.cost_bp.size() <= j || !detail::same(a.cost_bp[i], b.cost_bp[j]))) { d.note(at + "cost"); continue; }
        if (outcome_known(i) && !detail::same(a.actual[i], b.actual[j])) { d.note(at + "outcome"); continue; }
    }
    return d;
}

/// A builder, given how to transform its inputs.
using Build = std::function<CurriculumTrack(const Transform&)>;

struct BuilderCheck {
    Diff truncated;   ///< later bars removed: rows whose outcome was known by the cut
    Diff perturbed;   ///< later prices moved: rows decided by the cut
};

/// Rebuild a track from the world as known at `cut`, and from a world with a
/// different future; `full` is the track built from everything.
[[nodiscard]] inline BuilderCheck check_builder(const Build& build, const CurriculumTrack& full, std::int64_t cut) {
    BuilderCheck out;
    {
        const CurriculumTrack t = build(truncate_at(cut));
        const auto known = [cut](const CurriculumTrack& tr) {
            return detail::rows_where(tr, [&tr, cut](std::size_t i) { return tr.t_out[i] <= cut; });
        };
        out.truncated = compare_rows(full, known(full), t, known(t), [](std::size_t) { return true; });
    }
    {
        const CurriculumTrack t = build(perturb_after(cut));
        const auto decided = [cut](const CurriculumTrack& tr) {
            return detail::rows_where(tr, [&tr, cut](std::size_t i) { return tr.t[i] <= cut; });
        };
        out.perturbed = compare_rows(full, decided(full), t, decided(t),
                                     [&full, cut](std::size_t i) { return full.t_out[i] <= cut; });
    }
    return out;
}

/// The first `last + 1` rows of a track, row `last`'s own outcome changed:
/// what a model must not be able to tell from the real one when it decides.
[[nodiscard]] inline CurriculumTrack prefix_with_other_outcome(const CurriculumTrack& tr, std::size_t last) {
    CurriculumTrack t = tr;
    const std::size_t n = last + 1;
    t.x.resize(n * t.p);
    t.t.resize(n);
    t.t_out.resize(n);
    t.day.resize(n);
    t.anchor.resize(n);
    t.actual.resize(n);
    if (!t.cost_bp.empty()) { t.cost_bp.resize(n); }
    if (!t.pair.empty()) { t.pair.resize(n); }
    if (!t.slot.empty()) { t.slot.resize(n); }
    const double r = tr.ret(last);
    t.actual[last] = r != 0.0 ? tr.anchor[last] * std::exp(-2.0 * r) : tr.anchor[last] * 1.01;
    return t;
}

/// Calls of two runs, rows [from, to] inclusive: every model and ensemble.
[[nodiscard]] inline Diff compare_calls(const CurriculumTrack& tr, const CurriculumRun& a, const CurriculumRun& b,
                                        std::size_t from, std::size_t to) {
    Diff d;
    if (a.models != b.models || a.first_row != b.first_row) {
        d.note("the model list or the first forecast row differs");
        return d;
    }
    for (std::size_t m = 0; m < a.models.size(); ++m) {
        for (std::size_t i = from; i <= to; ++i) {
            const auto& x = a.calls[m][i - a.first_row];
            const auto& y = b.calls[m][i - b.first_row];
            ++d.compared;
            if (x.made != y.made || x.dir != y.dir || !detail::same(x.p_up, y.p_up) || !detail::same(x.mu, y.mu)
                || !detail::same(x.sigma, y.sigma)) {
                d.note(a.models[m] + " at the row decided " + da::format_audit_time(tr.t[i], false));
                break;
            }
        }
    }
    return d;
}

/// 80 % bands of two runs, rows [from, to] inclusive.
[[nodiscard]] inline Diff compare_bands(const CurriculumTrack& tr, const BandRun& a, const BandRun& b, std::size_t from,
                                        std::size_t to) {
    Diff d;
    if (a.models != b.models || a.first_row != b.first_row) {
        d.note("the band model list or the first band row differs");
        return d;
    }
    for (std::size_t m = 0; m < a.models.size(); ++m) {
        for (std::size_t i = from; i <= to; ++i) {
            ++d.compared;
            if (!detail::same(a.half[m][i - a.first_row], b.half[m][i - b.first_row])) {
                d.note(a.models[m] + " band at the row decided " + da::format_audit_time(tr.t[i], false));
                break;
            }
        }
    }
    return d;
}

struct ModelCheck {
    Diff calls, bands;
    std::size_t refusals = 0;   ///< look-ahead reads the rerun refused (must be 0)
    bool ran = false;
};

/// Models for a track (a test can plant one that holds the track itself).
using MakeModels = std::function<std::vector<std::unique_ptr<CurriculumModel>>(const CurriculumTrack&)>;

/// Rerun every model (and, with `bands`, every band model) on the rows up to
/// `cut` with that row's outcome changed, and compare with the full run.
[[nodiscard]] inline ModelCheck check_models(const CurriculumTrack& tr, const CurriculumRun& run, const BandRun* bands,
                                             std::size_t cut, const CurriculumOptions& opt,
                                             const MakeModels& make = {}) {
    ModelCheck out;
    if (cut < run.first_row || cut >= tr.rows()) { return out; }
    const CurriculumTrack t = prefix_with_other_outcome(tr, cut);
    auto models = make ? make(t) : curriculum_default_models();
    const auto rerun = curriculum_run(t, models, opt);
    if (!rerun) {
        out.calls.note(std::string{"the rerun was refused: "} + curriculum_error_text(rerun.error()));
        return out;
    }
    out.ran = true;
    out.refusals = rerun->lookahead_refusals;
    out.calls = compare_calls(tr, run, *rerun, run.first_row, cut);
    if (bands != nullptr) {
        auto bm = band_default_models();
        const auto brun = band_run(t, bm, opt);
        if (!brun) {
            out.bands.note("the band rerun was refused");
        } else {
            out.refusals += brun->lookahead_refusals;
            out.bands = compare_bands(tr, *bands, *brun, bands->first_row, cut);
        }
    }
    return out;
}

/// Where to cut a run: the middle of the test blocks of its third- and
/// second-to-last stages -- deep enough that every model has been refitted
/// many times, cheap enough to rerun (about 20 % and 40 % of the data).
[[nodiscard]] inline std::vector<std::size_t> cut_rows(const CurriculumRun& run) {
    std::vector<std::size_t> out;
    const std::size_t s = run.stages.size();
    for (const std::size_t back : {std::size_t{3}, std::size_t{2}}) {
        if (s < back + 1) { continue; }
        const CurriculumStage& st = run.stages[s - back];
        if (st.test_end <= st.test_begin) { continue; }
        const std::size_t mid = st.test_begin + (st.test_end - st.test_begin) / 2;
        if (out.empty() || out.back() != mid) { out.push_back(mid); }
    }
    if (out.empty() && s > 0) {
        const CurriculumStage& st = run.stages.back();
        if (st.test_end > st.test_begin) { out.push_back(st.test_begin + (st.test_end - st.test_begin) / 2); }
    }
    return out;
}

} // namespace altair::forecast_leak
