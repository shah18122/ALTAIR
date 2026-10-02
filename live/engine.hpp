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
// A model may not open a position while the feed is stale.
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
    bool simulated = false, replay = false;
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
    LiveEngine(std::vector<LiveInstrument> universe, LiveCostFn cost)
        : u_(std::move(universe)),
          book_([this](std::uint32_t tok) { return top(tok); }, std::move(cost)) {
        for (std::size_t i = 0; i < u_.size(); ++i) index_[u_[i].token] = i;
    }

    void add_model(std::unique_ptr<LiveModel> m) { models_.push_back(std::move(m)); }
    [[nodiscard]] const std::vector<std::unique_ptr<LiveModel>>& models() const noexcept { return models_; }

    // ---- the feed --------------------------------------------------------

    void on_trade(const PricePayload& p, std::int64_t ns) {
        LiveState& s = state_[p.token];
        s.ltp = p.last_paise;
        s.last_ns = ns;
        if (p.has(kPriceHasVolume)) s.volume = p.volume;
        if (p.has(kPriceHasOi)) s.oi = p.oi;
        s.simulated = p.has(kPriceSimulated);
        s.replay = p.has(kPriceReplay);
        simulated_ = simulated_ || s.simulated;
        bars_.feed(p.token, static_cast<double>(p.last_paise) / 100.0, ns, s.volume, [](std::uint32_t, const LiveBar&) {});
        advance(ns);
    }
    void on_quote(const QuotePayload& q) {
        LiveState& s = state_[q.token];
        if (q.has(kQuoteHasTop)) { s.bid = q.bid; s.ask = q.ask; }
        if (q.has(kQuoteHasPrevClose)) s.prev_close = q.prev_close;
        if (q.has(kQuoteHasOhlc)) s.open = q.open;
        s.simulated = s.simulated || q.has(kQuoteSimulated);
    }
    /// The feed went quiet (or came back). No new positions while stale.
    void set_stale(bool stale) noexcept { stale_ = stale; }
    [[nodiscard]] bool stale() const noexcept { return stale_; }

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
        return s == nullptr ? LiveTop{} : LiveTop{s->ltp, s->bid, s->ask};
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
               + (p.carry ? "true" : "false") + ", \"why_in\": \"" + json_escape(p.why_in) + "\"}";
        }
        o += "\n  ]\n}\n";
        return o;
    }

    // ---- the clock -------------------------------------------------------

    /// Advance the engine clock to `ns`: close bars, run the models for every
    /// minute boundary crossed, roll and square off on schedule.
    void advance(std::int64_t ns) {
        if (ns <= clock_ns_) return;
        clock_ns_ = ns;
        const std::int64_t minute = live_ist_minute_index(ns);
        const std::int64_t day = live_day_of(minute);
        if (day != day_) {
            if (day_ != 0) {
                // A new session: intraday positions should already be flat.
                book_.close_if([](const LivePosition& p) { return !p.carry; }, ns, "new session (left open)");
            }
            day_ = day;
            bars_.new_day();
            for (auto& m : models_) m->on_new_day(*this);
            last_minute_ = minute;
            return;
        }
        if (minute <= last_minute_) return;
        // A jump of many minutes (a late start, a gap in the feed) runs only the
        // latest boundary: replaying thirty stale decisions would be worse than none.
        const std::int64_t from = minute - last_minute_ > 30 ? minute : last_minute_ + 1;
        for (std::int64_t m = from; m <= minute; ++m) {
            bars_.advance(m * 60'000'000'000LL - 19800'000'000'000LL, [](std::uint32_t, const LiveBar&) {});
            const int close = live_minute_of_day(m);
            if (close < kLiveOpenMinute || close > kLiveCloseMinute) continue;
            if (close == kLiveRollMinute) {
                book_.close_if([this](const LivePosition& p) { return p.carry && p.inst.expiry_day != 0 && p.inst.expiry_day <= day_; },
                               ns, "expiry roll");
            }
            for (auto& mdl : models_) mdl->on_minute(*this, close);
            if (close == kLiveSquareOffMinute) {
                book_.close_if([](const LivePosition& p) { return !p.carry; }, ns, "15:20 square-off");
            }
        }
        last_minute_ = minute;
    }

private:
    std::vector<LiveInstrument> u_;
    std::unordered_map<std::uint32_t, std::size_t> index_;
    std::unordered_map<std::uint32_t, LiveState> state_;
    LiveBarBuilder bars_;
    LivePaperBook book_;
    std::vector<std::unique_ptr<LiveModel>> models_;
    std::int64_t clock_ns_ = 0, day_ = 0, last_minute_ = 0;
    std::size_t trades_out_ = 0, fills_out_ = 0, positions_seen_ = 0;
    bool stale_ = false, simulated_ = false;
};

} // namespace altair::live
