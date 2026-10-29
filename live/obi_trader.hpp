// live/obi_trader.hpp -- the beginner's order-book-imbalance trader: paper
// only, tick by tick, every decision with its reason.
//
// WHAT IT WATCHES. One book/flow.hpp FlowEngine per instrument, fed every book
// (5 levels from the FYERS data socket, 50 while the instrument holds a FYERS
// 50-level slot) and every trade print, on the bus's clock:
//   * the underlying's NEAR FUTURE -- the signal: futures lead options;
//   * the nearest expiry's calls and puts within `option_strikes` of the money
//     (what the option mode sells), and
//   * anything listed in data/live/obi_watch.csv (one FYERS symbol a line --
//     any strike of the streamed chain), shown on the Live Models page; the
//     price service puts those on the 50-level slots too.
//
// THE RULES (Smit's note of 2026-10-29, strategies A and B). LONG when ALL of
//   1. OBI over 5 levels >= +obi_entry, held for >= min_persist book updates
//      and >= min_persist_ns;
//   2. OFI over the window >= +ofi_min (normalised, -1..+1);
//   3. aggressive buying: buy prints >= one lot and > sell prints, and ask
//      size actually executed (not merely cancelled);
//   4. the mid breaks the short-term high (the breakout window before now);
//   5. the target, after the round trip's expenses (brokerage, STT, exchange,
//      SEBI, GST, stamp: the engine's own cost function), one spread of
//      slippage and a safety margin, still leaves an edge > 0, and the market
//      has moved at least half the target over the breakout window.
// SHORT is the mirror. Strategy C (absorption) and D (liquidity withdrawal)
// can stand in for rule 4 when switched on (off by default).
//
// HOW A SIGNAL IS TRADED (Smit: "both"). NEVER BUYS AN OPTION.
//   * Futures mode: long or short one lot of the near future.
//   * Option mode: long -> SELL a put, short -> SELL a call, `strikes_otm`
//     from the money, nearest expiry; only while that option's own book is
//     not leaning against the sale (its 5-level OBI below +obi_entry) and its
//     spread is at most `max_spread_ticks`. A short option is bought back only
//     to close it.
//
// EXITS (whichever comes first): the target; the stop in points (futures) or
// premium percent (options); a loss of `max_loss_trade` rupees after
// expenses; the setup invalidated (OBI over 5 levels persistently against
// the position); `max_hold_ns`; and the engine's 15:20 square-off. After a
// losing trade, `cooldown_ns` without entries; past `max_daily_loss` (this
// model, realised plus open) or `max_trades_day`, no entries until tomorrow.
//
// THE NUMBERS ARE A STARTING FRAMEWORK, NOT VALIDATED PARAMETERS. Nothing here
// has been tested out of sample; the paper record is the test. Paper only:
// the real-order bridge (app/live_real_orders.hpp) sends orders only for the
// strategies with a switch in the LIVE arm, and this one has none.
//
// THREADING: everything runs on the live engine's single feed thread; the
// model holds no locks and shares nothing.

#pragma once

#include <book/flow.hpp>
#include <live/engine.hpp>

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

struct ObiTraderRules {
    std::string under = "NIFTY";
    ObiMode mode = ObiMode::Futures;
    flow::FlowConfig flow{};               ///< per instrument (tick from the instrument)
    double obi_entry = 0.30;
    int min_persist = 3;                   ///< book updates
    std::int64_t min_persist_ns = 500'000'000;
    double ofi_min = 0.10;
    double target_pts = 10.0;              ///< futures: rupees a unit (index points)
    double stop_pts = 6.0;
    double safety_pts = 1.0;               ///< margin on top of costs and slippage
    double option_target_pct = 0.15;       ///< option mode: premium fall that takes the profit
    double option_stop_pct = 0.25;         ///< premium rise that stops it
    int option_strikes = 3;                ///< calls / puts watched each side of the money
    int strikes_otm = 1;                   ///< what is sold: 0 = at the money
    double max_spread_ticks = 4.0;
    double max_loss_trade = 2'000.0;       ///< rupees, after expenses
    double max_daily_loss = 5'000.0;       ///< rupees, this model
    int max_trades_day = 20;
    std::int64_t cooldown_ns = 120'000'000'000;
    std::int64_t max_hold_ns = 300'000'000'000;
    int first_entry_minute = 9 * 60 + 16;  ///< no entries in the opening minute
    int last_entry_minute = 15 * 60;       ///< none after 15:00
    bool use_absorption = false;           ///< strategy C may stand in for the breakout
    bool use_withdrawal = false;           ///< strategy D likewise
    std::int64_t lots = 1;
    std::string watch_file;                ///< data/live/obi_watch.csv; empty = none
};

class LiveObiTraderModel final : public LiveModel {
public:
    explicit LiveObiTraderModel(ObiTraderRules r) : r_(std::move(r)) {}

    [[nodiscard]] std::string name() const override {
        return "OBI " + std::string(r_.mode == ObiMode::Futures ? "futures " : "option-sell ") + r_.under;
    }
    [[nodiscard]] std::string family() const override { return "Order flow"; }
    [[nodiscard]] const ObiTraderRules& rules() const noexcept { return r_; }

    void on_new_day(LiveEngine& e) override {
        day_ = e.today();
        engines_.clear();
        ignored_.clear();
        watched_.clear();
        trades_today_ = 0;
        halted_ = false;
        cooldown_until_ = 0;
        last_blocked_ns_ = 0;
        held_ = 0;
        fut_ = e.near_future(r_.under);
        if (fut_ != nullptr) track(*fut_);
        step_ = step(e);
        if (r_.mode == ObiMode::SellOptions)   // the rest join as the index comes near them (wanted)
            for (const LiveKind k : {LiveKind::Call, LiveKind::Put})
                for (const LiveInstrument* i : e.chain(r_.under, k))
                    if (wanted(e, *i)) track(*i);
        reload_watch(e);
        note_ = fut_ == nullptr ? "no near future of " + r_.under + " is streamed" : "watching " + fut_->symbol;
    }

    void on_book_tick(LiveEngine& e, std::uint32_t token, std::int64_t ns) override {
        if (day_ != e.today()) on_new_day(e);
        auto it = engines_.find(token);
        if (it == engines_.end()) {
            // Not tracked: ignored until the next minute re-checks, unless it is
            // an option that came within range (the index moved).
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
        decide(e, token, ns);
    }

    void on_print(LiveEngine& e, const PricePayload& p, std::int64_t ns) override {
        if (day_ != e.today()) on_new_day(e);
        const auto it = engines_.find(p.token);
        if (it == engines_.end()) return;
        it->second->on_trade(p.last_paise, p.has(kPriceHasVolume) ? p.volume : -1, p.last_qty, ns);
        decide(e, p.token, ns);
    }

    void on_minute(LiveEngine& e, int close_minute) override {
        ignored_.clear();   // re-checked against where the index is now
        if (close_minute % 5 == 0) reload_watch(e);
        // Exits are checked on every tick; this catches a quiet tape.
        if (held_ != 0) manage(e, e.clock_ns());
    }

    [[nodiscard]] LiveModelView view(const LiveEngine& e) const override {
        using live_fmt::num;
        LiveModelView v;
        v.name = name();
        v.family = family();
        v.state = halted_ ? "done today" : (held_ != 0 ? "in position" : "watching");
        v.signal = signal_;
        v.reason = note_;
        const auto add = [&](const LiveInstrument& in, const flow::FlowSignals& s) {
            if (!s.valid) { v.fields.emplace_back(in.symbol, "no tradable book yet"); return; }
            char b[320];
            std::snprintf(b, sizeof b,
                          "%u lv | OBI1 %+.2f OBI5 %+.2f OBI%u %+.2f (w %+.2f) | micro %.2f (%+.2f t) | OFI %+.2f | "
                          "buy %lld sell %lld | cxl bid %lld ask %lld | hid %lld/%lld | S %+.2f | run %d%s%s",
                          static_cast<unsigned>(std::min(s.bid_levels, s.ask_levels)), s.obi1, s.obi_short,
                          static_cast<unsigned>(s.deep_used), s.obi_deep, s.obi_deep_w, s.microprice.value() / 100.0,
                          s.micro_offset_ticks, s.ofi_norm, static_cast<long long>(s.buy_exec),
                          static_cast<long long>(s.sell_exec), static_cast<long long>(s.bid_cancel),
                          static_cast<long long>(s.ask_cancel), static_cast<long long>(s.hidden_buy),
                          static_cast<long long>(s.hidden_sell), s.score, s.persist,
                          s.absorption ? (s.absorption > 0 ? " | ABSORB+" : " | ABSORB-") : "",
                          s.withdrawal ? (s.withdrawal > 0 ? " | WITHDRAW+" : " | WITHDRAW-") : "");
            v.fields.emplace_back(in.symbol, b);
        };
        if (fut_ != nullptr)
            if (const auto it = engines_.find(fut_->token); it != engines_.end()) add(*fut_, it->second->signals());
        for (const auto& [tok, eng] : engines_) {
            if (fut_ != nullptr && tok == fut_->token) continue;
            if (watched_.count(tok) == 0 && tok != held_) continue;   // the option candidates are not listed
            if (const LiveInstrument* in = e.instrument(tok)) add(*in, eng->signals());
        }
        v.fields.emplace_back("Trades today", std::to_string(trades_today_) + " of " + std::to_string(r_.max_trades_day));
        v.fields.emplace_back("Limits", "Rs " + num(r_.max_loss_trade, 0) + " a trade, Rs " + num(r_.max_daily_loss, 0) + " a day (paper)");
        return v;
    }

    /// The engine for `token`, if it is tracked (tests, the desktop).
    [[nodiscard]] const flow::FlowEngine* flow_of(std::uint32_t token) const {
        const auto it = engines_.find(token);
        return it == engines_.end() ? nullptr : it->second.get();
    }

private:
    [[nodiscard]] double step(const LiveEngine& e) const {
        const auto calls = e.chain(r_.under, LiveKind::Call);
        double best = 0.0;
        for (std::size_t i = 1; i < calls.size(); ++i) {
            const double d = calls[i]->strike - calls[i - 1]->strike;
            if (d > 0.0 && (best == 0.0 || d < best)) best = d;
        }
        return best > 0.0 ? best : (r_.under == "BANKNIFTY" ? 100.0 : 50.0);
    }

    [[nodiscard]] bool wanted(const LiveEngine& e, const LiveInstrument& in) const {
        if (watched_.count(in.token) != 0) return true;
        if (r_.mode != ObiMode::SellOptions || in.underlying != r_.under) return false;
        if (in.kind != LiveKind::Call && in.kind != LiveKind::Put) return false;
        const auto ch = e.chain(r_.under, in.kind);
        if (ch.empty() || ch.front()->expiry_day != in.expiry_day) return false;
        const double atm = e.underlying_level(r_.under);
        return atm > 0.0 && std::fabs(in.strike - atm) <= step_ * (r_.option_strikes + 0.5);
    }

    void track(const LiveInstrument& in) {
        if (engines_.count(in.token) != 0) return;
        flow::FlowConfig c = r_.flow;
        c.tick = std::max<std::int64_t>(1, std::llround(in.tick * 100.0));
        engines_.emplace(in.token, std::make_unique<flow::FlowEngine>(c));   // once per instrument a day
    }

    void reload_watch(LiveEngine& e) {
        if (r_.watch_file.empty()) return;
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(r_.watch_file, ec);
        if (ec || t == watch_time_) return;
        watch_time_ = t;
        watched_.clear();
        for (const auto& sym : read_obi_watch(r_.watch_file))
            for (const auto& in : e.universe())
                if (in.fyers == sym) { watched_.insert(in.token); track(in); }
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

    /// +1 long, -1 short, 0 none; `text` says why, with the numbers.
    [[nodiscard]] int setup(const LiveEngine& e, const flow::FlowSignals& s, std::string& text, std::string& blocked) const {
        if (!s.valid || fut_ == nullptr) return 0;
        const std::int64_t lot = std::max<std::int64_t>(1, fut_->lot);
        for (const int dir : {1, -1}) {
            const double obi = s.obi_short * dir, ofi = s.ofi_norm * dir;
            const std::int64_t mine = dir > 0 ? s.buy_exec : s.sell_exec, theirs = dir > 0 ? s.sell_exec : s.buy_exec;
            const std::int64_t taken = dir > 0 ? s.ask_exec_drop : s.bid_exec_drop;
            if (obi < r_.obi_entry || s.persist * dir < r_.min_persist || s.persist_ns < r_.min_persist_ns) continue;
            if (ofi < r_.ofi_min) { blocked = "OBI held but OFI " + live_fmt::num(s.ofi_norm) + " does not confirm"; continue; }
            if (mine < lot || mine <= theirs || taken <= 0) {
                blocked = "OBI and OFI agree but no aggressive " + std::string(dir > 0 ? "buying" : "selling") + " has executed";
                continue;
            }
            const bool breakout = dir > 0 ? (s.prior_high > 0.0 && s.mid > s.prior_high) : (s.prior_low > 0.0 && s.mid < s.prior_low);
            const bool c_ok = r_.use_absorption && s.absorption == dir;
            const bool d_ok = r_.use_withdrawal && s.withdrawal == dir;
            if (!breakout && !c_ok && !d_ok) { blocked = "flow agrees but the mid has not broken the short-term " + std::string(dir > 0 ? "high" : "low"); continue; }
            // Rule 5: the target after costs.
            const double px = s.mid / 100.0;
            const double qty = static_cast<double>(lot * r_.lots);
            const double cost = e.book().expense(*fut_, dir > 0, qty, px, s.ns) + e.book().expense(*fut_, dir < 0, qty, px, s.ns);
            const double cost_pts = std::isfinite(cost) ? cost / qty : std::numeric_limits<double>::infinity();
            const double slip_pts = static_cast<double>(s.ask - s.bid) / 100.0;
            const double edge = r_.target_pts - cost_pts - slip_pts - r_.safety_pts;
            const double range_pts = s.prior_high > 0.0 && s.prior_low > 0.0 ? (s.prior_high - s.prior_low) / 100.0 : 0.0;
            if (!(edge > 0.0)) { blocked = "setup there but target " + live_fmt::num(r_.target_pts) + " pts does not cover costs " + live_fmt::num(cost_pts) + " + spread " + live_fmt::num(slip_pts) + " + safety"; continue; }
            if (range_pts < 0.5 * r_.target_pts) { blocked = "setup there but the market moved only " + live_fmt::num(range_pts) + " pts in the breakout window"; continue; }
            char b[512];
            std::snprintf(b, sizeof b,
                          "%s: OBI5 %+.2f held %d updates / %.1fs (OBI%u %+.2f, weighted %+.2f), OFI %+.2f, "
                          "%s prints %lld vs %lld with %lld executed off the %s, mid %.2f %s %.2f%s; "
                          "target %.2f pts vs costs %.2f + spread %.2f + safety %.2f = edge %.2f pts; "
                          "microprice %+.2f ticks, S %+.2f",
                          dir > 0 ? "LONG" : "SHORT", s.obi_short, s.persist * dir, static_cast<double>(s.persist_ns) / 1e9,
                          static_cast<unsigned>(s.deep_used), s.obi_deep, s.obi_deep_w, s.ofi_norm, dir > 0 ? "buy" : "sell",
                          static_cast<long long>(mine), static_cast<long long>(theirs), static_cast<long long>(taken),
                          dir > 0 ? "ask" : "bid", px, dir > 0 ? "above the 30s high" : "below the 30s low",
                          (dir > 0 ? s.prior_high : s.prior_low) / 100.0,
                          breakout ? "" : (c_ok ? " (absorption instead)" : " (withdrawal instead)"), r_.target_pts, cost_pts,
                          slip_pts, r_.safety_pts, edge, s.micro_offset_ticks, s.score);
            text = b;
            return dir;
        }
        return 0;
    }

    void decide(LiveEngine& e, std::uint32_t token, std::int64_t ns) {
        if (held_ != 0) {
            if (token == held_ || (fut_ != nullptr && token == fut_->token)) manage(e, ns);
            return;
        }
        if (fut_ == nullptr || token != fut_->token) return;   // entries are signalled by the future
        const auto it = engines_.find(token);
        if (it == engines_.end()) return;
        const flow::FlowSignals& s = it->second->signals();
        std::string text, blocked;
        const int dir = setup(e, s, text, blocked);
        signal_ = s.valid ? "OBI5 " + live_fmt::num(s.obi_short) + ", OFI " + live_fmt::num(s.ofi_norm) + ", S " + live_fmt::num(s.score)
                          : "no book";
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
        const auto oe = engines_.find(opt->token);
        const flow::FlowSignals* os = oe == engines_.end() ? nullptr : &oe->second->signals();
        if (os == nullptr || !os->valid) { log_blocked(e, ns, "signal but " + opt->symbol + " has no tradable book"); return; }
        if (os->obi_short >= r_.obi_entry) { log_blocked(e, ns, "signal but " + opt->symbol + "'s own book is bid (OBI5 " + live_fmt::num(os->obi_short) + "): selling into buyers"); return; }
        if (os->spread_ticks > r_.max_spread_ticks) { log_blocked(e, ns, "signal but " + opt->symbol + " spread is " + live_fmt::num(os->spread_ticks, 0) + " ticks"); return; }
        enter(e, *opt, -1, ns, text + "; SELL " + opt->symbol + " (its OBI5 " + live_fmt::num(os->obi_short) + ", spread " + live_fmt::num(os->spread_ticks, 0) + " ticks)");
    }

    [[nodiscard]] const LiveInstrument* pick_option(const LiveEngine& e, LiveKind k) const {
        const double atm = e.underlying_level(r_.under);
        if (atm <= 0.0) return nullptr;
        const double st = step_;
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
        if (exit.empty() && fut_ != nullptr) {   // the setup invalidated: the future's book persistently against us
            const auto it = engines_.find(fut_->token);
            if (it != engines_.end()) {
                const auto& s = it->second->signals();
                const int view = option ? (p->inst.kind == LiveKind::Put ? 1 : -1) : p->side;
                if (s.valid && s.obi_short * view <= -r_.obi_entry && s.persist * view <= -r_.min_persist)
                    exit = "setup invalidated: OBI5 " + live_fmt::num(s.obi_short) + " against the position for " + std::to_string(-s.persist * view) + " updates";
            }
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
    std::unordered_map<std::uint32_t, std::unique_ptr<flow::FlowEngine>> engines_;
    std::unordered_set<std::uint32_t> watched_, ignored_;
    double step_ = 0.0;
    std::filesystem::file_time_type watch_time_{};
    const LiveInstrument* fut_ = nullptr;
    std::int64_t day_ = -1;
    std::uint32_t held_ = 0;
    std::int64_t entry_ns_ = 0, cooldown_until_ = 0, last_blocked_ns_ = 0;
    int trades_today_ = 0;
    bool halted_ = false;
    std::string note_, signal_, last_blocked_;
};

} // namespace altair::live
