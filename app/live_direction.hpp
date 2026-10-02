// app/live_direction.hpp -- the direction models, live, behind the magnitude gate.
//
// WHICH TRACK, AND WHY ONLY THIS ONE. The curriculum (ops/forecast-curriculum.md)
// measured break-even accuracy per horizon: at one and five minutes a futures
// round trip costs more than the typical move, so a call has to be right
// 80-99 % of the time to pay -- no model came close. The horizon that moves
// several times its cost is the session: decide at 10:15, hold to the close.
// That is the track the research shortlisted (AR/ARMA at 10:15), so that is
// the one run live. The faster tracks are not run: running them would only
// demonstrate a gate that never opens.
//
// WHAT HAPPENS AT 10:15 (IST, on the feed's clock):
//   1. today's 5-minute NIFTY and INDIA VIX bars, built from ticks, are
//      appended to the dataset's history and the session track is rebuilt with
//      today's row (app/forecast_tracks.hpp, partial_last_day) -- the same
//      features, the same arithmetic as every backtest row;
//   2. each model is fitted on every finished day and forecasts today;
//   3. q, the probability the call is right, comes from a walk-forward
//      calibration of the model's own past calls on that history
//      (models/magnitude.hpp, WalkForwardCalibrator), not from the model's
//      opinion of itself;
//   4. E|r| is the history's sd of 10:15-to-close returns, scaled by today's
//      HAR volatility against its usual level, times sqrt(2/pi);
//   5. the call is taken only when (2q - 1) x E|r| beats the round-trip cost:
//      one lot of the near NIFTY future, bought back at the 15:20 square-off.
// Every model trades under its own name; "Vote" is the majority of the five.

#pragma once

#include <app/forecast_tracks.hpp>
#include <live/engine.hpp>
#include <models/curriculum.hpp>
#include <models/magnitude.hpp>

#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace altair::live_direction {

namespace ft = altair::forecast_tracks;
namespace da = altair::data_audit;

inline constexpr int kDecideMinute = 10 * 60 + 15;

/// The base models run live: the shortlisted AR/ARMA and three learners.
[[nodiscard]] inline std::vector<std::unique_ptr<CurriculumModel>> direction_models() {
    std::vector<std::unique_ptr<CurriculumModel>> m;
    m.push_back(std::make_unique<CurriculumArma>(2, 0));
    m.push_back(std::make_unique<CurriculumArma>(1, 1));
    m.push_back(std::make_unique<CurriculumLogistic>());
    m.push_back(std::make_unique<CurriculumRidge>());
    m.push_back(std::make_unique<CurriculumGbdt>(0xA17A1Au + 2));
    return m;
}

/// The probability a call says it is right: P(up) for an up call, P(down)
/// for a down call; NaN for a model that publishes no probability.
[[nodiscard]] inline double stated_q(const CurriculumCall& c) noexcept {
    if (!c.made || c.dir == 0 || !std::isfinite(c.p_up)) return std::numeric_limits<double>::quiet_NaN();
    return c.dir > 0 ? c.p_up : 1.0 - c.p_up;
}

/// Everything the direction models share, built before the session.
struct DirectionShared {
    bool ok = false;
    std::string why;
    std::vector<da::AuditBar> nifty5, vix5;   ///< cleaned history, through yesterday
    double other_cost_bp = 1.3;
    double vol_ratio = 1.0;                   ///< today's HAR sigma over its 250-day median
    std::size_t history_days = 0;
    std::vector<std::string> names;           ///< base model names, in direction_models() order
    std::vector<WalkForwardCalibrator> cal;   ///< per base model, then one for the vote
    std::vector<double> accuracy;             ///< walk-forward hit rate, per base model, then the vote
    std::vector<std::size_t> scored;

    // Today's decision, computed once by the first model that needs it.
    std::int64_t decided_day = 0;
    std::string today_note;
    std::vector<CurriculumCall> today_call;   ///< per base model
    double today_sigma = 0.0, today_cost_bp = 0.0;
};

/// Walk the history forward once: every base model's out-of-sample calls,
/// into a calibrator each. Calls from stages trained on fewer than
/// `min_train_days` days are left out -- three-day fits are not what runs live.
inline void calibrate(DirectionShared& s, std::int32_t min_train_days = 120) {
    ft::TrackInfo info;
    const auto tr = ft::build_session({"NIFTY 10:15", "NIFTY", kDecideMinute, &s.nifty5, &s.vix5, s.other_cost_bp, nullptr, ""}, info);
    if (tr.rows() < 200) { s.why = "only " + std::to_string(tr.rows()) + " finished days of 5-minute history"; return; }
    auto models = direction_models();
    CurriculumOptions opt;
    opt.step_cap_days = 250;
    const auto run = curriculum_run(tr, models, opt);
    if (!run) { s.why = std::string("walk-forward refused: ") + curriculum_error_text(run.error()); return; }
    const std::size_t nb = models.size();
    s.names.clear();
    for (const auto& m : models) s.names.push_back(m->name());
    s.cal.assign(nb + 1, WalkForwardCalibrator{});
    s.accuracy.assign(nb + 1, std::numeric_limits<double>::quiet_NaN());
    s.scored.assign(nb + 1, 0);
    std::vector<std::size_t> right(nb + 1, 0);
    for (std::size_t r = run->first_row; r < tr.rows(); ++r) {
        const auto st = run->stage_of[r - run->first_row];
        if (run->stages[st].train_days < min_train_days) continue;
        const int out = curriculum_detail::outcome(tr, r);
        if (out == 0) continue;
        int up = 0, down = 0;
        for (std::size_t m = 0; m < nb; ++m) {
            const CurriculumCall c = run->calls[m][r - run->first_row];
            if (!c.made || c.dir == 0) continue;
            (c.dir > 0 ? up : down) += 1;
            const bool ok = c.dir == out;
            s.cal[m].add(stated_q(c), ok);
            ++s.scored[m];
            right[m] += ok ? 1 : 0;
        }
        if (up != down) {
            const bool ok = (up > down ? 1 : -1) == out;
            s.cal[nb].add(std::numeric_limits<double>::quiet_NaN(), ok);
            ++s.scored[nb];
            right[nb] += ok ? 1 : 0;
        }
    }
    for (std::size_t m = 0; m <= nb; ++m)
        if (s.scored[m] > 0) s.accuracy[m] = static_cast<double>(right[m]) / static_cast<double>(s.scored[m]);
    s.history_days = tr.rows();
    s.ok = true;
}

/// Today's 5-minute bars of `token` from the engine, as dataset bars (IST
/// seconds, bar start). Empty unless they run unbroken from 09:15.
[[nodiscard]] inline std::vector<da::AuditBar> today_bars(const live::LiveEngine& e, std::uint32_t token) {
    std::vector<da::AuditBar> out;
    const auto five = live::live_aggregate(e.bars(token), 5);
    for (std::size_t k = 0; k < five.size(); ++k) {
        if (five[k].start_of_day() != live::kLiveOpenMinute + 5 * static_cast<int>(k)) return {};
        da::AuditBar b;
        b.t = five[k].minute * 60;
        b.o = five[k].o; b.h = five[k].h; b.l = five[k].l; b.c = five[k].c;
        out.push_back(b);
    }
    return out;
}

/// Today's 10:15 call from every base model; run once per day.
inline void decide_today(DirectionShared& s, const live::LiveEngine& e) {
    if (s.decided_day == e.today()) return;
    s.decided_day = e.today();
    s.today_call.assign(s.names.size(), CurriculumCall{});
    s.today_note.clear();
    const auto* n = e.index_of("NIFTY");
    const auto* v = e.index_of("INDIAVIX");
    auto own = n ? today_bars(e, n->token) : std::vector<da::AuditBar>{};
    auto vix = v ? today_bars(e, v->token) : std::vector<da::AuditBar>{};
    if (own.size() < 12 || vix.size() < 12) {
        s.today_note = "Today's 5-minute bars do not run unbroken from 09:15 to 10:15 (did the feed start late?).";
        return;
    }
    std::vector<da::AuditBar> nb = s.nifty5, vb = s.vix5;
    nb.insert(nb.end(), own.begin(), own.end());
    vb.insert(vb.end(), vix.begin(), vix.end());
    ft::SessionInputs in{"NIFTY 10:15", "NIFTY", kDecideMinute, &nb, &vb, s.other_cost_bp, nullptr, ""};
    in.partial_last_day = true;
    ft::TrackInfo info;
    const auto tr = ft::build_session(in, info);
    if (!info.partial_last || tr.rows() < 2) { s.today_note = "Today's 10:15 row could not be built (no VIX bar at a stamp?)."; return; }
    const std::size_t i = tr.rows() - 1;
    CurriculumStage st;
    st.train_rows = i;
    st.test_begin = i;
    st.test_end = i + 1;
    st.train_days = tr.day[i];
    st.test_days = 1;
    const auto d = CurriculumDesign::make(tr, st);
    if (!d) { s.today_note = "The design was refused."; return; }
    auto models = direction_models();
    for (std::size_t m = 0; m < models.size() && m < s.today_call.size(); ++m) {
        const std::string why = models[m]->fit(*d);
        if (!why.empty()) continue;
        CurriculumCall c = models[m]->predict(*d, i);
        curriculum_detail::finish(c, *d);
        s.today_call[m] = c;
    }
    s.today_sigma = d->sd() * s.vol_ratio;
    s.today_cost_bp = tr.cost_bp.empty() ? 0.0 : tr.cost_bp[i];
}

class LiveDirectionModel final : public live::LiveModel {
public:
    /// `index` < names.size(): a base model; == names.size(): the vote.
    LiveDirectionModel(std::shared_ptr<DirectionShared> s, std::size_t index) : s_(std::move(s)), i_(index) {}

    [[nodiscard]] std::string name() const override {
        return "Direction 10:15 " + (i_ < s_->names.size() ? s_->names[i_] : std::string("Vote"));
    }
    [[nodiscard]] std::string family() const override { return "direction + magnitude gate"; }
    void on_new_day(live::LiveEngine&) override { done_ = false; note_.clear(); signal_.clear(); fields_.clear(); }

    void on_minute(live::LiveEngine& e, int m) override {
        if (m != kDecideMinute || done_ || !s_->ok) return;
        done_ = true;
        decide_today(*s_, e);
        if (!s_->today_note.empty()) { note_ = s_->today_note; return; }
        int dir = 0;
        double qraw = std::numeric_limits<double>::quiet_NaN();
        if (i_ < s_->names.size()) {
            const CurriculumCall& c = s_->today_call[i_];
            if (!c.made || c.dir == 0) { note_ = "The model abstained today."; return; }
            dir = c.dir;
            qraw = stated_q(c);
        } else {
            int up = 0, down = 0;
            for (const auto& c : s_->today_call) if (c.made && c.dir != 0) (c.dir > 0 ? up : down) += 1;
            if (up == down) { note_ = "The five models split evenly; no call."; return; }
            dir = up > down ? 1 : -1;
        }
        const double q = s_->cal[i_].calibrated(qraw);
        const double eabs_bp = 1e4 * expected_abs_move(s_->today_sigma);
        const double value = call_value_bp(q, eabs_bp, s_->today_cost_bp);
        signal_ = std::string(dir > 0 ? "UP" : "DOWN") + " to the close";
        fields_ = {{"calibrated q", live::live_fmt::pct(q, 1)},
                   {"E|r| to close", live::live_fmt::num(eabs_bp, 1) + " bp"},
                   {"round-trip cost", live::live_fmt::num(s_->today_cost_bp, 1) + " bp"},
                   {"value (2q-1)E|r| - cost", live::live_fmt::num(value, 1) + " bp"}};
        if (!(value > 0.0)) {
            note_ = "Gate shut: the call is worth " + live::live_fmt::num(value, 1) + " bp after costs.";
            return;
        }
        const auto* fut = e.near_future("NIFTY");
        if (fut == nullptr || e.stale()) { note_ = "Gate open, but no NIFTY future or a stale feed."; return; }
        std::string why;
        const std::string reason = signal_ + ": q " + live::live_fmt::pct(q, 1) + ", value " + live::live_fmt::num(value, 1) + " bp";
        if (e.book().open(name(), *fut, dir, 1, e.clock_ns(), reason, false, &why)) note_ = "Gate open: " + reason + ".";
        else note_ = "Gate open, but the order could not fill: " + why;
    }

    [[nodiscard]] live::LiveModelView view(const live::LiveEngine& e) const override {
        live::LiveModelView v;
        v.name = name();
        v.family = family();
        v.state = !s_->ok ? "abstaining" : (!e.book().flat(name()) ? "in position" : (done_ ? "done today" : "waiting for 10:15"));
        v.signal = signal_;
        v.reason = !s_->ok ? "No history: " + s_->why
                           : (note_.empty() ? "At 10:15: forecast 10:15-to-close, trade one NIFTY future lot only if (2q-1)E|r| beats the cost." : note_);
        v.fields = fields_;
        if (s_->ok && i_ < s_->accuracy.size())
            v.fields.push_back({"walk-forward hit rate", live::live_fmt::pct(s_->accuracy[i_], 1) + " of "
                                                         + std::to_string(s_->scored[i_]) + " days"});
        return v;
    }

private:
    std::shared_ptr<DirectionShared> s_;
    std::size_t i_;
    bool done_ = false;
    std::string note_, signal_;
    std::vector<std::pair<std::string, std::string>> fields_;
};

} // namespace altair::live_direction
