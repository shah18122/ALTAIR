// live/threshold.hpp -- threshold strategies. First: OHL (open = high / open = low).
//
// THE RULE, AS SMIT GAVE IT. For a future, at the open of the normal market:
//   * open == high  -> SELL one lot; stop-loss at high + 0.5 %;
//   * open == low   -> BUY one lot;  stop-loss at low  - 0.5 %;
//   * once the trade is 1.5 % in profit, a trailing stop follows the best
//     price by 0.25 % (it only tightens);
//   * anything still open goes at the 15:20 square-off (the engine's).
//
// WHEN. The open is set in the pre-open auction (09:00-09:08) and the normal
// market starts at 09:15:00. The check is made once the first `window` of
// trading (default one second) has printed: by then the exchange's high and
// low (the quote topic's OHLC) say whether the open has already been
// exceeded on one side. Equal to the paisa, as the rule says: an open that is
// both the high and the low (nothing has traded away from it) is no signal.
// If the exchange's OHLC is not on the feed, the first trades of the window
// set them.
//
// The decision is made on a quote, not at a minute's close: the rule is about
// the first second, and waiting for 09:16 would trade a minute late.

#pragma once

#include <live/engine.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace altair::live {

/// The OHL rule's numbers (Threshold page).
struct LiveOhlRule {
    std::int64_t window_ns = 1'000'000'000LL;   ///< the first second of 09:15
    double stop_pct = 0.5;                      ///< beyond the high (sell) or the low (buy)
    double trail_start_pct = 1.5;               ///< profit at which the trail starts
    double trail_pct = 0.25;                    ///< how far the trail follows the best price
    std::int64_t lots = 1;
};

/// One instrument's OHL state today.
struct LiveOhlLeg {
    std::uint32_t token = 0;
    std::string symbol;
    std::int64_t open = 0, high = 0, low = 0;   ///< paise, at the check
    int signal = 0;                             ///< +1 buy (open = low), -1 sell (open = high), 0 none
    double stop = 0.0;                          ///< rupees
    double best = 0.0;                          ///< best price since entry, rupees
    bool trailing = false;
    bool entered = false, done = false;
    std::string note;
};

class LiveOhlModel final : public LiveModel {
public:
    /// `underlyings` empty: every underlying whose near future streams.
    explicit LiveOhlModel(LiveOhlRule r = {}, std::vector<std::string> underlyings = {})
        : r_(r), unders_(std::move(underlyings)) {}

    [[nodiscard]] std::string name() const override { return "OHL"; }
    [[nodiscard]] std::string family() const override { return "threshold"; }

    void on_new_day(LiveEngine&) override {
        legs_.clear();
        seen_.clear();
        checked_ = false;
        note_.clear();
    }
    void on_minute(LiveEngine& e, int m) override {
        // A late start (after the first second) still checks: the exchange's
        // high and low say what the open did, whenever they are read.
        if (!checked_ && m >= kLiveOpenMinute) check(e, e.clock_ns());
    }
    void on_quote(LiveEngine& e, std::uint32_t token) override {
        const std::int64_t ns = e.quote_ns();
        if (!checked_) {
            const std::int64_t open_ns = (e.today() * 86400 + kLiveOpenMinute * 60 - 19800) * 1'000'000'000LL;
            if (ns < open_ns + r_.window_ns) { track(e, token); return; }
            check(e, ns);
            return;
        }
        manage(e, token, ns);
    }

    [[nodiscard]] LiveModelView view(const LiveEngine& e) const override {
        LiveModelView v;
        v.name = name();
        v.family = family();
        std::size_t held = 0;
        for (const auto& p : e.book().positions()) held += p.model == name() ? 1u : 0u;
        v.state = held > 0 ? "in position" : (checked_ ? "done today" : "watching");
        int buys = 0, sells = 0;
        for (const auto& [tok, l] : legs_) { buys += l.signal > 0 ? 1 : 0; sells += l.signal < 0 ? 1 : 0; }
        v.signal = checked_ ? std::to_string(buys) + " open = low (buy), " + std::to_string(sells) + " open = high (sell)" : "";
        v.reason = !note_.empty() ? note_
                 : "At 09:15:00 + " + live_fmt::num(static_cast<double>(r_.window_ns) / 1e9, 1) + " s: open = high sells, open = low buys; "
                   "stop " + live_fmt::num(r_.stop_pct, 2) + " % beyond the high / low; after " + live_fmt::num(r_.trail_start_pct, 1)
                   + " % profit a " + live_fmt::num(r_.trail_pct, 2) + " % trailing stop.";
        for (const auto& [tok, l] : legs_) {
            if (l.signal == 0) continue;
            v.fields.push_back({l.symbol, std::string(l.signal > 0 ? "BUY (open = low " : "SELL (open = high ")
                                              + live_fmt::num(static_cast<double>(l.open) / 100.0) + "), stop "
                                              + live_fmt::num(l.stop) + (l.trailing ? " trailing" : "") + (l.note.empty() ? "" : " · " + l.note)});
        }
        return v;
    }

    [[nodiscard]] const std::map<std::uint32_t, LiveOhlLeg>& legs() const noexcept { return legs_; }
    [[nodiscard]] const LiveOhlRule& rule() const noexcept { return r_; }

private:
    /// The futures it watches: the near future of each underlying.
    [[nodiscard]] std::vector<const LiveInstrument*> futures(const LiveEngine& e) const {
        std::vector<const LiveInstrument*> out;
        std::vector<std::string> seen;
        for (const auto& i : e.universe()) {
            if (i.kind != LiveKind::Future) continue;
            if (!unders_.empty() && std::find(unders_.begin(), unders_.end(), i.underlying) == unders_.end()) continue;
            if (std::find(seen.begin(), seen.end(), i.underlying) != seen.end()) continue;
            seen.push_back(i.underlying);
            if (const LiveInstrument* f = e.near_future(i.underlying)) out.push_back(f);
        }
        return out;
    }
    /// Before the check: the first trades' range, for a feed without OHLC.
    void track(LiveEngine& e, std::uint32_t token) {
        const LiveState* s = e.state(token);
        if (s == nullptr || s->ltp <= 0) return;
        auto& t = seen_[token];
        if (t.open == 0) t.open = s->ltp;
        t.high = std::max(t.high, s->ltp);
        t.low = t.low == 0 ? s->ltp : std::min(t.low, s->ltp);
    }
    void check(LiveEngine& e, std::int64_t ns) {
        checked_ = true;
        LiveDecisionScope log(e, name(), [this] { return note_; });
        int entered = 0, signals = 0;
        for (const LiveInstrument* f : futures(e)) {
            const LiveState* s = e.state(f->token);
            LiveOhlLeg l;
            l.token = f->token;
            l.symbol = f->symbol;
            if (s != nullptr && s->open > 0 && s->high > 0 && s->low > 0) {
                l.open = s->open; l.high = s->high; l.low = s->low;
            } else if (const auto it = seen_.find(f->token); it != seen_.end() && it->second.open > 0) {
                l.open = it->second.open; l.high = it->second.high; l.low = it->second.low;
            } else {
                l.note = "no open yet";
                legs_[f->token] = l;
                continue;
            }
            if (l.open == l.high && l.low < l.open) l.signal = -1;
            else if (l.open == l.low && l.high > l.open) l.signal = 1;
            if (l.signal == 0) { legs_[f->token] = l; continue; }
            ++signals;
            l.stop = l.signal < 0 ? static_cast<double>(l.high) / 100.0 * (1.0 + r_.stop_pct / 100.0)
                                  : static_cast<double>(l.low) / 100.0 * (1.0 - r_.stop_pct / 100.0);
            std::string why;
            const std::string reason = std::string(l.signal < 0 ? "open = high " : "open = low ")
                                     + live_fmt::num(static_cast<double>(l.open) / 100.0) + ", stop " + live_fmt::num(l.stop);
            if (e.stale()) { l.note = "feed stale: not entered"; legs_[f->token] = l; continue; }
            if (e.book().open(name(), *f, l.signal, r_.lots, ns, reason, false, &why)) {
                l.entered = true;
                ++entered;
            } else {
                l.note = "not entered: " + why;
            }
            legs_[f->token] = l;
        }
        note_ = "09:15 check at " + live_fmt::hhmm(live_minute_of_day(live_ist_minute_index(ns))) + ": " + std::to_string(signals)
              + " signal(s) among " + std::to_string(legs_.size()) + " futures, " + std::to_string(entered) + " entered.";
    }
    /// The stop and the trail, on every quote of a held future.
    void manage(LiveEngine& e, std::uint32_t token, std::int64_t ns) {
        const auto it = legs_.find(token);
        if (it == legs_.end() || !it->second.entered || it->second.done) return;
        LiveOhlLeg& l = it->second;
        const LivePosition* p = e.book().position(name(), token);
        if (p == nullptr) { l.done = true; return; }
        if (!p->filled() || p->state == LivePosState::Closing) return;
        const LiveTop t = e.top(token);
        // What the position could be closed at now: the bid for a long, the ask for a short.
        const double exit_px = static_cast<double>(p->side > 0 ? t.bid : t.ask) / 100.0;
        if (!(exit_px > 0.0) || !e.book().fresh(t.quote_ns, ns)) return;
        const double entry = p->entry;
        if (l.best == 0.0) l.best = entry;
        l.best = p->side > 0 ? std::max(l.best, exit_px) : std::min(l.best, exit_px);
        const double profit_pct = (exit_px - entry) / entry * 100.0 * static_cast<double>(p->side);
        if (profit_pct >= r_.trail_start_pct) l.trailing = true;
        if (l.trailing) {
            const double trail = p->side > 0 ? l.best * (1.0 - r_.trail_pct / 100.0) : l.best * (1.0 + r_.trail_pct / 100.0);
            l.stop = p->side > 0 ? std::max(l.stop, trail) : std::min(l.stop, trail);   // only tightens
        }
        const bool hit = p->side > 0 ? exit_px <= l.stop : exit_px >= l.stop;
        if (!hit) return;
        const std::string why = (l.trailing ? "trailing stop " : "stop-loss ") + live_fmt::num(l.stop) + " at " + live_fmt::num(exit_px);
        (void)e.book().close(name(), token, ns, why);
        l.done = true;
        l.note = why;
        e.note_decision(name(), l.symbol + ": " + why);
    }

    struct Seen { std::int64_t open = 0, high = 0, low = 0; };
    LiveOhlRule r_;
    std::vector<std::string> unders_;
    std::map<std::uint32_t, LiveOhlLeg> legs_;
    std::map<std::uint32_t, Seen> seen_;
    bool checked_ = false;
    std::string note_;
};

} // namespace altair::live
