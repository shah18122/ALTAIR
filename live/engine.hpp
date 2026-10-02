// live/engine.hpp -- the live models' engine: market state, bars, the clock,
// the paper book, and the models that act on them.
//
// ONE CLOCK, AND IT IS THE FEED'S. Bars close, decisions fire and positions
// are squared off by the time stamped on the trades the price service
// publishes -- never by the machine's clock. That is what lets the same engine
// run on a live FYERS session, on a simulated one at ten times speed, and in a
// test, and decide identically.
//
// THE DAY'S SCHEDULE (IST):
//   * every minute: the one-minute bars that ended close, then every model's
//     on_minute runs with that close time (09:20 is the close of the 09:19 bar);
//   * 15:15: positional holdings in a contract that expires TODAY are closed
//     ("expiry roll") before the models decide, so a model that still wants
//     the position reopens it in the next contract;
//   * 15:20: every intraday position is squared off.
// THE ROLL AND THE SQUARE-OFF ARE OBLIGATIONS, NOT MOMENTS. They run on the
// first minute at or after their time, however the clock got there -- a feed
// that jumps from 14:40 to 15:25 still rolls and still squares off -- and an
// exit, once submitted, works until it fills (live/paper.hpp). With the feed
// stopped, the clock stops too; watchdog() submits the overdue exits on the
// machine's clock (live feeds only) so they fill the moment quotes return.
//
// A TICK IS CONSUMED AFTER THE CLOCK MOVES. The clock advances to a trade's
// time first -- closing the previous minute, running its decisions, starting a
// new session -- and only then does the trade enter its bar. A late print for
// a minute already closed is refused, never written into history.
//
// GAPS PAUSE DECISIONS. A lost trade frame (a sequence gap) means the bars
// are incomplete: no model decides until one whole clean minute has passed.
// Obligations still run. Snapshot frames (a late joiner's bootstrap) update
// prices but are not trades and never enter a bar.
//
// ONE RISK CHECK BEFORE EVERY ENTRY (LiveRiskLimits): the feed is fresh, no
// halt or kill request, the position count, gross notional and the day's loss
// are inside their limits. Exits are never refused.
//
// NOTHING HERE CAN TRADE. The paper book is arithmetic; live/ links no broker
// and no OMS.

#pragma once

#include <live/bars.hpp>
#include <live/paper.hpp>
#include <live/universe.hpp>
#include <server/price_payload.hpp>
#include <server/quote_payload.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace altair::live {

inline constexpr int kLiveRollMinute = 15 * 60 + 15;       ///< 15:15
inline constexpr int kLiveSquareOffMinute = 15 * 60 + 20;  ///< 15:20

/// The newest state of one instrument, from the feed.
struct LiveState {
    std::int64_t ltp = 0, last_ns = 0, volume = -1, oi = -1;
    std::int64_t bid = 0, ask = 0, prev_close = 0, open = 0;
    std::int64_t bid_qty = 0, ask_qty = 0;
    std::int64_t quote_ns = 0;     ///< feed time of the last bid/ask
    std::int64_t book_ns = 0;      ///< feed time of the last five-level book
    std::uint16_t levels = 0;
    LiveLevel bids[kLiveDepth]{}, asks[kLiveDepth]{};
    bool simulated = false, replay = false;
};

/// The pre-trade risk limits every entry passes (exits are never refused).
struct LiveRiskLimits {
    std::size_t max_positions = 80;        ///< held or working, all models
    std::size_t max_per_model = 60;
    double max_gross_notional = 1.0e8;     ///< rupees: |qty x price| of everything held or working, plus the new order
    double max_daily_loss = 2.0e5;         ///< rupees: today's realised (gross where unpriced) plus open marks
};

/// One decision, in words, at the feed time it was taken: what the CLI
/// appends to decisions.csv -- the record a replay is compared against.
struct LiveDecisionNote {
    std::int64_t ns = 0;
    std::string model;
    std::string text;
};

/// What a model shows on the Live Models page.
struct LiveModelView {
    std::string name, family;
    std::string state;     ///< watching, in position, abstaining, done today
    std::string signal;    ///< the model's current reading, in words
    std::string reason;    ///< why it is (or is not) acting
    std::vector<std::pair<std::string, std::string>> fields;
};

class LiveEngine;

class LiveModel {
public:
    virtual ~LiveModel() = default;
    /// Unique; it is the "model" on every fill and trade this model makes.
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual std::string family() const = 0;
    /// Positional models carry overnight; intraday ones are squared off at 15:20.
    [[nodiscard]] virtual bool carry() const { return false; }
    /// After the one-minute bars ending at `close_minute` (IST minute of day) closed.
    virtual void on_minute(LiveEngine& e, int close_minute) = 0;
    virtual void on_new_day(LiveEngine&) {}
    [[nodiscard]] virtual LiveModelView view(const LiveEngine& e) const = 0;
};

namespace live_fmt {
[[nodiscard]] inline std::string num(double v, int digits = 2) {
    if (!std::isfinite(v)) return "—";
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", digits, v);
    return b;
}
[[nodiscard]] inline std::string pct(double fraction, int digits = 2) {
    return std::isfinite(fraction) ? num(100.0 * fraction, digits) + " %" : "—";
}
[[nodiscard]] inline std::string json_escape(const std::string& s) {
    std::string o;
    for (const char c : s) {
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
        else if (c == '\n') o += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) o.push_back(' ');
        else o.push_back(c);
    }
    return o;
}
[[nodiscard]] inline std::string hhmm(int minute_of_day) {
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d", minute_of_day / 60, minute_of_day % 60);
    return b;
}
} // namespace live_fmt

class LiveEngine {
public:
    LiveEngine(std::vector<LiveInstrument> universe, LiveCostFn cost, LiveExecPolicy policy = {})
        : u_(std::move(universe)),
          book_([this](std::uint32_t tok) { return top(tok); }, std::move(cost), policy) {
        for (std::size_t i = 0; i < u_.size(); ++i) index_[u_[i].token] = i;
        book_.set_risk([this](const LiveRiskRequest& r) { return risk_check(r); });
    }

    void add_model(std::unique_ptr<LiveModel> m) { models_.push_back(std::move(m)); }
    [[nodiscard]] const std::vector<std::unique_ptr<LiveModel>>& models() const noexcept { return models_; }

    // ---- the feed --------------------------------------------------------

    /// A trade, stamped `ns` (the exchange's time where it has one).
    void on_trade(const PricePayload& p, std::int64_t ns) {
        // The clock first: the previous minute closes and decides on what was
        // known then; a new session resets before its first tick is kept.
        advance(ns);
        LiveState& s = state_[p.token];
        s.ltp = p.last_paise;
        s.last_ns = ns;
        if (p.has(kPriceHasVolume)) s.volume = p.volume;
        if (p.has(kPriceHasOi)) s.oi = p.oi;
        s.simulated = p.has(kPriceSimulated);
        s.replay = p.has(kPriceReplay);
        simulated_ = simulated_ || s.simulated;
        replay_ = replay_ || s.replay;
        if (!bars_.feed(p.token, static_cast<double>(p.last_paise) / 100.0, ns, s.volume, [](std::uint32_t, const LiveBar&) {}))
            ++late_prints_;
        book_.on_market(p.token, std::max(ns, clock_ns_));
    }
    /// A late joiner's bootstrap: the last trade as it stands, not a trade now.
    /// Updates the price; never enters a bar or moves the clock.
    void on_trade_snapshot(const PricePayload& p) {
        LiveState& s = state_[p.token];
        if (p.last_paise > 0) s.ltp = p.last_paise;
        if (p.has(kPriceHasVolume)) s.volume = p.volume;
        if (p.has(kPriceHasOi)) s.oi = p.oi;
        s.simulated = p.has(kPriceSimulated);
        s.replay = p.has(kPriceReplay);
        simulated_ = simulated_ || s.simulated;
    }
    /// A quote, received at feed time `ns`.
    void on_quote(const QuotePayload& q, std::int64_t ns) {
        LiveState& s = state_[q.token];
        if (q.has(kQuoteHasTop)) {
            s.bid = q.bid; s.ask = q.ask; s.bid_qty = q.bid_qty; s.ask_qty = q.ask_qty;
            s.quote_ns = ns;
        }
        if (q.has(kQuoteHasPrevClose)) s.prev_close = q.prev_close;
        if (q.has(kQuoteHasOhlc)) s.open = q.open;
        s.simulated = s.simulated || q.has(kQuoteSimulated);
        simulated_ = simulated_ || s.simulated;
        if (q.has(kQuoteHasTop)) book_.on_market(q.token, std::max(ns, clock_ns_));
    }
    /// Five levels a side, at feed time `ns`.
    void on_book(std::uint32_t token, std::uint16_t levels, const LiveLevel* bids, const LiveLevel* asks, std::int64_t ns) {
        LiveState& s = state_[token];
        s.levels = static_cast<std::uint16_t>(std::min<std::size_t>(levels, kLiveDepth));
        for (std::size_t k = 0; k < kLiveDepth; ++k) {
            s.bids[k] = k < s.levels ? bids[k] : LiveLevel{};
            s.asks[k] = k < s.levels ? asks[k] : LiveLevel{};
        }
        s.book_ns = ns;
        book_.on_market(token, std::max(ns, clock_ns_));
    }
    /// Trade frames were lost (a sequence gap): today's bars are incomplete,
    /// so no model decides until a whole clean minute has passed.
    void on_trade_gap(std::int64_t ns, std::uint64_t missed) {
        const std::int64_t at = std::max(ns, clock_ns_);
        pause_until_minute_ = std::max(pause_until_minute_, live_ist_minute_index(at) + 2);
        gaps_ += 1;
        missed_trades_ += missed;
        pause_note_ = (missed > 0 ? std::to_string(missed) + " trade frame(s) lost at "
                                  : std::string("the trade stream restarted at "))
                    + live_fmt::hhmm(live_minute_of_day(live_ist_minute_index(at)));
    }
    /// The feed went quiet (or came back). No new positions while stale.
    void set_stale(bool stale) noexcept { stale_ = stale; }
    [[nodiscard]] bool stale() const noexcept { return stale_; }
    /// A halt the engine did not choose (the ledger cannot be written, or a
    /// kill request): no new entries until it clears. Empty clears it.
    void set_halt(std::string why) { halt_ = std::move(why); }
    [[nodiscard]] const std::string& halt() const noexcept { return halt_; }
    void set_kill(bool on) noexcept { kill_ = on; }
    void set_limits(const LiveRiskLimits& l) { limits_ = l; }
    [[nodiscard]] const LiveRiskLimits& limits() const noexcept { return limits_; }
    /// True while a trade gap holds decisions back.
    [[nodiscard]] bool paused() const noexcept { return live_ist_minute_index(clock_ns_) < pause_until_minute_; }
    [[nodiscard]] std::uint64_t late_prints() const noexcept { return late_prints_; }
    /// Minute boundaries at which the models ran: a frame that moves this
    /// carried a decision (the CLI times those separately).
    [[nodiscard]] std::uint64_t decision_minutes() const noexcept { return decision_minutes_; }
    /// A JSON object the CLI keeps current (latency percentiles); shown under "latency".
    void set_metrics(std::string json_object) { metrics_ = std::move(json_object); }

    /// Record a decision (models call this when they decide, or decide not to).
    void note_decision(const std::string& model, std::string text) {
        notes_.push_back(LiveDecisionNote{clock_ns_, model, std::move(text)});
    }
    /// Decisions since the last call.
    [[nodiscard]] std::vector<LiveDecisionNote> take_decisions() { return std::exchange(notes_, {}); }
    [[nodiscard]] std::uint64_t trade_gaps() const noexcept { return gaps_; }

    // ---- what models read -------------------------------------------------

    [[nodiscard]] std::int64_t clock_ns() const noexcept { return clock_ns_; }
    [[nodiscard]] std::int64_t today() const noexcept { return day_; }
    [[nodiscard]] bool simulated() const noexcept { return simulated_; }
    [[nodiscard]] const std::vector<LiveInstrument>& universe() const noexcept { return u_; }
    [[nodiscard]] LivePaperBook& book() noexcept { return book_; }
    [[nodiscard]] const LivePaperBook& book() const noexcept { return book_; }

    [[nodiscard]] const LiveInstrument* instrument(std::uint32_t token) const {
        const auto it = index_.find(token);
        return it == index_.end() ? nullptr : &u_[it->second];
    }
    [[nodiscard]] const LiveState* state(std::uint32_t token) const {
        const auto it = state_.find(token);
        return it == state_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] LiveTop top(std::uint32_t token) const {
        const LiveState* s = state(token);
        LiveTop t;
        if (s == nullptr) return t;
        t.ltp = s->ltp; t.bid = s->bid; t.ask = s->ask; t.bid_qty = s->bid_qty; t.ask_qty = s->ask_qty;
        t.quote_ns = s->quote_ns; t.book_ns = s->book_ns; t.levels = s->levels;
        for (std::size_t k = 0; k < kLiveDepth; ++k) { t.bids[k] = s->bids[k]; t.asks[k] = s->asks[k]; }
        return t;
    }
    /// Last price in rupees; 0 when none.
    [[nodiscard]] double ltp(std::uint32_t token) const {
        const LiveState* s = state(token);
        return s == nullptr ? 0.0 : static_cast<double>(s->ltp) / 100.0;
    }
    /// Mid in rupees when both sides are quoted, else the last price.
    [[nodiscard]] double mid(std::uint32_t token) const {
        const LiveState* s = state(token);
        if (s == nullptr) return 0.0;
        if (s->bid > 0 && s->ask > s->bid) return static_cast<double>(s->bid + s->ask) / 200.0;
        return static_cast<double>(s->ltp) / 100.0;
    }
    [[nodiscard]] const LiveInstrument* index_of(const std::string& under) const {
        for (const auto& i : u_) if (i.kind == LiveKind::Index && i.underlying == under) return &i;
        return nullptr;
    }
    /// The nearest future of `under` expiring today or later.
    [[nodiscard]] const LiveInstrument* near_future(const std::string& under) const {
        const LiveInstrument* best = nullptr;
        for (const auto& i : u_) {
            if (i.kind != LiveKind::Future || i.underlying != under || i.expiry_day < day_) continue;
            if (best == nullptr || i.expiry_day < best->expiry_day) best = &i;
        }
        return best;
    }
    /// The nearest future still alive after today: what a carried position rolls into.
    [[nodiscard]] const LiveInstrument* carry_future(const std::string& under) const {
        const LiveInstrument* best = nullptr;
        for (const auto& i : u_) {
            if (i.kind != LiveKind::Future || i.underlying != under || i.expiry_day <= day_) continue;
            if (best == nullptr || i.expiry_day < best->expiry_day) best = &i;
        }
        return best;
    }
    /// Calls or puts of the nearest streamed expiry of `under`, by strike.
    [[nodiscard]] std::vector<const LiveInstrument*> chain(const std::string& under, LiveKind kind) const {
        std::int64_t expiry = 0;
        for (const auto& i : u_)
            if (i.kind == kind && i.underlying == under && i.expiry_day >= day_ && (expiry == 0 || i.expiry_day < expiry))
                expiry = i.expiry_day;
        std::vector<const LiveInstrument*> out;
        for (const auto& i : u_) if (i.kind == kind && i.underlying == under && i.expiry_day == expiry) out.push_back(&i);
        std::sort(out.begin(), out.end(), [](const LiveInstrument* a, const LiveInstrument* b) { return a->strike < b->strike; });
        return out;
    }
    [[nodiscard]] const std::vector<LiveBar>& bars(std::uint32_t token) const { return bars_.bars(token); }

    // ---- outputs -------------------------------------------------------------

    /// Round trips closed since the last call: what the CLI appends to disk.
    [[nodiscard]] std::vector<LivePaperTrade> take_new_trades() {
        std::vector<LivePaperTrade> v(book_.trades().begin() + static_cast<std::ptrdiff_t>(trades_out_), book_.trades().end());
        trades_out_ = book_.trades().size();
        return v;
    }
    [[nodiscard]] std::vector<LivePaperFill> take_new_fills() {
        std::vector<LivePaperFill> v(book_.fills().begin() + static_cast<std::ptrdiff_t>(fills_out_), book_.fills().end());
        fills_out_ = book_.fills().size();
        return v;
    }
    /// True once since the book's open positions last changed.
    [[nodiscard]] bool take_positions_changed() {
        const std::size_t n = book_.fills().size();
        const bool changed = n != positions_seen_;
        positions_seen_ = n;
        return changed;
    }

    /// Everything the desktop's Live Models page shows, as JSON.
    [[nodiscard]] std::string state_json(const std::string& note) const {
        using live_fmt::json_escape;
        using live_fmt::num;
        std::string o = "{\n";
        o += "  \"engine_ns\": " + std::to_string(clock_ns_) + ",\n";
        o += "  \"source\": \"" + std::string(simulated_ ? "SIM" : "LIVE") + "\",\n";
        o += "  \"stale\": " + std::string(stale_ ? "true" : "false") + ",\n";
        o += "  \"paused\": \"" + json_escape(paused() ? "decisions paused: " + pause_note_ : std::string()) + "\",\n";
        o += "  \"halt\": \"" + json_escape(kill_ ? std::string("kill request in force") : halt_) + "\",\n";
        o += "  \"working_orders\": " + std::to_string(book_.working_orders()) + ",\n";
        o += "  \"late_prints\": " + std::to_string(late_prints_) + ",\n";
        o += "  \"trade_gaps\": " + std::to_string(gaps_) + ",\n";
        if (!metrics_.empty()) o += "  \"latency\": " + metrics_ + ",\n";
        o += "  \"note\": \"" + json_escape(note) + "\",\n";
        o += "  \"models\": [";
        for (std::size_t m = 0; m < models_.size(); ++m) {
            const LiveModelView v = models_[m]->view(*this);
            o += m ? ",\n    {" : "\n    {";
            o += "\"name\": \"" + json_escape(v.name) + "\", \"family\": \"" + json_escape(v.family) + "\", \"state\": \""
               + json_escape(v.state) + "\", \"signal\": \"" + json_escape(v.signal) + "\", \"reason\": \""
               + json_escape(v.reason) + "\", \"fields\": [";
            for (std::size_t f = 0; f < v.fields.size(); ++f)
                o += (f ? ", [\"" : "[\"") + json_escape(v.fields[f].first) + "\", \"" + json_escape(v.fields[f].second) + "\"]";
            o += "]}";
        }
        o += "\n  ],\n  \"positions\": [";
        const auto ps = book_.positions();
        for (std::size_t i = 0; i < ps.size(); ++i) {
            const auto& p = ps[i];
            o += i ? ",\n    {" : "\n    {";
            o += "\"model\": \"" + json_escape(p.model) + "\", \"symbol\": \"" + json_escape(p.inst.symbol)
               + "\", \"token\": " + std::to_string(p.inst.token) + ", \"side\": " + std::to_string(p.side)
               + ", \"qty\": " + std::to_string(p.qty) + ", \"lot\": " + std::to_string(p.inst.lot) + ", \"entry\": "
               + num(p.entry) + ", \"entry_ns\": " + std::to_string(p.entry_ns) + ", \"entry_expenses\": "
               + (std::isfinite(p.entry_expenses) ? num(p.entry_expenses) : std::string("null")) + ", \"carry\": "
               + (p.carry ? "true" : "false") + ", \"why_in\": \"" + json_escape(p.why_in) + "\", \"state\": \""
               + live_state_text(p.state) + "\", \"want\": " + std::to_string(p.want_qty) + ", \"exit_since_ns\": "
               + std::to_string(p.exit_since_ns) + ", \"exit_reason\": \"" + json_escape(p.exit_reason) + "\"}";
        }
        o += "\n  ]\n}\n";
        return o;
    }

    // ---- the clock -------------------------------------------------------

    /// Advance the engine clock to `ns`: close bars, run the models for every
    /// minute boundary crossed, roll and square off as obligations.
    void advance(std::int64_t ns) {
        if (ns <= clock_ns_) return;
        clock_ns_ = ns;
        const std::int64_t minute = live_ist_minute_index(ns);
        const std::int64_t day = live_day_of(minute);
        if (day != day_) {
            if (day_ != 0) {
                // A new session: intraday positions should already be flat, and
                // a carried contract that has expired should have been rolled.
                book_.close_if([](const LivePosition& p) { return !p.carry; }, ns, "new session (left open)");
                book_.close_if([day](const LivePosition& p) { return p.carry && p.inst.expiry_day != 0 && p.inst.expiry_day < day; },
                               ns, "expired contract (the roll was missed)");
            }
            day_ = day;
            bars_.new_day();
            bars_.advance(ns, [](std::uint32_t, const LiveBar&) {});
            for (auto& m : models_) m->on_new_day(*this);
            last_minute_ = minute;
            book_.on_clock(ns);
            return;
        }
        if (minute <= last_minute_) { book_.on_clock(ns); return; }
        // A jump of many minutes (a late start, a gap in the feed) runs the
        // models only at the latest boundary: replaying thirty stale decisions
        // would be worse than none. The obligations below are not skipped:
        // they test "at or after", not "exactly at".
        const std::int64_t from = minute - last_minute_ > 30 ? minute : last_minute_ + 1;
        for (std::int64_t m = from; m <= minute; ++m) {
            bars_.advance(m * 60'000'000'000LL - 19800'000'000'000LL, [](std::uint32_t, const LiveBar&) {});
            const int close = live_minute_of_day(m);
            if (close < kLiveOpenMinute) continue;
            obligations(close, ns, false);
            if (close <= kLiveCloseMinute && m >= pause_until_minute_) {
                for (auto& mdl : models_) mdl->on_minute(*this, close);
                ++decision_minutes_;
            }
            if (close >= kLiveSquareOffMinute) square_off(ns, close > kLiveSquareOffMinute && sq_day_ != day_, false);
        }
        last_minute_ = minute;
        book_.on_clock(ns);
    }

    /// The feed has stopped and the machine's clock (`wall_ns`) has moved on:
    /// submit the roll and the square-off that are now overdue, so they fill
    /// the moment quotes return. Live feeds only -- a simulated session's
    /// clock is not the machine's. True when it changed anything (the CLI
    /// records those calls on the tape so a replay makes them too).
    bool watchdog(std::int64_t wall_ns) {
        if (simulated_ || replay_ || day_ == 0 || wall_ns <= clock_ns_ + 60'000'000'000LL) return false;
        const std::int64_t m = live_ist_minute_index(wall_ns);
        if (live_day_of(m) != day_) return false;
        const int close = live_minute_of_day(m);
        if (close < kLiveOpenMinute) return false;
        const auto before = std::make_tuple(book_.version(), roll_day_, sq_day_);
        obligations(close, wall_ns, true);
        if (close >= kLiveSquareOffMinute) square_off(wall_ns, true, true);
        return before != std::make_tuple(book_.version(), roll_day_, sq_day_);
    }

private:
    /// The 15:15 expiry roll, on the first minute at or after it.
    void obligations(int close, std::int64_t ns, bool watchdog) {
        if (close >= kLiveRollMinute && roll_day_ != day_) {
            roll_day_ = day_;
            const std::int64_t today = day_;
            const std::uint64_t v = book_.version();
            const std::string why = watchdog ? "expiry roll (feed stopped; watchdog)" : close > kLiveRollMinute
                                                   ? "expiry roll (overdue: the clock reached " + live_fmt::hhmm(close) + ")"
                                                   : "expiry roll";
            book_.close_if([today](const LivePosition& p) { return p.carry && p.inst.expiry_day != 0 && p.inst.expiry_day <= today; },
                           ns, why);
            note_decision("engine", why + (book_.version() != v ? ": exits submitted" : ": nothing expiring"));
        }
    }
    /// Every intraday position out. Idempotent: an exit already working is left alone.
    void square_off(std::int64_t ns, bool overdue, bool watchdog) {
        const bool first = sq_day_ != day_;
        sq_day_ = day_;
        const std::uint64_t v = book_.version();
        const char* why = watchdog ? "15:20 square-off (feed stopped; watchdog)"
                                   : overdue ? "15:20 square-off (overdue)" : "15:20 square-off";
        book_.close_if([](const LivePosition& p) { return !p.carry; }, ns, why);
        if (first || book_.version() != v)
            note_decision("engine", std::string(why) + (book_.version() != v ? ": exits submitted" : ": nothing intraday held"));
    }

    /// The one pre-trade check every entry passes.
    [[nodiscard]] std::optional<std::string> risk_check(const LiveRiskRequest& r) const {
        if (kill_) return std::string("kill request in force");
        if (!halt_.empty()) return "halted: " + halt_;
        if (stale_) return std::string("feed stale");
        if (paused()) return "decisions paused: " + pause_note_;
        if (!r.carry && live_minute_of_day(live_ist_minute_index(r.ns)) >= kLiveSquareOffMinute)
            return std::string("intraday entry after the 15:20 square-off");
        const auto ps = book_.positions();
        if (ps.size() + 1 > limits_.max_positions)
            return "position limit (" + std::to_string(limits_.max_positions) + ")";
        std::size_t mine = 0;
        double gross = r.price * static_cast<double>(r.qty);
        for (const auto& p : ps) {
            mine += p.model == r.model ? 1u : 0u;
            const LiveTop t = top(p.inst.token);
            const double px = p.filled() ? p.entry : static_cast<double>(p.side > 0 ? t.ask : t.bid) / 100.0;
            gross += std::fabs(px) * static_cast<double>(p.filled() ? p.qty : p.want_qty);
        }
        if (mine + 1 > limits_.max_per_model)
            return "per-model position limit (" + std::to_string(limits_.max_per_model) + ")";
        if (gross > limits_.max_gross_notional)
            return "gross notional " + live_fmt::num(gross, 0) + " over " + live_fmt::num(limits_.max_gross_notional, 0);
        double day_pnl = 0.0;
        for (const auto& t : book_.trades())
            if (live_day_of(live_ist_minute_index(t.exit_ns)) == day_) day_pnl += std::isfinite(t.net) ? t.net : t.gross;
        for (const auto& p : ps) {
            const double u = book_.unrealised(p, clock_ns_);
            if (std::isfinite(u)) day_pnl += u;
        }
        if (-day_pnl > limits_.max_daily_loss)
            return "daily loss " + live_fmt::num(-day_pnl, 0) + " over " + live_fmt::num(limits_.max_daily_loss, 0);
        return std::nullopt;
    }

    std::vector<LiveInstrument> u_;
    std::unordered_map<std::uint32_t, std::size_t> index_;
    std::unordered_map<std::uint32_t, LiveState> state_;
    LiveBarBuilder bars_;
    LivePaperBook book_;
    std::vector<std::unique_ptr<LiveModel>> models_;
    std::int64_t clock_ns_ = 0, day_ = 0, last_minute_ = 0;
    std::int64_t roll_day_ = 0, sq_day_ = 0, pause_until_minute_ = 0;
    std::size_t trades_out_ = 0, fills_out_ = 0, positions_seen_ = 0;
    std::uint64_t late_prints_ = 0, gaps_ = 0, missed_trades_ = 0, decision_minutes_ = 0;
    std::string pause_note_, halt_, metrics_;
    std::vector<LiveDecisionNote> notes_;
    LiveRiskLimits limits_;
    bool stale_ = false, simulated_ = false, replay_ = false, kill_ = false;
};

/// Records a model's decision when the deciding scope ends, however it ends
/// (decisions return early on every reason not to act): `text()` is read then.
template <class Text>
class LiveDecisionScope {
public:
    LiveDecisionScope(LiveEngine& e, std::string model, Text text) : e_(e), model_(std::move(model)), text_(std::move(text)) {}
    ~LiveDecisionScope() { e_.note_decision(model_, text_()); }
    LiveDecisionScope(const LiveDecisionScope&) = delete;
    LiveDecisionScope& operator=(const LiveDecisionScope&) = delete;

private:
    LiveEngine& e_;
    std::string model_;
    Text text_;
};

} // namespace altair::live
