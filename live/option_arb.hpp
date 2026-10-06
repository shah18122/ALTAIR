// live/option_arb.hpp -- option arbitrage: put-call parity and box spreads,
// timed by the order book's imbalance.
//
// TWO LOCKS, EACH HELD TO EXPIRY OR UNTIL THE GAP CLOSES.
//   * Parity, for strike K and the future F of the SAME expiry:
//       conversion: sell the call, buy the put, buy the future  -> locks K - F + (C - P)
//       reversal:   buy the call, sell the put, sell the future -> locks F - K - (C - P)
//   * Box, for two adjacent strikes K1 < K2 of one expiry (no future needed,
//     so it works on the weekly options the monthly future does not match):
//       long box:  buy C1, sell C2, buy P2, sell P1 -> receives K2 - K1 at expiry
//       short box: sell C1, buy C2, sell P2, buy P1 -> pays K2 - K1 at expiry
// Every amount is taken to expiry: premiums paid or received now are carried
// at the rate. Priced at the touch (bid to sell, ask to buy); entered only
// when what it locks beats every leg's expenses in and out, the spreads it
// would cross to unwind early, and a margin. Carried (a lock is a lock only
// held to expiry); unwound early, at the touch, once what is left to make is
// under the exit threshold.
//
// THE BOOK DECIDES THE ORDER AND THE GO. With up to fifty levels a side (FYERS
// TBT), each leg's imbalance I = (bid size - ask size) / total over the top
// `levels` says which way its touch is about to move:
//   * every leg is priced one tick worse when its book leans against it, and
//     the lock must still pay at those prices -- a mispricing the book says is
//     about to vanish is not chased;
//   * legs go in order of urgency: a BUY whose book leans to the bid (about
//     to tick up) first, a SELL whose book leans to the ask likewise.
// It decides on every quote of one of its legs.
//
// Paper by default. Real orders only while LIVE is armed AND its own switch
// (Auto: Option arb) is on, through the order router with its caps.

#pragma once

#include <live/engine.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace altair::live {

struct LiveOptionArbRule {
    double rate = 0.065;           ///< carry for the premium legs
    double margin_bp = 3.0;        ///< of the reference price (strike, box width), over costs, to enter
    double exit_bp = 0.5;          ///< unwind when what is left to make is under this
    int levels = 5;                ///< book levels the imbalance reads
    double lean = 0.3;             ///< |I| beyond which a book leans
    std::size_t max_open = 3;      ///< locks at once
    std::size_t box_strikes = 6;   ///< boxes on adjacent strikes this far either side of the money
    std::int64_t lots = 1;
    double tick = 0.05;            ///< rupees: one tick worse when a book leans
    double fallback_cost_bp = 6.0; ///< per fill, when the engine has no expense function
};

/// Order-book imbalance over the top `levels`: (bid size - ask size) / total;
/// 0 for an empty book. From the touch alone when there is no depth.
[[nodiscard]] inline double live_imbalance(const LiveTop& t, int levels) {
    double b = 0.0, a = 0.0;
    const int n = std::min<int>(levels, static_cast<int>(t.levels));
    for (int k = 0; k < n; ++k) {
        b += static_cast<double>(t.bids[k].qty);
        a += static_cast<double>(t.asks[k].qty);
    }
    if (n == 0) { b = static_cast<double>(t.bid_qty); a = static_cast<double>(t.ask_qty); }
    return b + a > 0.0 ? (b - a) / (b + a) : 0.0;
}

/// The microprice: the touch weighted toward the side about to be taken. Rupees.
[[nodiscard]] inline double live_microprice(const LiveTop& t) {
    if (t.bid <= 0 || t.ask <= 0) return 0.0;
    const double bq = static_cast<double>(t.bid_qty), aq = static_cast<double>(t.ask_qty);
    if (bq + aq <= 0.0) return static_cast<double>(t.bid + t.ask) / 200.0;
    return (static_cast<double>(t.ask) * bq + static_cast<double>(t.bid) * aq) / (bq + aq) / 100.0;
}

/// One leg of a lock: the instrument, its side in the lock's +1 direction,
/// and what one rupee of it now is worth at expiry (e^{rT} for a premium, 1
/// for the future, which settles at expiry by itself).
struct LiveArbLeg {
    const LiveInstrument* in = nullptr;
    int side = 0;
    double weight = 1.0;
};

/// What a lock locks now, per unit, in rupees at expiry: positive pays.
struct LiveLockQuote {
    bool ok = false;
    int dir = 0;              ///< +1: the legs as written; -1: every side reversed
    double edge = 0.0;        ///< at the touch
    double edge_lean = 0.0;   ///< with leaning legs a tick worse
    double costs = 0.0;       ///< expenses in and out, and the spreads to unwind early
};

/// Price a lock whose +1 direction settles `fixed` at expiry (K for a
/// conversion, K2 - K1 for a long box); the -1 direction settles -fixed.
[[nodiscard]] inline LiveLockQuote live_lock_quote(const std::vector<LiveArbLeg>& legs, const std::vector<LiveTop>& tops,
                                                   double fixed, const LiveOptionArbRule& r, double cost_per_fill) {
    LiveLockQuote q;
    if (legs.size() != tops.size() || legs.empty()) return q;
    const auto px = [](std::int64_t v) { return static_cast<double>(v) / 100.0; };
    double plus = fixed, minus = -fixed, plus_l = fixed, minus_l = -fixed, spreads = 0.0;
    for (std::size_t k = 0; k < legs.size(); ++k) {
        const LiveTop& t = tops[k];
        if (t.bid <= 0 || t.ask <= 0 || t.ask < t.bid) return q;
        const double bid = px(t.bid), ask = px(t.ask), i = live_imbalance(t, r.levels), w = legs[k].weight;
        const double ask_l = ask + (i > r.lean ? r.tick : 0.0), bid_l = bid - (i < -r.lean ? r.tick : 0.0);
        spreads += ask - bid;
        // Buying pays the ask; selling receives the bid.
        if (legs[k].side > 0) { plus -= ask * w; plus_l -= ask_l * w; minus += bid * w; minus_l += bid_l * w; }
        else { plus += bid * w; plus_l += bid_l * w; minus -= ask * w; minus_l -= ask_l * w; }
    }
    q.ok = true;
    q.costs = 2.0 * static_cast<double>(legs.size()) * cost_per_fill + spreads;
    if (plus >= minus) { q.dir = 1; q.edge = plus; q.edge_lean = plus_l; }
    else { q.dir = -1; q.edge = minus; q.edge_lean = minus_l; }
    return q;
}

class LiveOptionArbModel final : public LiveModel {
public:
    explicit LiveOptionArbModel(LiveOptionArbRule r = {}) : r_(r) {}

    [[nodiscard]] std::string name() const override { return "Option arbitrage"; }
    [[nodiscard]] std::string family() const override { return "arbitrage"; }
    [[nodiscard]] bool carry() const override { return true; }

    void on_new_day(LiveEngine&) override { locks_.clear(); by_token_.clear(); owner_.clear(); built_for_ = 0; note_.clear(); }
    void on_minute(LiveEngine& e, int) override { build(e); }

    void on_quote(LiveEngine& e, std::uint32_t token) override {
        if (built_for_ != e.universe().size()) build(e);
        const auto it = by_token_.find(token);
        if (it == by_token_.end()) return;
        for (const std::size_t k : it->second) evaluate(e, k);
    }

    [[nodiscard]] LiveModelView view(const LiveEngine& e) const override {
        LiveModelView v;
        v.name = name();
        v.family = family();
        std::size_t held = 0;
        for (const auto& p : e.book().positions()) held += p.model == name() ? 1u : 0u;
        v.state = held > 0 ? "in position" : "watching";
        v.reason = !note_.empty() ? note_
                 : "Parity (with the future of the same expiry) and box spreads on every quote; legs priced a tick worse "
                   "when their book leans against them (top " + std::to_string(r_.levels) + " levels), sent in order of urgency.";
        std::size_t parity = 0, box = 0;
        for (const auto& l : locks_) (l.box ? box : parity) += 1;
        v.signal = std::to_string(parity) + " parity, " + std::to_string(box) + " box watched";
        std::vector<std::pair<double, std::string>> best;
        for (const auto& l : locks_)
            if (l.last.ok) best.push_back({l.last.edge - l.last.costs, l.label + (l.last.dir > 0 ? l.plus_name : l.minus_name)});
        std::sort(best.begin(), best.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (std::size_t k = 0; k < best.size() && k < 5; ++k)
            v.fields.push_back({best[k].second, "net " + live_fmt::num(best[k].first) + " per unit"});
        return v;
    }

    /// A lock the model watches.
    struct Lock {
        bool box = false;
        std::vector<LiveArbLeg> legs;
        double fixed = 0.0;        ///< settles to this at expiry, +1 direction
        std::int64_t expiry = 0;
        double ref = 0.0;          ///< for the margin and the exit threshold
        std::string label, plus_name, minus_name;
        LiveLockQuote last;
    };
    [[nodiscard]] const std::vector<Lock>& locks() const noexcept { return locks_; }

private:
    void build(LiveEngine& e) {
        if (built_for_ == e.universe().size()) return;
        built_for_ = e.universe().size();
        locks_.clear();
        by_token_.clear();
        owner_.clear();
        // Calls and puts by (underlying, expiry, strike).
        std::map<std::tuple<std::string, std::int64_t, double>, std::pair<const LiveInstrument*, const LiveInstrument*>> m;
        for (const auto& i : e.universe()) {
            if ((i.kind != LiveKind::Call && i.kind != LiveKind::Put) || i.expiry_day < e.today()) continue;
            auto& cp = m[{i.underlying, i.expiry_day, i.strike}];
            (i.kind == LiveKind::Call ? cp.first : cp.second) = &i;
        }
        // Parity: a strike with its call, its put and the same-expiry future.
        for (const auto& [key, cp] : m) {
            if (cp.first == nullptr || cp.second == nullptr) continue;
            const LiveInstrument* fut = nullptr;
            for (const auto& i : e.universe())
                if (i.kind == LiveKind::Future && i.underlying == std::get<0>(key) && i.expiry_day == std::get<1>(key)) fut = &i;
            if (fut == nullptr) continue;
            Lock l;
            l.legs = {{cp.first, -1, 1.0}, {cp.second, 1, 1.0}, {fut, 1, 1.0}};   // conversion: sell C, buy P, buy F
            l.fixed = std::get<2>(key);
            l.expiry = std::get<1>(key);
            l.ref = std::get<2>(key);
            l.label = std::get<0>(key) + " " + live_fmt::num(std::get<2>(key), 0);
            l.plus_name = " conversion";
            l.minus_name = " reversal";
            locks_.push_back(std::move(l));
        }
        // Boxes: adjacent strikes with both sides, nearest the money.
        std::map<std::pair<std::string, std::int64_t>, std::vector<std::tuple<double, const LiveInstrument*, const LiveInstrument*>>> chains;
        for (const auto& [key, cp] : m)
            if (cp.first != nullptr && cp.second != nullptr)
                chains[{std::get<0>(key), std::get<1>(key)}].emplace_back(std::get<2>(key), cp.first, cp.second);
        for (auto& [ue, ks] : chains) {
            std::sort(ks.begin(), ks.end(), [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
            const double level = e.underlying_level(ue.first);
            std::size_t atm = ks.size() / 2;
            if (level > 0.0)
                for (std::size_t k = 0; k < ks.size(); ++k)
                    if (std::fabs(std::get<0>(ks[k]) - level) < std::fabs(std::get<0>(ks[atm]) - level)) atm = k;
            const std::size_t lo = atm > r_.box_strikes ? atm - r_.box_strikes : 0;
            const std::size_t hi = std::min(ks.size(), atm + r_.box_strikes + 1);
            for (std::size_t k = lo; k + 1 < hi; ++k) {
                const auto& [k1, c1, p1] = ks[k];
                const auto& [k2, c2, p2] = ks[k + 1];
                Lock l;
                l.box = true;
                l.legs = {{c1, 1, 1.0}, {c2, -1, 1.0}, {p2, 1, 1.0}, {p1, -1, 1.0}};   // long box
                l.fixed = k2 - k1;
                l.expiry = ue.second;
                l.ref = k2;
                l.label = ue.first + " " + live_fmt::num(k1, 0) + "/" + live_fmt::num(k2, 0) + " box";
                l.plus_name = " (long)";
                l.minus_name = " (short)";
                locks_.push_back(std::move(l));
            }
        }
        for (std::size_t k = 0; k < locks_.size(); ++k)
            for (const auto& leg : locks_[k].legs) by_token_[leg.in->token].push_back(k);
        note_ = locks_.empty() ? "No strike streams both sides: nothing to watch yet." : std::string();
    }

    void evaluate(LiveEngine& e, std::size_t k) {
        Lock& l = locks_[k];
        const std::int64_t ns = e.quote_ns();
        tops_.clear();
        for (const auto& leg : l.legs) {
            tops_.push_back(e.top(leg.in->token));
            if (!e.book().fresh(tops_.back().quote_ns, ns)) return;
        }
        const double years = static_cast<double>((l.expiry * 86400 + 36000) * 1'000'000'000LL - ns) / (365.0 * 86400.0 * 1e9);
        if (!(years > 0.0)) return;
        const double g = std::exp(r_.rate * years);
        for (auto& leg : l.legs) leg.weight = leg.in->kind == LiveKind::Future ? 1.0 : g;
        const LiveTop& rt = tops_.back();
        const double ref_px = std::max(1.0, static_cast<double>(rt.bid + rt.ask) / 200.0);
        double fill_cost = e.book().expense(*l.legs.back().in, true, 1.0, ref_px, ns);
        if (!std::isfinite(fill_cost)) fill_cost = std::max(ref_px, l.ref) * r_.fallback_cost_bp / 1e4;
        l.last = live_lock_quote(l.legs, tops_, l.fixed, r_, fill_cost);
        if (!l.last.ok) return;

        // Held? A lock owns its legs; one position per (model, instrument).
        bool any = false, all = true;
        for (const auto& leg : l.legs) {
            const bool h = e.book().position(name(), leg.in->token) != nullptr;
            any = any || h;
            all = all && h;
        }
        if (!any) owner_.erase(k);
        if (any) {
            if (owner_.count(k) == 0) {
                // Held by another lock that shares a strike, or resumed: adopt a complete set only.
                if (!all) return;
                for (const auto& [other, d] : owner_)
                    for (const auto& a : locks_[other].legs)
                        for (const auto& b : l.legs)
                            if (a.in->token == b.in->token) return;
                const LivePosition* p0 = e.book().position(name(), l.legs.front().in->token);
                owner_[k] = p0 != nullptr && p0->side == l.legs.front().side ? 1 : -1;
            }
            for (const auto& leg : l.legs) {
                const LivePosition* p = e.book().position(name(), leg.in->token);
                if (p == nullptr || !p->filled() || p->state == LivePosState::Closing) return;
            }
            // What the held direction would still lock now.
            const double left = owner_[k] == l.last.dir ? l.last.edge : -l.last.edge;
            if (left < l.ref * r_.exit_bp / 1e4) {
                const std::string why = l.label + ": the gap closed (" + live_fmt::num(left) + " per unit left)";
                for (const auto& leg : l.legs) (void)e.book().close(name(), leg.in->token, ns, why);
                e.note_decision(name(), why);
            }
            return;
        }
        if (owner_.size() >= r_.max_open || e.stale()) return;
        const double need = l.last.costs + l.ref * r_.margin_bp / 1e4;
        if (l.last.edge <= need || l.last.edge_lean <= need) return;
        // Legs in order of urgency.
        const int d = l.last.dir;
        order_.clear();
        for (std::size_t i = 0; i < l.legs.size(); ++i) {
            const double imb = live_imbalance(tops_[i], r_.levels);
            order_.push_back({l.legs[i].side * d > 0 ? imb : -imb, i});
        }
        std::sort(order_.begin(), order_.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        const std::string reason = l.label + (d > 0 ? l.plus_name : l.minus_name) + ": locks " + live_fmt::num(l.last.edge)
                                 + " per unit against " + live_fmt::num(need) + " of costs and margin";
        for (const auto& [urgency, i] : order_) {
            std::string why;
            if (!e.book().open(name(), *l.legs[i].in, l.legs[i].side * d, r_.lots, ns, reason, true, &why)) {
                for (const auto& leg : l.legs) (void)e.book().close(name(), leg.in->token, ns, "leg refused: " + why);
                note_ = l.label + ": a leg was refused (" + why + "); the others were cancelled.";
                e.note_decision(name(), note_);
                return;
            }
        }
        owner_[k] = d;
        note_ = reason + ".";
        e.note_decision(name(), note_);
    }

    LiveOptionArbRule r_;
    std::vector<Lock> locks_;
    std::map<std::uint32_t, std::vector<std::size_t>> by_token_;
    std::map<std::size_t, int> owner_;   ///< lock -> the direction it holds
    std::size_t built_for_ = 0;
    std::string note_;
    std::vector<LiveTop> tops_;                         ///< reused: no allocation per quote
    std::vector<std::pair<double, std::size_t>> order_;
};

} // namespace altair::live
