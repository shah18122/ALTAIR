// app/live_real_orders.hpp -- a strategy's demo orders, made real when (and
// only when) it is switched on.
//
// THE MODELS ENGINE PLACES NOTHING. It writes REQUESTS to
// data/order_intents.jsonl -- the queue the Terminal writes to -- and the
// order router (oms/live_router.hpp) is what checks and sends them. A request
// goes only when ALL hold, read from the files every second:
//   * the feed is FYERS's live market -- never the SIM, never a replay;
//   * LIVE is armed (data/live_trading.json, the Terminal's typed switch);
//   * the strategy's own switch in it is on (Auto: Arbitrage / OHL / Option
//     arb) -- the other models never send anything;
//   * no halt (data/kill_request.json);
//   * the strategy's real P&L today is above its own loss cap.
// Exits are always sent for what is really held, switch or no switch: an exit
// refused leaves a position on.
//
// HOW. Every order the paper book accepts (LivePaperBook::on_order) is the
// strategy's decision as it is made. An ENTRY becomes an IOC limit at the
// touch; the legs of one decision go back to back, in the order the model
// gave them (the option arbitrage orders them by the book's imbalance), once
// the decision is complete (LiveEngine::after_event). An EXIT becomes a DAY
// limit through the touch (it should fill now), for what really filled and is
// not already on its way out.
//
// SIZE. The legs of one decision are sized together, keeping their ratio, to
// the most the caps allow: the strategy's lots and rupees a leg and the arm's
// (its rupees with 2 % to spare, so the exit -- priced through the touch --
// still passes). The router's ceiling is 50 lots an order; an equity's lot is
// one share, so a real arbitrage leg is at most 50 shares whatever the demo
// trades. Caps under one lot of every leg send nothing.
//
// LEGGING. A multi-leg decision (an arbitrage pair, a parity or box lock)
// whose legs did not all fill is flattened at once: what filled is sent back
// out, and the round trip is recorded as a legging loss.
//
// THE RECORD. Every real round trip -- entry and exit both filled at FYERS --
// is appended to data/live_orders/strategy_trades.csv with FYERS's average
// prices, the gross P&L, the expenses (the engine's own expense function)
// and the net: the same columns as the demo record, so the Threshold and
// Arbitrage pages show the two side by side.

#pragma once

#include <live/engine.hpp>
#include <oms/live_router.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <vector>

namespace altair::live_real {

namespace fs = std::filesystem;

/// The strategies that may send real orders, by model name.
[[nodiscard]] inline std::string strategy_key(const std::string& model) {
    if (model == "Cross-exchange arbitrage") return "arbitrage";
    if (model == "OHL" || model.rfind("OHL ", 0) == 0) return "ohl";
    if (model == "Option arbitrage") return "option_arb";
    return {};
}

/// One request line, in the queue's exact field order (oms/order_intent.hpp).
[[nodiscard]] inline std::string intent_line(const std::string& id, const std::string& at, const std::string& by,
                                             std::uint32_t token, const std::string& symbol, const std::string& exchange,
                                             bool buy, std::int64_t lots, std::int64_t limit_paise,
                                             const std::string& product, const std::string& validity) {
    return "{\"v\":1,\"id\":\"" + id + "\",\"at\":\"" + at + "\",\"by\":\"" + by + "\",\"token\":" + std::to_string(token)
         + ",\"symbol\":\"" + symbol + "\",\"exchange\":\"" + exchange + "\",\"side\":\"" + (buy ? "BUY" : "SELL")
         + "\",\"lots\":" + std::to_string(lots) + ",\"order_type\":\"LIMIT\",\"limit_paise\":" + std::to_string(limit_paise)
         + ",\"product\":\"" + product + "\",\"validity\":\"" + validity + "\"}";
}

/// UTC, to the second, with Z (the queue refuses a time without an offset).
[[nodiscard]] inline std::string utc_text(std::int64_t ns) {
    const std::int64_t s = ns / 1'000'000'000LL;
    const std::int64_t days = s / 86400, sod = s % 86400;
    // civil_from_days
    std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    char b[64];
    std::snprintf(b, sizeof b, "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ", static_cast<long long>(y + (m <= 2 ? 1 : 0)), m, d,
                  static_cast<long long>(sod / 3600), static_cast<long long>(sod / 60 % 60), static_cast<long long>(sod % 60));
    return b;
}

/// The exchange a request names for an instrument (the router checks it).
[[nodiscard]] inline std::string request_exchange(const live::LiveInstrument& in) {
    const bool bse = in.fyers.rfind("BSE:", 0) == 0;
    if (in.kind == live::LiveKind::Equity) return bse ? "BSE" : "NSE";
    return bse ? "BFO" : "NFO";
}

/// One real order the engine asked for.
struct RealLeg {
    std::string intent, model, key;
    std::uint32_t token = 0;
    std::string symbol;
    int side = 0;
    std::int64_t lots = 0, lot = 1;
    std::int64_t decided_ns = 0;
    bool exit = false;
    std::string reason;            ///< why the strategy decided it
    std::string status;            ///< the router's word for it
    std::int64_t filled = 0;       ///< units
    double avg = 0.0;              ///< rupees
    bool final = false;
};

/// What is really held, per (model, token), from filled entries less filled exits.
struct RealHolding {
    std::int64_t qty = 0;          ///< signed units
    double entry = 0.0;            ///< rupees, volume-weighted
    std::int64_t entry_ns = 0;
    double entry_expenses = 0.0;
    std::string why;
};

class RealOrderBridge {
public:
    RealOrderBridge(fs::path root, live::LiveCostFn cost) : root_(std::move(root)), cost_(std::move(cost)) {}

    /// Hook the engine's paper book: every accepted order comes here.
    void attach(live::LiveEngine& e) {
        engine_ = &e;
        e.book().on_order = [this](const live::LiveOrderEvent& ev) { on_order(ev); };
        e.after_event = [this] { flush(); };
    }
    [[nodiscard]] const std::vector<RealLeg>& legs() const noexcept { return legs_; }
    [[nodiscard]] const std::map<std::pair<std::string, std::uint32_t>, RealHolding>& holdings() const noexcept { return held_; }
    [[nodiscard]] bool armed() const noexcept { return armed_; }
    [[nodiscard]] std::string switches_text() const {
        std::string t;
        for (const auto& s : arm_.strategies) t += (t.empty() ? "" : ", ") + s.key + (s.on ? " ON" : " off");
        return t;
    }
    /// Tests: a clock for the request's `at` and id (the feed time by default).
    void set_wall_clock(std::function<std::int64_t()> f) { wall_ = std::move(f); }

    /// Once a second: the switches, the halt, what the router did, legging, the record.
    void poll(std::int64_t now_ns) {
        flush();
        read_switches(now_ns);
        read_orders();
        settle(now_ns);
    }

    /// The entries decided since the last call, each decision's legs together.
    void flush() {
        if (pending_.empty()) return;
        std::vector<Pending> batch;
        batch.swap(pending_);
        for (std::size_t i = 0; i < batch.size();) {
            std::size_t j = i + 1;
            while (j < batch.size() && batch[j].ev.model == batch[i].ev.model && batch[j].ev.decided_ns == batch[i].ev.decided_ns) ++j;
            send_decision(batch, i, j);
            i = j;
        }
    }

private:
    void read_switches(std::int64_t now_ns) {
        const fs::path arm = root_ / oms::kRouterArmFile;
        std::error_code ec;
        const std::string text = oms::router_detail::file_text(arm.string());
        const auto r = fs::exists(arm, ec) ? oms::read_router_arm(text, now_ns / 1'000'000'000LL) : oms::RouterArmRead{};
        armed_ = r.verdict == oms::RouterArmVerdict::Armed;
        arm_ = r.arm;
        std::error_code kec;
        killed_ = fs::exists(root_ / oms::kRouterKillFile, kec) || kec;
    }

    [[nodiscard]] std::int64_t wall_ns() const {
        if (wall_) return wall_();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    /// The strategy's real net P&L today (from the record), for its loss cap.
    [[nodiscard]] double real_net_today(const std::string& key) const {
        auto it = day_net_.find(key);
        return it == day_net_.end() ? 0.0 : it->second;
    }

    struct Pending {
        live::LiveOrderEvent ev;
        std::string key;
    };

    /// Units of (model, token) already on their way out: exits sent, not yet final.
    [[nodiscard]] std::int64_t leaving(const std::pair<std::string, std::uint32_t>& hk) const {
        std::int64_t n = 0;
        for (const auto& l : legs_)
            if (l.exit && !l.final && l.model == hk.first && l.token == hk.second) n += std::max<std::int64_t>(0, l.lots * l.lot - l.filled);
        return n;
    }

    /// What is really held and not yet leaving, signed.
    [[nodiscard]] std::int64_t to_exit(const std::pair<std::string, std::uint32_t>& hk) const {
        const auto it = held_.find(hk);
        if (it == held_.end() || it->second.qty == 0) return 0;
        const std::int64_t left = std::max<std::int64_t>(0, std::llabs(it->second.qty) - leaving(hk));
        return it->second.qty > 0 ? left : -left;
    }

    void on_order(const live::LiveOrderEvent& ev) {
        const std::string key = strategy_key(ev.model);
        if (key.empty()) return;
        const auto hk = std::make_pair(ev.model, ev.inst.token);
        if (ev.exit) {
            // An entry not yet sent is simply dropped.
            for (auto it = pending_.begin(); it != pending_.end(); ++it)
                if (it->ev.model == ev.model && it->ev.inst.token == ev.inst.token) { pending_.erase(it); return; }
            // Out with what is really held and not already leaving. An entry
            // still working at FYERS (its fill not read yet) is followed out
            // the moment it fills.
            if (const std::int64_t q = to_exit(hk); q != 0) {
                live::LiveOrderEvent out = ev;
                out.side = q > 0 ? -1 : 1;
                send(out, key, true, std::llabs(q));
                return;
            }
            for (const auto& l : legs_)
                if (!l.exit && !l.final && l.model == ev.model && l.token == ev.inst.token) { exit_wanted_.insert(hk); break; }
            return;
        }
        if (!armed_ || killed_) return;
        const oms::RouterStrategy* s = arm_.strategy(key);
        if (s == nullptr || !s->on) return;
        // Never a real order from a simulated or replayed price.
        if (engine_ != nullptr && (engine_->simulated() || engine_->replayed())) {
            if (!sim_noted_) note("the feed is simulated or replayed: " + key + " stays in demo, nothing real is sent");
            sim_noted_ = true;
            return;
        }
        if (real_net_today(key) * 100.0 <= -static_cast<double>(s->max_daily_loss_paise)) {
            note("strategy " + key + ": its real loss today has reached the cap; no new entries");
            return;
        }
        pending_.push_back(Pending{ev, key});
    }

    /// The limit a request carries: the touch for an entry; through it by half
    /// a percent (inside the router's band) for an exit, so it fills now. On
    /// the tick, never better than asked. 0 when there is no touch.
    [[nodiscard]] static std::int64_t limit_for(const live::LiveOrderEvent& ev, bool exit) {
        const std::int64_t tick = std::max<std::int64_t>(1, std::llround(ev.inst.tick * 100.0));
        std::int64_t px = ev.touch_paise;
        if (px <= 0) return 0;
        if (exit) {
            const std::int64_t through = std::max<std::int64_t>(tick, px / 200);
            px += ev.side > 0 ? through : -through;
        }
        return (px + (ev.side > 0 ? tick - 1 : 0)) / tick * tick;
    }

    /// One decision's entries, sized together to the caps, keeping their ratio.
    void send_decision(const std::vector<Pending>& b, std::size_t from, std::size_t to) {
        const std::string& key = b[from].key;
        const oms::RouterStrategy* s = arm_.strategy(key);
        if (!armed_ || killed_ || s == nullptr || !s->on) return;
        std::vector<std::int64_t> want(to - from);
        std::int64_t g = 0;
        for (std::size_t k = from; k < to; ++k) {
            const std::int64_t lot = b[k].ev.inst.lot > 0 ? b[k].ev.inst.lot : 1;
            want[k - from] = b[k].ev.qty / lot;
            if (want[k - from] <= 0) return;
            g = std::gcd(g, want[k - from]);
        }
        const std::int64_t lots_cap = std::min(s->max_lots, arm_.max_lots);
        const double value_cap = std::min(static_cast<double>(s->max_order_value_paise),
                                          static_cast<double>(arm_.max_order_value_paise) / 1.02);
        std::int64_t m = g;   // the multiple of the decision's smallest whole ratio
        for (std::size_t k = from; k < to; ++k) {
            const std::int64_t base = want[k - from] / g;
            const std::int64_t lot = b[k].ev.inst.lot > 0 ? b[k].ev.inst.lot : 1;
            const std::int64_t px = limit_for(b[k].ev, false);
            if (px <= 0) { note(b[k].ev.inst.symbol + ": no touch price; " + key + " decision not sent"); return; }
            const auto by_value = static_cast<std::int64_t>(value_cap / (static_cast<double>(px) * static_cast<double>(lot)));
            m = std::min(m, std::min(lots_cap, by_value) / base);
        }
        if (m <= 0) {
            note("strategy " + key + ": its caps are under one lot of every leg; " + b[from].ev.inst.symbol + " not sent");
            return;
        }
        for (std::size_t k = from; k < to; ++k) {
            const std::int64_t lot = b[k].ev.inst.lot > 0 ? b[k].ev.inst.lot : 1;
            send(b[k].ev, key, false, want[k - from] / g * m * lot);
        }
    }

    void send(const live::LiveOrderEvent& ev, const std::string& key, bool exit, std::int64_t qty) {
        const std::int64_t lot = ev.inst.lot > 0 ? ev.inst.lot : 1;
        const std::int64_t lots = qty / lot;
        if (lots <= 0) return;
        const std::int64_t px = limit_for(ev, exit);
        if (px <= 0) { note(ev.inst.symbol + ": no touch price; request not written"); return; }
        const std::int64_t wall = wall_ns();
        const std::string id = std::to_string(wall / 1'000'000) + "-" + std::to_string(ev.inst.token) + "-" + key
                             + (exit ? "-x" : "-n") + std::to_string(++seq_);
        const std::string by = "strategy." + key + (exit ? ".exit" : "");
        const bool carry = ev.model == "Option arbitrage";
        const std::string product = carry ? "NRML" : "MIS";
        const std::string line = intent_line(id, utc_text(wall), by, ev.inst.token, ev.inst.symbol, request_exchange(ev.inst),
                                             ev.side > 0, lots, px, product == "NRML" && ev.inst.kind == live::LiveKind::Equity
                                                                      ? std::string("CNC") : product,
                                             exit ? "DAY" : "IOC");
        std::ofstream f(root_ / oms::kIntentFile, std::ios::binary | std::ios::app);
        f << line << '\n';
        f.flush();
        if (!f) { note("could not write a request for " + ev.inst.symbol); return; }
        RealLeg l;
        l.intent = id; l.model = ev.model; l.key = key; l.token = ev.inst.token; l.symbol = ev.inst.symbol;
        l.side = ev.side; l.lots = lots; l.lot = lot; l.decided_ns = ev.decided_ns; l.exit = exit; l.reason = ev.reason;
        l.status = "SENT";
        legs_.push_back(std::move(l));
        insts_[ev.inst.token] = ev.inst;
        note(std::string(exit ? "REAL exit " : "REAL entry ") + (ev.side > 0 ? "BUY " : "SELL ") + std::to_string(lots) + " lot(s) "
             + ev.inst.symbol + " @ " + live::live_fmt::num(static_cast<double>(px) / 100.0) + " (" + key + ")");
    }

    /// What the router did with each request (data/live_orders/orders.json).
    void read_orders() {
        const auto j = oms::parse_router_json(oms::router_detail::file_text((root_ / oms::kRouterDir / "orders.json").string()));
        if (!j) return;
        const oms::RouterJson* orders = j->get("orders");
        if (orders == nullptr || orders->kind != oms::RouterJson::Kind::Array) return;
        for (const auto& o : orders->items) {
            const std::string id = o.str("intent");
            for (auto& l : legs_) {
                if (l.intent != id || l.final) continue;
                const std::string st = o.str("status");
                const auto filled = static_cast<std::int64_t>(o.num("filled"));
                const double avg = o.num("avg_paise") / 100.0;
                if (filled > l.filled) {
                    apply_fill(l, filled - l.filled, avg > 0.0 ? avg : 0.0);
                    l.filled = filled;
                    follow_out(l);
                }
                if (avg > 0.0) l.avg = avg;
                l.status = st;
                const auto rs = oms::router_status_from_text(st);
                l.final = rs && oms::router_final(*rs);
            }
        }
    }

    /// An entry filled after its strategy already left the trade: out at once.
    void follow_out(const RealLeg& l) {
        const auto hk = std::make_pair(l.model, l.token);
        if (l.exit || exit_wanted_.count(hk) == 0 || engine_ == nullptr) return;
        const std::int64_t q = to_exit(hk);
        const auto in = insts_.find(l.token);
        if (q == 0 || in == insts_.end()) return;
        const live::LiveTop t = engine_->top(l.token);
        live::LiveOrderEvent ev;
        ev.model = l.model;
        ev.inst = in->second;
        ev.side = q > 0 ? -1 : 1;
        ev.qty = std::llabs(q);
        ev.touch_paise = ev.side > 0 ? t.ask : t.bid;
        ev.exit = true;
        ev.reason = "the strategy left before this entry's fill was read";
        exit_wanted_.erase(hk);
        send(ev, l.key, true, ev.qty);
    }

    /// A fill moves the real holding; a closing fill completes a round trip.
    void apply_fill(const RealLeg& l, std::int64_t units, double price) {
        auto& h = held_[{l.model, l.token}];
        const std::int64_t signed_units = units * l.side;
        const auto it = insts_.find(l.token);
        // NaN when the expense schedule cannot price it: the round trip then
        // has no net, and the record says so instead of showing gross as net.
        const double exp = it != insts_.end() && cost_ ? cost_(it->second, l.side > 0, static_cast<double>(units), price, 0)
                                                       : std::numeric_limits<double>::quiet_NaN();
        if (h.qty == 0 || (h.qty > 0) == (signed_units > 0)) {
            const double q0 = static_cast<double>(std::llabs(h.qty)), q1 = static_cast<double>(units);
            h.entry = (h.entry * q0 + price * q1) / (q0 + q1);
            if (h.qty == 0) { h.entry_ns = wall_ns(); h.entry_expenses = 0.0; }
            h.qty += signed_units;
            h.entry_expenses += exp;
            return;
        }
        // Closing (some or all).
        const std::int64_t closing = std::min<std::int64_t>(units, std::llabs(h.qty));
        const int side = h.qty > 0 ? 1 : -1;
        const double gross = (price - h.entry) * static_cast<double>(closing) * side;
        const double entry_share = h.entry_expenses * static_cast<double>(closing) / static_cast<double>(std::llabs(h.qty));
        const double expenses = entry_share + exp;
        record(l, closing, side, h, price, gross, expenses);
        h.entry_expenses -= entry_share;
        h.qty -= closing * side;
        if (h.qty == 0) h.entry_expenses = 0.0;
        // The loss cap counts the net, or the gross when the expenses are unpriced.
        day_net_[l.key] += std::isfinite(expenses) ? gross - expenses : gross;
    }

    void record(const RealLeg& l, std::int64_t qty, int side, const RealHolding& h, double exit_px, double gross, double expenses) {
        const fs::path path = root_ / oms::kRouterDir / "strategy_trades.csv";
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        const bool fresh = !fs::exists(path, ec);
        std::ofstream f(path, std::ios::binary | std::ios::app);
        if (fresh) f << "date,model,symbol,token,side,qty,entry_time,entry,exit_time,exit,gross,expenses,net,why_in,why_out,source,costs\n";
        const auto ist = [](std::int64_t ns) {
            const std::int64_t s = ns / 1'000'000'000LL + 19800;
            char b[32];
            std::snprintf(b, sizeof b, "%02lld:%02lld:%02lld", static_cast<long long>(s / 3600 % 24),
                          static_cast<long long>(s / 60 % 60), static_cast<long long>(s % 60));
            return std::string(b);
        };
        const std::int64_t now = wall_ns();
        std::string why_out = l.reason.empty() ? std::string("real exit") : l.reason;
        for (char& c : why_out) if (c == '"' || c == '\n' || c == '\r') c = '\'';
        const auto num = [](double v) {
            if (!std::isfinite(v)) return std::string();   // unpriced: blank, as in the demo record
            char b[48];
            std::snprintf(b, sizeof b, "%.2f", v);
            return std::string(b);
        };
        char line[1024];
        std::snprintf(line, sizeof line, "%s,\"%s\",%s,%u,%s,%lld,%s,%.2f,%s,%.2f,%.2f,%s,%s,\"%s\",\"%s\",REAL,%s\n",
                      utc_text(now).substr(0, 10).c_str(), l.model.c_str(), l.symbol.c_str(), l.token, side > 0 ? "long" : "short",
                      static_cast<long long>(qty), ist(h.entry_ns).c_str(), h.entry, ist(now).c_str(), exit_px, gross,
                      num(expenses).c_str(), num(gross - expenses).c_str(), "real entry", why_out.c_str(),
                      std::isfinite(expenses) ? "FYERS" : "FYERS (expenses unpriced)");
        f << line;
    }

    /// Legging: a decision whose entries are all final but not all filled is
    /// flattened -- what filled goes back out at once.
    void settle(std::int64_t now_ns) {
        std::map<std::pair<std::string, std::int64_t>, std::vector<RealLeg*>> groups;
        for (auto& l : legs_) if (!l.exit) groups[{l.model, l.decided_ns}].push_back(&l);
        for (auto& [g, ls] : groups) {
            if (ls.size() < 2 || flattened_.count(g) != 0) continue;
            bool all_final = true, all_filled = true, any_filled = false;
            for (const RealLeg* l : ls) {
                all_final = all_final && l->final;
                const bool full = l->filled >= l->lots * l->lot;
                all_filled = all_filled && full;
                any_filled = any_filled || l->filled > 0;
            }
            if (!all_final || all_filled || !any_filled) continue;
            flattened_.insert(g);
            for (const RealLeg* l : ls) {
                const std::int64_t q = to_exit({l->model, l->token});
                const auto in = insts_.find(l->token);
                if (q == 0 || in == insts_.end() || engine_ == nullptr) continue;
                const live::LiveTop t = engine_->top(l->token);
                live::LiveOrderEvent ev;
                ev.model = l->model;
                ev.inst = in->second;
                ev.side = q > 0 ? -1 : 1;
                ev.qty = std::llabs(q);
                ev.touch_paise = ev.side > 0 ? t.ask : t.bid;
                ev.ns = now_ns;
                ev.decided_ns = now_ns;
                ev.exit = true;
                ev.reason = "legging: the other leg did not fill";
                send(ev, l->key, true, ev.qty);
            }
            note("legging: " + g.first + "'s legs did not all fill; what filled was sent back out");
        }
    }

    void note(const std::string& s) {
        std::printf("  %s\n", s.c_str());
        std::fflush(stdout);
        if (engine_ != nullptr) engine_->note_decision("real orders", s);
    }

    fs::path root_;
    live::LiveCostFn cost_;
    live::LiveEngine* engine_ = nullptr;
    oms::RouterArm arm_;
    bool armed_ = false, killed_ = true;
    std::vector<RealLeg> legs_;
    std::vector<Pending> pending_;
    std::map<std::uint32_t, live::LiveInstrument> insts_;
    std::map<std::pair<std::string, std::uint32_t>, RealHolding> held_;
    std::map<std::string, double> day_net_;
    std::set<std::pair<std::string, std::int64_t>> flattened_;
    std::set<std::pair<std::string, std::uint32_t>> exit_wanted_;
    std::function<std::int64_t()> wall_;
    std::uint64_t seq_ = 0;
    bool sim_noted_ = false;
};

} // namespace altair::live_real
