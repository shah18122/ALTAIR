// live/diffusion.hpp -- information diffusion, live: who moves first between
// BANKNIFTY's future, its index, its bank stocks and its options (and NYKAA's
// stock, future and options), and a paper trader for the follower that has
// not caught up yet. Paper only, tick by tick, every decision with its reason.
//
// TWO PIECES, like the OBI engine (live/obi_trader.hpp).
//   * DiffusionHub ("Diffusion engine"): one book/diffusion.hpp PairStats per
//     leader/follower pair, fed every quote, book and print of either side.
//     It never trades. Pairs per underlying:
//       Spot             near future -> index (BANKNIFTY), or stock -> near
//                        future (a stock underlying such as NYKAA); the
//                        price discovery shares are read on these;
//       StockToFuture    each bank stock of `stock_leaders` -> BANKNIFTY's
//                        near future (and a stock underlying's own stock);
//       FutureToOption   near future -> each call and put of the nearest
//                        expiry within `option_strikes` of the money (ATM
//                        +/- 2 = 5 strikes each side), added as the money moves.
//     It writes the day's report, one row a pair, to
//     <live dir>/diffusion/diffusion_<day>.csv every 15 minutes and at 15:30.
//     A tape replay (altair_live_engine --replay) writes the same report
//     under the replay's directory: that replay IS the study of recorded
//     sessions, with the very code the live engine runs.
//   * LiveDiffusionTraderModel: one underlying, one mode.
//       option-sell: the future moved and an option's BID is still above the
//         fair price the move implies (the put after a rise, the call after a
//         fall): SELL it at the bid. Never buys an option except to close.
//       futures: the bank stocks (or the stock) moved and the future is still
//         behind: buy below fair, or sell above it, one lot.
//     Entered only when the bid (or ask) beats fair by costs + safety, and
//     only on a pair that passes the GATE.
//
// THE GATE, the plan's kill rule read live (book/diffusion.hpp measures it):
// the leader must lead (best lag > 0, lead-lag ratio >= min_llr), the lag must
// exceed `min_lag_ns` (the FYERS jitter floor: below it the "lead" is the
// network, not the market), and the best correlation must be >= min_rho.
// A pair that fails is shown, never traded.
//
// EXITS (whichever first): the follower closes 80% of the gap to the fair
// price it was entered against (the gap closed); the stop (premium % for an option, points
// for a future); Rs max_loss_trade after expenses; a time stop of 4 gap
// half-lives (at least min_hold_ns, at most max_hold_ns); the engine's 15:20
// square-off. After a loss, cooldown_ns without entries; past max_daily_loss
// (realised) or max_trades_day, none until tomorrow.
//
// REASON LOG: every entry and exit is a decision in the engine's log and a row
// of <live dir>/diffusion/trades.csv (leader, lag, correlation, LLR, beta,
// half-life, gap, costs, edge, the leader's OBI5 when the OBI engine tracks it).
//
// THE NUMBERS ARE A STARTING FRAMEWORK, NOT VALIDATED PARAMETERS. Nothing is
// fitted out of sample; the gate and the paper record are the test. Paper
// only: no switch in the real-order bridge names these models.
//
// THREADING: everything runs on the live engine's one feed thread; no locks.
// Traders read the hub through a const pointer; the hub is added first.

#pragma once

#include <book/diffusion.hpp>
#include <live/engine.hpp>
#include <live/obi_trader.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace altair::live {

enum class DiffRole : std::uint8_t { Spot, StockToFuture, FutureToOption };

[[nodiscard]] inline const char* diff_role_text(DiffRole r) noexcept {
    switch (r) {
    case DiffRole::Spot: return "spot";
    case DiffRole::StockToFuture: return "stock->future";
    case DiffRole::FutureToOption: return "future->option";
    }
    return "?";
}

struct DiffusionGate {
    std::int64_t min_lag_ns = 100 * diffusion::kMsNs;   ///< the FYERS jitter floor
    double min_llr = 1.5;
    double min_rho = 0.10;
};

struct DiffPair {
    std::string under;
    DiffRole role = DiffRole::Spot;
    LiveInstrument leader, follower;
    diffusion::PairStats st;
    DiffPair(std::string u, DiffRole r, LiveInstrument l, LiveInstrument f, diffusion::Config c)
        : under(std::move(u)), role(r), leader(std::move(l)), follower(std::move(f)), st(std::move(c)) {}
};

/// Whether `p` may be traded; `why`, when given, says so with the numbers
/// either way (formatted only then: the trader asks on every tick).
[[nodiscard]] inline bool diffusion_passes(const DiffPair& p, const DiffusionGate& g, std::string* why = nullptr) {
    const auto& ll = p.st.lead_lag();
    const char* verdict = nullptr;
    if (!ll.valid) verdict = "warming up";
    else if (ll.best_lag_ns <= 0) verdict = "the leader does not lead";
    else if (ll.best_lag_ns < g.min_lag_ns) verdict = "lag under the jitter floor";
    else if (ll.llr < g.min_llr) verdict = "lead-lag ratio too low";
    else if (std::fabs(ll.best_rho) < g.min_rho) verdict = "correlation too weak";
    if (why != nullptr) {
        if (!ll.valid) {
            *why = "warming up (" + std::to_string(ll.leader_moves) + "/" + std::to_string(ll.follower_moves) + " moves)";
        } else {
            const double hl = p.st.half_life_s();
            char b[220];
            std::snprintf(b, sizeof b, "lag %+lld ms, rho %+.2f, LLR %.2f, beta %.3f, half-life %s: %s",
                          static_cast<long long>(ll.best_lag_ns / diffusion::kMsNs), ll.best_rho, std::min(ll.llr, 999.0), ll.beta,
                          std::isfinite(hl) ? (live_fmt::num(hl, 1) + " s").c_str() : "n/a", verdict == nullptr ? "leads" : verdict);
            *why = b;
        }
    }
    return verdict == nullptr;
}

/// The pairs, measured. Never trades.
class DiffusionHub final : public LiveModel {
public:
    struct Config {
        std::vector<std::string> unders{"BANKNIFTY"};
        /// Stocks whose moves may reach the underlying's future late.
        std::map<std::string, std::vector<std::string>> stock_leaders{
            {"BANKNIFTY", {"HDFCBANK", "ICICIBANK", "SBIN", "KOTAKBANK", "AXISBANK"}}};
        int option_strikes = 2;           ///< ATM +/- 2: 5 calls and 5 puts
        std::size_t max_option_pairs = 40;///< per underlying, as the money moves
        diffusion::Config pair{};
        DiffusionGate gate{};
        std::string report_dir;           ///< <live dir>/diffusion; empty = no report
    };
    explicit DiffusionHub(Config c) : c_(std::move(c)) {}

    [[nodiscard]] std::string name() const override { return "Diffusion engine"; }
    [[nodiscard]] std::string family() const override { return "Order flow"; }
    [[nodiscard]] const Config& config() const noexcept { return c_; }
    [[nodiscard]] const DiffusionGate& gate() const noexcept { return c_.gate; }

    void on_new_day(LiveEngine& e) override {
        if (day_ > 0) write_report(day_, last_ns_);
        day_ = e.today();
        pairs_.clear();
        routes_.clear();
        for (const auto& u : c_.unders) {
            const LiveInstrument* fut = e.near_future(u);
            if (fut == nullptr) continue;
            if (const LiveInstrument* idx = e.index_of(u)) add(u, DiffRole::Spot, *fut, *idx, true);
            else if (const LiveInstrument* st = stock_of(e, u)) add(u, DiffRole::Spot, *st, *fut, true);
            if (const auto it = c_.stock_leaders.find(u); it != c_.stock_leaders.end())
                for (const auto& s : it->second)
                    if (const LiveInstrument* st = stock_of(e, s)) add(u, DiffRole::StockToFuture, *st, *fut, false);
            add_options(e, u, *fut);
        }
    }

    void on_quote(LiveEngine& e, std::uint32_t token) override { update(e, token, e.quote_ns()); }
    void on_book_tick(LiveEngine& e, std::uint32_t token, std::int64_t ns) override { update(e, token, ns); }
    void on_print(LiveEngine& e, const PricePayload& p, std::int64_t ns) override { update(e, p.token, ns); }

    void on_minute(LiveEngine& e, int close_minute) override {
        if (day_ != e.today()) on_new_day(e);
        for (const auto& u : c_.unders)
            if (const LiveInstrument* fut = e.near_future(u)) add_options(e, u, *fut);
        if (close_minute % 15 == 0 || close_minute == kLiveCloseMinute) write_report(day_, e.clock_ns());
    }

    [[nodiscard]] LiveModelView view(const LiveEngine&) const override {
        LiveModelView v;
        v.name = name();
        v.family = family();
        v.state = "computing";
        int leading = 0;
        for (const auto& p : pairs_) {
            std::string why;
            const bool ok = diffusion_passes(*p, c_.gate, &why);
            leading += ok ? 1 : 0;
            if (p->role != DiffRole::FutureToOption || ok) v.fields.emplace_back(p->leader.symbol + " -> " + p->follower.symbol, why + discovery_text(*p));
        }
        v.reason = std::to_string(pairs_.size()) + " pairs measured, " + std::to_string(leading) + " pass the gate; it never trades";
        return v;
    }

    [[nodiscard]] const std::vector<std::unique_ptr<DiffPair>>& pairs() const noexcept { return pairs_; }

    /// Writes the day's report now (also called at 15-minute marks).
    void write_report(std::int64_t day, std::int64_t ns) const {
        if (c_.report_dir.empty() || day <= 0 || pairs_.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(c_.report_dir, ec);
        const std::string path = c_.report_dir + "/diffusion_" + day_text(day) + ".csv";
        std::ofstream o(path, std::ios::trunc);
        if (!o) return;
        o << "day,as_of,underlying,role,leader,follower,leader_moves,follower_moves,best_lag_ms,best_rho,llr,beta,"
             "half_life_s,cs_leader,is_leader_lo,is_leader_hi,gate,reason\n";
        const int mod = live_minute_of_day(live_ist_minute_index(ns));
        for (const auto& p : pairs_) {
            const auto& ll = p->st.lead_lag();
            const auto& d = p->st.discovery();
            std::string why;
            const bool ok = diffusion_passes(*p, c_.gate, &why);
            for (auto& ch : why) if (ch == ',') ch = ';';
            char b[512];
            std::snprintf(b, sizeof b, "%s,%s,%s,%s,%s,%s,%zu,%zu,%lld,%.4f,%.3f,%.5f,%s,%s,%s,%s,%s,%s\n", day_text(day).c_str(),
                          live_fmt::hhmm(mod).c_str(), p->under.c_str(), diff_role_text(p->role), p->leader.symbol.c_str(), p->follower.symbol.c_str(),
                          ll.leader_moves, ll.follower_moves, static_cast<long long>(ll.best_lag_ns / diffusion::kMsNs), ll.best_rho,
                          std::min(ll.llr, 1e6), ll.beta, csv_num(p->st.half_life_s()).c_str(), csv_num(d.cs_leader).c_str(),
                          csv_num(d.is_leader_lo).c_str(), csv_num(d.is_leader_hi).c_str(), ok ? "pass" : "fail", why.c_str());
            o << b;
        }
    }

private:
    [[nodiscard]] static std::string csv_num(double v) { return std::isfinite(v) ? live_fmt::num(v, 4) : ""; }
    [[nodiscard]] static std::string discovery_text(const DiffPair& p) {
        const auto& d = p.st.discovery();
        if (!d.valid) return "";
        return " | discovery: leader CS " + live_fmt::num(d.cs_leader) + ", IS " + live_fmt::num(d.is_leader_lo) + ".." + live_fmt::num(d.is_leader_hi);
    }

    [[nodiscard]] static const LiveInstrument* stock_of(const LiveEngine& e, const std::string& sym) {
        for (const auto& i : e.universe())
            if (i.kind == LiveKind::Equity && (i.symbol == sym || i.underlying == sym)) return &i;
        return nullptr;
    }

    [[nodiscard]] static double step(const LiveEngine& e, const std::string& under) {
        const auto calls = e.chain(under, LiveKind::Call);
        double best = 0.0;
        for (std::size_t i = 1; i < calls.size(); ++i) {
            const double d = calls[i]->strike - calls[i - 1]->strike;
            if (d > 0.0 && (best == 0.0 || d < best)) best = d;
        }
        return best;
    }

    void add_options(const LiveEngine& e, const std::string& u, const LiveInstrument& fut) {
        const double atm = e.underlying_level(u), st = step(e, u);
        if (!(atm > 0.0) || !(st > 0.0)) return;
        std::size_t have = 0;
        for (const auto& p : pairs_) if (p->under == u && p->role == DiffRole::FutureToOption) ++have;
        for (const LiveKind k : {LiveKind::Call, LiveKind::Put}) {
            const auto ch = e.chain(u, k);
            if (ch.empty()) continue;
            for (const LiveInstrument* i : ch) {
                if (have >= c_.max_option_pairs) return;
                if (i->expiry_day != ch.front()->expiry_day || std::fabs(i->strike - atm) > st * (c_.option_strikes + 0.5)) continue;
                if (has_follower(u, i->token)) continue;
                add(u, DiffRole::FutureToOption, fut, *i, false);
                ++have;
            }
        }
    }

    [[nodiscard]] bool has_follower(const std::string& u, std::uint32_t tok) const {
        for (const auto& p : pairs_) if (p->under == u && p->follower.token == tok) return true;
        return false;
    }

    void add(const std::string& u, DiffRole r, const LiveInstrument& leader, const LiveInstrument& follower, bool same_asset) {
        diffusion::Config c = c_.pair;
        c.same_asset = same_asset;
        pairs_.push_back(std::make_unique<DiffPair>(u, r, leader, follower, c));
        DiffPair* p = pairs_.back().get();
        routes_[leader.token].push_back({p, true});
        routes_[follower.token].push_back({p, false});
    }

    void update(LiveEngine& e, std::uint32_t token, std::int64_t ns) {
        if (day_ != e.today()) on_new_day(e);
        const auto it = routes_.find(token);
        if (it == routes_.end()) return;
        const double px = e.mid(token);
        if (!(px > 0.0)) return;
        last_ns_ = std::max(last_ns_, ns);
        for (const auto& [p, lead] : it->second) {
            if (lead) p->st.on_leader(px, ns);
            else p->st.on_follower(px, ns);
        }
    }

    Config c_;
    std::vector<std::unique_ptr<DiffPair>> pairs_;
    std::unordered_map<std::uint32_t, std::vector<std::pair<DiffPair*, bool>>> routes_;
    std::int64_t day_ = -1, last_ns_ = 0;
};

enum class DiffMode : std::uint8_t { SellOptions, Futures };

struct DiffusionRules {
    std::string under = "BANKNIFTY";
    DiffMode mode = DiffMode::SellOptions;
    double safety_pts = 2.0;           ///< futures: margin over costs, rupees a unit
    double option_safety = 0.5;        ///< options: margin over costs, rupees a unit of premium
    double stop_pts = 15.0;            ///< futures stop
    double option_stop_pct = 0.25;     ///< premium rise that stops a sold option
    double max_spread_ticks = 4.0;
    double max_loss_trade = 2'000.0;   ///< rupees, after expenses
    double max_daily_loss = 5'000.0;   ///< rupees, this model
    int max_trades_day = 40;
    std::int64_t cooldown_ns = 30'000'000'000;
    std::int64_t min_hold_ns = 10'000'000'000;    ///< the time stop's floor
    std::int64_t max_hold_ns = 300'000'000'000;   ///< and its ceiling
    int first_entry_minute = 9 * 60 + 16;
    int last_entry_minute = 15 * 60;
    std::int64_t lots = 1;
    std::string log_file;              ///< <live dir>/diffusion/trades.csv; empty = none
};

class LiveDiffusionTraderModel final : public LiveModel {
public:
    LiveDiffusionTraderModel(DiffusionRules r, const DiffusionHub* hub, const ObiHub* obi = nullptr)
        : r_(std::move(r)), hub_(hub), obi_(obi) {}

    [[nodiscard]] std::string name() const override {
        return std::string("Diffusion ") + (r_.mode == DiffMode::Futures ? "futures " : "option-sell ") + r_.under;
    }
    [[nodiscard]] std::string family() const override { return "Order flow"; }
    [[nodiscard]] const DiffusionRules& rules() const noexcept { return r_; }

    void on_new_day(LiveEngine& e) override {
        day_ = e.today();
        trades_today_ = 0;
        halted_ = false;
        cooldown_until_ = 0;
        held_ = 0;
        note_ = "watching " + r_.under;
    }
    void on_quote(LiveEngine& e, std::uint32_t token) override { tick(e, token, e.quote_ns()); }
    void on_book_tick(LiveEngine& e, std::uint32_t token, std::int64_t ns) override { tick(e, token, ns); }
    void on_print(LiveEngine& e, const PricePayload& p, std::int64_t ns) override { tick(e, p.token, ns); }
    void on_minute(LiveEngine& e, int) override {
        if (day_ != e.today()) on_new_day(e);
        if (held_ != 0) manage(e, e.clock_ns());
    }

    [[nodiscard]] LiveModelView view(const LiveEngine&) const override {
        LiveModelView v;
        v.name = name();
        v.family = family();
        v.state = halted_ ? "done today" : (held_ != 0 ? "in position" : "watching");
        v.reason = note_;
        v.fields.emplace_back("Trades today", std::to_string(trades_today_) + " of " + std::to_string(r_.max_trades_day));
        v.fields.emplace_back("Limits", "Rs " + live_fmt::num(r_.max_loss_trade, 0) + " a trade, Rs " + live_fmt::num(r_.max_daily_loss, 0) + " a day (paper)");
        return v;
    }

private:
    struct Candidate {
        const DiffPair* pair = nullptr;
        const LiveInstrument* inst = nullptr;
        int side = 0;
        double price = 0.0, fair = 0.0, cost = 0.0, safety = 0.0, edge = -1.0;
        std::string gate;
    };

    [[nodiscard]] bool mine(const DiffPair& p) const {
        if (p.under != r_.under) return false;
        if (r_.mode == DiffMode::SellOptions) return p.role == DiffRole::FutureToOption;
        // Futures: the future must be the follower.
        return p.follower.kind == LiveKind::Future && (p.role == DiffRole::StockToFuture || p.role == DiffRole::Spot);
    }

    void tick(LiveEngine& e, std::uint32_t token, std::int64_t ns) {
        if (day_ != e.today()) on_new_day(e);
        if (hub_ == nullptr) return;
        if (held_ != 0) {
            if (token == held_) manage(e, ns);
            return;
        }
        bool touches = false;
        for (const auto& p : hub_->pairs())
            if (mine(*p) && (p->leader.token == token || p->follower.token == token)) { touches = true; break; }
        if (!touches) return;
        Candidate best;
        for (const auto& p : hub_->pairs()) {
            if (!mine(*p)) continue;
            Candidate c;
            if (evaluate(e, *p, ns, c) && c.edge > best.edge) best = std::move(c);
        }
        if (best.pair == nullptr || !(best.edge > 0.0)) return;
        (void)diffusion_passes(*best.pair, hub_->gate(), &best.gate);   // the words, only for the one traded
        std::string why;
        if (!can_enter(e, ns, why)) { blocked(e, ns, "signal on " + best.inst->symbol + " but " + why); return; }
        enter(e, best, ns);
    }

    /// The follower's quote against its fair price; true with a candidate when it is tradable.
    [[nodiscard]] bool evaluate(const LiveEngine& e, const DiffPair& p, std::int64_t ns, Candidate& c) const {
        if (!diffusion_passes(p, hub_->gate())) return false;
        const diffusion::Gap g = p.st.gap();
        if (!g.valid) return false;
        const LiveInstrument* in = e.instrument(p.follower.token);
        const LiveState* s = e.state(p.follower.token);
        if (in == nullptr || s == nullptr || s->bid <= 0 || s->ask <= s->bid) return false;
        const double bid = static_cast<double>(s->bid) / 100.0, ask = static_cast<double>(s->ask) / 100.0;
        if (in->tick > 0.0 && (ask - bid) / in->tick > r_.max_spread_ticks + 1e-9) return false;
        const bool option = in->kind == LiveKind::Call || in->kind == LiveKind::Put;
        int side = 0;
        double price = 0.0;
        if (g.fair < bid) { side = -1; price = bid; }            // rich: sell at the bid
        else if (!option && g.fair > ask) { side = 1; price = ask; }   // cheap future: buy at the ask
        if (side == 0) return false;
        const double qty = static_cast<double>(std::max<std::int64_t>(1, in->lot) * r_.lots);
        const double cost = (e.book().expense(*in, side > 0, qty, price, ns) + e.book().expense(*in, side < 0, qty, g.fair, ns)) / qty;
        if (!std::isfinite(cost)) return false;
        const double safety = option ? r_.option_safety : r_.safety_pts;
        c.pair = &p; c.inst = in; c.side = side; c.price = price; c.fair = g.fair; c.cost = cost; c.safety = safety;
        c.edge = std::fabs(price - g.fair) - cost - safety;
        return true;
    }

    [[nodiscard]] bool can_enter(const LiveEngine& e, std::int64_t ns, std::string& why) const {
        if (halted_) { why = "done for the day"; return false; }
        if (e.paused()) { why = "engine paused"; return false; }
        if (e.stale()) { why = "feed stale"; return false; }
        const int mod = live_minute_of_day(live_ist_minute_index(ns));
        if (mod < r_.first_entry_minute || mod >= r_.last_entry_minute) { why = "outside entry hours"; return false; }
        if (ns < cooldown_until_) { why = "cooling down after a loss"; return false; }
        if (trades_today_ >= r_.max_trades_day) { why = "trade limit for the day"; return false; }
        return true;
    }

    void enter(LiveEngine& e, const Candidate& c, std::int64_t ns) {
        const bool option = c.inst->kind == LiveKind::Call || c.inst->kind == LiveKind::Put;
        if (option && c.side > 0) {   // RULE: options are sold only
            e.note_decision(name(), "refused: an entry would BUY " + c.inst->symbol + " (options are sold only)");
            return;
        }
        const auto& ll = c.pair->st.lead_lag();
        std::string obi;
        if (obi_ != nullptr)
            if (const auto* f = obi_->flow_of(c.pair->leader.token); f != nullptr && f->signals().valid) obi = live_fmt::num(f->signals().obi_short);
        char b[400];
        std::snprintf(b, sizeof b, "%s moved first (%s); %s %s %.2f vs fair %.2f: gap %.2f, costs %.2f + safety %.2f = edge %.2f a unit%s",
                      c.pair->leader.symbol.c_str(), c.gate.c_str(), c.side > 0 ? "BUY" : "SELL", c.inst->symbol.c_str(), c.price, c.fair,
                      std::fabs(c.price - c.fair), c.cost, c.safety, c.edge, obi.empty() ? "" : ("; leader OBI5 " + obi).c_str());
        const std::string reason = b;
        std::string why;
        if (!e.book().open(name(), *c.inst, c.side, r_.lots, ns, reason, false, &why)) { blocked(e, ns, "entry refused: " + why); return; }
        held_ = c.inst->token;
        entry_ns_ = ns;
        target_ = c.price + 0.8 * (c.fair - c.price);   // 80% of the gap: the fair price itself is an estimate
        const double hl = c.pair->st.half_life_s();
        const std::int64_t by_hl = std::isfinite(hl) ? static_cast<std::int64_t>(4.0 * hl * 1e9) : r_.max_hold_ns;
        hold_ns_ = std::clamp(by_hl, r_.min_hold_ns, r_.max_hold_ns);
        ++trades_today_;
        note_ = reason;
        e.note_decision(name(), note_);
        log_row(ns, "ENTRY", c.inst->symbol, c.side, c.price, c.pair->leader.symbol, ll.best_lag_ns, ll.best_rho, ll.llr, ll.beta, hl,
                c.fair, c.cost, c.edge, obi, std::numeric_limits<double>::quiet_NaN(), reason);
    }

    void manage(LiveEngine& e, std::int64_t ns) {
        const LivePosition* p = e.book().position(name(), held_);
        if (p == nullptr) { finish(e, ns); return; }
        if (p->state == LivePosState::Closing || !p->filled()) return;
        const double mark = e.mid(held_);
        if (!(mark > 0.0)) return;
        const bool option = p->inst.kind == LiveKind::Call || p->inst.kind == LiveKind::Put;
        const double u = e.book().unrealised(*p, ns);
        std::string exit;
        if ((p->side < 0 && mark <= target_) || (p->side > 0 && mark >= target_)) exit = "gap closed: mark " + live_fmt::num(mark) + " reached " + live_fmt::num(target_) + " (80% of the way to fair)";
        else if (option && p->entry > 0.0 && (mark - p->entry) / p->entry >= r_.option_stop_pct) exit = "stop: premium up " + live_fmt::pct((mark - p->entry) / p->entry, 1);
        else if (!option && (mark - p->entry) * p->side <= -r_.stop_pts) exit = "stop: " + live_fmt::num((mark - p->entry) * p->side) + " pts";
        if (exit.empty() && std::isfinite(u) && -u >= r_.max_loss_trade) exit = "max loss a trade: Rs " + live_fmt::num(-u, 0);
        if (exit.empty() && ns - entry_ns_ >= hold_ns_) exit = "time stop after " + live_fmt::num(static_cast<double>(hold_ns_) / 1e9, 0) + " s (4 half-lives)";
        if (exit.empty()) return;
        const std::string sym = p->inst.symbol;
        (void)e.book().close(name(), held_, ns, exit);
        e.note_decision(name(), "EXIT " + sym + ": " + exit);
        note_ = "exiting " + sym + ": " + exit;
        exit_reason_ = exit;
        if (e.book().position(name(), held_) == nullptr) finish(e, ns);
    }

    void finish(LiveEngine& e, std::int64_t ns) {
        double last = 0.0, day = 0.0;
        bool found = false;
        std::string sym;
        for (const auto& t : e.book().trades()) {
            if (t.model != name() || live_day_of(live_ist_minute_index(t.exit_ns)) != e.today()) continue;
            const double net = std::isfinite(t.net) ? t.net : t.gross;
            day += net;
            if (t.token == held_) { last = net; found = true; }
        }
        if (const LiveInstrument* in = e.instrument(held_)) sym = in->symbol;
        if (found) log_row(ns, "EXIT", sym, 0, e.mid(held_), "", 0, 0.0, 0.0, 0.0, std::numeric_limits<double>::quiet_NaN(), target_, 0.0, 0.0, "", last, exit_reason_);
        if (found && last < 0.0) cooldown_until_ = ns + r_.cooldown_ns;
        if (-day >= r_.max_daily_loss) {
            halted_ = true;
            e.note_decision(name(), "daily loss Rs " + live_fmt::num(-day, 0) + " reached the Rs " + live_fmt::num(r_.max_daily_loss, 0) + " limit: no entries until tomorrow");
        }
        note_ = found ? "last trade Rs " + live_fmt::num(last, 0) + ", today Rs " + live_fmt::num(day, 0) : "entry did not fill";
        held_ = 0;
        exit_reason_.clear();
    }

    void blocked(LiveEngine& e, std::int64_t ns, const std::string& why) {
        note_ = why;
        if (ns - last_blocked_ns_ < 30'000'000'000 && why == last_blocked_) return;
        last_blocked_ns_ = ns;
        last_blocked_ = why;
        e.note_decision(name(), "no entry: " + why);
    }

    void log_row(std::int64_t ns, const char* what, const std::string& sym, int side, double price, const std::string& leader,
                 std::int64_t lag_ns, double rho, double llr, double beta, double hl, double fair, double cost, double edge,
                 const std::string& obi, double pnl, std::string reason) const {
        if (r_.log_file.empty()) return;
        std::error_code ec;
        const std::filesystem::path path(r_.log_file);
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
        const bool fresh = !std::filesystem::exists(path, ec);
        std::ofstream o(path, std::ios::app);
        if (!o) return;
        if (fresh) o << "time_ns,day,time,model,event,symbol,side,price,leader,lag_ms,rho,llr,beta,half_life_s,fair_or_target,cost,edge,leader_obi5,pnl,reason\n";
        for (auto& ch : reason) if (ch == ',' || ch == '\n') ch = ';';
        const std::int64_t m = live_ist_minute_index(ns);
        char b[512];
        std::snprintf(b, sizeof b, "%lld,%s,%s,%s,%s,%s,%d,%.2f,%s,%lld,%.4f,%.3f,%.5f,%s,%.2f,%.4f,%.4f,%s,%s,", static_cast<long long>(ns),
                      day_text(live_day_of(m)).c_str(), live_fmt::hhmm(live_minute_of_day(m)).c_str(), name().c_str(), what, sym.c_str(), side, price,
                      leader.c_str(), static_cast<long long>(lag_ns / diffusion::kMsNs), rho, std::min(llr, 1e6), beta,
                      std::isfinite(hl) ? live_fmt::num(hl, 3).c_str() : "", fair, cost, edge, obi.c_str(),
                      std::isfinite(pnl) ? live_fmt::num(pnl, 2).c_str() : "");
        o << b << reason << "\n";
    }

    DiffusionRules r_;
    const DiffusionHub* hub_ = nullptr;
    const ObiHub* obi_ = nullptr;
    std::int64_t day_ = -1;
    std::uint32_t held_ = 0;
    std::int64_t entry_ns_ = 0, hold_ns_ = 0, cooldown_until_ = 0, last_blocked_ns_ = 0;
    double target_ = 0.0;
    int trades_today_ = 0;
    bool halted_ = false;
    std::string note_, last_blocked_, exit_reason_;
};

} // namespace altair::live
