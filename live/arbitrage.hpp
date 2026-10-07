// live/arbitrage.hpp -- cross-exchange arbitrage: the same stock on NSE and BSE.
//
// A NIFTY 50 stock trades on both exchanges. When one exchange's BID is above
// the other's ASK by more than the two fills cost, buying at the cheap ask and
// selling at the dear bid locks in the difference. That is the whole trade:
// TWO LEGS. The long in one listing and the short in the other are settled
// against each other by the clearing corporation (interoperability), so there
// is no exit trade and no exit expense; the paper book nets the pair the
// moment both legs have filled (LivePaperBook::net_off) and books it as one
// round trip. This model does it in PAPER, on every quote
// (LiveModel::on_quote), for every stock whose NSE and BSE quotes are both
// streamed (live/universe.hpp adds the BSE twins).
//
// THE MATCH. Bid against ask across the exchanges, never the last trade:
// NSE bid - BSE ask (sell NSE, buy BSE) or BSE bid - NSE ask (sell BSE, buy
// NSE). It enters only when that gap, in basis points of the price, covers
// the TWO fills' expenses -- priced by the engine's own expense function at
// this size and on each exchange's own schedule (brokerage, STT, exchange,
// SEBI, GST, stamp), else `cost_bps` while charges are unpriced -- and
// `min_profit_bps` more.
//
// THE SIZE: the LOWER of the two visible quantities -- the bid size on the
// exchange it sells on, the ask size on the one it buys on -- so both legs can
// fill and the pair settles both ways (`max_notional` caps it when set; real
// orders are capped again by the LIVE switch's own limits).
//
// A leg that does not fill is unwound (the paper book's entry timeout closes
// the other leg). After a pair, the same stock is not entered again until both
// of its quotes have changed and `cooldown_ns` has passed: a gap already taken
// is not counted twice. At most `max_open` pairs are in flight at once; no new
// pair after `last_entry_minute`. Intraday only.
#pragma once

#include <live/engine.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace altair::live {

struct LiveCrossArbRule {
    double cost_bps = 6.0;                               ///< the two fills' expenses, bp, when the engine cannot price them
    double min_profit_bps = 2.0;                         ///< over the two fills' expenses
    double max_notional = 0.0;                           ///< rupees a leg; 0: no cap but the visible size
    int max_open = 5;                                    ///< pairs in flight (legs not yet both filled) at once
    std::int64_t max_hold_ns = 30LL * 60 * 1'000'000'000;   ///< a pair whose legs are still unfilled by then is unwound
    std::int64_t quote_age_ns = 2'000'000'000;           ///< both quotes at least this fresh to enter
    std::int64_t cooldown_ns = 5'000'000'000;            ///< after a pair, before the same stock again
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
        last_.clear();
        seen_today_ = entries_today_ = netted_today_ = 0;
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
        for (auto it = held_.begin(); it != held_.end();) {
            const auto p = find(it->first);
            if (p != nullptr && settle(e, *p, it->second, now)) { it = held_.erase(it); continue; }
            const LivePosition* n = p != nullptr ? e.book().position(name(), p->nse) : nullptr;
            const LivePosition* b = p != nullptr ? e.book().position(name(), p->bse) : nullptr;
            if (p != nullptr && n == nullptr && b == nullptr) { it = held_.erase(it); continue; }   // neither leg filled
            const bool late = close_minute >= r_.close_minute;
            if (p == nullptr || late || now - it->second.entry_ns >= r_.max_hold_ns) {
                // Legs that never both filled: whatever did fill is unwound.
                if (p != nullptr) unwind(e, *p, now, late ? "15:15: arbitrage is intraday" : "a leg did not fill in time");
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
            const LiveState* n = e.state(p.nse);
            const LiveState* b = e.state(p.bse);
            const std::int64_t qty = qty_for(q, n, b, dir);
            const double need = cost_bps(e, p, q, dir, qty, e.clock_ns()) + r_.min_profit_bps;
            const auto px = [](std::int64_t paise) { return live_fmt::num(static_cast<double>(paise) / 100.0); };
            const std::string text = "NSE " + px(q.nse_bid) + "/" + px(q.nse_ask) + " · BSE " + px(q.bse_bid) + "/" + px(q.bse_ask)
                                   + (dir > 0 ? " · sell NSE bid, buy BSE ask" : " · sell BSE bid, buy NSE ask") + " x"
                                   + std::to_string(qty) + " · edge " + live_fmt::num(edge, 1) + " bp, needs " + live_fmt::num(need, 1);
            rows.emplace_back(edge - need, p.symbol + ": " + text);
            if (edge - need > best) { best = edge - need; best_text = p.symbol + " " + text; }
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        const bool in = e.book().engaged(name());
        v.state = in ? "in position" : pairs_.empty() ? "abstaining" : "watching";
        v.signal = best_text;
        v.reason = pairs_.empty() ? "no stock has both its NSE and BSE quotes streamed (the feed adds the BSE twins)"
                 : in ? std::to_string(held_.size()) + " pair(s) waiting for both legs to fill"
                      : "enters when one exchange's bid beats the other's ask by the two fills' expenses and "
                            + live_fmt::num(r_.min_profit_bps, 0) + " bp; the pair is netted by the clearing corporation";
        v.fields.emplace_back("pairs", std::to_string(pairs_.size()) + " (" + std::to_string(quoted) + " quoted both sides)");
        v.fields.emplace_back("opportunities today", std::to_string(seen_today_));
        v.fields.emplace_back("entries today", std::to_string(entries_today_));
        v.fields.emplace_back("netted today", std::to_string(netted_today_));
        v.fields.emplace_back("in flight", std::to_string(held_.size()) + " of at most " + std::to_string(r_.max_open));
        v.fields.emplace_back("rule", "2 legs: sell at the dear bid, buy at the cheap ask; the lower of the two sizes; edge over the "
                                      "two fills' expenses + " + live_fmt::num(r_.min_profit_bps, 1)
                                      + " bp; netted by the clearing corporation, no exit; paper, intraday");
        for (std::size_t i = 0; i < rows.size() && i < 12; ++i) v.fields.emplace_back("pair", rows[i].second);
        return v;
    }

private:
    struct Held {
        int dir = 0;              ///< +1: sold NSE, bought BSE; -1: bought NSE, sold BSE
        std::int64_t entry_ns = 0;
    };
    struct Last {
        std::int64_t ns = 0;                 ///< the last entry
        std::int64_t nse_quote = 0, bse_quote = 0;   ///< the quotes it was taken on
    };

    /// The lower of the two visible quantities (sell side's bid, buy side's
    /// ask), capped by `max_notional` when set.
    [[nodiscard]] std::int64_t qty_for(const LiveCrossArbQuote& q, const LiveState* n, const LiveState* b, int dir) const {
        if (n == nullptr || b == nullptr) return 0;
        std::int64_t qty = dir > 0 ? std::min(n->bid_qty, b->ask_qty) : std::min(b->bid_qty, n->ask_qty);
        if (r_.max_notional > 0.0 && q.mid > 0.0)
            qty = std::min(qty, static_cast<std::int64_t>(r_.max_notional / (q.mid / 100.0)));
        return std::max<std::int64_t>(qty, 0);
    }

    /// The two fills -- sell at the dear bid, buy at the cheap ask -- by the
    /// engine's expense function (each on its own exchange's schedule), in bp
    /// of the notional; `cost_bps` when either is unpriced.
    [[nodiscard]] double cost_bps(const LiveEngine& e, const LiveCrossArbPair& p, const LiveCrossArbQuote& q, int dir,
                                  std::int64_t qty, std::int64_t now) const {
        const LiveInstrument* ni = e.instrument(p.nse);
        const LiveInstrument* bi = e.instrument(p.bse);
        if (ni == nullptr || bi == nullptr || qty <= 0 || q.mid <= 0.0) return r_.cost_bps;
        const double n = static_cast<double>(qty);
        const auto rs = [](std::int64_t paise) { return static_cast<double>(paise) / 100.0; };
        const auto& b = e.book();
        const double c = dir > 0 ? b.expense(*ni, false, n, rs(q.nse_bid), now) + b.expense(*bi, true, n, rs(q.bse_ask), now)
                                 : b.expense(*bi, false, n, rs(q.bse_bid), now) + b.expense(*ni, true, n, rs(q.nse_ask), now);
        return std::isfinite(c) ? c / (n * q.mid / 100.0) * 1e4 : r_.cost_bps;
    }

    [[nodiscard]] const LiveCrossArbPair* find(const std::string& symbol) const {
        for (const auto& p : pairs_) if (p.symbol == symbol) return &p;
        return nullptr;
    }

    /// Both legs filled: the clearing corporation nets them. True once netted.
    bool settle(LiveEngine& e, const LiveCrossArbPair& p, const Held& h, std::int64_t now) {
        const std::uint32_t buy = h.dir > 0 ? p.bse : p.nse, sell = h.dir > 0 ? p.nse : p.bse;
        if (!e.book().net_off(name(), buy, sell, now, "the pair is netted by the clearing corporation")) return false;
        ++netted_today_;
        e.note_decision(name(), p.symbol + ": both legs filled; netted by the clearing corporation, no exit");
        // Anything left over (a leg filled more than the other) is unwound.
        if (e.book().position(name(), buy) != nullptr || e.book().position(name(), sell) != nullptr)
            unwind(e, p, now, "the part of one leg the other did not match");
        return true;
    }

    void act(LiveEngine& e, const LiveCrossArbPair& p) {
        const std::int64_t now = e.quote_ns();
        const LiveState* n = e.state(p.nse);
        const LiveState* b = e.state(p.bse);
        const LiveInstrument* ni = e.instrument(p.nse);
        const LiveInstrument* bi = e.instrument(p.bse);
        if (const auto it = held_.find(p.symbol); it != held_.end()) {
            if (settle(e, p, it->second, now)) held_.erase(it);
            return;
        }
        const auto q = live_cross_quote(n, b);
        if (!q.ok || ni == nullptr || bi == nullptr) return;
        const int minute = live_minute_of_day(live_ist_minute_index(now));
        if (minute >= r_.last_entry_minute || now - n->quote_ns > r_.quote_age_ns || now - b->quote_ns > r_.quote_age_ns) return;
        // A gap already taken is not taken again on the same quotes.
        if (const auto l = last_.find(p.symbol); l != last_.end()
            && (now - l->second.ns < r_.cooldown_ns || n->quote_ns <= l->second.nse_quote || b->quote_ns <= l->second.bse_quote))
            return;
        const int dir = q.sell_nse_bps >= q.sell_bse_bps ? +1 : -1;
        const double edge = dir > 0 ? q.sell_nse_bps : q.sell_bse_bps;
        const std::int64_t qty = qty_for(q, n, b, dir);
        if (qty < 1) return;
        const double need = cost_bps(e, p, q, dir, qty, now) + r_.min_profit_bps;
        if (edge < need) return;
        ++seen_today_;
        if (static_cast<int>(held_.size()) >= r_.max_open) return;
        const auto money = [](std::int64_t paise) { return live_fmt::num(static_cast<double>(paise) / 100.0); };
        const std::string why = p.symbol + (dir > 0 ? ": sell NSE at the bid " + money(q.nse_bid) + ", buy BSE at the ask " + money(q.bse_ask)
                                                    : ": sell BSE at the bid " + money(q.bse_bid) + ", buy NSE at the ask " + money(q.nse_ask))
                              + " x" + std::to_string(qty) + " (the lower size): edge " + live_fmt::num(edge, 1) + " bp over "
                              + live_fmt::num(need, 1) + " needed";
        std::string no;
        const bool a = e.book().open(name(), *bi, dir, qty, now, why, false, &no);
        const bool c = a && e.book().open(name(), *ni, -dir, qty, now, why, false, &no);
        if (!a || !c) {
            if (a) (void)e.book().close(name(), p.bse, now, "the other leg was refused: " + no);
            e.note_decision(name(), p.symbol + ": not entered (" + no + ")");
            return;
        }
        held_[p.symbol] = Held{dir, now};
        last_[p.symbol] = Last{now, n->quote_ns, b->quote_ns};
        ++entries_today_;
        e.note_decision(name(), why);
        // Filled at once (no latency): netted at once.
        if (settle(e, p, held_[p.symbol], now)) held_.erase(p.symbol);
    }

    void unwind(LiveEngine& e, const LiveCrossArbPair& p, std::int64_t now, const std::string& why) {
        (void)e.book().close(name(), p.nse, now, why);
        (void)e.book().close(name(), p.bse, now, why);
        e.note_decision(name(), p.symbol + ": out, " + why);
    }

    LiveCrossArbRule r_;
    std::vector<LiveCrossArbPair> pairs_;
    std::map<std::string, Held> held_;
    std::map<std::string, Last> last_;
    int seen_today_ = 0, entries_today_ = 0, netted_today_ = 0;
};

} // namespace altair::live
