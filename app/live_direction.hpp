// app/live_direction.hpp -- the direction models, live, behind the magnitude gate.
//
// WHICH TRACK, AND WHY ONLY THIS ONE. The curriculum (ops/forecast-curriculum.md)
// measured break-even accuracy per horizon: at one and five minutes a futures
// round trip costs more than the typical move, so a call has to be right
// 80-99 % of the time to pay -- no model came close. The horizon that moves
// several times its cost is the session: decide at 10:15, hold to the close
// -- here, to the 15:20 square-off, which is the horizon it is trained and
// calibrated on (SessionInputs::exit_minute = 920), not 15:30.
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
//   4. what a RIGHT call gains and a WRONG one loses are measured separately,
//      per model, from its walk-forward calls, as multiples of the volatility
//      forecast made with each call (models/magnitude.hpp, WalkForwardPayoff)
//      -- not assumed equal -- and scaled by today's forecast: the RMS of the
//      last 60 sessions' 10:15-to-15:20 returns, the same estimator for the
//      history and for today;
//   5. the call is taken only when q x gain - (1 - q) x loss - cost is
//      positive by `gate_z` standard errors (q's, gain's and loss's, by the
//      delta method): one lot of the near NIFTY future, bought back at the
//      15:20 square-off.
// Every model trades under its own name; "Vote" is the majority of the five.
//
// NOTHING IS FITTED INSIDE THE MARKET-DATA PATH. The five models are fitted
// before the session on every finished day (CurriculumDesign::make_fit_only);
// at 10:15 today's row is built from today's bars and the fitted models only
// predict it -- the same calls a fit at 10:15 would make, because the
// training rows and their scaling are the same (tests/test_live_direction).

#pragma once

#include <app/forecast_tracks.hpp>
#include <live/engine.hpp>
#include <live/fingerprint.hpp>
#include <models/curriculum.hpp>
#include <models/magnitude.hpp>

#include <chrono>
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
/// The decision times the models are calibrated for: every hour from 10:15
/// (the shortlisted track) to 14:15, each to the 15:20 square-off. A model
/// trades at the first of them where its own gate opens.
inline constexpr int kDecideMinutes[] = {10 * 60 + 15, 11 * 60 + 15, 12 * 60 + 15, 13 * 60 + 15, 14 * 60 + 15};

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
    int decide_minute = kDecideMinute;        ///< IST minute of day this track decides at
    std::vector<da::AuditBar> nifty5, vix5;   ///< cleaned history, through yesterday
    double other_cost_bp = 1.3;
    std::size_t history_days = 0;
    std::vector<std::string> names;           ///< base model names, in direction_models() order
    std::vector<WalkForwardCalibrator> cal;   ///< per base model, then one for the vote
    std::vector<WalkForwardPayoff> payoff;    ///< gain if right / loss if wrong, in sigmas; same order
    std::vector<double> accuracy;             ///< walk-forward hit rate, per base model, then the vote
    std::vector<std::size_t> scored;
    double gate_z = 1.0;                      ///< standard errors the value must clear

    /// Every out-of-sample walk-forward call the calibration learned from:
    /// what the model said, what the calibration made of it then, what happened.
    struct OosCall {
        std::int64_t t = 0;        ///< the decision's stamp (track time)
        std::size_t model = 0;     ///< index into names; names.size() is the vote
        int dir = 0;
        double p_up = std::numeric_limits<double>::quiet_NaN();
        double q_cal = std::numeric_limits<double>::quiet_NaN();   ///< calibrated, before this outcome was known
        double ret = 0.0;          ///< the realised log return to 15:20
        bool right = false;
    };
    std::vector<OosCall> oos;

    // Fitted before the session, on every finished day.
    std::vector<std::unique_ptr<CurriculumModel>> fitted;   ///< per base model; null where the fit abstained
    std::vector<std::string> fit_note;        ///< why a model abstained, or what its fit chose
    std::vector<std::string> fit_params;      ///< the fitted parameters, where they print
    std::vector<std::string> fit_fingerprint; ///< a digest of every training-row call: pins the fitted function
    std::vector<std::string> feature_names;
    std::vector<double> scaler_mean, scaler_sd;
    std::string track_fingerprint;            ///< a digest of the training rows (features and outcomes)
    std::size_t fitted_rows = 0;              ///< finished days the fit used
    std::int32_t fitted_last_day = 0;         ///< the track's day ordinal of the last of them
    double fit_seconds = 0.0;

    // Today's decision, computed once by the first model that needs it.
    std::int64_t decided_day = 0;
    std::string today_note;
    std::vector<CurriculumCall> today_call;   ///< per base model
    double today_sigma_bp = 0.0, today_cost_bp = 0.0;
    double decide_ms = 0.0;                   ///< how long today's 10:15 decision took
};

/// The volatility forecast a call is made with: the RMS of the `n` returns
/// before row `i` (all known at row i's decision), in log-return units.
/// NaN with fewer than 20 of them. The same estimator scores the history
/// and sizes today.
[[nodiscard]] inline double trailing_sigma(const CurriculumTrack& tr, std::size_t i, std::size_t n = 60) {
    const std::size_t from = i > n ? i - n : 0;
    if (i - from < 20) return std::numeric_limits<double>::quiet_NaN();
    double ss = 0.0;
    for (std::size_t k = from; k < i; ++k) { const double r = tr.ret(k); ss += r * r; }
    return std::sqrt(ss / static_cast<double>(i - from));
}

/// Walk the history forward once: every base model's out-of-sample calls,
/// into a calibrator each. Calls from stages trained on fewer than
/// `min_train_days` days are left out -- three-day fits are not what runs live.
inline void calibrate(DirectionShared& s, std::int32_t min_train_days = 120) {
    ft::TrackInfo info;
    const std::string label = "NIFTY " + live::live_fmt::hhmm(s.decide_minute);
    ft::SessionInputs hist{label, "NIFTY", s.decide_minute, &s.nifty5, &s.vix5, s.other_cost_bp, nullptr, ""};
    hist.exit_minute = live::kLiveSquareOffMinute;   // trained on the horizon it trades: the decision to the 15:20 square-off
    const auto tr = ft::build_session(hist, info);
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
    s.payoff.assign(nb + 1, WalkForwardPayoff{});
    s.accuracy.assign(nb + 1, std::numeric_limits<double>::quiet_NaN());
    s.scored.assign(nb + 1, 0);
    std::vector<std::size_t> right(nb + 1, 0);
    for (std::size_t r = run->first_row; r < tr.rows(); ++r) {
        const auto st = run->stage_of[r - run->first_row];
        if (run->stages[st].train_days < min_train_days) continue;
        const int out = curriculum_detail::outcome(tr, r);
        if (out == 0) continue;
        const double sig = trailing_sigma(tr, r);
        const double z_abs = std::isfinite(sig) && sig > 0.0 ? std::fabs(tr.ret(r)) / sig : std::numeric_limits<double>::quiet_NaN();
        int up = 0, down = 0;
        for (std::size_t m = 0; m < nb; ++m) {
            const CurriculumCall c = run->calls[m][r - run->first_row];
            if (!c.made || c.dir == 0) continue;
            (c.dir > 0 ? up : down) += 1;
            const bool ok = c.dir == out;
            s.oos.push_back({tr.t[r], m, c.dir, c.p_up, s.cal[m].calibrated(stated_q(c)), tr.ret(r), ok});
            s.cal[m].add(stated_q(c), ok);
            s.payoff[m].add(ok, z_abs);
            ++s.scored[m];
            right[m] += ok ? 1 : 0;
        }
        if (up != down) {
            const bool ok = (up > down ? 1 : -1) == out;
            s.oos.push_back({tr.t[r], nb, up > down ? 1 : -1, std::numeric_limits<double>::quiet_NaN(),
                             s.cal[nb].calibrated(std::numeric_limits<double>::quiet_NaN()), tr.ret(r), ok});
            s.cal[nb].add(std::numeric_limits<double>::quiet_NaN(), ok);
            s.payoff[nb].add(ok, z_abs);
            ++s.scored[nb];
            right[nb] += ok ? 1 : 0;
        }
    }
    for (std::size_t m = 0; m <= nb; ++m)
        if (s.scored[m] > 0) s.accuracy[m] = static_cast<double>(right[m]) / static_cast<double>(s.scored[m]);
    s.history_days = tr.rows();

    // The fit today's decision will use: every finished day, now, not at 10:15.
    const auto t0 = std::chrono::steady_clock::now();
    const auto d = CurriculumDesign::make_fit_only(tr, tr.rows());
    if (!d) { s.why = "the fit design was refused"; return; }
    s.fitted.clear();
    s.fit_note.assign(nb, std::string());
    s.fit_params.assign(nb, std::string());
    s.fit_fingerprint.assign(nb, std::string());
    auto fresh = direction_models();
    for (std::size_t m = 0; m < nb; ++m) {
        const std::string why = fresh[m]->fit(*d);
        s.fit_note[m] = why.empty() ? fresh[m]->tuned() : "abstains: " + why;
        if (why.empty()) {
            s.fit_params[m] = fresh[m]->params();
            // The fitted function, pinned: its call on every training row.
            live::Fingerprint fp;
            for (std::size_t i = 0; i < tr.rows(); ++i) {
                const CurriculumCall c = fresh[m]->predict(*d, i);
                fp.i64(c.made ? c.dir : 99).f64(c.p_up).f64(c.mu).f64(c.sigma);
            }
            s.fit_fingerprint[m] = fp.hex();
        }
        s.fitted.push_back(why.empty() ? std::move(fresh[m]) : nullptr);
    }
    s.feature_names = tr.feature_names;
    s.scaler_mean.assign(d->scaler_mean().begin(), d->scaler_mean().end());
    s.scaler_sd.assign(d->scaler_sd().begin(), d->scaler_sd().end());
    live::Fingerprint tfp;
    for (const double x : tr.x) tfp.f64(x);
    for (std::size_t i = 0; i < tr.rows(); ++i) tfp.i64(tr.t[i]).f64(tr.anchor[i]).f64(tr.actual[i]).f64(tr.cost_bp[i]);
    s.track_fingerprint = tfp.hex();
    s.fitted_rows = tr.rows();
    s.fitted_last_day = tr.day.back();
    s.fit_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
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

/// Today's 10:15 calls from today's 5-minute bars so far (NIFTY and VIX):
/// the fitted models predict today's row; nothing is fitted here.
inline void decide_from_bars(DirectionShared& s, const std::vector<da::AuditBar>& own, const std::vector<da::AuditBar>& vix) {
    const auto t0 = std::chrono::steady_clock::now();
    struct Timer {
        DirectionShared& s;
        std::chrono::steady_clock::time_point t0;
        ~Timer() { s.decide_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
    } timer{s, t0};
    s.today_call.assign(s.names.size(), CurriculumCall{});
    s.today_note.clear();
    const std::string at = live::live_fmt::hhmm(s.decide_minute);
    const std::size_t need = static_cast<std::size_t>((s.decide_minute - live::kLiveOpenMinute) / 5);
    if (own.size() < need || vix.size() < need) {
        s.today_note = "Today's 5-minute bars do not run unbroken from 09:15 to " + at + " (did the feed start late?).";
        return;
    }
    std::vector<da::AuditBar> nb = s.nifty5, vb = s.vix5;
    nb.insert(nb.end(), own.begin(), own.end());
    vb.insert(vb.end(), vix.begin(), vix.end());
    ft::SessionInputs in{"NIFTY " + at, "NIFTY", s.decide_minute, &nb, &vb, s.other_cost_bp, nullptr, ""};
    in.partial_last_day = true;
    in.exit_minute = live::kLiveSquareOffMinute;
    ft::TrackInfo info;
    const auto tr = ft::build_session(in, info);
    if (!info.partial_last || tr.rows() < 2) { s.today_note = "Today's " + at + " row could not be built (no VIX bar at a stamp?)."; return; }
    const std::size_t i = tr.rows() - 1;
    // The models were fitted before the session on exactly the rows before
    // today's. If the history rows differ (the dataset changed under a
    // running engine), the fit is not today's fit: say so, do not trade.
    if (i != s.fitted_rows || (i > 0 && tr.day[i - 1] != s.fitted_last_day)) {
        s.today_note = "Today's track does not line up with the fit made before the session ("
                     + std::to_string(i) + " rows against " + std::to_string(s.fitted_rows) + "); restart the engine.";
        return;
    }
    CurriculumStage st;
    st.train_rows = i;
    st.test_begin = i;
    st.test_end = i + 1;
    st.train_days = tr.day[i];
    st.test_days = 1;
    const auto d = CurriculumDesign::make(tr, st);
    if (!d) { s.today_note = "The design was refused."; return; }
    for (std::size_t m = 0; m < s.fitted.size() && m < s.today_call.size(); ++m) {
        if (!s.fitted[m]) continue;
        CurriculumCall c = s.fitted[m]->predict(*d, i);
        curriculum_detail::finish(c, *d);
        s.today_call[m] = c;
    }
    const double sig = trailing_sigma(tr, i);
    s.today_sigma_bp = std::isfinite(sig) ? 1e4 * sig : 0.0;
    s.today_cost_bp = tr.cost_bp.empty() ? 0.0 : tr.cost_bp[i];
}

/// Today's 10:15 call from every base model; run once per day.
inline void decide_today(DirectionShared& s, const live::LiveEngine& e) {
    if (s.decided_day == e.today()) return;
    s.decided_day = e.today();
    const auto* n = e.index_of("NIFTY");
    const auto* v = e.index_of("INDIAVIX");
    decide_from_bars(s, n ? today_bars(e, n->token) : std::vector<da::AuditBar>{},
                     v ? today_bars(e, v->token) : std::vector<da::AuditBar>{});
}

class LiveDirectionModel final : public live::LiveModel {
public:
    /// `index` < names.size(): a base model; == names.size(): the vote. One
    /// shared track per decision time (kDecideMinutes), all for the same models.
    LiveDirectionModel(std::vector<std::shared_ptr<DirectionShared>> s, std::size_t index) : s_(std::move(s)), i_(index) {}
    LiveDirectionModel(std::shared_ptr<DirectionShared> s, std::size_t index)
        : LiveDirectionModel(std::vector<std::shared_ptr<DirectionShared>>{std::move(s)}, index) {}

    [[nodiscard]] std::string name() const override {
        const DirectionShared& z = *s_.front();
        return "Direction " + (i_ < z.names.size() ? z.names[i_] : std::string("Vote"));
    }
    [[nodiscard]] std::string family() const override { return "direction + magnitude gate"; }
    void on_new_day(live::LiveEngine&) override { entered_ = false; note_.clear(); signal_.clear(); fields_.clear(); }

    void on_minute(live::LiveEngine& e, int m) override {
        // The decision times whose track was calibrated; a model that is in a
        // position (or entered today) holds it to the square-off.
        DirectionShared* sh = nullptr;
        for (const auto& x : s_) if (x->ok && x->decide_minute == m) sh = x.get();
        if (sh == nullptr) return;
        if (entered_ || !e.book().flat(name())) {
            if (!entered_) {
                entered_ = true;
                note_ = "Holding the position resumed from earlier today to the square-off.";
                e.note_decision(name(), note_);
            }
            return;
        }
        DirectionShared& s = *sh;
        const std::string at = live::live_fmt::hhmm(m);
        // The gate's inputs go on the record with the outcome.
        live::LiveDecisionScope log(e, name(), [this] {
            std::string t = note_;
            for (const auto& [k, v] : fields_) t += " | " + k + " " + v;
            return t;
        });
        decide_today(s, e);
        if (!s.today_note.empty()) { note_ = at + ": " + s.today_note; return; }
        int dir = 0;
        double qraw = std::numeric_limits<double>::quiet_NaN();
        if (i_ < s.names.size()) {
            const CurriculumCall& c = s.today_call[i_];
            if (!c.made || c.dir == 0) { note_ = at + ": the model abstained."; return; }
            dir = c.dir;
            qraw = stated_q(c);
        } else {
            int up = 0, down = 0;
            for (const auto& c : s.today_call) if (c.made && c.dir != 0) (c.dir > 0 ? up : down) += 1;
            if (up == down) { note_ = at + ": the five models split evenly; no call."; return; }
            dir = up > down ? 1 : -1;
        }
        if (!(s.today_sigma_bp > 0.0)) { note_ = at + ": no volatility forecast (fewer than 20 sessions of history)."; return; }
        const double q = s.cal[i_].calibrated(qraw);
        const GateValue g = gate_value(q, s.cal[i_].calibrated_se(qraw), s.payoff[i_].estimate(), s.today_sigma_bp,
                                       s.today_cost_bp, s.gate_z);
        signal_ = std::string(dir > 0 ? "UP" : "DOWN") + " " + at + " to 15:20";
        fields_ = {{"decision", at},
                   {"calibrated q", live::live_fmt::pct(q, 1) + " ± " + live::live_fmt::pct(g.q_se, 1)},
                   {"gain if right", live::live_fmt::num(g.gain_bp, 1) + " bp"},
                   {"loss if wrong", live::live_fmt::num(g.loss_bp, 1) + " bp"},
                   {"round-trip cost", live::live_fmt::num(g.cost_bp, 1) + " bp"},
                   {"value q·gain − (1−q)·loss − cost", live::live_fmt::num(g.value_bp, 1) + " ± " + live::live_fmt::num(g.se_bp, 1) + " bp"},
                   {"gate bound (value − " + live::live_fmt::num(s.gate_z, 1) + " se)", live::live_fmt::num(g.lower_bp, 1) + " bp"}};
        if (!g.open()) {
            note_ = at + ": gate shut, worth " + live::live_fmt::num(g.value_bp, 1) + " ± " + live::live_fmt::num(g.se_bp, 1)
                  + " bp after costs; it must clear zero by " + live::live_fmt::num(s.gate_z, 1) + " standard error(s).";
            return;
        }
        const double value = g.value_bp;
        const auto* fut = e.near_future("NIFTY");
        if (fut == nullptr || e.stale()) { note_ = at + ": gate open, but no NIFTY future or a stale feed."; return; }
        std::string why;
        const std::string reason = signal_ + ": q " + live::live_fmt::pct(q, 1) + ", value " + live::live_fmt::num(value, 1) + " bp";
        if (e.book().open(name(), *fut, dir, 1, e.clock_ns(), reason, false, &why)) {
            entered_ = true;
            note_ = "Gate open: " + reason + ".";
        } else {
            note_ = at + ": gate open, but the entry was refused: " + why;
        }
    }

    [[nodiscard]] live::LiveModelView view(const live::LiveEngine& e) const override {
        live::LiveModelView v;
        v.name = name();
        v.family = family();
        const DirectionShared& z = *s_.front();
        bool any = false;
        for (const auto& x : s_) any = any || x->ok;
        v.state = !any ? "abstaining" : (!e.book().flat(name()) ? "in position" : (entered_ ? "done today" : "watching"));
        v.signal = signal_;
        std::string times;
        for (const auto& x : s_) if (x->ok) times += (times.empty() ? "" : ", ") + live::live_fmt::hhmm(x->decide_minute);
        v.reason = !any ? "No history: " + z.why
                        : (note_.empty() ? "At " + times + ": forecast to the 15:20 square-off; trade one NIFTY future lot at the first of them where q·gain − (1−q)·loss − cost clears zero by the gate's margin." : note_);
        v.fields = fields_;
        for (const auto& x : s_)
            if (x->ok && i_ < x->accuracy.size())
                v.fields.push_back({"walk-forward hit rate " + live::live_fmt::hhmm(x->decide_minute),
                                    live::live_fmt::pct(x->accuracy[i_], 1) + " of " + std::to_string(x->scored[i_]) + " days"});
        return v;
    }

private:
    std::vector<std::shared_ptr<DirectionShared>> s_;
    std::size_t i_;
    bool entered_ = false;
    std::string note_, signal_;
    std::vector<std::pair<std::string, std::string>> fields_;
};

} // namespace altair::live_direction
