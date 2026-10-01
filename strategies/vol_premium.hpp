// strategies/vol_premium.hpp -- sell the variance premium, delta hedged.
//
// THE TRADE AN OPTIONS DESK MAKES. Not "where will the index go" but "will it
// move as much as the option price says". Each day at the close:
//   * the at-the-money straddle of the first monthly expiry at least
//     `min_sessions` away is priced, and its implied volatility solved;
//   * a realised-volatility forecast for the same sessions (HAR,
//     analytics/har_rv.hpp, supplied by the caller) is set against it;
//   * when implied exceeds forecast by `entry_spread`, one lot of the
//     straddle is sold, and its delta is held flat with whole futures lots,
//     reset at every close (Bakshi & Kapadia 2003; Carr & Wu 2009);
//   * the straddle is bought back at the close once `exit_sessions` or fewer
//     remain, or when its cost reaches `stop_multiple` x the credit.
// Hedged, the P&L is close to (implied variance - realised variance) x vega
// -- the premium -- rather than a bet on direction.
//
// THE MARKET IS AN INTERFACE. Quotes, forwards and listed strikes come from
// the caller: synthetic Black-76 at INDIA VIX for the dataset, or NSE
// bhavcopy closes when the files are there (app/bhavcopy.hpp). The engine
// cannot tell them apart, and the results say which one ran.
//
// TIME is calendar: (expiry 15:30 - today 15:30) / 365 days. Every mark is at
// a close, so the intraday-clock question of band_option_fade.hpp does not
// arise; the overnight gap is inside every step.

#pragma once

#include <analytics/greeks.hpp>
#include <analytics/iv.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <set>
#include <vector>

namespace altair {

struct VrpQuote {
    double call = std::numeric_limits<double>::quiet_NaN();   ///< per unit, rupees
    double put = std::numeric_limits<double>::quiet_NaN();
};

struct VrpMarket {
    std::vector<std::int64_t> day;   ///< trading days (days since 1970-01-01), ascending
    std::vector<double> spot;        ///< the index's close on each
    std::set<std::int64_t> expiries; ///< monthly expiry days
    /// Option closes at day i for (expiry, strike); NaN where not quoted.
    std::function<VrpQuote(std::size_t, std::int64_t, double)> quote;
    /// The forward (the same-expiry future's close) at day i; NaN where none.
    std::function<double(std::size_t, std::int64_t)> forward;
    /// The listed strike nearest `level` for the expiry at day i; NaN where none.
    std::function<double(std::size_t, std::int64_t, double)> strike_near;
    /// Annualised realised-vol forecast over the next h sessions, made at day i's close; NaN where none.
    std::function<double(std::size_t, std::size_t)> rv_forecast;
    /// Optional: true vetoes an entry at day i (e.g. a forecast that INDIA VIX rises).
    std::function<bool(std::size_t)> veto;
};

struct VrpPolicy {
    double lot_size = 0.0;           ///< from the spec store, never a literal
    int lots = 1;
    double rate = 0.065;
    double entry_spread = 0.02;      ///< implied minus forecast vol, annualised: 0.02 = 2 vol points
    std::size_t min_sessions = 10;   ///< to expiry, at entry
    std::size_t exit_sessions = 1;   ///< buy back once this many or fewer remain
    double stop_multiple = 0.0;      ///< buy back once the straddle costs this x the credit; 0 = never
    bool hedge = true;
    double slippage_pts = 1.0;       ///< premium points given up per option fill
};

struct VrpFill {
    std::size_t i = 0;   ///< day index
    long lots = 0;       ///< signed: positive buys futures
    double price = 0.0;
};

struct VrpTrade {
    std::size_t entry = 0, exit = 0;
    std::int64_t expiry = 0;
    double strike = 0.0, fwd_in = 0.0, fwd_out = 0.0;
    double credit = 0.0, debit = 0.0;            ///< straddle, per unit, after slippage
    double call_in = 0.0, put_in = 0.0, call_out = 0.0, put_out = 0.0;
    double iv_in = 0.0, rv_fcst = 0.0, rv_real = 0.0;   ///< annualised
    double qty = 0.0;
    double option_pnl = 0.0, hedge_pnl = 0.0, gross = 0.0;
    std::vector<VrpFill> fills;
    const char* exit_reason = "expiry";
};

/// What the signal was each day the engine was flat and could price.
struct VrpSignal {
    std::size_t i = 0;
    std::int64_t expiry = 0;
    std::size_t sessions = 0;
    double iv = 0.0, rv_fcst = 0.0;
    bool entered = false, vetoed = false;
};

struct VrpResult {
    std::vector<VrpTrade> trades;
    std::vector<VrpSignal> signals;
};

enum class VrpError : std::uint8_t { BadPolicy, BadMarket };

[[nodiscard]] inline const char* vrp_error_text(VrpError e) noexcept {
    return e == VrpError::BadPolicy ? "lot size, lots, sessions and stop are inconsistent"
                                    : "the market needs days, closes, expiries and every price function";
}

namespace vrp_detail {

inline constexpr double kYearDays = 365.0;

[[nodiscard]] inline int weekday(std::int64_t d) noexcept {   // Monday = 0
    return static_cast<int>(((d + 3) % 7 + 7) % 7);
}

/// Sessions in (day[i], expiry]: the market's own days, then weekdays past its end.
[[nodiscard]] inline std::size_t sessions_left(const VrpMarket& m, std::size_t i, std::int64_t expiry) noexcept {
    std::size_t n = 0;
    std::size_t k = i + 1;
    for (; k < m.day.size() && m.day[k] <= expiry; ++k) { ++n; }
    if (k == m.day.size()) {
        for (std::int64_t d = m.day.back() + 1; d <= expiry; ++d) {
            if (weekday(d) < 5) { ++n; }
        }
    }
    return n;
}

[[nodiscard]] inline double years(std::int64_t today, std::int64_t expiry) noexcept {
    return static_cast<double>(expiry - today) / kYearDays;
}

/// Implied vol of the straddle's legs, averaged over those that solve; NaN if neither.
[[nodiscard]] inline double straddle_iv(const VrpQuote& q, double fwd, double strike, double T, double rate) noexcept {
    double s = 0.0;
    int n = 0;
    for (const bool call : {true, false}) {
        const double px = call ? q.call : q.put;
        if (!std::isfinite(px)) { continue; }
        const auto paise = [](double rupees) { return Price{static_cast<std::int64_t>(std::llround(rupees * 100.0))}; };
        const auto iv = implied_vol_black76(call ? OptionRight::Call : OptionRight::Put, paise(px), paise(fwd),
                                            paise(strike), Years{T}, rate);
        if (iv) { s += iv->vol.raw(); ++n; }
    }
    return n > 0 ? s / n : std::numeric_limits<double>::quiet_NaN();
}

/// The straddle's delta per unit against the forward, at vol `iv`; NaN if it cannot be priced.
[[nodiscard]] inline double straddle_delta(double fwd, double strike, double T, double iv, double rate) noexcept {
    double d = 0.0;
    for (const bool call : {true, false}) {
        const auto g = black76(call ? OptionRight::Call : OptionRight::Put,
                               Price{static_cast<std::int64_t>(std::llround(fwd * 100.0))},
                               Price{static_cast<std::int64_t>(std::llround(strike * 100.0))}, Years{T}, Vol{iv}, rate);
        if (!g) { return std::numeric_limits<double>::quiet_NaN(); }
        d += g->delta * std::exp(rate * T);   // dV/dF carried to the future's own move
    }
    return d;
}

} // namespace vrp_detail

[[nodiscard]] inline std::expected<VrpResult, VrpError> vrp_run(const VrpMarket& m, const VrpPolicy& p) {
    if (!(p.lot_size > 0.0) || p.lots < 1 || p.min_sessions == 0 || p.exit_sessions >= p.min_sessions
        || !(p.stop_multiple == 0.0 || p.stop_multiple > 1.0) || !(p.slippage_pts >= 0.0) || !std::isfinite(p.rate)
        || !std::isfinite(p.entry_spread)) {
        return std::unexpected(VrpError::BadPolicy);
    }
    if (m.day.empty() || m.spot.size() != m.day.size() || m.expiries.empty() || !m.quote || !m.forward
        || !m.strike_near || !m.rv_forecast) {
        return std::unexpected(VrpError::BadMarket);
    }
    using namespace vrp_detail;
    VrpResult out;
    const double qty = static_cast<double>(p.lots) * p.lot_size;
    bool open = false;
    VrpTrade t;
    long lots = 0;
    double last_fwd = 0.0, last_iv = 0.0;
    const auto rehedge = [&](std::size_t i, double fwd, double iv) {
        if (!p.hedge) { return; }
        const double T = years(m.day[i], t.expiry);
        const double d = straddle_delta(fwd, t.strike, T, iv, p.rate);
        if (!std::isfinite(d)) { return; }
        const long want = std::lround(qty * d / p.lot_size);   // the short straddle is -qty x d; the hedge holds +qty x d
        if (want != lots) {
            t.fills.push_back({i, want - lots, fwd});
            lots = want;
        }
    };
    const auto close = [&](std::size_t i, const VrpQuote& q, double fwd, const char* why) {
        t.exit = i;
        t.fwd_out = fwd;
        t.call_out = q.call;
        t.put_out = q.put;
        t.debit = q.call + q.put + 2.0 * p.slippage_pts;
        t.option_pnl = (t.credit - t.debit) * t.qty;
        if (lots != 0) {
            t.fills.push_back({i, -lots, fwd});
            lots = 0;
        }
        double ss = 0.0;
        for (std::size_t k = t.entry + 1; k <= i; ++k) {
            const double r = std::log(m.spot[k] / m.spot[k - 1]);
            ss += r * r;
        }
        t.rv_real = i > t.entry ? std::sqrt(ss / static_cast<double>(i - t.entry) * 252.0) : 0.0;
        t.gross = t.option_pnl + t.hedge_pnl;
        t.exit_reason = why;
        out.trades.push_back(t);
        open = false;
    };

    for (std::size_t i = 0; i < m.day.size(); ++i) {
        if (open) {
            const double fwd = m.forward(i, t.expiry);
            if (std::isfinite(fwd)) {
                t.hedge_pnl += static_cast<double>(lots) * p.lot_size * (fwd - last_fwd);
                last_fwd = fwd;
            }
            const VrpQuote q = m.quote(i, t.expiry, t.strike);
            if (!std::isfinite(q.call) || !std::isfinite(q.put) || !std::isfinite(fwd)) { continue; }   // unquoted today: hold
            const double T = years(m.day[i], t.expiry);
            const double iv = straddle_iv(q, fwd, t.strike, T, p.rate);
            if (std::isfinite(iv)) { last_iv = iv; }
            if (sessions_left(m, i, t.expiry) <= p.exit_sessions) { close(i, q, fwd, "before expiry"); continue; }
            if (p.stop_multiple > 0.0 && q.call + q.put + 2.0 * p.slippage_pts >= p.stop_multiple * t.credit) {
                close(i, q, fwd, "stop");
                continue;
            }
            rehedge(i, fwd, last_iv);
            continue;
        }
        // Flat: price the candidate at today's close.
        std::int64_t expiry = -1;
        std::size_t left = 0;
        for (const std::int64_t e : m.expiries) {
            if (e <= m.day[i]) { continue; }
            left = sessions_left(m, i, e);
            if (left >= p.min_sessions) { expiry = e; break; }
        }
        if (expiry < 0) { continue; }
        const double fwd = m.forward(i, expiry);
        if (!std::isfinite(fwd) || !(fwd > 0.0)) { continue; }
        const double k = m.strike_near(i, expiry, fwd);
        if (!std::isfinite(k) || !(k > 0.0)) { continue; }
        const VrpQuote q = m.quote(i, expiry, k);
        if (!std::isfinite(q.call) || !std::isfinite(q.put)) { continue; }
        const double T = years(m.day[i], expiry);
        const double iv = straddle_iv(q, fwd, k, T, p.rate);
        const double rvf = m.rv_forecast(i, left);
        if (!std::isfinite(iv) || !std::isfinite(rvf)) { continue; }
        VrpSignal s{i, expiry, left, iv, rvf, false, false};
        const double credit = q.call + q.put - 2.0 * p.slippage_pts;
        if (iv - rvf >= p.entry_spread && credit > 0.0) {
            if (m.veto && m.veto(i)) {
                s.vetoed = true;
            } else {
                s.entered = true;
                t = VrpTrade{};
                t.entry = i;
                t.expiry = expiry;
                t.strike = k;
                t.fwd_in = fwd;
                t.call_in = q.call;
                t.put_in = q.put;
                t.credit = credit;
                t.iv_in = iv;
                t.rv_fcst = rvf;
                t.qty = qty;
                open = true;
                lots = 0;
                last_fwd = fwd;
                last_iv = iv;
                rehedge(i, fwd, iv);
            }
        }
        out.signals.push_back(s);
    }
    return out;   // a position still open at the data's end is not reported: it has no exit price
}

} // namespace altair
