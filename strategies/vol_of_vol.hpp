// strategies/vol_of_vol.hpp -- sell vol-of-vol: short the wings, long the
// money, vega-neutral, delta-hedged.
//
// THE IDEA. Vega-neutral is not vol-neutral: an option's vega itself moves
// with vol (volga, analytics/greeks2.hpp), and the 25-delta wings carry far
// more volga per unit of vega than the at-the-money straddle. The market
// charges for that -- the smile's wings sit above the ATM vol, which is the
// vanna-volga price of volga (analytics/vanna_volga.hpp) -- and the
// literature finds the charge too high on average: vol-of-vol risk carries a
// negative premium (Huang, Schlag, Shaliastovich & Thimme 2018; Park 2015 on
// VVIX). So the trade: SELL the 25-delta strangle, BUY enough ATM straddles
// to cancel its vega, hedge the delta with whole futures lots at every close.
// What is left is short volga (and a little vanna): it earns when vol stays
// put and loses when vol jumps -- a premium for insurance against
// vol-of-vol, not a free lunch.
//
// EACH CLOSE, when flat (and the gate allows): the first monthly expiry at
// least `min_sessions` away; its smile from the traded OTM closes (put below
// the forward, call above); the vanna-volga pillars (25-delta put, ATM,
// 25-delta call). Short `strangle_lots` of the strangle, long the straddle
// lots that make the vega nearest zero. Held `hold_sessions` closes (or
// until `exit_sessions` remain), re-hedged at every close, then closed and
// opened afresh. `side` = +1 is the mirror (long vol-of-vol), the sign check.
//
// THE GATE. "always" trades every week; "rich wings" only when the 25-delta
// butterfly -- (vol_put + vol_call)/2 - vol_atm, from the pillars -- is above
// its median over the previous `lookback` closes: the wings dear against
// their own recent history. Everything is decided at a close from that close
// and earlier; P&L is marked on later closes only.
//
// PRICES are the caller's: NSE bhavcopy closes in the app (traded strikes
// only; a leg with no trade that day keeps its last mark and cannot be
// closed until it trades, or it settles at expiry). Wings trade thinner than
// the money, so their closes can be stale: the results say so.

#pragma once

#include <analytics/greeks.hpp>
#include <analytics/iv.hpp>
#include <analytics/vanna_volga.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <vector>

namespace altair::vol_of_vol {

/// One traded strike of a chain at a close: per-unit closes, NaN untraded.
struct VovStrike {
    double k = 0.0;
    double call = std::numeric_limits<double>::quiet_NaN();
    double put = std::numeric_limits<double>::quiet_NaN();
};

struct VovMarket {
    std::vector<std::int64_t> day;   ///< trading days (days since 1970-01-01), ascending
    std::set<std::int64_t> expiries; ///< monthly expiry days
    /// The same-expiry future's close at day i; NaN where none.
    std::function<double(std::size_t, std::int64_t)> forward;
    /// The traded strikes of that expiry at day i (any order).
    std::function<std::vector<VovStrike>(std::size_t, std::int64_t)> chain;
};

enum class VovGate : std::uint8_t { Always, RichWings };

struct VovPolicy {
    double lot_size = 0.0;           ///< from the spec store, never a literal
    int strangle_lots = 5;           ///< the short strangle; the straddle is sized against it
    int side = -1;                   ///< -1 sell vol-of-vol (the trade), +1 buy it (the mirror)
    VovGate gate = VovGate::Always;
    std::size_t lookback = 60;       ///< closes of butterfly history the gate's median uses
    std::size_t min_history = 20;    ///< fewer butterflies than this: the gate stays shut
    double rate = 0.065;
    std::size_t min_sessions = 10;   ///< to expiry, at entry
    std::size_t exit_sessions = 2;   ///< close once this many or fewer remain
    std::size_t hold_sessions = 5;   ///< close after this many closes, then trade afresh
    double slippage_pts = 1.0;       ///< premium points given up per option fill
    bool hedge = true;
};

/// One fill: an option leg (call/put at k) or the future (k = 0).
struct VovFill {
    std::size_t i = 0;
    bool future = false, call = false;
    double k = 0.0;
    double units = 0.0;              ///< signed: + buys
    double price = 0.0;              ///< per unit, after slippage
};

struct VovTrade {
    std::size_t entry = 0, exit = 0;
    std::int64_t expiry = 0;
    double k_put = 0, k_atm = 0, k_call = 0;
    double vol_put = 0, vol_atm = 0, vol_call = 0;   ///< pillars' market vols at entry
    double butterfly = 0;                            ///< (vol_put + vol_call)/2 - vol_atm at entry
    double strangle_units = 0, straddle_units = 0;   ///< signed (short negative)
    double vega_left = 0;                            ///< rupees per vol point left after sizing
    double volga = 0;                                ///< rupees per vol point squared, the book's at entry
    double option_pnl = 0, hedge_pnl = 0, gross = 0;
    std::vector<VovFill> fills;
    const char* exit_reason = "hold";
};

struct VovResult {
    std::vector<VovTrade> trades;
    std::size_t priced_days = 0;     ///< closes with pillars
    std::size_t gated = 0;           ///< closes the gate kept flat
};

enum class VovError : std::uint8_t { BadPolicy, BadMarket };

[[nodiscard]] inline const char* error_text(VovError e) noexcept {
    return e == VovError::BadPolicy ? "lot size, lots, side and sessions are inconsistent"
                                 : "the market needs days, expiries, forwards and chains";
}

namespace detail {

inline constexpr double kVovYearDays = 365.0;

[[nodiscard]] inline double nan() noexcept { return std::numeric_limits<double>::quiet_NaN(); }

[[nodiscard]] inline std::size_t sessions_left(const VovMarket& m, std::size_t i, std::int64_t expiry) noexcept {
    std::size_t n = 0;
    for (std::size_t k = i + 1; k < m.day.size() && m.day[k] <= expiry; ++k) ++n;
    if (!m.day.empty() && m.day.back() < expiry)
        for (std::int64_t d = m.day.back() + 1; d <= expiry; ++d)
            if (((d + 3) % 7 + 7) % 7 < 5) ++n;
    return n;
}

[[nodiscard]] inline double iv_of(bool call, double px, double F, double K, double T, double r) {
    if (!(px > 0) || !(F > 0) || !(T > 0)) return nan();
    const auto paise = [](double rupees) { return Price{static_cast<std::int64_t>(std::llround(rupees * 100.0))}; };
    const auto iv = implied_vol_black76(call ? OptionRight::Call : OptionRight::Put, paise(px), paise(F), paise(K), Years{T}, r);
    return iv ? iv->vol.raw() : nan();
}

struct VovGreeks { double price = 0, delta = 0, vega = 0, volga = 0; };
[[nodiscard]] inline VovGreeks greeks(bool call, double F, double K, double T, double s, double r) {
    const auto g = altair::detail::black76_unchecked(call ? OptionRight::Call : OptionRight::Put, F, K, T, s, r);
    const double d1 = (std::log(F / K) + 0.5 * s * s * T) / (s * std::sqrt(T));
    const double d2 = d1 - s * std::sqrt(T);
    // dV/dF carried to the future's own move; vega per vol point; volga per vol point^2.
    return {g.price, g.delta * std::exp(r * T), g.vega / 100.0, g.vega * d1 * d2 / s / 10'000.0};
}

/// The chain's OTM smile at a close.
[[nodiscard]] inline std::vector<vanna_volga::SmilePoint> smile(const std::vector<VovStrike>& ch, double F, double T, double r) {
    std::vector<vanna_volga::SmilePoint> out;
    for (const auto& s : ch) {
        const bool call = s.k >= F;
        const double v = iv_of(call, call ? s.call : s.put, F, s.k, T, r);
        if (std::isfinite(v) && v > 0.01 && v < 2.0) out.push_back({s.k, v});
    }
    return out;
}

[[nodiscard]] inline const VovStrike* find(const std::vector<VovStrike>& ch, double k) {
    for (const auto& s : ch) if (s.k == k) return &s;
    return nullptr;
}

} // namespace detail

[[nodiscard]] inline std::expected<VovResult, VovError> run(const VovMarket& m, const VovPolicy& p) {
    if (!(p.lot_size > 0) || p.strangle_lots < 1 || (p.side != -1 && p.side != 1) || p.min_sessions == 0
        || p.exit_sessions >= p.min_sessions || p.hold_sessions == 0 || !(p.slippage_pts >= 0) || !std::isfinite(p.rate))
        return std::unexpected(VovError::BadPolicy);
    if (m.day.empty() || m.expiries.empty() || !m.forward || !m.chain) return std::unexpected(VovError::BadMarket);
    using namespace detail;
    VovResult out;
    std::vector<double> fly_hist;   // butterflies of earlier closes (the gate's history)
    bool open = false;
    VovTrade t;
    struct Leg { bool call; double k; double units; double mark; double iv; };
    std::vector<Leg> legs;
    double hedge_units = 0.0, last_fwd = 0.0;
    std::size_t held = 0;

    const auto leg_price = [&](const std::vector<VovStrike>& ch, const Leg& l) {
        const VovStrike* s = find(ch, l.k);
        return s == nullptr ? nan() : (l.call ? s->call : s->put);
    };
    const auto rehedge = [&](std::size_t i, double F, double T) {
        if (!p.hedge) return;
        double delta = 0.0;
        for (const auto& l : legs) delta += l.units * greeks(l.call, F, l.k, T, l.iv, p.rate).delta;
        const double want = -std::round(delta / p.lot_size) * p.lot_size;
        if (want != hedge_units) {
            t.fills.push_back({i, true, false, 0.0, want - hedge_units, F});
            hedge_units = want;
        }
    };

    for (std::size_t i = 0; i < m.day.size(); ++i) {
        const std::int64_t today = m.day[i];
        // This close's smile on the front eligible expiry, and its butterfly.
        // The gate compares it with the butterflies of EARLIER closes only.
        std::int64_t front = 0;
        for (const auto e : m.expiries)
            if (e > today && sessions_left(m, i, e) >= p.min_sessions) { front = e; break; }
        const double Ff = front != 0 ? m.forward(i, front) : nan();
        const double Tf = front != 0 ? static_cast<double>(front - today) / kVovYearDays : 0.0;
        std::vector<VovStrike> chf;
        std::optional<vanna_volga::Pillars> pil;
        if (front != 0 && std::isfinite(Ff) && Ff > 0 && Tf > 0) {
            chf = m.chain(i, front);
            pil = vanna_volga::pillars(Ff, Tf, smile(chf, Ff, Tf, p.rate));
        }
        const double fly = pil ? 0.5 * (pil->vol[0] + pil->vol[2]) - pil->vol[1] : nan();
        bool rich = false;
        {
            const std::size_t n = std::min(fly_hist.size(), p.lookback);
            if (pil && n >= p.min_history) {
                std::vector<double> recent(fly_hist.end() - static_cast<std::ptrdiff_t>(n), fly_hist.end());
                std::nth_element(recent.begin(), recent.begin() + static_cast<std::ptrdiff_t>(n / 2), recent.end());
                rich = fly > recent[n / 2];
            }
        }
        if (pil) { ++out.priced_days; fly_hist.push_back(fly); }

        if (open) {
            const double F = m.forward(i, t.expiry);
            const double T = static_cast<double>(t.expiry - today) / kVovYearDays;
            if (std::isfinite(F) && F > 0) {
                t.hedge_pnl += hedge_units * (F - last_fwd);
                last_fwd = F;
            }
            ++held;
            const auto ch = t.expiry == front ? chf : m.chain(i, t.expiry);
            bool all_quoted = true;
            for (auto& l : legs) {
                const double px = leg_price(ch, l);
                if (std::isfinite(px) && px > 0) {
                    l.mark = px;
                    const double v = iv_of(l.call, px, last_fwd, l.k, T, p.rate);
                    if (std::isfinite(v)) l.iv = v;
                } else {
                    all_quoted = false;
                }
            }
            const std::size_t left = sessions_left(m, i, t.expiry);
            const bool expired = today >= t.expiry;
            const bool due = held >= p.hold_sessions || left <= p.exit_sessions || expired;
            if (due && (all_quoted || expired)) {
                for (auto& l : legs) {
                    // At expiry with no trade: settled at intrinsic against the future.
                    const double px = all_quoted ? l.mark : std::max(0.0, l.call ? last_fwd - l.k : l.k - last_fwd);
                    const double fill = l.units > 0 ? std::max(0.0, px - p.slippage_pts) : px + p.slippage_pts;
                    t.fills.push_back({i, false, l.call, l.k, -l.units, fill});
                    t.option_pnl += l.units * fill;   // the closing trade is -units: its cash is +units x fill
                }
                if (hedge_units != 0.0) t.fills.push_back({i, true, false, 0.0, -hedge_units, last_fwd});
                t.exit = i;
                t.exit_reason = expired ? "expiry" : left <= p.exit_sessions ? "near expiry" : "hold";
                t.gross = t.option_pnl + t.hedge_pnl;
                out.trades.push_back(t);
                open = false;
                legs.clear();
                hedge_units = 0.0;
            } else if (!expired && T > 0) {
                rehedge(i, last_fwd, T);
            }
            if (open) continue;
        }

        // Flat: open on this close's front expiry when the gate allows.
        if (!pil) continue;
        if (p.gate == VovGate::RichWings && !rich) { ++out.gated; continue; }
        const double F = Ff, T = Tf;
        const VovStrike* sp = find(chf, pil->k[0]);
        const VovStrike* sa = find(chf, pil->k[1]);
        const VovStrike* sc = find(chf, pil->k[2]);
        if (sp == nullptr || sa == nullptr || sc == nullptr || !(sp->put > 0) || !(sc->call > 0) || !(sa->call > 0) || !(sa->put > 0))
            continue;
        // Size: the strangle, and the straddle lots that cancel its vega.
        const double s_units = static_cast<double>(p.strangle_lots) * p.lot_size;
        const auto gp = greeks(false, F, pil->k[0], T, pil->vol[0], p.rate);
        const auto gc = greeks(true, F, pil->k[2], T, pil->vol[2], p.rate);
        const auto ga_c = greeks(true, F, pil->k[1], T, pil->vol[1], p.rate);
        const auto ga_p = greeks(false, F, pil->k[1], T, pil->vol[1], p.rate);
        const double strangle_vega = gp.vega + gc.vega, straddle_vega = ga_c.vega + ga_p.vega;
        if (!(straddle_vega > 0)) continue;
        const double st_lots = std::max(1.0, std::round(s_units * strangle_vega / straddle_vega / p.lot_size));
        const double a_units = st_lots * p.lot_size;
        const double sgn = static_cast<double>(p.side);   // -1: short the strangle, long the straddle
        t = VovTrade{};
        t.entry = i;
        t.expiry = front;
        t.k_put = pil->k[0]; t.k_atm = pil->k[1]; t.k_call = pil->k[2];
        t.vol_put = pil->vol[0]; t.vol_atm = pil->vol[1]; t.vol_call = pil->vol[2];
        t.butterfly = fly;
        t.strangle_units = sgn * s_units;
        t.straddle_units = -sgn * a_units;
        t.vega_left = t.strangle_units * strangle_vega + t.straddle_units * straddle_vega;
        t.volga = t.strangle_units * (gp.volga + gc.volga) + t.straddle_units * (ga_c.volga + ga_p.volga);
        legs = {{false, pil->k[0], t.strangle_units, sp->put, pil->vol[0]},
                {true, pil->k[2], t.strangle_units, sc->call, pil->vol[2]},
                {true, pil->k[1], t.straddle_units, sa->call, pil->vol[1]},
                {false, pil->k[1], t.straddle_units, sa->put, pil->vol[1]}};
        for (const auto& l : legs) {
            const double fill = l.units > 0 ? l.mark + p.slippage_pts : std::max(0.0, l.mark - p.slippage_pts);
            t.fills.push_back({i, false, l.call, l.k, l.units, fill});
            t.option_pnl -= l.units * fill;
        }
        open = true;
        held = 0;
        last_fwd = F;
        hedge_units = 0.0;
        rehedge(i, F, T);
    }
    return out;
}

} // namespace altair::vol_of_vol
