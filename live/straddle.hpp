// live/straddle.hpp -- the tick-by-tick straddle seller: sells the at-the-money
// straddle of the nearest expiry when its implied vol, on the bid, is at least
// the forecast of the vol the index will realise.
//
// WHAT IT READS, ON EVERY QUOTE OF ITS INDEX OR ITS CHAIN (09:15 to 15:10):
//   * IMPLIED VOL: the forward from put-call parity at the strike nearest the
//     index (F = K + C - P, mids), the strike nearest that forward, and the
//     straddle's Black-76 vol twice -- at the mid (the reading) and at the
//     bids (what a sale gets, and what the rule uses);
//   * REALISED VOL SO FAR: squared one-minute returns of the index since the
//     previous close (the gap included), plus the minute in progress;
//   * THE REALISED-VOL FORECAST FOR TODAY, updated with every tick: what has
//     been realised so far, plus HAR's forecast of the session's variance
//     (LiveVolInputs: fitted on closes before today) for the share of the
//     session still to run -- annualised, so it compares with the implied;
//   * INDIA VIX now, and HAR's forecast of today's VIX close (shown only: on
//     NSE closes 2016-2026 it does not beat "no change", R2 -1.6 %).
// The implied vol needs no forecast here: the straddle is sold at the bid
// seen on the tick, so the vol it is sold at is observed, not predicted.
//
// THE RULE (Smit, 2026-10-25): sell when forecast IV >= forecast RV, with no
// fixed margin. One straddle a session, one lot a leg, at the first tick it
// holds; held to the engine's 15:20 square-off. No stop: 1:2 stops in bps of
// spot lowered every variant tested. No futures hedge: on NSE opens and
// closes 2019-2026 the unhedged intraday sale did best (Rs 8.5L per lot
// against 6.0L hedged hourly).
//
// SELLS ONLY. It opens short options and buys them back to close; it never
// opens a long option. Paper only: the real-order bridge sends only the
// strategies switched on in the LIVE arm, and this one has no switch there.
//
// EVERY DECISION HAS ITS REASON: the entry carries the readings that made it
// (the fill's why_in), and a session without a sale records why at 15:10, so
// decisions.csv has one finding per index per day, trade or not.
//
// HONESTY. The backtest behind the rule sold at NSE's opening prints, the
// first trade of each option; if a seller gets 2 % less than that print, the
// edge falls to t 1.2. This model trades the live bid instead, which is
// exactly what the backtest could not see: its paper record is the test.

#pragma once

#include <live/engine.hpp>
#include <live/models.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace altair::live {

/// What the straddle seller knows before the session.
struct LiveStraddleInputs {
    LiveVolInputs vol;                ///< HAR's sigma of today, the session's share, the previous close
    double vix_close = 0.0;           ///< INDIA VIX's last close before today
    double vix_forecast = 0.0;        ///< HAR's forecast of today's VIX close; 0 when none
};

namespace live_straddle_detail {

inline constexpr double kRate = 0.065;
inline constexpr double kYearSec = 365.0 * 86400.0;
/// 15:30 IST, the options' expiry time, as UTC seconds into the day.
inline constexpr std::int64_t kExpiryUtcSec = 15 * 3600 + 30 * 60 - 19800;

[[nodiscard]] inline double ncdf(double x) noexcept { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

/// Black-76 straddle price (call + put) at vol `s`.
[[nodiscard]] inline double straddle_price(double F, double K, double T, double s) noexcept {
    const double df = std::exp(-kRate * T);
    const double sd = s * std::sqrt(T);
    const double d1 = (std::log(F / K) + 0.5 * sd * sd) / sd, d2 = d1 - sd;
    return df * (F * (2.0 * ncdf(d1) - 1.0) - K * (2.0 * ncdf(d2) - 1.0));
}

/// The vol that prices the straddle at `price`; NaN when no vol does
/// (below intrinsic, or above any sane vol). Bisection: the straddle is
/// increasing in vol, so it cannot miss.
[[nodiscard]] inline double straddle_iv(double price, double F, double K, double T) noexcept {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (!(price > 0.0) || !(F > 0.0) || !(K > 0.0) || !(T > 0.0)) return nan;
    double lo = 1e-4, hi = 5.0;
    if (straddle_price(F, K, T, lo) > price || straddle_price(F, K, T, hi) < price) return nan;
    for (int i = 0; i < 100; ++i) {
        const double mid = 0.5 * (lo + hi);
        (straddle_price(F, K, T, mid) < price ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

/// Years from `ns` to 15:30 IST on `expiry_day`; at least one minute.
[[nodiscard]] inline double years_to(std::int64_t ns, std::int64_t expiry_day) noexcept {
    const double left = static_cast<double>(expiry_day * 86400 + kExpiryUtcSec) - static_cast<double>(ns) / 1e9;
    return std::max(left, 60.0) / kYearSec;
}

} // namespace live_straddle_detail

/// One reading of the straddle and the vols, as of a tick.
struct LiveStraddleReading {
    bool ok = false;
    std::string why;                         ///< when not ok
    const LiveInstrument* call = nullptr;
    const LiveInstrument* put = nullptr;
    double spot = 0.0, forward = 0.0, strike = 0.0, years = 0.0;
    double bid = 0.0, mid = 0.0;             ///< straddle, rupees
    double iv_mid = 0.0, iv_bid = 0.0;       ///< annualised, fractions
    double rv_so_far = 0.0;                  ///< today's realised vol so far, annualised
    double rv_forecast = 0.0;                ///< realised so far + HAR for the rest, annualised
    double vix = 0.0;
};

class LiveStraddleModel final : public LiveModel {
public:
    explicit LiveStraddleModel(LiveStraddleInputs in) : in_(std::move(in)) {}

    static constexpr int kFirstMinute = kLiveOpenMinute;          ///< 09:15
    static constexpr int kLastEntryMinute = 15 * 60 + 10;         ///< 15:10
    /// A leg whose spread is wider than this share of its mid (or 1 rupee, if more) is not read.
    static constexpr double kMaxSpread = 0.05;

    [[nodiscard]] std::string name() const override { return "Straddle IV>=RV " + in_.vol.under; }
    [[nodiscard]] std::string family() const override { return "option selling"; }

    void on_new_day(LiveEngine&) override {
        traded_ = false; closed_note_ = false; resumed_checked_ = false;
        note_.clear(); last_ = {}; call_ = put_ = nullptr; ticks_ = 0;
    }

    void on_quote(LiveEngine& e, std::uint32_t token) override {
        if (traded_) return;
        const LiveInstrument* in = e.instrument(token);
        if (in == nullptr || in->underlying != in_.vol.under) return;
        consider(e, e.quote_ns());
    }

    void on_minute(LiveEngine& e, int m) override {
        if (!resumed_checked_) resume(e, m);
        if (traded_) return;
        if (m > kLastEntryMinute) {
            if (!closed_note_) {
                closed_note_ = true;
                note_ = "No sale today: " + (last_.ok ? reading_text(last_) + "; forecast IV stayed below forecast RV"
                                                      : (last_.why.empty() ? std::string("no reading") : last_.why))
                      + " (" + std::to_string(ticks_) + " ticks read).";
                e.note_decision(name(), note_);
            }
            return;
        }
        consider(e, e.clock_ns());   // the minute's close is a tick too, for a quiet chain
    }

    [[nodiscard]] LiveModelView view(const LiveEngine& e) const override {
        LiveModelView v;
        v.name = name();
        v.family = family();
        v.state = !e.book().flat(name()) ? "in position" : (traded_ || closed_note_ ? "done today" : "watching");
        v.signal = last_.ok ? "IV " + live_fmt::num(100 * last_.iv_bid, 2) + " vs RV fc " + live_fmt::num(100 * last_.rv_forecast, 2)
                                  + (last_.iv_bid >= last_.rv_forecast ? " → SELL" : " → wait")
                            : std::string();
        v.reason = !note_.empty() ? note_
                                  : "On every tick 09:15-15:10: sells the ATM straddle of the nearest " + in_.vol.under
                                        + " expiry, one lot a leg, once forecast IV (the bid) >= forecast RV; bought back by 15:20.";
        if (last_.ok) {
            v.fields.push_back({"straddle", live_fmt::num(last_.strike, 0) + ": bid " + live_fmt::num(last_.bid) + ", mid " + live_fmt::num(last_.mid)});
            v.fields.push_back({"implied vol", "mid " + live_fmt::num(100 * last_.iv_mid, 2) + ", bid " + live_fmt::num(100 * last_.iv_bid, 2)});
            v.fields.push_back({"realised vol", "so far " + live_fmt::num(100 * last_.rv_so_far, 2) + ", forecast " + live_fmt::num(100 * last_.rv_forecast, 2)});
        } else if (!last_.why.empty()) {
            v.fields.push_back({"reading", last_.why});
        }
        v.fields.push_back({"INDIA VIX", vix_text(last_.vix)});
        v.fields.push_back({"HAR sigma today", live_fmt::num(100 * in_.vol.sigma_day, 3) + " % (" + live_fmt::num(100 * in_.vol.sigma_day * std::sqrt(252.0), 2) + " annualised)"});
        return v;
    }

    /// The newest reading (for tests and the page).
    [[nodiscard]] const LiveStraddleReading& reading() const noexcept { return last_; }

    /// Read the straddle and the vols as they stand at `ns`.
    [[nodiscard]] LiveStraddleReading read(const LiveEngine& e, std::int64_t ns) const {
        namespace d = live_straddle_detail;
        LiveStraddleReading r;
        const LiveInstrument* vix = e.index_of("INDIAVIX");
        r.vix = vix ? e.mid(vix->token) : 0.0;
        const LiveInstrument* idx = e.index_of(in_.vol.under);
        r.spot = idx ? e.mid(idx->token) : 0.0;
        if (!(r.spot > 0.0)) { r.why = "no " + in_.vol.under + " index price"; return r; }
        if (!(in_.vol.sigma_day > 0.0)) { r.why = "no HAR forecast of today's realised vol"; return r; }
        const auto calls = e.chain(in_.vol.under, LiveKind::Call);
        const auto puts = e.chain(in_.vol.under, LiveKind::Put);
        // The pair at strike k, with both legs quoted sanely now.
        struct Leg { const LiveInstrument* c = nullptr; const LiveInstrument* p = nullptr; double cb = 0, ca = 0, pb = 0, pa = 0; };
        const auto pair_at = [&](double k, Leg& out) {
            const LiveInstrument* c = nullptr;
            const LiveInstrument* p = nullptr;
            for (const auto* i : calls) if (i->strike == k) c = i;
            for (const auto* i : puts) if (i->strike == k) p = i;
            if (c == nullptr || p == nullptr) return false;
            const LiveTop tc = e.top(c->token), tp = e.top(p->token);
            if (!e.book().fresh(tc.quote_ns, ns) || !e.book().fresh(tp.quote_ns, ns)) return false;
            out = {c, p, static_cast<double>(tc.bid) / 100.0, static_cast<double>(tc.ask) / 100.0,
                   static_cast<double>(tp.bid) / 100.0, static_cast<double>(tp.ask) / 100.0};
            for (const auto& [b, a] : {std::pair{out.cb, out.ca}, std::pair{out.pb, out.pa}}) {
                if (!(b > 0.0) || !(a > b)) return false;
                if (a - b > std::max(1.0, kMaxSpread * 0.5 * (a + b))) return false;
            }
            return true;
        };
        const auto nearest = [&](double x, Leg& out) {
            double best = std::numeric_limits<double>::infinity();
            bool found = false;
            for (const auto* c : calls) {
                Leg l;
                if (std::fabs(c->strike - x) >= best || !pair_at(c->strike, l)) continue;
                best = std::fabs(c->strike - x);
                out = l;
                found = true;
            }
            return found;
        };
        Leg at;
        if (!nearest(r.spot, at)) { r.why = "no strike near " + live_fmt::num(r.spot, 0) + " with both legs quoted within 5 %"; return r; }
        // Put-call parity at that strike gives the expiry's forward; the ATM strike is the one nearest it.
        const double k0 = at.c->strike;
        r.years = d::years_to(ns, at.c->expiry_day);
        r.forward = k0 + (0.5 * (at.ca + at.cb) - 0.5 * (at.pa + at.pb)) * std::exp(d::kRate * r.years);
        Leg atm = at;
        (void)nearest(r.forward, atm);
        r.call = atm.c; r.put = atm.p; r.strike = atm.c->strike;
        r.bid = atm.cb + atm.pb;
        r.mid = 0.5 * (atm.ca + atm.cb + atm.pa + atm.pb);
        r.iv_mid = d::straddle_iv(r.mid, r.forward, r.strike, r.years);
        r.iv_bid = d::straddle_iv(r.bid, r.forward, r.strike, r.years);
        if (!std::isfinite(r.iv_bid) || !std::isfinite(r.iv_mid)) { r.why = "the straddle at " + live_fmt::num(r.strike, 0) + " has no implied vol"; return r; }
        // Realised so far: one-minute closes since the previous close, plus the minute in progress.
        const auto& bars = e.bars(idx->token);
        double so_far = live_model_detail::realised_so_far(bars, in_.vol.prev_close);
        const double last = bars.empty() ? in_.vol.prev_close : bars.back().c;
        if (last > 0.0) { const double x = std::log(r.spot / last); so_far += x * x; }
        const int m = live_minute_of_day(live_ist_minute_index(ns));
        const double rest = in_.vol.intraday_share * in_.vol.sigma_day * in_.vol.sigma_day * live_model_detail::remaining_fraction(m);
        r.rv_so_far = std::sqrt(so_far * 252.0);
        r.rv_forecast = std::sqrt((so_far + rest) * 252.0);
        r.ok = true;
        return r;
    }

private:
    void consider(LiveEngine& e, std::int64_t ns) {
        const int m = live_minute_of_day(live_ist_minute_index(ns));
        if (m < kFirstMinute || m > kLastEntryMinute) return;
        ++ticks_;
        last_ = read(e, ns);
        if (!last_.ok || last_.iv_bid < last_.rv_forecast) return;
        if (e.stale()) { last_.why = "the feed is stale"; return; }
        traded_ = true;
        const std::string reason = live_fmt::hhmm(m) + " " + reading_text(last_) + ": forecast IV >= forecast RV, SELL";
        LiveDecisionScope log(e, name(), [this] { return note_; });
        std::string why;
        if (!e.book().open(name(), *last_.call, -1, 1, ns, reason, false, &why)) { note_ = "Could not sell the call: " + why; return; }
        if (!e.book().open(name(), *last_.put, -1, 1, ns, reason, false, &why)) {
            (void)e.book().close(name(), last_.call->token, ns, "put leg refused");
            note_ = "Could not sell the put: " + why;
            return;
        }
        call_ = last_.call; put_ = last_.put;
        note_ = "Sold " + call_->symbol + " + " + put_->symbol + " at " + reason;
    }

    /// A restart: legs this model sold earlier today are in the book. Hold them; never sell twice.
    void resume(LiveEngine& e, int m) {
        resumed_checked_ = true;
        for (const auto& p : e.book().positions()) {
            if (p.model != name()) continue;
            const LiveInstrument* in = e.instrument(p.inst.token);
            if (in == nullptr) continue;
            if (in->kind == LiveKind::Call) call_ = in;
            if (in->kind == LiveKind::Put) put_ = in;
        }
        if (call_ == nullptr && put_ == nullptr) return;
        traded_ = true;
        note_ = "Resumed at " + live_fmt::hhmm(m) + ": holding the straddle sold earlier today.";
        e.note_decision(name(), note_);
    }

    [[nodiscard]] std::string vix_text(double now) const {
        return (now > 0.0 ? live_fmt::num(now) : std::string("—")) + " (close " + live_fmt::num(in_.vix_close) + ", HAR forecast "
             + (in_.vix_forecast > 0.0 ? live_fmt::num(in_.vix_forecast) : std::string("—")) + ": shown only, no skill)";
    }
    [[nodiscard]] std::string reading_text(const LiveStraddleReading& r) const {
        return "spot " + live_fmt::num(r.spot, 1) + ", fwd " + live_fmt::num(r.forward, 1) + ", " + live_fmt::num(r.strike, 0)
             + " straddle bid " + live_fmt::num(r.bid) + " (IV bid " + live_fmt::num(100 * r.iv_bid, 2) + ", mid "
             + live_fmt::num(100 * r.iv_mid, 2) + ") vs RV forecast " + live_fmt::num(100 * r.rv_forecast, 2) + " (so far "
             + live_fmt::num(100 * r.rv_so_far, 2) + "), VIX " + vix_text(r.vix);
    }

    LiveStraddleInputs in_;
    bool traded_ = false, closed_note_ = false, resumed_checked_ = false;
    std::uint64_t ticks_ = 0;
    std::string note_;
    LiveStraddleReading last_;
    const LiveInstrument* call_ = nullptr;
    const LiveInstrument* put_ = nullptr;
};

} // namespace altair::live
