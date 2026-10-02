// live/models.hpp -- the live models that need nothing but the market and
// their history inputs: the volatility band, the band-edge strangle, the
// BANKNIFTY/NIFTY pair and the NIFTY 50 stat-arb.
//
// EACH ONE IS THE RESEARCH RULE, RUN FORWARD, NOT A NEW IDEA. Where the live
// version has to differ from the backtest it says so in its view:
//   * Strangle (strategies/band_option_fade.hpp): sell the 80 % band's edges at
//     09:20, one lot each, bought back at 15:20; the stop2x variant buys a leg
//     back at the first 5-minute close where its premium has doubled. The
//     backtest sold the monthly contract at a modelled price; live it sells
//     the NEAREST streamed expiry at the real bid.
//   * Pairs (strategies/pairs_futures.hpp): BANKNIFTY against NIFTY, the
//     spread from a 250-day formation, entered at |z| >= 2, out at |z| <= 0.5,
//     stopped at |z| >= 4; decided once a day at 15:15 like the daily backtest;
//     futures in whole lots; carried overnight, rolled on expiry.
//   * Stat-arb (strategies/residual_reversion.hpp): the same s-scores, with
//     today's return so far as the last day, decided at 15:15; one lot of each
//     stock's near future, unhedged (a beta hedge in whole NIFTY lots is far
//     coarser than one stock's lot); carried, closed after 60 sessions.
// The volatility band trades nothing: it is the reading the strangle sells on,
// shown live.

#pragma once

#include <live/engine.hpp>
#include <strategies/residual_reversion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace altair::live {

/// z of the 80 % two-sided band.
inline constexpr double kLiveZ80 = 1.2815515655446004;

/// One underlying's volatility inputs, computed from history before the session.
struct LiveVolInputs {
    std::string under;              ///< "NIFTY", "BANKNIFTY"
    double sigma_day = 0.0;         ///< HAR forecast of today's close-to-close sd (fraction)
    double intraday_share = 0.75;   ///< share of a day's variance inside the session (trailing 250 days)
    double prev_close = 0.0;        ///< rupees
};

namespace live_model_detail {

/// Realised variance of today so far, from one-minute closes, plus the gap.
[[nodiscard]] inline double realised_so_far(const std::vector<LiveBar>& bars, double prev_close) {
    double s = 0.0;
    double last = prev_close;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const double c = bars[i].c;
        const double from = i == 0 ? (prev_close > 0.0 ? prev_close : bars[0].o) : last;
        if (from > 0.0 && c > 0.0) { const double r = std::log(c / from); s += r * r; }
        last = c;
    }
    return s;
}

/// Remaining share of the session's variance after minute `m`, linear in time.
[[nodiscard]] inline double remaining_fraction(int m) {
    const double left = static_cast<double>(kLiveCloseMinute - m) / static_cast<double>(kLiveCloseMinute - kLiveOpenMinute);
    return left <= 0.0 ? 0.0 : (left >= 1.0 ? 1.0 : left);   // RULE 11: proven -- a time-of-day fraction lies in [0, 1].
}

} // namespace live_model_detail

// ---------------------------------------------------------------------------
// The volatility band
// ---------------------------------------------------------------------------

class LiveVolBandModel final : public LiveModel {
public:
    LiveVolBandModel(std::vector<LiveVolInputs> in, double ratio_mean, double ratio_sd)
        : in_(std::move(in)), ratio_mean_(ratio_mean), ratio_sd_(ratio_sd) {}

    [[nodiscard]] std::string name() const override { return "Vol band (HAR)"; }
    [[nodiscard]] std::string family() const override { return "volatility"; }
    void on_minute(LiveEngine&, int close_minute) override { minute_ = close_minute; }

    [[nodiscard]] LiveModelView view(const LiveEngine& e) const override {
        LiveModelView v;
        v.name = name();
        v.family = family();
        v.state = "watching";
        v.reason = "Trades nothing: it is the range the strangles sell, recomputed every minute.";
        for (const auto& x : in_) {
            const LiveInstrument* idx = e.index_of(x.under);
            const double spot = idx ? e.ltp(idx->token) : 0.0;
            if (!(x.sigma_day > 0.0) || !(spot > 0.0) || !(x.prev_close > 0.0)) {
                v.fields.push_back({x.under, "no forecast or no price yet"});
                continue;
            }
            const double rv = live_model_detail::realised_so_far(idx ? e.bars(idx->token) : std::vector<LiveBar>{}, x.prev_close);
            const double rem = x.intraday_share * x.sigma_day * x.sigma_day * live_model_detail::remaining_fraction(minute_);
            const double half = kLiveZ80 * std::sqrt(rem);
            const double z = std::log(spot / x.prev_close) / x.sigma_day;
            v.fields.push_back({x.under + " σ day (HAR)", live_fmt::pct(x.sigma_day)});
            v.fields.push_back({x.under + " realised so far", live_fmt::pct(std::sqrt(rv))});
            v.fields.push_back({x.under + " move / σ", live_fmt::num(z) + " σ"});
            v.fields.push_back({x.under + " 80% close band", live_fmt::num(spot * std::exp(-half), 1) + " – "
                                                            + live_fmt::num(spot * std::exp(half), 1)});
        }
        const LiveInstrument* n = e.index_of("NIFTY");
        const LiveInstrument* b = e.index_of("BANKNIFTY");
        if (n && b && e.ltp(n->token) > 0.0 && e.ltp(b->token) > 0.0 && ratio_sd_ > 0.0) {
            const double r = std::log(e.ltp(b->token) / e.ltp(n->token));
            v.fields.push_back({"BANKNIFTY/NIFTY ratio", live_fmt::num(std::exp(r), 4)});
            v.fields.push_back({"ratio z (60 d)", live_fmt::num((r - ratio_mean_) / ratio_sd_)});
        }
        v.signal = v.fields.empty() ? "" : v.fields.front().first + " " + v.fields.front().second;
        return v;
    }

private:
    std::vector<LiveVolInputs> in_;
    double ratio_mean_, ratio_sd_;
    int minute_ = kLiveOpenMinute;
};

// ---------------------------------------------------------------------------
// The band-edge strangle
// ---------------------------------------------------------------------------

class LiveStrangleModel final : public LiveModel {
public:
    LiveStrangleModel(LiveVolInputs in, double stop_multiple, double min_premium = 1.0)
        : in_(std::move(in)), stop_(stop_multiple), min_premium_(min_premium) {}

    [[nodiscard]] std::string name() const override {
        return "Strangle 80% " + in_.under + (stop_ > 0.0 ? " stop2x" : "");
    }
    [[nodiscard]] std::string family() const override { return "option selling"; }

    void on_new_day(LiveEngine&) override { traded_today_ = false; note_.clear(); call_ = put_ = nullptr; }

    void on_minute(LiveEngine& e, int m) override {
        if (m == kDecideMinute && !traded_today_) {
            LiveDecisionScope log(e, name(), [this] { return note_; });
            decide(e);
        }
        if (m > kDecideMinute && !traded_today_) {
            // The engine joined after 09:20: today's decision was never taken.
            traded_today_ = true;
            note_ = "Missed today: the engine's first bar closed at " + live_fmt::hhmm(m) + ", after 09:20.";
            e.note_decision(name(), note_);
        }
        if (stop_ > 0.0 && m % 5 == 0 && m > kDecideMinute && m < kLiveSquareOffMinute) {
            for (const LiveInstrument* leg : {call_, put_}) {
                if (leg == nullptr) continue;
                const LivePosition* p = e.book().position(name(), leg->token);
                // Only a sold leg, not already being bought back, against an ask that is live now.
                if (p == nullptr || !p->filled() || p->state == LivePosState::Closing) continue;
                const LiveTop t = e.top(leg->token);
                if (t.ask <= 0 || !e.book().fresh(t.quote_ns, e.clock_ns())) continue;
                const double ask = static_cast<double>(t.ask) / 100.0;
                if (ask >= stop_ * p->entry) {
                    (void)e.book().close(name(), leg->token, e.clock_ns(), "premium doubled: stop at " + live_fmt::hhmm(m));
                    e.note_decision(name(), "stop: " + leg->symbol + " ask " + live_fmt::num(ask) + " against "
                                                + live_fmt::num(p->entry) + " sold; buying back");
                }
            }
        }
    }

    [[nodiscard]] LiveModelView view(const LiveEngine& e) const override {
        LiveModelView v;
        v.name = name();
        v.family = family();
        const bool open = !e.book().flat(name());
        v.state = open ? "in position" : (traded_today_ ? "done today" : "waiting for 09:20");
        v.reason = note_.empty() ? "Sells the 80 % band's two edges at 09:20, one lot each; bought back by 15:20"
                                       + std::string(stop_ > 0.0 ? ", or a leg at the first 5-minute close where its premium doubled." : ".")
                                 : note_;
        if (call_ && put_) {
            v.signal = "short " + call_->symbol + " + " + put_->symbol;
            double sold = 0.0, now = 0.0;
            bool priced = true;
            for (const LiveInstrument* leg : {call_, put_}) {
                const LivePosition* p = e.book().position(name(), leg->token);
                const LiveTop t = e.top(leg->token);
                const double ask = static_cast<double>(t.ask > 0 ? t.ask : t.ltp) / 100.0;
                if (p && p->filled()) { sold += p->entry; now += ask; } else { priced = false; }
                v.fields.push_back({leg->symbol, p == nullptr ? std::string("closed")
                                                 : !p->filled() ? "selling (" + std::string(live_state_text(p->state)) + ")"
                                                 : "sold " + live_fmt::num(p->entry) + ", ask " + live_fmt::num(ask)
                                                       + (p->state == LivePosState::Closing ? " (buying back)" : "")});
            }
            if (priced) v.fields.push_back({"premium left", live_fmt::num(now) + " of " + live_fmt::num(sold) + " sold"});
        }
        v.fields.push_back({"band at 09:20", band_text_.empty() ? "—" : band_text_});
        return v;
    }

    static constexpr int kDecideMinute = 9 * 60 + 20;

private:
    void decide(LiveEngine& e) {
        traded_today_ = true;
        if (e.stale()) { note_ = "Skipped today: the feed was stale at 09:20."; return; }
        const LiveInstrument* idx = e.index_of(in_.under);
        const double spot = idx ? e.ltp(idx->token) : 0.0;
        if (!(spot > 0.0) || !(in_.sigma_day > 0.0)) { note_ = "Skipped today: no spot or no volatility forecast at 09:20."; return; }
        const double rem = in_.intraday_share * in_.sigma_day * in_.sigma_day
                         * live_model_detail::remaining_fraction(kDecideMinute);
        const double half = kLiveZ80 * std::sqrt(rem);
        const double upper = spot * std::exp(half), lower = spot * std::exp(-half);
        band_text_ = live_fmt::num(lower, 1) + " – " + live_fmt::num(upper, 1) + " around " + live_fmt::num(spot, 1);
        const auto calls = e.chain(in_.under, LiveKind::Call);
        const auto puts = e.chain(in_.under, LiveKind::Put);
        const LiveInstrument* c = nullptr;
        const LiveInstrument* p = nullptr;
        for (const auto* i : calls) if (i->strike >= upper) { c = i; break; }
        for (auto it = puts.rbegin(); it != puts.rend(); ++it) if ((*it)->strike <= lower) { p = *it; break; }
        if (c == nullptr || p == nullptr) { note_ = "Skipped today: a band edge lies outside the streamed chain."; return; }
        const LiveTop tc = e.top(c->token), tp = e.top(p->token);
        if (static_cast<double>(tc.bid) < min_premium_ * 100.0 || static_cast<double>(tp.bid) < min_premium_ * 100.0) {
            note_ = "Skipped today: a leg bids under " + live_fmt::num(min_premium_) + " rupees.";
            return;
        }
        std::string why;
        const std::string reason = "09:20 80% band " + band_text_;
        if (!e.book().open(name(), *c, -1, 1, e.clock_ns(), reason, false, &why)) { note_ = "Could not sell the call: " + why; return; }
        if (!e.book().open(name(), *p, -1, 1, e.clock_ns(), reason, false, &why)) {
            (void)e.book().close(name(), c->token, e.clock_ns(), "put leg unfilled");
            note_ = "Could not sell the put: " + why;
            return;
        }
        call_ = c;
        put_ = p;
        note_ = "Sold at 09:20: " + c->symbol + " and " + p->symbol + ".";
    }

    LiveVolInputs in_;
    double stop_, min_premium_;
    bool traded_today_ = false;
    std::string note_, band_text_;
    const LiveInstrument* call_ = nullptr;
    const LiveInstrument* put_ = nullptr;
};

// ---------------------------------------------------------------------------
// BANKNIFTY against NIFTY
// ---------------------------------------------------------------------------

/// The pair's formation, from daily closes before today.
struct LivePairsInputs {
    bool ok = false;
    std::string why;               ///< when not ok
    double alpha = 0.0, beta = 0.0, mu = 0.0, sd = 0.0, half_life = 0.0;
    std::size_t days = 0;
};

/// OLS of ln B on ln A over the last `days` closes, and the residual's mean,
/// sd and AR(1) half-life. Closes are aligned by the caller.
[[nodiscard]] inline LivePairsInputs live_pairs_formation(const std::vector<double>& a, const std::vector<double>& b,
                                                          std::size_t days = 250) {
    LivePairsInputs f;
    if (a.size() != b.size() || a.size() < days || days < 60) { f.why = "fewer than " + std::to_string(days) + " aligned daily closes"; return f; }
    const std::size_t n = days, off = a.size() - days;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = std::log(a[off + i]), y = std::log(b[off + i]);
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    const double den = static_cast<double>(n) * sxx - sx * sx;
    if (!(std::fabs(den) > 0.0)) { f.why = "constant prices"; return f; }
    f.beta = (static_cast<double>(n) * sxy - sx * sy) / den;
    f.alpha = (sy - f.beta * sx) / static_cast<double>(n);
    std::vector<double> e(n);
    double m = 0.0;
    for (std::size_t i = 0; i < n; ++i) { e[i] = std::log(b[off + i]) - f.alpha - f.beta * std::log(a[off + i]); m += e[i]; }
    m /= static_cast<double>(n);
    double ss = 0.0, num = 0.0, dd = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        ss += (e[i] - m) * (e[i] - m);
        if (i > 0) { num += (e[i] - m) * (e[i - 1] - m); dd += (e[i - 1] - m) * (e[i - 1] - m); }
    }
    f.mu = m;
    f.sd = std::sqrt(ss / static_cast<double>(n - 1));
    const double phi = dd > 0.0 ? num / dd : 1.0;
    f.half_life = phi > 0.0 && phi < 1.0 ? -std::log(2.0) / std::log(phi) : std::numeric_limits<double>::infinity();
    f.days = n;
    f.ok = f.sd > 0.0;
    if (!f.ok) f.why = "the spread does not vary";
    return f;
}

/// The backtest's thresholds (strategies/pairs_futures.hpp, PairsPolicy).
struct LivePairsRule {
    double entry = 2.0, exit = 0.5, stop = 4.0, max_half_life = 30.0;
};

class LivePairsModel final : public LiveModel {
public:
    explicit LivePairsModel(LivePairsInputs f, LivePairsRule r = {}) : f_(std::move(f)), r_(r) {}

    [[nodiscard]] std::string name() const override { return "Pairs BANKNIFTY/NIFTY"; }
    [[nodiscard]] std::string family() const override { return "pairs"; }
    [[nodiscard]] bool carry() const override { return true; }

    void on_minute(LiveEngine& e, int m) override {
        z_ = zscore(e);
        if (m != kLiveRollMinute || !f_.ok || !std::isfinite(z_)) return;
        LiveDecisionScope log(e, name(), [this] { return "z " + live_fmt::num(z_) + ": " + note_; });
        const LiveInstrument* a = e.carry_future("NIFTY");
        const LiveInstrument* b = e.carry_future("BANKNIFTY");
        const bool in = e.book().engaged(name());
        if (in) {
            if (std::fabs(z_) <= r_.exit || std::fabs(z_) >= r_.stop) {
                const std::string why = std::fabs(z_) <= r_.exit ? "spread back to mean (z " + live_fmt::num(z_) + ")"
                                                                 : "stop (z " + live_fmt::num(z_) + ")";
                e.book().close_if([this](const LivePosition& p) { return p.model == name(); }, e.clock_ns(), why);
                note_ = "Closed at 15:15: " + why + ".";
            }
            return;
        }
        if (std::fabs(z_) < r_.entry) { note_ = "Flat: |z| " + live_fmt::num(std::fabs(z_)) + " is under " + live_fmt::num(r_.entry) + "."; return; }
        if (std::fabs(z_) >= r_.stop) { note_ = "Not entered: |z| is already past the stop."; return; }
        if (f_.half_life > r_.max_half_life) { note_ = "Not entered: half-life " + live_fmt::num(f_.half_life, 1) + " d is too slow."; return; }
        if (e.stale() || a == nullptr || b == nullptr) { note_ = "Not entered: no futures or a stale feed."; return; }
        const double pa = e.mid(a->token), pb = e.mid(b->token);
        if (!(pa > 0.0) || !(pb > 0.0)) { note_ = "Not entered: a future has no price."; return; }
        // B is one lot; A is the whole number of lots nearest beta x B's notional.
        const double lots_a_f = f_.beta * pb * static_cast<double>(b->lot) / (pa * static_cast<double>(a->lot));
        const std::int64_t lots_a = std::max<std::int64_t>(1, std::llround(lots_a_f));   // RULE 11: safe-side floor -- a hedge leg is at least one lot.
        const int side_b = z_ > 0.0 ? -1 : 1;   // spread rich: sell B, buy A
        const std::string why = "z " + live_fmt::num(z_) + " at 15:15";
        if (!e.book().open(name(), *b, side_b, 1, e.clock_ns(), why, true)
            || !e.book().open(name(), *a, -side_b, lots_a, e.clock_ns(), why, true)) {
            e.book().close_if([this](const LivePosition& p) { return p.model == name(); }, e.clock_ns(), "leg unfilled");
            note_ = "Could not fill both legs.";
            return;
        }
        note_ = std::string(side_b < 0 ? "Short BANKNIFTY / long " : "Long BANKNIFTY / short ") + std::to_string(lots_a)
              + " lot(s) NIFTY at z " + live_fmt::num(z_) + ".";
    }

    [[nodiscard]] LiveModelView view(const LiveEngine& e) const override {
        LiveModelView v;
        v.name = name();
        v.family = family();
        v.state = !f_.ok ? "abstaining" : (e.book().flat(name()) ? "watching" : "in position");
        v.signal = std::isfinite(z_) ? "z " + live_fmt::num(z_) : "";
        v.reason = !f_.ok ? "No formation: " + f_.why
                          : (note_.empty() ? "Decides once a day at 15:15: in at |z| >= 2, out at |z| <= 0.5, stop at 4; carried overnight." : note_);
        if (f_.ok) {
            v.fields.push_back({"beta (250 d)", live_fmt::num(f_.beta, 3)});
            v.fields.push_back({"spread sd", live_fmt::num(f_.sd, 4)});
            v.fields.push_back({"half-life", live_fmt::num(f_.half_life, 1) + " d"});
        }
        return v;
    }

private:
    [[nodiscard]] double zscore(const LiveEngine& e) const {
        const LiveInstrument* a = e.index_of("NIFTY");
        const LiveInstrument* b = e.index_of("BANKNIFTY");
        if (!f_.ok || a == nullptr || b == nullptr) return std::numeric_limits<double>::quiet_NaN();
        const double pa = e.ltp(a->token), pb = e.ltp(b->token);
        if (!(pa > 0.0) || !(pb > 0.0)) return std::numeric_limits<double>::quiet_NaN();
        return (std::log(pb) - f_.alpha - f_.beta * std::log(pa) - f_.mu) / f_.sd;
    }

    LivePairsInputs f_;
    LivePairsRule r_;
    double z_ = std::numeric_limits<double>::quiet_NaN();
    std::string note_;
};

// ---------------------------------------------------------------------------
// NIFTY 50 stat-arb
// ---------------------------------------------------------------------------

/// The panel through yesterday, and which streamed instruments each stock is.
struct LiveStatArbInputs {
    bool ok = false;
    std::string why;
    RrPanel history;                       ///< daily log returns through yesterday
    std::vector<std::string> symbols;      ///< per panel stock
    std::vector<double> last_close;        ///< rupees, yesterday, per stock
    double market_last_close = 0.0;        ///< NIFTY, yesterday
};

class LiveStatArbModel final : public LiveModel {
public:
    explicit LiveStatArbModel(LiveStatArbInputs in, RrPolicy pol = {}) : in_(std::move(in)), pol_(pol) {}

    [[nodiscard]] std::string name() const override { return "Stat-arb NIFTY 50"; }
    [[nodiscard]] std::string family() const override { return "stat-arb"; }
    [[nodiscard]] bool carry() const override { return true; }

    void on_minute(LiveEngine& e, int m) override {
        if (m != kLiveRollMinute || !in_.ok) return;
        LiveDecisionScope log(e, name(), [this] { return note_; });
        const LiveInstrument* idx = e.index_of("NIFTY");
        const double mkt = idx ? e.ltp(idx->token) : 0.0;
        if (!(mkt > 0.0) || !(in_.market_last_close > 0.0)) { note_ = "No NIFTY price at 15:15."; return; }
        // Today's return so far is the panel's last day.
        RrPanel p = in_.history;
        p.day.push_back(e.today());
        p.market.push_back(std::log(mkt / in_.market_last_close));
        std::size_t priced = 0;
        for (std::size_t i = 0; i < p.ret.size(); ++i) {
            const LiveInstrument* eq = equity(e, in_.symbols[i]);
            const double px = eq ? e.ltp(eq->token) : 0.0;
            const bool have = px > 0.0 && in_.last_close[i] > 0.0;
            p.ret[i].push_back(have ? std::log(px / in_.last_close[i]) : std::numeric_limits<double>::quiet_NaN());
            priced += have ? 1u : 0u;
        }
        const auto sc = rr_scores_at_last(p, pol_);
        if (!sc) { note_ = std::string("Scores refused: ") + rr_error_text(sc.error()); return; }
        scores_ = *sc;
        std::size_t opened = 0, closed = 0;
        for (std::size_t i = 0; i < scores_.size(); ++i) {
            const LiveInstrument* fut = e.carry_future(in_.symbols[i]);
            const double s = scores_[i].s;
            // Close first.
            for (const auto& pos : e.book().positions()) {
                if (pos.model != name() || pos.inst.underlying != in_.symbols[i] || pos.state == LivePosState::Closing) continue;
                const bool done = std::isfinite(s) && ((pos.side > 0 && s > -pol_.exit_long) || (pos.side < 0 && s < pol_.exit_short));
                const bool stale = e.clock_ns() - pos.entry_ns > static_cast<std::int64_t>(pol_.max_hold) * 86'400'000'000'000LL * 7 / 5;
                if (done || stale) {
                    closed += e.book().close(name(), pos.inst.token, e.clock_ns(), done ? "s " + live_fmt::num(s) : "max hold") ? 1u : 0u;
                }
            }
            if (!std::isfinite(s) || fut == nullptr || e.stale()) continue;
            bool held = false;
            for (const auto& pos : e.book().positions())
                held = held || (pos.model == name() && pos.inst.underlying == in_.symbols[i] && pos.state != LivePosState::Closing);
            if (held) continue;
            const int side = s < -pol_.entry ? 1 : (s > pol_.entry ? -1 : 0);
            if (side == 0) continue;
            opened += e.book().open(name(), *fut, side, 1, e.clock_ns(), "s " + live_fmt::num(s) + " at 15:15", true) ? 1u : 0u;
        }
        note_ = "15:15: " + std::to_string(priced) + " stocks priced, " + std::to_string(opened) + " opened, "
              + std::to_string(closed) + " closed.";
    }

    [[nodiscard]] LiveModelView view(const LiveEngine& e) const override {
        LiveModelView v;
        v.name = name();
        v.family = family();
        std::size_t n = 0;
        for (const auto& p : e.book().positions()) n += p.model == name() ? 1u : 0u;
        v.state = !in_.ok ? "abstaining" : (n > 0 ? "in position" : "watching");
        v.signal = n > 0 ? std::to_string(n) + " stock future(s) held" : "";
        v.reason = !in_.ok ? "No history: " + in_.why
                           : (note_.empty() ? "Decides once a day at 15:15 on Avellaneda-Lee s-scores; one lot of each stock's future; carried." : note_);
        // The strongest five scores.
        std::vector<std::pair<double, std::string>> best;
        for (std::size_t i = 0; i < scores_.size(); ++i)
            if (std::isfinite(scores_[i].s)) best.push_back({scores_[i].s, in_.symbols[i]});
        std::sort(best.begin(), best.end(), [](const auto& a, const auto& b) { return std::fabs(a.first) > std::fabs(b.first); });
        for (std::size_t k = 0; k < best.size() && k < 5; ++k) v.fields.push_back({best[k].second, "s " + live_fmt::num(best[k].first)});
        return v;
    }

private:
    [[nodiscard]] static const LiveInstrument* equity(const LiveEngine& e, const std::string& sym) {
        for (const auto& i : e.universe()) if (i.kind == LiveKind::Equity && i.symbol == sym) return &i;
        return nullptr;
    }

    LiveStatArbInputs in_;
    RrPolicy pol_;
    std::vector<RrLiveScore> scores_;
    std::string note_;
};

} // namespace altair::live
