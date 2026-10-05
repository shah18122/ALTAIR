// live/arbitrage.hpp -- cross-exchange arbitrage: the same stock on NSE and BSE.
//
// A NIFTY 50 stock trades on both exchanges. When one exchange's bid is above
// the other's ask by more than it costs to get in and out, buying the cheap
// side and selling the dear one locks in the difference; the position is
// unwound when the two prices meet again. This model does that in PAPER, on
// every quote (LiveModel::on_quote), for every stock whose NSE and BSE quotes
// are both streamed (live/universe.hpp adds the BSE twins).
//
// WHAT IT COUNTS AS AN EDGE. Entering takes the touch on both exchanges, so
// the edge is the crossed spread itself: NSE bid - BSE ask (sell NSE, buy
// BSE) or BSE bid - NSE ask. It enters only when that, in basis points of the
// price, covers the four fills' expenses (priced by the engine's own expense
// function at this size -- brokerage, STT, exchange, SEBI, GST, stamp --
// else `cost_bps` while charges are unpriced), the two spreads it will cross
// to get out at today's width, and `min_profit_bps` more. The
// paper book then fills each leg at the touch after its latency -- a
// dislocation that is gone by then fills at the worse price, or not at all,
// and the unfilled leg is closed.
//
// OUT: when the gap between the two mids has closed to a quarter of what it
// was at entry (or under 1 bp), after `max_hold_ns`, or at 15:15; the engine
// squares off what is left at 15:20. One position per stock; at most
// `max_open` stocks at once. Intraday only.
#pragma once

#include <live/engine.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace altair::live {

struct LiveCrossArbRule {
    double cost_bps = 12.0;                              ///< four fills' expenses, bp, when the engine cannot price them
    double min_profit_bps = 2.0;                         ///< over the costs and the spreads out
    double notional = 2'00'000.0;                        ///< rupees a leg (a flat Rs 20 an order weighs less on more)
    int max_open = 5;                                    ///< stocks held at once
    std::int64_t max_hold_ns = 30LL * 60 * 1'000'000'000;
    std::int64_t quote_age_ns = 2'000'000'000;           ///< both quotes at least this fresh to enter
    int last_entry_minute = 15 * 60 + 10;
    int close_minute = 15 * 60 + 15;
};

/// One stock's two listings, by token (the universe can grow mid-session).
struct LiveCrossArbPair {
    std::string symbol;
    std::uint32_t nse = 0, bse = 0;
};

/// The current reading of one pair, in paise and basis points of the price.
struct LiveCrossArbQuote {
    bool ok = false;
    std::int64_t nse_bid = 0, nse_ask = 0, bse_bid = 0, bse_ask = 0;
    double mid = 0.0;                 ///< paise, of the four prices
    double sell_nse_bps = 0.0;        ///< NSE bid over BSE ask
    double sell_bse_bps = 0.0;        ///< BSE bid over NSE ask
    double spreads_bps = 0.0;         ///< both spreads: the cost of getting out
    double basis_bps = 0.0;           ///< NSE mid over BSE mid
};

/// The NSE and BSE listings of each stock in the universe (BSE twins are the
/// equities whose FYERS symbol starts "BSE:").
[[nodiscard]] inline std::vector<LiveCrossArbPair> live_cross_pairs(const std::vector<LiveInstrument>& u) {
    std::map<std::string, LiveCrossArbPair> by;
    for (const auto& i : u) {
        if (i.kind != LiveKind::Equity) continue;
        auto& p = by[i.symbol];
        p.symbol = i.symbol;
        if (i.fyers.rfind("BSE:", 0) == 0) p.bse = i.token;
        else if (i.fyers.rfind("NSE:", 0) == 0) p.nse = i.token;
    }
    std::vector<LiveCrossArbPair> out;
    for (auto& [s, p] : by)
        if (p.nse != 0 && p.bse != 0) out.push_back(p);
    return out;
}

[[nodiscard]] inline LiveCrossArbQuote live_cross_quote(const LiveState* n, const LiveState* b) {
    LiveCrossArbQuote q;
    if (n == nullptr || b == nullptr || n->bid <= 0 || n->ask <= n->bid || b->bid <= 0 || b->ask <= b->bid) return q;
    q.ok = true;
    q.nse_bid = n->bid; q.nse_ask = n->ask; q.bse_bid = b->bid; q.bse_ask = b->ask;
    q.mid = static_cast<double>(n->bid + n->ask + b->bid + b->ask) / 4.0;
    const auto bp = [&q](double paise) { return paise / q.mid * 1e4; };
    q.sell_nse_bps = bp(static_cast<double>(n->bid - b->ask));
    q.sell_bse_bps = bp(static_cast<double>(b->bid - n->ask));
    q.spreads_bps = bp(static_cast<double>((n->ask - n->bid) + (b->ask - b->bid)));
    q.basis_bps = bp(static_cast<double>(n->bid + n->ask - b->bid - b->ask) / 2.0);
    return q;
}

class LiveCrossArbModel final : public LiveModel {
public:
    explicit LiveCrossArbModel(LiveCrossArbRule rule = {}) : r_(rule) {}

    [[nodiscard]] std::string name() const override { return "Cross-exchange arbitrage"; }
    [[nodiscard]] std::string family() const override { return "arbitrage"; }

    void on_new_day(LiveEngine& e) override {
        pairs_ = live_cross_pairs(e.universe());
        held_.clear();
        seen_today_ = entries_today_ = 0;
    }

    void on_quote(LiveEngine& e, std::uint32_t token) override {
        if (pairs_.empty()) pairs_ = live_cross_pairs(e.universe());
        for (const auto& p : pairs_) {
            if (p.nse != token && p.bse != token) continue;
            act(e, p);
            return;
        }
    }

    void on_minute(LiveEngine& e, int close_minute) override {
        const std::int64_t now = e.clock_ns();
        const auto positions = e.book().positions();
        const auto holds = [&](std::uint32_t tok) {
            return std::any_of(positions.begin(), positions.end(), [&](const LivePosition& x) { return x.model == name() && x.inst.token == tok; });
        };
        for (auto it = held_.begin(); it != held_.end();) {
            const auto p = find(it->first);
            const bool late = close_minute >= r_.close_minute;
            // A leg whose entry timed out leaves the other one naked: out at once.
            const bool lopsided = p != nullptr && holds(p->nse) != holds(p->bse);
            if (p != nullptr && !holds(p->nse) && !holds(p->bse)) { it = held_.erase(it); continue; }   // neither entry filled
            if (p == nullptr || late || lopsided || now - it->second.entry_ns >= r_.max_hold_ns) {
                const std::string why = late ? "15:15: arbitrage is intraday"
                                      : lopsided ? "one leg did not fill: the other is closed"
                                                 : "held 30 minutes without the prices meeting";
                if (p != nullptr) unwind(e, *p, now, why);
                it = held_.erase(it);
            } else {
                ++it;
            }
        }
    }

    [[nodiscard]] LiveModelView view(const LiveEngine& e) const override {
        LiveModelView v;
        v.name = name();
        v.family = family();
        std::vector<std::pair<double, std::string>> rows;
        int quoted = 0;
        double best = -1e9;
        std::string best_text = "no stock quoted on both exchanges yet";
        for (const auto& p : pairs_) {
            const auto q = live_cross_quote(e.state(p.nse), e.state(p.bse));
            if (!q.ok) continue;
            ++quoted;
            const int dir = q.sell_nse_bps >= q.sell_bse_bps ? +1 : -1;
            const double edge = std::max(q.sell_nse_bps, q.sell_bse_bps);
            const double need = cost_bps(e, p, q, dir, qty_for(q), e.clock_ns()) + q.spreads_bps + r_.min_profit_bps;
            const auto px = [](std::int64_t paise) { return live_fmt::num(static_cast<double>(paise) / 100.0); };
            const std::string text = "NSE " + px(q.nse_bid) + "/" + px(q.nse_ask) + " · BSE " + px(q.bse_bid) + "/" + px(q.bse_ask)
                                   + " · edge " + live_fmt::num(edge, 1) + " bp, needs " + live_fmt::num(need, 1);
            rows.emplace_back(edge - need, p.symbol + ": " + text);
            if (edge - need > best) { best = edge - need; best_text = p.symbol + " " + text; }
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        const bool in = e.book().engaged(name());
        v.state = in ? "in position" : pairs_.empty() ? "abstaining" : "watching";
        v.signal = best_text;
        v.reason = pairs_.empty() ? "no stock has both its NSE and BSE quote streamed (the feed adds the BSE twins)"
                 : in ? std::to_string(held_.size()) + " stock(s) held both ways until their prices meet"
                      : "enters when one exchange's bid beats the other's ask by the costs, both spreads and "
                            + live_fmt::num(r_.min_profit_bps, 0) + " bp";
        v.fields.emplace_back("pairs", std::to_string(pairs_.size()) + " (" + std::to_string(quoted) + " quoted both sides)");
        v.fields.emplace_back("opportunities today", std::to_string(seen_today_));
        v.fields.emplace_back("entries today", std::to_string(entries_today_));
        v.fields.emplace_back("held", std::to_string(held_.size()) + " of at most " + std::to_string(r_.max_open));
        v.fields.emplace_back("rule", "four fills' expenses + both spreads + " + live_fmt::num(r_.min_profit_bps, 1) + " bp; Rs "
                                          + live_fmt::num(r_.notional, 0) + " a leg; paper, intraday");
        for (std::size_t i = 0; i < rows.size() && i < 12; ++i) v.fields.emplace_back("pair", rows[i].second);
        return v;
    }

private:
    struct Held {
        int dir = 0;              ///< +1: sold NSE, bought BSE; -1: bought NSE, sold BSE
        double entry_basis = 0.0; ///< bp, NSE mid over BSE mid, at entry
        std::int64_t entry_ns = 0;
    };

    [[nodiscard]] std::int64_t qty_for(const LiveCrossArbQuote& q) const {
        return q.mid > 0.0 ? static_cast<std::int64_t>(r_.notional / (q.mid / 100.0)) : 0;
    }

    /// The four fills (in at the touch both ways, out the same) by the
    /// engine's expense function, in bp of the notional; `cost_bps` when any
    /// of them is unpriced.
    [[nodiscard]] double cost_bps(const LiveEngine& e, const LiveCrossArbPair& p, const LiveCrossArbQuote& q, int dir,
                                  std::int64_t qty, std::int64_t now) const {
        const LiveInstrument* ni = e.instrument(p.nse);
        const LiveInstrument* bi = e.instrument(p.bse);
        if (ni == nullptr || bi == nullptr || qty <= 0 || q.mid <= 0.0) return r_.cost_bps;
        const double n = static_cast<double>(qty);
        const auto rs = [](std::int64_t paise) { return static_cast<double>(paise) / 100.0; };
        const auto& b = e.book();
        const double c = dir > 0
            ? b.expense(*ni, false, n, rs(q.nse_bid), now) + b.expense(*bi, true, n, rs(q.bse_ask), now)
                  + b.expense(*ni, true, n, rs(q.nse_ask), now) + b.expense(*bi, false, n, rs(q.bse_bid), now)
            : b.expense(*bi, false, n, rs(q.bse_bid), now) + b.expense(*ni, true, n, rs(q.nse_ask), now)
                  + b.expense(*bi, true, n, rs(q.bse_ask), now) + b.expense(*ni, false, n, rs(q.nse_bid), now);
        return std::isfinite(c) ? c / (n * q.mid / 100.0) * 1e4 : r_.cost_bps;
    }

    [[nodiscard]] const LiveCrossArbPair* find(const std::string& symbol) const {
        for (const auto& p : pairs_) if (p.symbol == symbol) return &p;
        return nullptr;
    }

    void act(LiveEngine& e, const LiveCrossArbPair& p) {
        const std::int64_t now = e.quote_ns();
        const LiveState* n = e.state(p.nse);
        const LiveState* b = e.state(p.bse);
        const LiveInstrument* ni = e.instrument(p.nse);
        const LiveInstrument* bi = e.instrument(p.bse);
        const auto q = live_cross_quote(n, b);
        if (!q.ok || ni == nullptr || bi == nullptr) return;
        const auto it = held_.find(p.symbol);
        if (it != held_.end()) {
            // Out when the two mids have met: the gap, signed the way it was
            // entered, is a quarter of what it was, or under 1 bp.
            const double gap = it->second.dir * q.basis_bps;
            if (gap <= std::max(1.0, 0.25 * it->second.dir * it->second.entry_basis)) {
                unwind(e, p, now, "the prices met: the gap is " + live_fmt::num(gap, 1) + " bp, from "
                                      + live_fmt::num(it->second.dir * it->second.entry_basis, 1));
                held_.erase(it);
            }
            return;
        }
        const int minute = live_minute_of_day(live_ist_minute_index(now));
        if (minute >= r_.last_entry_minute || now - n->quote_ns > r_.quote_age_ns || now - b->quote_ns > r_.quote_age_ns) return;
        const int dir = q.sell_nse_bps >= q.sell_bse_bps ? +1 : -1;
        const double edge = dir > 0 ? q.sell_nse_bps : q.sell_bse_bps;
        // Size: the notional, and no more than both touches show.
        std::int64_t qty = qty_for(q);
        const std::int64_t touch = dir > 0 ? std::min(n->bid_qty, b->ask_qty) : std::min(b->bid_qty, n->ask_qty);
        if (touch > 0) qty = std::min(qty, touch);
        if (qty < 1) return;
        const double need = cost_bps(e, p, q, dir, qty, now) + q.spreads_bps + r_.min_profit_bps;
        if (edge < need) return;
        ++seen_today_;
        if (static_cast<int>(held_.size()) >= r_.max_open) return;
        const auto money = [](std::int64_t paise) { return live_fmt::num(static_cast<double>(paise) / 100.0); };
        const std::string why = p.symbol + (dir > 0 ? ": sell NSE " + money(q.nse_bid) + ", buy BSE " + money(q.bse_ask)
                                                    : ": sell BSE " + money(q.bse_bid) + ", buy NSE " + money(q.nse_ask))
                              + " x" + std::to_string(qty) + ": edge " + live_fmt::num(edge, 1) + " bp over "
                              + live_fmt::num(need, 1) + " needed";
        std::string no;
        const bool a = e.book().open(name(), *bi, dir, qty, now, why, false, &no);
        const bool c = a && e.book().open(name(), *ni, -dir, qty, now, why, false, &no);
        if (!a || !c) {
            if (a) (void)e.book().close(name(), p.bse, now, "the other leg was refused: " + no);
            e.note_decision(name(), p.symbol + ": not entered (" + no + ")");
            return;
        }
        held_[p.symbol] = Held{dir, q.basis_bps, now};
        ++entries_today_;
        e.note_decision(name(), why);
    }

    void unwind(LiveEngine& e, const LiveCrossArbPair& p, std::int64_t now, const std::string& why) {
        (void)e.book().close(name(), p.nse, now, why);
        (void)e.book().close(name(), p.bse, now, why);
        e.note_decision(name(), p.symbol + ": out, " + why);
    }

    LiveCrossArbRule r_;
    std::vector<LiveCrossArbPair> pairs_;
    std::map<std::string, Held> held_;
    int seen_today_ = 0, entries_today_ = 0;
};

} // namespace altair::live
