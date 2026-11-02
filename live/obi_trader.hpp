// live/obi_trader.hpp -- the order-book imbalance trader: paper only, tick by
// tick, every decision with its reason.
//
// TWO PIECES.
//   * ObiHub ("OBI engine"): ONE book/flow.hpp FlowEngine per instrument, fed
//     every book (5 levels from the FYERS data socket, 50 while the
//     instrument holds a FYERS 50-level slot) and every trade print, once per
//     tick for every trader. It watches each underlying's NEAR FUTURE (the
//     signal: futures lead options), the nearest expiry's calls and puts
//     within `option_strikes` of the money (ATM +/- 5 = 11 strikes, what the
//     option mode sells), and anything in data/live/obi_watch.csv. It never
//     trades. It is registered before the traders, and the engine calls the
//     models in order on its one feed thread, so a trader always reads the
//     book the hub has just applied.
//   * LiveObiTraderModel: one TRADE TYPE on one underlying in one mode, with
//     its own paper record, limits and reasons.
//
// THE FOUR TRADE TYPES (Smit's note of 2026-10-29), each read off the
// underlying's near future, each on its own:
//   A  bid-side pressure -> LONG: OBI over 5 levels >= +obi_entry, held for
//      >= min_persist book updates and >= min_persist_ns, and more buying than
//      selling printed in the window;
//   B  ask-side pressure -> SHORT: the mirror;
//   C  absorption -> the REVERSAL: heavy selling into bids that refill while
//      the bid holds -> long (and the mirror -> short);
//   D  liquidity withdrawal -> the BREAKOUT: asks pulled while real buys lift
//      successive asks -> long (and the mirror -> short).
// The one check every type passes: the target covers the round trip's
// expenses (the engine's own cost function), one spread and the safety
// margin. Without it every trade loses by construction. Nothing else: no
// breakout, OFI or range filter (Smit, 2026-11-02: "take trades wherever").
//
// HOW A SIGNAL IS TRADED. NEVER BUYS AN OPTION.
//   * Futures mode: long or short one lot of the near future, at the touch.
//   * Option mode: long -> SELL a put, short -> SELL a call, `strikes_otm`
//     from the money, nearest expiry; not while that option's own book leans
//     against the sale (its OBI5 >= +obi_entry) or its spread is wider than
//     `max_spread_ticks`. A short option is bought back only to close it.
//
// EXITS (whichever comes first): the target; the stop in points (futures) or
// premium percent (options); a loss of `max_loss_trade` rupees after
// expenses; for A and B, the pressure reversed (OBI5 persistently against
// the position); `max_hold_ns`; and the engine's 15:20 square-off. After a
// losing trade, `cooldown_ns` without entries; past `max_daily_loss` (this
// model, realised) or `max_trades_day`, no entries until tomorrow.
//
// WHICH UNDERLYINGS. BANKNIFTY (target in index points) and every NSE F&O
// stock of config/obi_underlyings.csv (live/obi_underlyings.hpp; NYKAA) with
// its own target, stop and safety in rupees a share. A stock's options are
// its nearest (monthly) chain. Lot and tick come from the instrument master.
//
// THE NUMBERS ARE A STARTING FRAMEWORK, NOT VALIDATED PARAMETERS. Nothing here
// has been tested out of sample; the paper record, one per type, is the test.
// Paper only: the real-order bridge (app/live_real_orders.hpp) sends orders
// only for the strategies with a switch in the LIVE arm, and these have none.
//
// THREADING: everything runs on the live engine's single feed thread; the
// models hold no locks. The traders read the hub through a const pointer.

#pragma once

#include <book/flow.hpp>
#include <live/engine.hpp>
#include <live/obi_signals.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace altair::live {

enum class ObiMode : std::uint8_t { Futures, SellOptions };
/// The shared books: one FlowEngine per instrument, applied once per tick.
class ObiHub final : public LiveModel {
public:
    struct Config {
        std::vector<std::string> unders{"BANKNIFTY"};
        flow::FlowConfig flow{};       ///< per instrument (tick from the instrument)
        int option_strikes = 5;        ///< calls / puts tracked each side of the money: 11 strikes
        std::string watch_file;        ///< data/live/obi_watch.csv; empty = none
    };
    explicit ObiHub(Config c) : c_(std::move(c)) {}

    [[nodiscard]] std::string name() const override { return "OBI engine"; }
    [[nodiscard]] std::string family() const override { return "Order flow"; }
    [[nodiscard]] const Config& config() const noexcept { return c_; }

    void on_new_day(LiveEngine& e) override {
        day_ = e.today();
        engines_.clear();
        ignored_.clear();
        watched_.clear();
        steps_.clear();
        for (const auto& u : c_.unders) {
            if (const LiveInstrument* f = e.near_future(u)) track(*f);
            steps_[u] = step(e, u);
            for (const LiveKind k : {LiveKind::Call, LiveKind::Put})
                for (const LiveInstrument* i : e.chain(u, k))
                    if (wanted(e, *i)) track(*i);
        }
        reload_watch(e);
    }

    void on_book_tick(LiveEngine& e, std::uint32_t token, std::int64_t ns) override {
        if (day_ != e.today()) on_new_day(e);
        auto it = engines_.find(token);
        if (it == engines_.end()) {
            // Not tracked: ignored until the next minute re-checks, unless it is
            // an option that came within range (the underlying moved).
            if (ignored_.count(token) != 0) return;
            const LiveInstrument* in = e.instrument(token);
            if (in == nullptr || !wanted(e, *in)) { ignored_.insert(token); return; }
            track(*in);
            it = engines_.find(token);
        }
        const LiveState* s = e.state(token);
        if (s == nullptr) return;
        if (s->levels > 0) it->second->on_book(s->bids, s->levels, s->asks, s->levels, ns);
        else {   // a touch-only quote
            const LiveLevel b{s->bid, s->bid_qty}, a{s->ask, s->ask_qty};
            it->second->on_book(&b, 1, &a, 1, ns);
        }
    }

    void on_print(LiveEngine& e, const PricePayload& p, std::int64_t ns) override {
        if (day_ != e.today()) on_new_day(e);
        const auto it = engines_.find(p.token);
        if (it == engines_.end()) return;
        it->second->on_trade(p.last_paise, p.has(kPriceHasVolume) ? p.volume : -1, p.last_qty, ns);
    }

    void on_minute(LiveEngine& e, int close_minute) override {
        ignored_.clear();   // re-checked against where the underlying is now
        if (close_minute % 5 == 0) reload_watch(e);
    }

    [[nodiscard]] LiveModelView view(const LiveEngine& e) const override {
        LiveModelView v;
        v.name = name();
        v.family = family();
        v.state = "computing";
        v.reason = std::to_string(engines_.size()) + " books tracked (near futures, ATM +/- " + std::to_string(c_.option_strikes)
                   + " strikes, the watch list); it never trades";
        for (const auto& u : c_.unders)
            if (const LiveInstrument* f = e.near_future(u))
                if (const auto* fe = flow_of(f->token)) v.fields.emplace_back(f->symbol, describe(fe->signals()));
        for (const auto tok : watched_)
            if (const LiveInstrument* in = e.instrument(tok))
                if (const auto* fe = flow_of(tok)) v.fields.emplace_back(in->symbol, describe(fe->signals()));
        return v;
    }

    /// The engine for `token`, if it is tracked (traders, tests, the desktop).
    [[nodiscard]] const flow::FlowEngine* flow_of(std::uint32_t token) const {
        const auto it = engines_.find(token);
        return it == engines_.end() ? nullptr : it->second.get();
    }
    /// The strike step of `under`'s chain (0 without one).
    [[nodiscard]] double step_of(const std::string& under) const {
        const auto it = steps_.find(under);
        return it == steps_.end() ? 0.0 : it->second;
    }
    [[nodiscard]] std::size_t tracked() const noexcept { return engines_.size(); }

    /// One line of every reading, for the views.
    [[nodiscard]] static std::string describe(const flow::FlowSignals& s) {
        if (!s.valid) return "no tradable book yet";
        char b[320];
        std::snprintf(b, sizeof b,
                      "%u lv | OBI1 %+.2f OBI5 %+.2f OBI%u %+.2f (w %+.2f) | micro %.2f (%+.2f t) | OFI %+.2f | "
                      "buy %lld sell %lld | cxl bid %lld ask %lld | hid %lld/%lld | run %d%s%s",
                      static_cast<unsigned>(std::min(s.bid_levels, s.ask_levels)), s.obi1, s.obi_short,
                      static_cast<unsigned>(s.deep_used), s.obi_deep, s.obi_deep_w, s.microprice.value() / 100.0,
                      s.micro_offset_ticks, s.ofi_norm, static_cast<long long>(s.buy_exec),
                      static_cast<long long>(s.sell_exec), static_cast<long long>(s.bid_cancel),
                      static_cast<long long>(s.ask_cancel), static_cast<long long>(s.hidden_buy),
                      static_cast<long long>(s.hidden_sell), s.persist,
                      s.absorption ? (s.absorption > 0 ? " | ABSORB+" : " | ABSORB-") : "",
                      s.withdrawal ? (s.withdrawal > 0 ? " | WITHDRAW+" : " | WITHDRAW-") : "");
        return b;
    }

private:
    [[nodiscard]] static double step(const LiveEngine& e, const std::string& under) {
        const auto calls = e.chain(under, LiveKind::Call);
        double best = 0.0;
        for (std::size_t i = 1; i < calls.size(); ++i) {
            const double d = calls[i]->strike - calls[i - 1]->strike;
            if (d > 0.0 && (best == 0.0 || d < best)) best = d;
        }
        if (best > 0.0) return best;
        return under == "BANKNIFTY" ? 100.0 : under == "NIFTY" ? 50.0 : 0.0;   // a stock without a chain sells nothing
    }

    [[nodiscard]] bool wanted(const LiveEngine& e, const LiveInstrument& in) const {
        if (watched_.count(in.token) != 0) return true;
        if (in.kind != LiveKind::Call && in.kind != LiveKind::Put) return false;
        if (std::find(c_.unders.begin(), c_.unders.end(), in.underlying) == c_.unders.end()) return false;
        const auto ch = e.chain(in.underlying, in.kind);
        if (ch.empty() || ch.front()->expiry_day != in.expiry_day) return false;
        const double atm = e.underlying_level(in.underlying);
        return atm > 0.0 && std::fabs(in.strike - atm) <= step_of(in.underlying) * (c_.option_strikes + 0.5);
    }

    void track(const LiveInstrument& in) {
        if (engines_.count(in.token) != 0) return;
        flow::FlowConfig c = c_.flow;
        c.tick = std::max<std::int64_t>(1, std::llround(in.tick * 100.0));
        engines_.emplace(in.token, std::make_unique<flow::FlowEngine>(c));   // once per instrument a day
    }

    void reload_watch(LiveEngine& e) {
        if (c_.watch_file.empty()) return;
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(c_.watch_file, ec);
        if (ec || t == watch_time_) return;
        watch_time_ = t;
        watched_.clear();
        for (const auto& sym : read_obi_watch(c_.watch_file))
            for (const auto& in : e.universe())
                if (in.fyers == sym) { watched_.insert(in.token); track(in); }
    }

    Config c_;
    std::unordered_map<std::uint32_t, std::unique_ptr<flow::FlowEngine>> engines_;
    std::unordered_set<std::uint32_t> watched_, ignored_;
    std::unordered_map<std::string, double> steps_;
    std::filesystem::file_time_type watch_time_{};
    std::int64_t day_ = -1;
};

struct ObiTraderRules {
    std::string under = "BANKNIFTY";
    ObiType type = ObiType::A;
    ObiMode mode = ObiMode::Futures;
    double obi_entry = 0.30;
    int min_persist = 3;                   ///< book updates (A, B)
    std::int64_t min_persist_ns = 500'000'000;
    double target_pts = 25.0;              ///< futures: rupees a unit (index points)
    double stop_pts = 15.0;
    double safety_pts = 2.0;               ///< margin on top of costs and slippage
    double option_target_pct = 0.15;       ///< option mode: premium fall that takes the profit
    double option_stop_pct = 0.25;         ///< premium rise that stops it
    int strikes_otm = 1;                   ///< what is sold: 0 = at the money
    double max_spread_ticks = 4.0;
    double max_loss_trade = 2'000.0;       ///< rupees, after expenses
    double max_daily_loss = 5'000.0;       ///< rupees, this model
    int max_trades_day = 40;
    std::int64_t cooldown_ns = 30'000'000'000;
    std::int64_t max_hold_ns = 300'000'000'000;
    int first_entry_minute = 9 * 60 + 16;  ///< no entries in the opening minute
    int last_entry_minute = 15 * 60;       ///< none after 15:00
    std::int64_t lots = 1;
};

class LiveObiTraderModel final : public LiveModel {
public:
    LiveObiTraderModel(ObiTraderRules r, const ObiHub* hub) : r_(std::move(r)), hub_(hub) {}

    [[nodiscard]] std::string name() const override {
        return std::string("OBI ") + obi_type_letter(r_.type) + " " + (r_.mode == ObiMode::Futures ? "futures " : "option-sell ") + r_.under;
    }
    [[nodiscard]] std::string family() const override { return "Order flow"; }
    [[nodiscard]] const ObiTraderRules& rules() const noexcept { return r_; }
    [[nodiscard]] const ObiHub* hub() const noexcept { return hub_; }

    void on_new_day(LiveEngine& e) override {
        day_ = e.today();
        trades_today_ = 0;
        halted_ = false;
        cooldown_until_ = 0;
        last_blocked_ns_ = 0;
        held_ = 0;
        fut_ = e.near_future(r_.under);
        note_ = fut_ == nullptr ? "no near future of " + r_.under + " is streamed" : "watching " + fut_->symbol;
    }

    void on_book_tick(LiveEngine& e, std::uint32_t token, std::int64_t ns) override {
        if (day_ != e.today()) on_new_day(e);
        decide(e, token, ns);
    }
    void on_print(LiveEngine& e, const PricePayload& p, std::int64_t ns) override {
        if (day_ != e.today()) on_new_day(e);
        decide(e, p.token, ns);
    }
    void on_minute(LiveEngine& e, int) override {
        if (held_ != 0) manage(e, e.clock_ns());   // exits are checked on every tick; this catches a quiet tape
    }

    [[nodiscard]] LiveModelView view(const LiveEngine& e) const override {
        using live_fmt::num;
        LiveModelView v;
        v.name = name();
        v.family = family();
        v.state = halted_ ? "done today" : (held_ != 0 ? "in position" : "watching");
        if (sig_seen_)   // formatted here, for the screen, not on every tick
            v.signal = sig_.valid ? "OBI5 " + num(sig_.obi_short) + ", OFI " + num(sig_.ofi_norm) + ", S " + num(sig_.score) : "no book";
        v.reason = note_;
        v.fields.emplace_back("Trade type", std::string(obi_type_letter(r_.type)) + " -- " + obi_type_name(r_.type));
        if (fut_ != nullptr && hub_ != nullptr)
            if (const auto* f = hub_->flow_of(fut_->token)) v.fields.emplace_back(fut_->symbol, ObiHub::describe(f->signals()));
        if (held_ != 0 && hub_ != nullptr && (fut_ == nullptr || held_ != fut_->token))
            if (const auto* f = hub_->flow_of(held_))
                if (const LiveInstrument* in = e.instrument(held_)) v.fields.emplace_back(in->symbol, ObiHub::describe(f->signals()));
        v.fields.emplace_back("Trades today", std::to_string(trades_today_) + " of " + std::to_string(r_.max_trades_day));
        v.fields.emplace_back("Limits", "Rs " + num(r_.max_loss_trade, 0) + " a trade, Rs " + num(r_.max_daily_loss, 0) + " a day (paper)");
        return v;
    }

private:
    /// +1 long, -1 short, 0 none; `text` says why, with the numbers.
    [[nodiscard]] int setup(const LiveEngine& e, const flow::FlowSignals& s, std::string& text, std::string& blocked) const {
        if (fut_ == nullptr) return 0;
        const ObiSignalRules rules{r_.obi_entry, r_.min_persist, r_.min_persist_ns};
        // The gate: the bare decision, no text. Most ticks stop here; the
        // reason is formatted only when the trade type fires.
        if (obi_decide(r_.type, s, rules) == 0) return 0;
        std::string why;
        const int dir = obi_signal(r_.type, s, rules, why);
        if (dir == 0) return 0;
        // The one check: the target after costs.
        const std::int64_t lot = std::max<std::int64_t>(1, fut_->lot);
        const double px = s.mid / 100.0;
        const double qty = static_cast<double>(lot * r_.lots);
        const double cost = e.book().expense(*fut_, dir > 0, qty, px, s.ns) + e.book().expense(*fut_, dir < 0, qty, px, s.ns);
        const double cost_pts = std::isfinite(cost) ? cost / qty : std::numeric_limits<double>::infinity();
        const double slip_pts = static_cast<double>(s.ask - s.bid) / 100.0;
        const double edge = r_.target_pts - cost_pts - slip_pts - r_.safety_pts;
        if (!(edge > 0.0)) {
            blocked = why + "; but target " + live_fmt::num(r_.target_pts) + " pts does not cover costs " + live_fmt::num(cost_pts) + " + spread "
                      + live_fmt::num(slip_pts) + " + safety";
            return 0;
        }
        char b[200];
        std::snprintf(b, sizeof b, "; target %.2f pts vs costs %.2f + spread %.2f + safety %.2f = edge %.2f pts; microprice %+.2f ticks, S %+.2f",
                      r_.target_pts, cost_pts, slip_pts, r_.safety_pts, edge, s.micro_offset_ticks, s.score);
        text = std::string(obi_type_letter(r_.type)) + " " + (dir > 0 ? "LONG" : "SHORT") + ": " + why + b;
        return dir;
    }

    void decide(LiveEngine& e, std::uint32_t token, std::int64_t ns) {
        if (hub_ == nullptr) return;
        if (held_ != 0) {
            if (token == held_ || (fut_ != nullptr && token == fut_->token)) manage(e, ns);
            return;
        }
        if (fut_ == nullptr || token != fut_->token) return;   // entries are signalled by the future
        const flow::FlowEngine* fe = hub_->flow_of(token);
        if (fe == nullptr) return;
        const flow::FlowSignals& s = fe->signals();
        std::string text, blocked;
        const int dir = setup(e, s, text, blocked);
        sig_ = {s.valid, s.obi_short, s.ofi_norm, s.score};   // three numbers, not a string: view() formats them
        sig_seen_ = true;
        if (dir == 0) {
            if (!blocked.empty()) log_blocked(e, ns, blocked);
            return;
        }
        std::string why;
        if (!can_enter(e, ns, why)) { log_blocked(e, ns, "signal " + std::string(dir > 0 ? "LONG" : "SHORT") + " but " + why); return; }
        if (r_.mode == ObiMode::Futures) {
            enter(e, *fut_, dir, ns, text + "; one lot of " + fut_->symbol);
            return;
        }
        // Option mode: a long view sells a put, a short view sells a call.
        const LiveKind k = dir > 0 ? LiveKind::Put : LiveKind::Call;
        const LiveInstrument* opt = pick_option(e, k);
        if (opt == nullptr) { log_blocked(e, ns, "signal but no " + std::string(dir > 0 ? "put" : "call") + " to sell is streamed"); return; }
        const flow::FlowEngine* oe = hub_->flow_of(opt->token);
        const flow::FlowSignals* os = oe == nullptr ? nullptr : &oe->signals();
        if (os == nullptr || !os->valid) { log_blocked(e, ns, "signal but " + opt->symbol + " has no tradable book"); return; }
        if (os->obi_short >= r_.obi_entry) { log_blocked(e, ns, "signal but " + opt->symbol + "'s own book is bid (OBI5 " + live_fmt::num(os->obi_short) + "): selling into buyers"); return; }
        if (os->spread_ticks > r_.max_spread_ticks) { log_blocked(e, ns, "signal but " + opt->symbol + " spread is " + live_fmt::num(os->spread_ticks, 0) + " ticks"); return; }
        enter(e, *opt, -1, ns, text + "; SELL " + opt->symbol + " (its OBI5 " + live_fmt::num(os->obi_short) + ", spread " + live_fmt::num(os->spread_ticks, 0) + " ticks)");
    }

    [[nodiscard]] bool can_enter(const LiveEngine& e, std::int64_t ns, std::string& why) const {
        if (halted_) { why = "done for the day"; return false; }
        if (e.paused()) { why = "engine paused (a trade gap)"; return false; }
        if (e.stale()) { why = "feed stale"; return false; }
        const int mod = live_minute_of_day(live_ist_minute_index(ns));
        if (mod < r_.first_entry_minute || mod >= r_.last_entry_minute) { why = "outside entry hours"; return false; }
        if (ns < cooldown_until_) { why = "cooling down after a loss"; return false; }
        if (trades_today_ >= r_.max_trades_day) { why = "trade limit for the day"; return false; }
        return true;
    }

    [[nodiscard]] const LiveInstrument* pick_option(const LiveEngine& e, LiveKind k) const {
        const double atm = e.underlying_level(r_.under);
        const double st = hub_->step_of(r_.under);
        if (atm <= 0.0 || !(st > 0.0)) return nullptr;
        // Out of the money: puts below the money, calls above.
        const double want = k == LiveKind::Put ? atm - st * r_.strikes_otm : atm + st * r_.strikes_otm;
        const LiveInstrument* best = nullptr;
        for (const LiveInstrument* i : e.chain(r_.under, k))
            if (best == nullptr || std::fabs(i->strike - want) < std::fabs(best->strike - want)) best = i;
        return best;
    }

    void enter(LiveEngine& e, const LiveInstrument& in, int side, std::int64_t ns, const std::string& reason) {
        // RULE: never open a long option.
        if (side > 0 && (in.kind == LiveKind::Call || in.kind == LiveKind::Put)) {
            e.note_decision(name(), "refused: an entry would BUY " + in.symbol + " (options are sold only)");
            return;
        }
        std::string why;
        if (!e.book().open(name(), in, side, r_.lots, ns, reason, false, &why)) {
            log_blocked(e, ns, "entry refused: " + why);
            return;
        }
        held_ = in.token;
        entry_ns_ = ns;
        ++trades_today_;
        note_ = std::string(side > 0 ? "BUY " : "SELL ") + in.symbol + ": " + reason;
        e.note_decision(name(), note_);
    }

    void manage(LiveEngine& e, std::int64_t ns) {
        const LivePosition* p = e.book().position(name(), held_);
        if (p == nullptr) {   // closed (target, stop, square-off) or the entry never filled
            finish(e, ns);
            return;
        }
        if (p->state == LivePosState::Closing || !p->filled()) return;   // an exit or the entry still working
        const double mark = e.mid(held_);
        if (!(mark > 0.0)) return;
        const bool option = p->inst.kind == LiveKind::Call || p->inst.kind == LiveKind::Put;
        const double move = (mark - p->entry) * p->side;   // rupees a unit in the position's favour
        const double u = e.book().unrealised(*p, ns);
        std::string exit;
        if (option) {
            const double chg = p->entry > 0.0 ? (mark - p->entry) / p->entry : 0.0;   // short: a fall is the profit
            if (chg <= -r_.option_target_pct) exit = "target: premium down " + live_fmt::pct(-chg, 1);
            else if (chg >= r_.option_stop_pct) exit = "stop: premium up " + live_fmt::pct(chg, 1);
        } else {
            if (move >= r_.target_pts) exit = "target: +" + live_fmt::num(move) + " pts";
            else if (move <= -r_.stop_pts) exit = "stop: " + live_fmt::num(move) + " pts";
        }
        if (exit.empty() && std::isfinite(u) && -u >= r_.max_loss_trade) exit = "max loss a trade: Rs " + live_fmt::num(-u, 0);
        if (exit.empty() && ns - entry_ns_ >= r_.max_hold_ns) exit = "time stop after " + live_fmt::num(static_cast<double>(r_.max_hold_ns) / 60e9, 0) + " min";
        // A and B trade the pressure: it reversing is the exit. C and D trade a
        // turn or a break, where the book is expected to look the other way.
        if (exit.empty() && fut_ != nullptr && (r_.type == ObiType::A || r_.type == ObiType::B))
            if (const flow::FlowEngine* fe = hub_->flow_of(fut_->token)) {
                const auto& s = fe->signals();
                const int view = option ? (p->inst.kind == LiveKind::Put ? 1 : -1) : p->side;
                if (s.valid && s.obi_short * view <= -r_.obi_entry && s.persist * view <= -r_.min_persist)
                    exit = "pressure reversed: OBI5 " + live_fmt::num(s.obi_short) + " against the position for " + std::to_string(-s.persist * view) + " updates";
            }
        if (exit.empty()) return;
        char b[160];
        std::snprintf(b, sizeof b, " (mark %.2f, entry %.2f, open P&L Rs %.0f)", mark, p->entry, std::isfinite(u) ? u : 0.0);
        exit += b;
        const std::string sym = p->inst.symbol;   // `p` points into the book, which close() changes
        (void)e.book().close(name(), held_, ns, exit);
        e.note_decision(name(), "EXIT " + sym + ": " + exit);
        note_ = "exiting " + sym + ": " + exit;
        if (e.book().position(name(), held_) == nullptr) finish(e, ns);   // it filled at once
    }

    /// The position is gone: book its result into the day's limits.
    void finish(LiveEngine& e, std::int64_t ns) {
        double last = 0.0;
        bool found = false;
        double day = 0.0;
        for (const auto& t : e.book().trades()) {
            if (t.model != name() || live_day_of(live_ist_minute_index(t.exit_ns)) != e.today()) continue;
            const double net = std::isfinite(t.net) ? t.net : t.gross;
            day += net;
            if (t.token == held_) { last = net; found = true; }
        }
        if (found && last < 0.0) cooldown_until_ = ns + r_.cooldown_ns;
        if (-day >= r_.max_daily_loss) {
            halted_ = true;
            e.note_decision(name(), "daily loss Rs " + live_fmt::num(-day, 0) + " reached the Rs " + live_fmt::num(r_.max_daily_loss, 0) + " limit: no entries until tomorrow");
        }
        note_ = found ? "last trade Rs " + live_fmt::num(last, 0) + ", today Rs " + live_fmt::num(day, 0) : "entry did not fill";
        held_ = 0;
    }

    /// A signal that could not be acted on, recorded at most every 30 s.
    void log_blocked(LiveEngine& e, std::int64_t ns, const std::string& why) {
        note_ = why;
        if (ns - last_blocked_ns_ < 30'000'000'000 && why == last_blocked_) return;
        last_blocked_ns_ = ns;
        last_blocked_ = why;
        e.note_decision(name(), "no entry: " + why);
    }

    ObiTraderRules r_;
    const ObiHub* hub_ = nullptr;
    const LiveInstrument* fut_ = nullptr;
    std::int64_t day_ = -1;
    std::uint32_t held_ = 0;
    std::int64_t entry_ns_ = 0, cooldown_until_ = 0, last_blocked_ns_ = 0;
    int trades_today_ = 0;
    bool halted_ = false;
    std::string note_, last_blocked_;
    /// The future's last reading, for view().
    struct SignalSeen { bool valid = false; double obi_short = 0.0, ofi_norm = 0.0, score = 0.0; } sig_;
    bool sig_seen_ = false;
};

} // namespace altair::live
