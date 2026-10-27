// strategies/as_market_maker.hpp -- Avellaneda-Stoikov quoting on index
// futures, simulated on one-minute bars, on paper.
//
// WHY FUTURES. The quotes are two-sided: a bid is a buy. Smit's rule is never
// to buy an OPTION; a futures bid is allowed, and the book is flat by 15:20 so
// nothing is carried. On options only analytics/avellaneda_stoikov.hpp's
// sell-only ask is used (the least premium worth selling at); this file never
// quotes an option.
//
// EACH BAR, IN ORDER.
//   1. The quotes posted at the last bar's close were live for this bar. The
//      ask fills when the bar's high trades THROUGH it by a tick, the bid when
//      the low does: touching a price says nothing about a resting order's
//      place in the queue, trading through it does. Fills are at the quote.
//      Both can fill in one bar.
//   2. Sigma (EWMA of squared one-minute changes, points) and the excursion
//      sample for k (high - previous close and previous close - low) learn
//      this bar. Both use only bars up to this one; the first bar of a
//      session teaches neither, because its change is the overnight gap.
//   3. New quotes for the next bar from analytics/avellaneda_stoikov.hpp:
//      tau = minutes to the 15:20 flatten, gamma times (1 + yesterday's HMM
//      P(stress)) when a regime series is given. The bid is not quoted at the
//      inventory cap, the ask not at minus the cap. Post-only: a bid is kept
//      at least a tick under the close and an ask a tick over, because a quote
//      through the market is a market order and would not fill at its price.
//   4. At 15:20 (or the session's last bar, if earlier) whatever is held is
//      closed at the close, two ticks worse, and that day is done.
//
// Every fill carries its reason: the mid, the reservation price and why it
// sits where it does, the half spread's two parts, sigma, k and tau.
//
// LIMITS, SAID. Bars are not the book: queue position, partial fills and the
// real touch are unknown, and the trade-through rule is the conservative
// stand-in. Fill markouts (mid five bars later against the fill) are written
// so adverse selection is visible rather than assumed away; they are for the
// report only and never feed a decision.

#pragma once

#include <analytics/avellaneda_stoikov.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace altair {

/// One bar. `t` is the bar's START in IST seconds since the epoch (the
/// convention of app/data_audit.hpp's AuditBar), so t / 86400 is the IST day.
struct AsBar {
    std::int64_t t = 0;
    double o = 0.0, h = 0.0, l = 0.0, c = 0.0;
};

struct AsMmParams {
    double gamma = 1e-4;            ///< risk aversion, per point per lot
    int max_lots = 5;               ///< inventory cap either way
    double tick = 0.05;             ///< trade-through needed, and quote rounding
    double cost_bp_per_fill = 2.0;  ///< every expense of one fill, bp of notional
    double flatten_ticks = 2.0;     ///< the 15:20 close pays this many ticks
    double lot_units = 65.0;        ///< rupees per point per lot (NIFTY 65, BANKNIFTY 30)
    int first_quote_min = 9 * 60 + 20;   ///< no quotes before (minute of day, bar start)
    int last_quote_min = 15 * 60 + 15;   ///< no new quotes from this bar on
    int flatten_min = 15 * 60 + 20;      ///< flat at this bar's close
    double sigma_half_life = 30.0;  ///< bars
    std::size_t k_window = 1875;    ///< excursions kept (about five sessions, both sides)
    std::size_t k_min = 200;        ///< no quotes until this many positive excursions
    std::size_t sigma_min = 30;     ///< no quotes until this many changes seen
    std::size_t markout_bars = 5;
};

struct AsFill {
    std::int64_t t = 0;        ///< the bar it filled in
    int side = 0;              ///< +1 bought, -1 sold
    double price = 0.0;
    int q_before = 0, q_after = 0;
    double mid = 0.0, reservation = 0.0, spread = 0.0, sigma = 0.0, k = 0.0, tau = 0.0;
    double gamma = 0.0, p_stress = 0.0;
    double cost_rs = 0.0;
    double markout_pts = std::numeric_limits<double>::quiet_NaN();   ///< report only
    bool flatten = false;
    std::string reason;
};

/// The quotes decided at one bar's close, for the next bar.
struct AsQuoteRow {
    std::int64_t t = 0;
    bool bid_on = false, ask_on = false;
    double bid = 0.0, ask = 0.0;
    int q = 0;
};

struct AsDay {
    std::int64_t day = 0;
    std::size_t fills = 0;
    double gross_rs = 0.0, cost_rs = 0.0, net_rs = 0.0;
    int max_abs_q = 0;
};

struct AsResult {
    std::vector<AsFill> fills;
    std::vector<AsQuoteRow> quotes;
    std::vector<AsDay> days;
};

namespace as_detail {

inline constexpr std::int64_t kAsDaySec = 86'400;

[[nodiscard]] constexpr std::int64_t day_of(std::int64_t t) noexcept {
    return (t >= 0 ? t : t - (kAsDaySec - 1)) / kAsDaySec;
}
[[nodiscard]] constexpr int minute_of(std::int64_t t) noexcept {
    return static_cast<int>((t - day_of(t) * kAsDaySec) / 60);
}
[[nodiscard]] inline std::string hhmm(int m) {
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d", m / 60, m % 60);
    return b;
}

} // namespace as_detail

/// Run the quoter over `bars` (ascending). `p_stress_by_day` maps an IST day
/// to the HMM's P(stress) as of that day's close; a day uses the latest entry
/// strictly BEFORE it, so nothing from the session being traded is read.
[[nodiscard]] inline AsResult run_as_market_maker(const std::vector<AsBar>& bars, const AsMmParams& p,
                                                  const std::map<std::int64_t, double>* p_stress_by_day = nullptr) {
    namespace d = as_detail;
    AsResult out;
    const double alpha = 1.0 - std::exp(std::log(0.5) / std::max(1.0, p.sigma_half_life));
    double var = 0.0;
    std::size_t var_n = 0;
    std::deque<double> exc;
    double exc_sum = 0.0;
    std::size_t exc_pos = 0;   // positive entries in `exc`

    std::int64_t day = -1;
    int q = 0;
    double cash = 0.0;   // points x lots, this session
    bool done_today = false;
    AsQuoteRow live{};
    bool live_valid = false;
    double day_gamma = p.gamma, day_p = 0.0;
    AsDay cur{};

    const auto close_day = [&]() {
        if (day < 0) { return; }
        cur.gross_rs = cash * p.lot_units;
        cur.net_rs = cur.gross_rs - cur.cost_rs;
        out.days.push_back(cur);
    };
    const auto fill_cost = [&](double price) { return std::fabs(price) * p.lot_units * p.cost_bp_per_fill / 1e4; };
    const auto push_exc = [&](double x) {
        exc.push_back(x);
        if (x > 0.0) { exc_sum += x; ++exc_pos; }
        while (exc.size() > p.k_window) {
            const double y = exc.front();
            exc.pop_front();
            if (y > 0.0) { exc_sum -= y; --exc_pos; }
        }
    };
    // The quote that produced a fill, kept for its reason.
    AsQuote live_model{};
    double live_mid = 0.0, live_sigma = 0.0, live_k = 0.0, live_tau = 0.0;

    for (std::size_t i = 0; i < bars.size(); ++i) {
        const AsBar& b = bars[i];
        const std::int64_t bd = d::day_of(b.t);
        const int m = d::minute_of(b.t);
        const bool new_day = bd != day;
        if (new_day) {
            // A session that ended in the data without its 15:20 bar is flat
            // already: the last bar of each day flattens (below).
            close_day();
            day = bd;
            cur = AsDay{};
            cur.day = bd;
            q = 0;
            cash = 0.0;
            done_today = false;
            live_valid = false;
            day_gamma = p.gamma;
            day_p = 0.0;
            if (p_stress_by_day != nullptr && !p_stress_by_day->empty()) {
                auto it = p_stress_by_day->lower_bound(bd);   // first >= today
                if (it != p_stress_by_day->begin()) {
                    --it;
                    if (std::isfinite(it->second)) {
                        day_p = std::clamp(it->second, 0.0, 1.0);
                        day_gamma = p.gamma * (1.0 + day_p);
                    }
                }
            }
        }

        // ---- 1. fills against the quotes live for this bar -----------------------
        if (live_valid && !done_today) {
            const auto fill = [&](int side, double price) {
                AsFill f;
                f.t = b.t;
                f.side = side;
                f.price = price;
                f.q_before = q;
                q += side;
                f.q_after = q;
                f.mid = live_mid;
                f.reservation = live_model.reservation;
                f.spread = live_model.spread;
                f.sigma = live_sigma;
                f.k = live_k;
                f.tau = live_tau;
                f.gamma = day_gamma;
                f.p_stress = day_p;
                f.cost_rs = fill_cost(price);
                cash -= side * price;
                cur.cost_rs += f.cost_rs;
                ++cur.fills;
                cur.max_abs_q = std::max(cur.max_abs_q, std::abs(q));
                char buf[512];
                std::snprintf(buf, sizeof buf,
                    "%s 1 lot at %.2f: %s = reservation %.2f (mid %.2f %s inventory %+d x gamma %.2g x sigma^2 %.2f x tau %.0f min = %+.2f) "
                    "%s half spread %.2f (risk %.2f + liquidity %.2f, k %.3f/pt); bar %s %.2f traded through by a tick; P(stress) %.2f",
                    side > 0 ? "bought" : "sold", price, side > 0 ? "bid" : "ask", live_model.reservation, live_mid,
                    live_model.inventory_skew <= 0.0 ? "minus" : "plus", f.q_before, day_gamma, live_sigma * live_sigma, live_tau,
                    live_model.inventory_skew, side > 0 ? "minus" : "plus", 0.5 * live_model.spread, live_model.risk_half,
                    live_model.liquidity_half, live_k, side > 0 ? "low" : "high", side > 0 ? b.l : b.h, day_p);
                f.reason = buf;
                out.fills.push_back(std::move(f));
            };
            const bool ask_hit = live.ask_on && b.h >= live.ask + p.tick - 1e-9;
            const bool bid_hit = live.bid_on && b.l <= live.bid - p.tick + 1e-9;
            if (ask_hit) { fill(-1, live.ask); }
            if (bid_hit) { fill(+1, live.bid); }
        }
        live_valid = false;

        // ---- 2. learn this bar ---------------------------------------------------
        if (!new_day && i > 0) {
            const double prev = bars[i - 1].c;
            const double dc = b.c - prev;
            var = var_n == 0 ? dc * dc : (1.0 - alpha) * var + alpha * dc * dc;
            ++var_n;
            push_exc(b.h - prev);
            push_exc(prev - b.l);
        }

        if (done_today) { continue; }

        // ---- 4. flatten at 15:20, or at the session's last bar -------------------
        const bool last_of_day = i + 1 == bars.size() || d::day_of(bars[i + 1].t) != bd;
        if (m >= p.flatten_min || last_of_day) {
            if (q != 0) {
                const int side = q > 0 ? -1 : +1;
                const int lots = std::abs(q);
                AsFill f;
                f.t = b.t;
                f.side = side;
                f.price = b.c + side * p.flatten_ticks * p.tick;   // worse by the ticks
                f.q_before = q;
                f.q_after = 0;
                f.mid = b.c;
                f.reservation = b.c;
                f.flatten = true;
                f.gamma = day_gamma;
                f.p_stress = day_p;
                f.cost_rs = fill_cost(f.price) * lots;
                cash -= side * f.price * lots;
                cur.cost_rs += f.cost_rs;
                cur.fills += static_cast<std::size_t>(lots);
                char buf[256];
                std::snprintf(buf, sizeof buf,
                    "%s %d lot%s at %.2f: flat by %s (close %.2f, %.0f ticks worse); nothing is carried overnight",
                    side > 0 ? "bought" : "sold", lots, lots == 1 ? "" : "s", f.price,
                    last_of_day && m < p.flatten_min ? "the session's last bar" : d::hhmm(p.flatten_min).c_str(),
                    b.c, p.flatten_ticks);
                f.reason = buf;
                out.fills.push_back(std::move(f));
                q = 0;
            }
            done_today = true;
            continue;
        }

        // ---- 3. quotes for the next bar ------------------------------------------
        if (m < p.first_quote_min || m >= p.last_quote_min) { continue; }
        if (var_n < p.sigma_min || exc_pos < p.k_min || !(var > 0.0)) { continue; }
        const double sigma = std::sqrt(var);
        const double k = static_cast<double>(exc_pos) / exc_sum;
        const double tau = static_cast<double>(p.flatten_min - (m + 1));
        const auto qt = as_quote(b.c, static_cast<double>(q), sigma, day_gamma, k, std::max(0.0, tau));
        if (!qt) { continue; }
        AsQuoteRow row;
        row.t = b.t;
        row.q = q;
        row.bid_on = q < p.max_lots;
        row.ask_on = q > -p.max_lots;
        row.bid = std::min(std::floor(qt->bid / p.tick) * p.tick, b.c - p.tick);
        row.ask = std::max(std::ceil(qt->ask / p.tick) * p.tick, b.c + p.tick);
        out.quotes.push_back(row);
        live = row;
        live_valid = row.bid_on || row.ask_on;
        live_model = *qt;
        live_mid = b.c;
        live_sigma = sigma;
        live_k = k;
        live_tau = tau;
    }
    close_day();

    // Markouts, for the report: the close `markout_bars` later, same session.
    std::size_t j = 0;
    for (auto& f : out.fills) {
        if (f.flatten) { continue; }
        while (j < bars.size() && bars[j].t < f.t) { ++j; }
        const std::size_t at = j + p.markout_bars;
        if (j < bars.size() && at < bars.size() && d::day_of(bars[at].t) == d::day_of(f.t)) {
            f.markout_pts = f.side * (bars[at].c - f.price);
        }
    }
    return out;
}

/// Bars after `cut` replaced with another path: every quote decided and every
/// fill made at or before bars[cut] must come out the same. Returns the number
/// of differences (0 is the pass).
[[nodiscard]] inline std::size_t as_leak_check(const std::vector<AsBar>& bars, const AsMmParams& p, std::size_t cut,
                                               const std::map<std::int64_t, double>* p_stress_by_day = nullptr) {
    if (cut + 1 >= bars.size()) { return 0; }
    auto other = bars;
    std::uint64_t s = 0x2545F4914F6CDD1Dull;
    for (std::size_t i = cut + 1; i < other.size(); ++i) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        const double f = 1.0 + 0.004 * (static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0) - 0.5);
        other[i].o *= f; other[i].h *= f * 1.001; other[i].l *= f * 0.999; other[i].c *= f;
    }
    const std::int64_t tc = bars[cut].t;
    const auto a = run_as_market_maker(bars, p, p_stress_by_day);
    const auto b = run_as_market_maker(other, p, p_stress_by_day);
    std::size_t diff = 0;
    const auto upto_q = [tc](const std::vector<AsQuoteRow>& v) {
        std::vector<AsQuoteRow> o;
        for (const auto& r : v) { if (r.t <= tc) { o.push_back(r); } }
        return o;
    };
    const auto upto_f = [tc](const std::vector<AsFill>& v) {
        std::vector<AsFill> o;
        for (const auto& r : v) { if (r.t <= tc) { o.push_back(r); } }
        return o;
    };
    const auto qa = upto_q(a.quotes), qb = upto_q(b.quotes);
    const auto fa = upto_f(a.fills), fb = upto_f(b.fills);
    if (qa.size() != qb.size()) { ++diff; }
    for (std::size_t i = 0; i < std::min(qa.size(), qb.size()); ++i) {
        if (qa[i].t != qb[i].t || qa[i].bid != qb[i].bid || qa[i].ask != qb[i].ask
            || qa[i].bid_on != qb[i].bid_on || qa[i].ask_on != qb[i].ask_on) { ++diff; }
    }
    if (fa.size() != fb.size()) { ++diff; }
    for (std::size_t i = 0; i < std::min(fa.size(), fb.size()); ++i) {
        if (fa[i].t != fb[i].t || fa[i].side != fb[i].side || fa[i].price != fb[i].price) { ++diff; }
    }
    return diff;
}

} // namespace altair
