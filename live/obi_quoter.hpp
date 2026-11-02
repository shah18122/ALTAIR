// live/obi_quoter.hpp -- sell-side option quoting skewed by the order book:
// paper research, never buys an option to open, every decision with its reason.
//
// THE IDEA is hftbacktest's order-book-imbalance market maker, cut down to
// what the house rules allow (options are SOLD only; Smit, 2026-11-02: "option
// sell only, futures have more costing involved"). A market maker quotes both
// sides and leans its quotes away from the side the book says is about to be
// run over. Here the "two sides" are the two option legs that can be SOLD:
//   * a resting SELL of the out-of-the-money PUT (it loses if the market falls);
//   * a resting SELL of the out-of-the-money CALL (it loses if the market rises).
// Each rests at its own ask (LivePaperBook::open_passive, filled through the
// queue model of book/queue_fill.hpp), so what it earns when it works is the
// spread and the premium's decay, not a view.
//
// THE SKEW is the near future's OBI over 5 levels (book/flow.hpp via the OBI
// hub; futures lead options):
//   * OBI5 >= +pull_obi (bids heavy, buyers coming): the CALL quote is pulled
//     (cancelled if resting, not placed if not) -- a call sold now is the one
//     that gets lifted just before it rises. The put stays.
//   * OBI5 <= -pull_obi: the mirror; the PUT is pulled.
//   * In between: both legs are quoted.
// A leg is also pulled while its own book is bid (its OBI5 >= pull_obi: the
// buyers are on that option) or its spread is wider than max_spread_ticks.
// Pulling is the whole defence against adverse selection a retail feed can
// afford; the 1 s markout in altair_tune's stats sheet is what says whether
// it is enough.
//
// THE ONE ENTRY CHECK: the target (the premium falling by target_pct) pays the
// round trip's expenses and the safety margin; otherwise the leg is not quoted.
//
// EXITS are brackets on the paper book (LiveBracket): the premium down
// target_pct (the profit, bought back at the ask), up stop_pct (the stop), or
// max_hold_ns (the time stop); the engine's 15:20 square-off ends the day.
// Buying back to close is the only buy. A pulled quote is cancelled with its
// reason in the log. An unfilled quote times out with the
// book's entry timeout and is re-placed at the new ask on the next tick that
// still wants it (that is the re-quote). After a losing trade, cooldown_ns
// without quoting that leg; past max_daily_loss (this model, realised) or
// max_trades_day, no quotes until tomorrow.
//
// NOT VALIDATED. NSE pays no maker rebate (hftbacktest's OBI maker needed one
// to make money on Binance futures), so the edge has to come from the spread
// and decay alone, net of STT on the sale and the other charges. The paper
// record and the tapes are the test. Paper only: no LIVE switch exists.
//
// THREADING: the live engine's single feed thread; the hub is read through a
// const pointer and is registered before this model.

#pragma once

#include <live/obi_trader.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace altair::live {

struct ObiQuoterRules {
    std::string under = "BANKNIFTY";
    double pull_obi = 0.30;                ///< future's OBI5 at which the leg it threatens is pulled
    int strikes_otm = 2;                   ///< what is quoted: strikes out of the money
    double max_spread_ticks = 6.0;
    double target_pct = 0.10;              ///< premium fall that takes the profit
    double stop_pct = 0.25;                ///< premium rise that stops it
    double safety_pct = 0.02;              ///< of the premium, on top of the expenses
    double min_premium = 5.0;              ///< rupees: below it the spread is all there is
    double max_daily_loss = 5'000.0;       ///< rupees, this model
    int max_trades_day = 40;               ///< round trips (filled sales) a day
    std::int64_t cooldown_ns = 60'000'000'000;
    std::int64_t max_hold_ns = 900'000'000'000;
    int first_entry_minute = 9 * 60 + 20;  ///< the opening's spreads are not the day's
    int last_entry_minute = 15 * 60;
    std::int64_t lots = 1;
};

class LiveObiQuoterModel final : public LiveModel {
public:
    LiveObiQuoterModel(ObiQuoterRules r, const ObiHub* hub) : r_(std::move(r)), hub_(hub) {}

    [[nodiscard]] std::string name() const override { return "OBI quoter " + r_.under; }
    [[nodiscard]] std::string family() const override { return "Order flow"; }
    [[nodiscard]] const ObiQuoterRules& rules() const noexcept { return r_; }

    void on_new_day(LiveEngine& e) override {
        day_ = e.today();
        trades_today_ = 0;
        halted_ = false;
        // A quote still in the book (placed before the engine had dated the
        // session from its first print) stays this model's to manage.
        for (auto& l : legs_) {
            const std::uint32_t keep = l.token != 0 && e.book().position(name(), l.token) != nullptr ? l.token : 0;
            l = Leg{};
            l.token = keep;
        }
        legs_[0].kind = LiveKind::Put;
        legs_[1].kind = LiveKind::Call;
        fut_ = e.near_future(r_.under);
        note_ = fut_ == nullptr ? "no near future of " + r_.under + " is streamed" : "skew read off " + fut_->symbol;
    }

    void on_book_tick(LiveEngine& e, std::uint32_t token, std::int64_t ns) override { tick(e, token, ns); }
    void on_print(LiveEngine& e, const PricePayload& p, std::int64_t ns) override { tick(e, p.token, ns); }
    void on_minute(LiveEngine& e, int) override {
        if (day_ != e.today()) on_new_day(e);
        for (auto& l : legs_) settle(e, l, e.clock_ns());   // a quiet tape still books what closed
    }

    [[nodiscard]] LiveModelView view(const LiveEngine& e) const override {
        LiveModelView v;
        v.name = name();
        v.family = family();
        v.state = halted_ ? "done today" : (legs_[0].token != 0 || legs_[1].token != 0 ? "quoting" : "watching");
        v.signal = signal_;
        v.reason = note_;
        for (const auto& l : legs_) {
            const LiveInstrument* in = l.token != 0 ? e.instrument(l.token) : nullptr;
            const LivePosition* p = in != nullptr ? e.book().position(name(), l.token) : nullptr;
            std::string s = in == nullptr ? "not quoted" : in->symbol;
            if (p != nullptr) s += p->filled() ? " SOLD at " + live_fmt::num(p->entry) : " resting at the ask";
            v.fields.emplace_back(l.kind == LiveKind::Put ? "Put leg" : "Call leg", s + (l.note.empty() ? "" : " -- " + l.note));
        }
        v.fields.emplace_back("Trades today", std::to_string(trades_today_) + " of " + std::to_string(r_.max_trades_day));
        v.fields.emplace_back("Limits", "Rs " + live_fmt::num(r_.max_daily_loss, 0) + " a day (paper)");
        return v;
    }

private:
    struct Leg {
        LiveKind kind = LiveKind::Put;
        std::uint32_t token = 0;          ///< the option quoted or held (0: none)
        std::int64_t cooldown_until = 0;
        std::int64_t last_note_ns = 0;
        std::string note;
    };

    void tick(LiveEngine& e, std::uint32_t token, std::int64_t ns) {
        if (day_ != e.today()) on_new_day(e);
        if (hub_ == nullptr || fut_ == nullptr) return;
        const bool ours = token == fut_->token || token == legs_[0].token || token == legs_[1].token;
        if (!ours) return;
        const flow::FlowEngine* fe = hub_->flow_of(fut_->token);
        if (fe == nullptr) return;
        const flow::FlowSignals& s = fe->signals();
        signal_ = s.valid ? "future OBI5 " + live_fmt::num(s.obi_short) : "no future book";
        for (auto& l : legs_) {
            settle(e, l, ns);
            work(e, l, s, ns);
        }
    }

    /// The leg's position is gone (bought back, square-off, or the quote timed
    /// out): book its result into the day's limits and free the leg.
    void settle(LiveEngine& e, Leg& l, std::int64_t ns) {
        if (l.token == 0 || e.book().position(name(), l.token) != nullptr) return;
        double last = 0.0, day = 0.0;
        bool found = false;
        for (const auto& t : e.book().trades()) {
            if (t.model != name() || live_day_of(live_ist_minute_index(t.exit_ns)) != e.today()) continue;
            const double net = std::isfinite(t.net) ? t.net : t.gross;
            day += net;
            if (t.token == l.token && t.exit_ns >= placed_ns(l)) { last = net; found = true; }
        }
        if (found) ++trades_today_;
        if (found && last < 0.0) l.cooldown_until = ns + r_.cooldown_ns;
        if (-day >= r_.max_daily_loss && !halted_) {
            halted_ = true;
            e.note_decision(name(), "daily loss Rs " + live_fmt::num(-day, 0) + " reached the Rs " + live_fmt::num(r_.max_daily_loss, 0)
                                        + " limit: no quotes until tomorrow");
        }
        if (found) l.note = "last trade Rs " + live_fmt::num(last, 0) + ", today Rs " + live_fmt::num(day, 0);
        else if (l.note.rfind("pulled", 0) != 0) l.note = "quote expired unfilled (re-quoted at the new ask when still wanted)";
        l.token = 0;
    }

    [[nodiscard]] std::int64_t placed_ns(const Leg& l) const {
        return l.kind == LiveKind::Put ? placed_[0] : placed_[1];
    }

    void work(LiveEngine& e, Leg& l, const flow::FlowSignals& s, std::int64_t ns) {
        const bool put = l.kind == LiveKind::Put;
        const char* leg = put ? "put" : "call";
        // Holding a sale: the brackets on the book manage it.
        if (l.token != 0) {
            const LivePosition* p = e.book().position(name(), l.token);
            if (p == nullptr || p->filled() || p->state != LivePosState::Opening) return;
            // Resting: pull it if the skew, or its own book, turned against it.
            std::string why;
            if (!wanted(l, s, why)) {
                const LiveInstrument* in = e.instrument(l.token);
                const std::string sym = in != nullptr ? in->symbol : std::to_string(l.token);
                (void)e.book().close(name(), l.token, ns, "pulled: " + why);
                e.note_decision(name(), "PULL " + sym + " (" + leg + " quote): " + why);
                l.note = "pulled: " + why;
                if (e.book().position(name(), l.token) == nullptr) l.token = 0;
            }
            return;
        }
        std::string why;
        if (!can_quote(e, l, ns, why) || !s.valid) {
            if (s.valid) log(e, l, ns, why);
            return;
        }
        if (!wanted(l, s, why)) { log(e, l, ns, why); return; }
        const LiveInstrument* opt = pick(e, l.kind);
        if (opt == nullptr) { log(e, l, ns, std::string("no ") + leg + " to quote is streamed"); return; }
        const flow::FlowEngine* oe = hub_->flow_of(opt->token);
        if (oe == nullptr || !oe->signals().valid) { log(e, l, ns, opt->symbol + " has no tradable book"); return; }
        const flow::FlowSignals& os = oe->signals();
        if (!leg_ok(os, why)) { log(e, l, ns, opt->symbol + ": " + why); return; }
        // The one check: the target pays the round trip and the safety margin.
        const double ask = static_cast<double>(os.ask) / 100.0;
        if (ask < r_.min_premium) { log(e, l, ns, opt->symbol + " ask " + live_fmt::num(ask) + " is under the Rs " + live_fmt::num(r_.min_premium, 0) + " floor"); return; }
        const double qty = static_cast<double>(std::max<std::int64_t>(1, opt->lot) * r_.lots);
        const double exit_px = ask * (1.0 - r_.target_pct);
        const double cost = e.book().expense(*opt, false, qty, ask, ns) + e.book().expense(*opt, true, qty, exit_px, ns);
        const double cost_unit = std::isfinite(cost) ? cost / qty : std::numeric_limits<double>::infinity();
        const double edge = ask * r_.target_pct - cost_unit - ask * r_.safety_pct;
        if (!(edge > 0.0)) {
            log(e, l, ns, opt->symbol + ": target " + live_fmt::num(ask * r_.target_pct) + " a unit does not cover costs "
                              + live_fmt::num(cost_unit) + " + safety " + live_fmt::num(ask * r_.safety_pct));
            return;
        }
        char b[260];
        std::snprintf(b, sizeof b,
                      "QUOTE SELL %s at the ask %.2f (bid %.2f, %.0f ticks; its OBI5 %+.2f): future OBI5 %+.2f (this %s is pulled at %s%.2f); target %.2f a unit vs costs %.2f + safety %.2f = edge %.2f",
                      opt->symbol.c_str(), ask, static_cast<double>(os.bid) / 100.0, os.spread_ticks, os.obi_short, s.obi_short,
                      leg, put ? "-" : "+", r_.pull_obi, ask * r_.target_pct, cost_unit, ask * r_.safety_pct, edge);
        std::string refused;
        if (!e.book().open_passive(name(), *opt, -1, r_.lots, ns, b, &refused)) {
            log(e, l, ns, "quote refused: " + refused);
            return;
        }
        LiveBracket br;
        br.target_pct = r_.target_pct;
        br.stop_pct = r_.stop_pct;
        br.expire_ns = ns + r_.max_hold_ns;
        (void)e.book().set_bracket(name(), opt->token, br);
        l.token = opt->token;
        (put ? placed_[0] : placed_[1]) = ns;
        l.note = b;
        e.note_decision(name(), b);
    }

    /// Whether the skew lets this leg be quoted (or stay quoted).
    [[nodiscard]] bool wanted(const Leg& l, const flow::FlowSignals& s, std::string& why) const {
        if (!s.valid) { why = "the future has no tradable book"; return false; }
        const bool put = l.kind == LiveKind::Put;
        char b[160];
        if (put && s.obi_short <= -r_.pull_obi) {
            std::snprintf(b, sizeof b, "future OBI5 %+.2f: sellers lean on the book, a put sold now is the one run over", s.obi_short);
            why = b;
            return false;
        }
        if (!put && s.obi_short >= r_.pull_obi) {
            std::snprintf(b, sizeof b, "future OBI5 %+.2f: buyers lean on the book, a call sold now is the one lifted", s.obi_short);
            why = b;
            return false;
        }
        if (l.token != 0)
            if (const flow::FlowEngine* oe = hub_->flow_of(l.token); oe != nullptr && oe->signals().valid)
                if (!leg_ok(oe->signals(), why)) return false;
        return true;
    }

    [[nodiscard]] bool leg_ok(const flow::FlowSignals& os, std::string& why) const {
        if (os.obi_short >= r_.pull_obi) { why = "its own book is bid (OBI5 " + live_fmt::num(os.obi_short) + "): buyers are on it"; return false; }
        if (os.spread_ticks > r_.max_spread_ticks) { why = "spread " + live_fmt::num(os.spread_ticks, 0) + " ticks"; return false; }
        return true;
    }

    [[nodiscard]] bool can_quote(const LiveEngine& e, const Leg& l, std::int64_t ns, std::string& why) const {
        if (halted_) { why = "done for the day"; return false; }
        if (e.today() == 0) { why = "waiting for the session's first trade print"; return false; }
        if (e.paused()) { why = "engine paused (a trade gap)"; return false; }
        if (e.stale()) { why = "feed stale"; return false; }
        const int mod = live_minute_of_day(live_ist_minute_index(ns));
        if (mod < r_.first_entry_minute || mod >= r_.last_entry_minute) { why = "outside quoting hours"; return false; }
        if (ns < l.cooldown_until) { why = "cooling down after a loss on this leg"; return false; }
        if (trades_today_ >= r_.max_trades_day) { why = "quote limit for the day"; return false; }
        return true;
    }

    [[nodiscard]] const LiveInstrument* pick(const LiveEngine& e, LiveKind k) const {
        const double atm = e.underlying_level(r_.under);
        const double st = hub_->step_of(r_.under);
        if (atm <= 0.0 || !(st > 0.0)) return nullptr;
        const double want = k == LiveKind::Put ? atm - st * r_.strikes_otm : atm + st * r_.strikes_otm;
        const LiveInstrument* best = nullptr;
        for (const LiveInstrument* i : e.chain(r_.under, k))
            if (best == nullptr || std::fabs(i->strike - want) < std::fabs(best->strike - want)) best = i;
        return best;
    }

    /// A leg that could not be quoted: on its view always, in the decision log
    /// at most once a minute a leg (the reasons carry live numbers).
    void log(LiveEngine& e, Leg& l, std::int64_t ns, const std::string& why) {
        l.note = why;
        if (l.last_note_ns != 0 && ns - l.last_note_ns < 60'000'000'000) return;
        l.last_note_ns = ns;
        e.note_decision(name(), std::string("no ") + (l.kind == LiveKind::Put ? "put" : "call") + " quote: " + why);
    }

    ObiQuoterRules r_;
    const ObiHub* hub_ = nullptr;
    const LiveInstrument* fut_ = nullptr;
    std::int64_t day_ = -1;
    std::array<Leg, 2> legs_{};
    std::array<std::int64_t, 2> placed_{};
    int trades_today_ = 0;
    bool halted_ = false;
    std::string note_, signal_;
};

} // namespace altair::live
