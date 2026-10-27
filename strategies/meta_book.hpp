// strategies/meta_book.hpp -- the book altair_trader's combination search
// chooses from: every strategy it may deploy, at any minute of a session, and
// what each would have made. SELL-ONLY: no strategy here ever buys an option
// to open. Protection comes from index futures (delta hedges) and stops.
//
// THE STRATEGIES (one lot each; intraday ones square off at 15:20):
//   FutLong, FutShort           the index future, entry to square-off
//   FutLongStop, FutShortStop   the same with a stop one expected remaining
//                               move away (HAR forecast vol, scaled to the
//                               time left in the session)
//   Straddle                    short ATM call + put
//   StraddleHedged              the same, delta-hedged with the future every
//                               5 minutes when the net delta passes half a lot
//   StraddleStop                the same, bought back when the loss reaches
//                               half the premium taken in
//   Strangle                    short call and put one expected move to expiry
//                               out of the money (rounded out to the strike step)
//   StrangleHedged              the strangle, delta-hedged like the straddle
//   CallPlusLongFut             long future + short ATM call (a covered call)
//   PutPlusShortFut             short future + short ATM put (a covered put)
//   StraddleToExpiry            short ATM straddle held to expiry, delta-hedged
//                               with the future at each 15:20
//
// FILLS. A decision at a minute's start uses bars that ended by then; it is
// filled at that minute's open. A stop or a hedge decided at a bar's close is
// filled at the next bar's open. Futures fill at the price +- `fut_slip_pts`;
// an option at its value -+ max(`opt_slip_min`, `opt_slip_pct` of it).
// Every fill pays the exchange's and broker's charges through `cost` (the
// caller prices them from config/charges.toml).
//
// OPTION PRICES ARE A MODEL UNTIL CHAINS ARE RECORDED. FYERS serves no
// expired option contract, so every option leg here is priced by Black-Scholes
// (no rates, no dividends) on the FYERS index price, at the implied vol the
// caller supplies each minute (India VIX, or a scaled VIX for BANKNIFTY).
// Every option result says `synthetic`; they are evidence of nothing until
// recorded chains replace the model. Time to expiry runs on trading time: a
// session is (1 - kOvernightShare) of a trading day spread evenly over its
// 375 minutes, each night before a later session kOvernightShare of one.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace altair::metabook {

inline constexpr double kOvernightShare = 0.2;
inline constexpr double kBookTradingDays = 252.0;
inline constexpr std::int64_t kBookDaySec = 86'400;
inline constexpr std::int64_t kSessionOpenSec = 9 * 3600 + 15 * 60;
inline constexpr std::int64_t kSessionMinutes = 375;
inline constexpr std::int64_t kSquareOffSec = 15 * 3600 + 20 * 60;

enum class BookStrategy : int {
    FutLong, FutShort, FutLongStop, FutShortStop,
    Straddle, StraddleHedged, StraddleStop,
    Strangle, StrangleHedged,
    CallPlusLongFut, PutPlusShortFut,
    StraddleToExpiry,
    Count
};
inline constexpr int kBookStrategies = static_cast<int>(BookStrategy::Count);

[[nodiscard]] inline const char* book_strategy_name(int s) noexcept {
    static constexpr const char* names[] = {
        "fut_long", "fut_short", "fut_long_stop", "fut_short_stop",
        "straddle", "straddle_hedged", "straddle_stop",
        "strangle", "strangle_hedged",
        "call_plus_long_fut", "put_plus_short_fut",
        "straddle_to_expiry"};
    return s >= 0 && s < kBookStrategies ? names[s] : "?";
}
[[nodiscard]] inline bool book_uses_options(int s) noexcept {
    return s >= static_cast<int>(BookStrategy::Straddle);
}

/// One one-minute bar; t is its start, IST seconds.
struct BookBar {
    std::int64_t t = 0;
    double o = 0, h = 0, l = 0, c = 0;
};

/// One session of one underlying.
struct BookDay {
    std::int64_t day = 0;                 ///< epoch day (IST)
    std::vector<BookBar> spot;            ///< the index, 1m, time order
    std::vector<BookBar> fut;             ///< the near future, 1m, same minutes when present; may be empty
    std::vector<double> iv;               ///< per spot bar: annual implied vol known at that bar's end
    double stop_vol = std::numeric_limits<double>::quiet_NaN();   ///< annual vol forecast known at the open (stops)
    std::int64_t expiry_day = 0;          ///< epoch day of the nearest expiry, on or after `day`
    int sessions_to_expiry = 0;           ///< sessions after this one up to and including the expiry's
};

struct BookConfig {
    double lot = 65;
    double strike_step = 50;
    double fut_slip_pts = 1.0;
    double opt_slip_min = 0.5;
    double opt_slip_pct = 0.01;
    int hedge_every_min = 5;
    double stop_premium_share = 0.5;
};

/// Charges of one fill, rupees: (is_option, is_buy, qty, price, IST seconds).
using BookCost = std::function<double(bool, bool, double, double, std::int64_t)>;

struct BookResult {
    double pnl = std::numeric_limits<double>::quiet_NaN();   ///< net rupees; NaN: not tradable here
    double gross = 0, costs = 0;
    std::int64_t known_at = 0;
    bool synthetic = false;
    std::string why;
};

// ---- pricing ---------------------------------------------------------------------------

[[nodiscard]] inline double ncdf(double x) noexcept { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

struct OptQuote { double price = 0, delta = 0; };

/// Black-Scholes, no rates or dividends; at or past expiry, intrinsic.
[[nodiscard]] inline OptQuote bs_quote(bool call, double s, double k, double tau_years, double vol) noexcept {
    if (!(tau_years > 0) || !(vol > 0)) {
        const double in = call ? std::max(0.0, s - k) : std::max(0.0, k - s);
        return {in, call ? (s > k ? 1.0 : 0.0) : (s < k ? -1.0 : 0.0)};
    }
    const double sd = vol * std::sqrt(tau_years);
    const double d1 = (std::log(s / k) + 0.5 * sd * sd) / sd, d2 = d1 - sd;
    if (call) return {s * ncdf(d1) - k * ncdf(d2), ncdf(d1)};
    return {k * ncdf(-d2) - s * ncdf(-d1), ncdf(d1) - 1.0};
}

/// Trading-time years from IST second `t` of session `d` to the expiry's 15:30.
[[nodiscard]] inline double tau_years(const BookDay& d, std::int64_t t) noexcept {
    const std::int64_t open = d.day * kBookDaySec + kSessionOpenSec;
    const double left = std::clamp(static_cast<double>(open + kSessionMinutes * 60 - t) / 60.0, 0.0,
                                   static_cast<double>(kSessionMinutes));
    const double days = left / static_cast<double>(kSessionMinutes) * (1.0 - kOvernightShare) + d.sessions_to_expiry;
    return days / kBookTradingDays;
}

// ---- the book --------------------------------------------------------------------------

class MetaBook {
public:
    MetaBook(const std::vector<BookDay>& days, BookConfig cfg, BookCost cost)
        : days_(days), cfg_(cfg), cost_(std::move(cost)) {}

    /// Strategy `s` entered on session `di` at spot bar `e` (decision at that bar's start).
    [[nodiscard]] BookResult run(int s, std::size_t di, std::size_t e) const {
        BookResult r;
        const BookDay& d = days_[di];
        if (e == 0 || e >= d.spot.size()) return r;
        const std::size_t x = square_off_bar(d);
        if (x <= e) return r;
        r.synthetic = book_uses_options(s);
        switch (static_cast<BookStrategy>(s)) {
            case BookStrategy::FutLong: return fut(di, e, x, +1, false);
            case BookStrategy::FutShort: return fut(di, e, x, -1, false);
            case BookStrategy::FutLongStop: return fut(di, e, x, +1, true);
            case BookStrategy::FutShortStop: return fut(di, e, x, -1, true);
            case BookStrategy::Straddle: return shorts(di, e, x, 0.0, false, false, 0);
            case BookStrategy::StraddleHedged: return shorts(di, e, x, 0.0, true, false, 0);
            case BookStrategy::StraddleStop: return shorts(di, e, x, 0.0, false, true, 0);
            case BookStrategy::Strangle: return shorts(di, e, x, 1.0, false, false, 0);
            case BookStrategy::StrangleHedged: return shorts(di, e, x, 1.0, true, false, 0);
            case BookStrategy::CallPlusLongFut: return shorts(di, e, x, 0.0, false, false, +1);
            case BookStrategy::PutPlusShortFut: return shorts(di, e, x, 0.0, false, false, -1);
            case BookStrategy::StraddleToExpiry: return to_expiry(di, e);
            case BookStrategy::Count: break;
        }
        return r;
    }

    /// Index of the 15:20 bar (or one past the last bar when the session ends sooner).
    [[nodiscard]] static std::size_t square_off_bar(const BookDay& d) noexcept {
        const std::int64_t so = d.day * kBookDaySec + kSquareOffSec;
        std::size_t i = 0;
        while (i < d.spot.size() && d.spot[i].t < so) ++i;
        return i < d.spot.size() ? i : (d.spot.empty() ? 0 : d.spot.size() - 1);
    }

private:
    [[nodiscard]] double fut_px(const BookDay& d, std::size_t i, bool open) const noexcept {
        if (d.fut.size() == d.spot.size() && d.fut[i].t == d.spot[i].t) return open ? d.fut[i].o : d.fut[i].c;
        return open ? d.spot[i].o : d.spot[i].c;
    }
    [[nodiscard]] double vol_at(const BookDay& d, std::size_t i) const noexcept {
        // IV known at the end of bar i
        return i < d.iv.size() ? d.iv[i] : std::numeric_limits<double>::quiet_NaN();
    }
    [[nodiscard]] double slip(double v) const noexcept { return std::max(cfg_.opt_slip_min, cfg_.opt_slip_pct * v); }

    static std::string hm(std::int64_t t) {
        const std::int64_t s = ((t % kBookDaySec) + kBookDaySec) % kBookDaySec;
        char b[8];
        std::snprintf(b, sizeof b, "%02d:%02d", static_cast<int>(s / 3600), static_cast<int>(s / 60 % 60));
        return b;
    }

    BookResult fut(std::size_t di, std::size_t e, std::size_t x, int side, bool stop) const {
        const BookDay& d = days_[di];
        BookResult r;
        const double lot = cfg_.lot;
        const double in = fut_px(d, e, true) + side * cfg_.fut_slip_pts;
        r.costs += cost_(false, side > 0, lot, in, d.spot[e].t);
        const double s0 = d.spot[e].o;
        double stop_px = 0;
        if (stop) {
            const double v = std::isfinite(d.stop_vol) ? d.stop_vol : vol_at(d, e - 1);
            if (!(v > 0)) return BookResult{};
            const double left = static_cast<double>(x - e) / static_cast<double>(kSessionMinutes) * (1.0 - kOvernightShare);
            stop_px = s0 - side * s0 * v * std::sqrt(left / kBookTradingDays);
        }
        std::size_t out = x;
        std::string why = "square-off 15:20";
        if (stop) {
            for (std::size_t i = e; i + 1 < x; ++i) {
                const double c = d.spot[i].c;
                if ((side > 0 && c <= stop_px) || (side < 0 && c >= stop_px)) {
                    out = i + 1;
                    why = "stop at " + hm(d.spot[i].t + 60);
                    break;
                }
            }
        }
        const double ex = fut_px(d, out, true) - side * cfg_.fut_slip_pts;
        r.costs += cost_(false, side < 0, lot, ex, d.spot[out].t);
        r.gross = side * (ex - in) * lot;
        r.pnl = r.gross - r.costs;
        r.known_at = d.spot[out].t;
        char b[200];
        std::snprintf(b, sizeof b, "%s future %.2f at %s, out %.2f (%s)%s", side > 0 ? "bought" : "sold", in,
                      hm(d.spot[e].t).c_str(), ex, why.c_str(),
                      d.fut.size() == d.spot.size() ? "" : " [index price: no futures bars]");
        r.why = b;
        return r;
    }

    /// Short call + put (`width` expected moves out; 0 = ATM), optional delta
    /// hedge or premium stop; `fut_side` != 0 replaces one leg with a future:
    /// +1 = long future + short call, -1 = short future + short put.
    BookResult shorts(std::size_t di, std::size_t e, std::size_t x, double width, bool hedge, bool stop,
                      int fut_side) const {
        const BookDay& d = days_[di];
        BookResult r;
        r.synthetic = true;
        const double lot = cfg_.lot, step = cfg_.strike_step;
        const double v0 = vol_at(d, e - 1);
        if (!(v0 > 0)) return BookResult{};
        const double s0 = d.spot[e].o;
        const std::int64_t t0 = d.spot[e].t;
        const double tau0 = tau_years(d, t0);
        if (!(tau0 > 0)) return BookResult{};
        double kc = std::round(s0 / step) * step, kp = kc;
        if (width > 0) {
            const double mv = s0 * v0 * std::sqrt(tau0) * width;
            kc = std::ceil((s0 + mv) / step) * step;
            kp = std::floor((s0 - mv) / step) * step;
            if (kc <= s0) kc += step;
            if (kp >= s0) kp -= step;
        }
        const bool has_call = fut_side >= 0, has_put = fut_side <= 0;
        double prem_in = 0;
        double call_in = 0, put_in = 0;
        if (has_call) {
            const double v = bs_quote(true, s0, kc, tau0, v0).price;
            call_in = std::max(0.05, v - slip(v));
            r.costs += cost_(true, false, lot, call_in, t0);
            prem_in += call_in;
        }
        if (has_put) {
            const double v = bs_quote(false, s0, kp, tau0, v0).price;
            put_in = std::max(0.05, v - slip(v));
            r.costs += cost_(true, false, lot, put_in, t0);
            prem_in += put_in;
        }
        double hedge_units = 0, hedge_cash = 0;   // futures units held, cash from their trades
        int hedges = 0;
        const auto trade_fut = [&](double units, std::size_t i) {
            if (units == 0) return;
            const double px = fut_px(d, i, true) + (units > 0 ? cfg_.fut_slip_pts : -cfg_.fut_slip_pts);
            hedge_cash -= units * px;
            hedge_units += units;
            r.costs += cost_(false, units > 0, std::fabs(units), px, d.spot[i].t);
        };
        if (fut_side != 0) trade_fut(fut_side * lot, e);
        std::size_t out = x;
        std::string why = "square-off 15:20";
        double pending = 0;
        for (std::size_t i = e; i < x; ++i) {
            if (i > e && pending != 0) { trade_fut(pending, i); ++hedges; pending = 0; }
            const double v = vol_at(d, i);
            const double si = d.spot[i].c;
            const double tau = tau_years(d, d.spot[i].t + 60);
            const auto cq = has_call ? bs_quote(true, si, kc, tau, v) : OptQuote{};
            const auto pq = has_put ? bs_quote(false, si, kp, tau, v) : OptQuote{};
            if (stop && i + 1 < x) {
                const double loss = (cq.price + pq.price) - prem_in;
                if (loss >= cfg_.stop_premium_share * prem_in) {
                    out = i + 1;
                    why = "premium stop at " + hm(d.spot[i].t + 60);
                    break;
                }
            }
            if (hedge && i + 1 < x && (i - e + 1) % static_cast<std::size_t>(cfg_.hedge_every_min) == 0) {
                const double net = -lot * (cq.delta + pq.delta) + hedge_units;
                if (std::fabs(net) > 0.5 * lot) pending = -lot * std::round(net / lot);
            }
        }
        // close: options bought back at `out`'s open, futures closed there
        const double so = d.spot[out].o;
        const std::int64_t to = d.spot[out].t;
        const double vo = vol_at(d, out - 1);
        const double tau = tau_years(d, to);
        double prem_out = 0;
        if (has_call) {
            const double v = bs_quote(true, so, kc, tau, vo).price;
            const double px = v + slip(v);
            r.costs += cost_(true, true, lot, px, to);
            prem_out += px;
        }
        if (has_put) {
            const double v = bs_quote(false, so, kp, tau, vo).price;
            const double px = v + slip(v);
            r.costs += cost_(true, true, lot, px, to);
            prem_out += px;
        }
        if (hedge_units != 0) trade_fut(-hedge_units, out);
        r.gross = (prem_in - prem_out) * lot + hedge_cash;
        r.pnl = r.gross - r.costs;
        r.known_at = to;
        char b[320];
        if (fut_side != 0) {
            std::snprintf(b, sizeof b, "%s future + sold %s %.0f at %.2f (IV %.1f%%) at %s; out %.2f (%s); net legs %+.0f",
                          fut_side > 0 ? "bought" : "sold", fut_side > 0 ? "CE" : "PE", fut_side > 0 ? kc : kp,
                          prem_in, 100 * v0, hm(t0).c_str(), prem_out, why.c_str(), r.gross);
        } else {
            std::snprintf(b, sizeof b, "sold %.0f CE %.2f + %.0f PE %.2f (IV %.1f%%, spot %.2f) at %s; bought back %.2f (%s)%s",
                          kc, call_in, kp, put_in, 100 * v0, s0, hm(t0).c_str(), prem_out, why.c_str(),
                          hedge ? (", " + std::to_string(hedges) + " futures hedges").c_str() : "");
        }
        r.why = std::string(b) + " [synthetic option prices]";
        return r;
    }

    /// Short ATM straddle held to expiry, delta-hedged at each 15:20 with
    /// futures priced on the index (no roll inside the hold). Settled at the
    /// expiry session's last close, intrinsic value.
    BookResult to_expiry(std::size_t di, std::size_t e) const {
        const BookDay& d0 = days_[di];
        BookResult r;
        r.synthetic = true;
        const double lot = cfg_.lot, step = cfg_.strike_step;
        const double v0 = vol_at(d0, e - 1);
        if (!(v0 > 0)) return BookResult{};
        const double s0 = d0.spot[e].o;
        const std::int64_t t0 = d0.spot[e].t;
        const double tau0 = tau_years(d0, t0);
        if (!(tau0 > 0)) return BookResult{};
        const double k = std::round(s0 / step) * step;
        const auto slip_px = [&](double v) { return slip(v); };
        const double cv = bs_quote(true, s0, k, tau0, v0).price, pv = bs_quote(false, s0, k, tau0, v0).price;
        const double ci = std::max(0.05, cv - slip_px(cv)), pi = std::max(0.05, pv - slip_px(pv));
        r.costs += cost_(true, false, lot, ci, t0) + cost_(true, false, lot, pi, t0);
        double units = 0, cash = 0;
        int hedges = 0;
        const auto trade = [&](double u, double px, std::int64_t t) {
            if (u == 0) return;
            const double f = px + (u > 0 ? cfg_.fut_slip_pts : -cfg_.fut_slip_pts);
            cash -= u * f;
            units += u;
            r.costs += cost_(false, u > 0, std::fabs(u), f, t);
        };
        std::size_t last_di = di;
        bool reached = false;
        for (std::size_t j = di; j < days_.size(); ++j) {
            const BookDay& d = days_[j];
            if (d.spot.empty()) continue;
            if (d.day > d0.expiry_day) break;
            last_di = j;
            if (d.day == d0.expiry_day) { reached = true; break; }
            const std::size_t so = square_off_bar(d);
            if (j == di && so <= e) continue;
            const double si = d.spot[so].o;
            const double v = vol_at(d, so - 1);
            const double tau = tau_years(d, d.spot[so].t);
            const double net = -lot * (bs_quote(true, si, k, tau, v).delta + bs_quote(false, si, k, tau, v).delta) + units;
            if (std::fabs(net) > 0.5 * lot) { trade(-lot * std::round(net / lot), si, d.spot[so].t); ++hedges; }
        }
        const BookDay& dl = days_[last_di];
        const double sl = dl.spot.back().c;
        const std::int64_t tl = dl.spot.back().t + 60;
        double settle;
        std::string why;
        if (reached) {
            settle = std::max(0.0, sl - k) + std::max(0.0, k - sl);
            r.costs += cost_(true, true, lot, settle, tl);
            why = "settled at expiry";
        } else {
            // the data ends before expiry: bought back at the last close, model value
            const double tau = tau_years(dl, tl);
            const double v = vol_at(dl, dl.spot.size() - 1);
            const double c = bs_quote(true, sl, k, tau, v).price, p = bs_quote(false, sl, k, tau, v).price;
            settle = c + slip_px(c) + p + slip_px(p);
            r.costs += cost_(true, true, lot, settle, tl);
            why = "bought back at the data's last close (expiry not reached)";
        }
        if (units != 0) trade(-units, sl, tl);
        r.gross = (ci + pi - settle) * lot + cash;
        r.pnl = r.gross - r.costs;
        r.known_at = tl;
        char b[300];
        std::snprintf(b, sizeof b, "sold %.0f straddle %.2f (IV %.1f%%) at %s, %d futures hedges; %s at %.2f",
                      k, ci + pi, 100 * v0, hm(t0).c_str(), hedges, why.c_str(), settle);
        r.why = std::string(b) + " [synthetic option prices]";
        return r;
    }

    const std::vector<BookDay>& days_;
    BookConfig cfg_;
    BookCost cost_;
};

} // namespace altair::metabook
