// strategies/residual_reversion.hpp -- cross-sectional statistical arbitrage.
//
// THE QUANT-FUND VERSION OF PAIRS TRADING (Avellaneda & Lee 2010,
// "Statistical Arbitrage in the US Equities Market"; research/papers/index.md).
// Not one pair: every stock in a universe, each against the factors that move
// it, many small bets at once.
//
// Each close t, for every stock with a full window of returns:
//   1. regress its last `window` daily returns on the market's and, when the
//      stock has sector peers in the universe, on the leave-one-out mean of
//      their returns: r = alpha + b_m r_m + b_s r_sec + e;
//   2. the cumulative residual X_k = e_1 + ... + e_k is modelled as an
//      Ornstein-Uhlenbeck process: X_{k+1} = a + b X_k + z. Mean reversion
//      needs 0 < b < 1; its speed is kappa = -ln(b) x 252, and a stock
//      slower than `min_kappa` (about 30 days) is not traded;
//   3. the s-score is (X_W - m) / sigma_eq with m = a / (1 - b) and
//      sigma_eq = sd(z) / sqrt(1 - b^2); as in the paper, m is centred on the
//      day's cross-sectional mean so the book does not lean on the common drift.
// Open long below -entry, short above +entry; close a long once s rises
// above -exit_long, a short once it falls below exit_short.
//
// HEDGED AT ENTRY. A position is one unit of notional in the stock against
// b_m units of the market and b_s units of its sector mean, the betas frozen
// on the day it opens. Its return from close t to close t+1 is
// side x (r_i - b_m r_m - b_s r_sec). Nothing about day t+1 is known when the
// position is decided at close t; the test changes the future and checks.
//
// Costs are the caller's: every trade carries its entry and exit day, so the
// CLI prices each leg through risk/cost.hpp.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <vector>

namespace altair {

struct RrPanel {
    std::vector<std::int64_t> day;              ///< T trading days
    std::vector<double> market;                 ///< the market's log return on each day (NaN on the first)
    std::vector<std::vector<double>> ret;       ///< [stock][day] log returns; NaN where missing
    std::vector<int> sector;                    ///< sector id per stock; -1 for none
};

struct RrPolicy {
    std::size_t window = 60;
    double entry = 1.25;
    double exit_long = 0.50;     ///< a long closes once s > -exit_long
    double exit_short = 0.75;    ///< a short closes once s < exit_short
    double min_kappa = 252.0 / 30.0;
    bool sector_factor = true;
    std::size_t max_hold = 60;   ///< sessions; a stuck position is closed
};

struct RrTrade {
    std::size_t stock = 0;
    int side = 0;                ///< +1 long the stock, -1 short
    std::size_t entry = 0, exit = 0;   ///< day indices (decided at those closes)
    double s_entry = 0.0, s_exit = 0.0;
    double beta_m = 0.0, beta_s = 0.0, kappa = 0.0;
    double ret = 0.0;            ///< hedged log return per unit notional over the hold
    const char* why = "signal";  ///< signal, max hold, no data, end
};

struct RrDay {
    std::size_t longs = 0, shorts = 0;
    double ret = 0.0;            ///< sum over open positions, per unit notional each
};

struct RrResult {
    std::vector<RrTrade> trades;
    std::vector<RrDay> daily;    ///< one per panel day
    std::size_t scored = 0;      ///< stock-days with a usable s-score
};

enum class RrError : std::uint8_t { BadPolicy, BadPanel };

[[nodiscard]] inline const char* rr_error_text(RrError e) noexcept {
    return e == RrError::BadPolicy ? "window, thresholds or speed are inconsistent"
                                   : "the panel's days, market and stock series must line up";
}

namespace rr_detail {

/// OLS of y on [1, x1(, x2)]; returns false when singular.
inline bool ols(const std::vector<double>& y, const std::vector<double>& x1, const std::vector<double>* x2,
                double& a, double& b1, double& b2, std::vector<double>& resid) {
    const std::size_t n = y.size();
    const std::size_t k = x2 ? 3 : 2;
    double m[3][3] = {}, v[3] = {};
    for (std::size_t i = 0; i < n; ++i) {
        const double row[3] = {1.0, x1[i], x2 ? (*x2)[i] : 0.0};
        for (std::size_t p = 0; p < k; ++p) {
            for (std::size_t q = 0; q < k; ++q) { m[p][q] += row[p] * row[q]; }
            v[p] += row[p] * y[i];
        }
    }
    // Gauss-Jordan on the k x k system.
    double sol[3] = {};
    for (std::size_t c = 0; c < k; ++c) {
        std::size_t piv = c;
        for (std::size_t r = c + 1; r < k; ++r) { if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) { piv = r; } }
        if (!(std::fabs(m[piv][c]) > 1e-14)) { return false; }
        for (std::size_t q = 0; q < k; ++q) { std::swap(m[c][q], m[piv][q]); }
        std::swap(v[c], v[piv]);
        for (std::size_t r = 0; r < k; ++r) {
            if (r == c) { continue; }
            const double f = m[r][c] / m[c][c];
            for (std::size_t q = c; q < k; ++q) { m[r][q] -= f * m[c][q]; }
            v[r] -= f * v[c];
        }
    }
    for (std::size_t c = 0; c < k; ++c) { sol[c] = v[c] / m[c][c]; }
    a = sol[0];
    b1 = sol[1];
    b2 = x2 ? sol[2] : 0.0;
    resid.resize(n);
    for (std::size_t i = 0; i < n; ++i) { resid[i] = y[i] - a - b1 * x1[i] - (x2 ? b2 * (*x2)[i] : 0.0); }
    return true;
}

struct RrScore {
    bool ok = false;
    double m = 0.0, sigma_eq = 0.0, x_last = 0.0, kappa = 0.0, beta_m = 0.0, beta_s = 0.0;
};


/// Every stock's s-score inputs at close t, from returns t-window+1 .. t;
/// `msum`/`mn` accumulate the scored stocks' m for the cross-sectional centre.
/// Shared by the walk-forward (rr_run) and the live model (rr_scores_at), so
/// the two cannot drift apart.
inline void score_day(const RrPanel& p, const std::vector<std::vector<double>>& sec, const std::vector<bool>& has_peers,
                      std::size_t t, const RrPolicy& pol, std::vector<RrScore>& sc, double& msum, std::size_t& mn) {
    const std::size_t N = p.ret.size();
    std::vector<double> y, x1, x2, e, xs, xn;
    for (std::size_t i = 0; i < N; ++i) {
        sc[i] = {};
        y.clear(); x1.clear(); x2.clear();
        const bool use_sec = pol.sector_factor && has_peers[i];
        bool ok = true;
        for (std::size_t k = t + 1 - pol.window; k <= t && ok; ++k) {
            const double ri = p.ret[i][k], rm = p.market[k];
            const double rs = use_sec ? sec[i][k] : 0.0;
            if (!std::isfinite(ri) || !std::isfinite(rm) || !std::isfinite(rs)) { ok = false; break; }
            y.push_back(ri);
            x1.push_back(rm);
            x2.push_back(rs);
        }
        if (!ok) { continue; }
        double a = 0, bm = 0, bs = 0;
        if (!ols(y, x1, use_sec ? &x2 : nullptr, a, bm, bs, e)) { continue; }
        // Cumulative residual and its AR(1).
        xs.assign(e.size(), 0.0);
        double c = 0.0;
        for (std::size_t k = 0; k < e.size(); ++k) { c += e[k]; xs[k] = c; }
        xn.assign(xs.begin() + 1, xs.end());
        const std::vector<double> xl(xs.begin(), xs.end() - 1);
        double aa = 0, bb = 0, unused = 0;
        std::vector<double> z;
        if (!ols(xn, xl, nullptr, aa, bb, unused, z)) { continue; }
        if (!(bb > 0.0) || !(bb < 1.0)) { continue; }
        const double kappa = -std::log(bb) * 252.0;
        if (kappa < pol.min_kappa) { continue; }
        double zz = 0.0;
        for (const double v : z) { zz += v * v; }
        const double var_z = zz / static_cast<double>(z.size() - 2);
        const double sigma_eq = std::sqrt(var_z / (1.0 - bb * bb));
        if (!(sigma_eq > 0.0)) { continue; }
        sc[i] = {true, aa / (1.0 - bb), sigma_eq, xs.back(), kappa, bm, use_sec ? bs : 0.0};
        msum += sc[i].m;
        ++mn;
    }
}

} // namespace rr_detail

/// The leave-one-out mean return of stock i's sector peers on day t; NaN when it has none that day.
[[nodiscard]] inline double rr_sector_return(const RrPanel& p, std::size_t i, std::size_t t) noexcept {
    if (p.sector[i] < 0) { return std::numeric_limits<double>::quiet_NaN(); }
    double s = 0.0;
    std::size_t n = 0;
    for (std::size_t j = 0; j < p.ret.size(); ++j) {
        if (j == i || p.sector[j] != p.sector[i] || !std::isfinite(p.ret[j][t])) { continue; }
        s += p.ret[j][t];
        ++n;
    }
    return n > 0 ? s / static_cast<double>(n) : std::numeric_limits<double>::quiet_NaN();
}

[[nodiscard]] inline std::expected<RrResult, RrError> rr_run(const RrPanel& p, const RrPolicy& pol) {
    if (pol.window < 20 || !(pol.entry > 0.0) || !(pol.exit_long >= 0.0) || !(pol.exit_long < pol.entry)
        || !(pol.exit_short >= 0.0) || !(pol.exit_short < pol.entry) || !(pol.min_kappa >= 0.0) || pol.max_hold == 0) {
        return std::unexpected(RrError::BadPolicy);
    }
    const std::size_t T = p.day.size(), N = p.ret.size();
    if (T == 0 || p.market.size() != T || p.sector.size() != N) { return std::unexpected(RrError::BadPanel); }
    for (const auto& r : p.ret) { if (r.size() != T) { return std::unexpected(RrError::BadPanel); } }

    // Sector returns once: [stock][day]. A stock alone in its sector has no
    // peers to hedge against and is fitted on the market alone.
    std::vector<bool> has_peers(N, false);
    for (std::size_t i = 0; i < N; ++i) {
        for (std::size_t j = 0; j < N && p.sector[i] >= 0; ++j) {
            if (j != i && p.sector[j] == p.sector[i]) { has_peers[i] = true; break; }
        }
    }
    std::vector<std::vector<double>> sec(N, std::vector<double>(T, std::numeric_limits<double>::quiet_NaN()));
    if (pol.sector_factor) {
        for (std::size_t i = 0; i < N; ++i) {
            for (std::size_t t = 0; t < T; ++t) { sec[i][t] = rr_sector_return(p, i, t); }
        }
    }

    RrResult out;
    out.daily.assign(T, RrDay{});
    struct Open { bool on = false; RrTrade t; };
    std::vector<Open> open(N);
    std::vector<rr_detail::RrScore> sc(N);
    for (std::size_t t = 0; t < T; ++t) {
        // 1. Positions held from close t-1 to close t earn day t's hedged return.
        for (std::size_t i = 0; i < N; ++i) {
            if (!open[i].on) { continue; }
            RrTrade& tr = open[i].t;
            const double ri = p.ret[i][t], rm = p.market[t];
            double rs = 0.0;
            if (tr.beta_s != 0.0) { rs = sec[i][t]; }
            if (!std::isfinite(ri) || !std::isfinite(rm) || !std::isfinite(rs)) {
                tr.exit = t;   // no price: closed at the last one (the return so far stands)
                tr.why = "no data";
                out.trades.push_back(tr);
                open[i].on = false;
                continue;
            }
            const double r = static_cast<double>(tr.side) * (ri - tr.beta_m * rm - tr.beta_s * rs);
            tr.ret += r;
            out.daily[t].ret += r;
            (tr.side > 0 ? out.daily[t].longs : out.daily[t].shorts) += 1;
        }
        // 2. Scores at close t, from returns t-window+1 .. t.
        if (t + 1 < pol.window) { continue; }
        double msum = 0.0;
        std::size_t mn = 0;
        rr_detail::score_day(p, sec, has_peers, t, pol, sc, msum, mn);
        if (mn == 0) { continue; }
        const double mbar = msum / static_cast<double>(mn);
        out.scored += mn;
        // 3. Decisions at close t.
        for (std::size_t i = 0; i < N; ++i) {
            const bool have = sc[i].ok;
            const double s = have ? (sc[i].x_last - (sc[i].m - mbar)) / sc[i].sigma_eq : 0.0;
            if (open[i].on) {
                RrTrade& tr = open[i].t;
                const bool done = (have && tr.side > 0 && s > -pol.exit_long) || (have && tr.side < 0 && s < pol.exit_short);
                const bool stale = t - tr.entry >= pol.max_hold;
                if (done || stale || t + 1 == T) {
                    tr.exit = t;
                    tr.s_exit = s;
                    tr.why = done ? "signal" : (stale ? "max hold" : "end");
                    out.trades.push_back(tr);
                    open[i].on = false;
                }
                continue;
            }
            if (!have || t + 1 == T) { continue; }
            const int side = s < -pol.entry ? 1 : (s > pol.entry ? -1 : 0);
            if (side == 0) { continue; }
            RrTrade tr;
            tr.stock = i;
            tr.side = side;
            tr.entry = t;
            tr.s_entry = s;
            tr.beta_m = sc[i].beta_m;
            tr.beta_s = sc[i].beta_s;
            tr.kappa = sc[i].kappa;
            open[i] = {true, tr};
        }
    }
    return out;
}


/// One stock's s-score at the panel's LAST day, as rr_run would compute it at
/// that close: NaN when the stock has no usable score. The live model appends
/// today's return so far as the last day and decides near the close on these.
struct RrLiveScore {
    double s = std::numeric_limits<double>::quiet_NaN();
    double kappa = 0.0, beta_m = 0.0, beta_s = 0.0;
};

[[nodiscard]] inline std::expected<std::vector<RrLiveScore>, RrError> rr_scores_at_last(const RrPanel& p, const RrPolicy& pol) {
    const std::size_t T = p.day.size(), N = p.ret.size();
    if (pol.window < 20) { return std::unexpected(RrError::BadPolicy); }
    if (T < pol.window || p.market.size() != T || p.sector.size() != N) { return std::unexpected(RrError::BadPanel); }
    for (const auto& r : p.ret) { if (r.size() != T) { return std::unexpected(RrError::BadPanel); } }
    std::vector<bool> has_peers(N, false);
    for (std::size_t i = 0; i < N; ++i) {
        for (std::size_t j = 0; j < N && p.sector[i] >= 0; ++j) {
            if (j != i && p.sector[j] == p.sector[i]) { has_peers[i] = true; break; }
        }
    }
    std::vector<std::vector<double>> sec(N, std::vector<double>(T, std::numeric_limits<double>::quiet_NaN()));
    if (pol.sector_factor) {
        for (std::size_t i = 0; i < N; ++i) {
            for (std::size_t t = T - pol.window; t < T; ++t) { sec[i][t] = rr_sector_return(p, i, t); }
        }
    }
    std::vector<rr_detail::RrScore> sc(N);
    double msum = 0.0;
    std::size_t mn = 0;
    rr_detail::score_day(p, sec, has_peers, T - 1, pol, sc, msum, mn);
    std::vector<RrLiveScore> out(N);
    if (mn == 0) { return out; }
    const double mbar = msum / static_cast<double>(mn);
    for (std::size_t i = 0; i < N; ++i) {
        if (!sc[i].ok) { continue; }
        out[i].s = (sc[i].x_last - (sc[i].m - mbar)) / sc[i].sigma_eq;
        out[i].kappa = sc[i].kappa;
        out[i].beta_m = sc[i].beta_m;
        out[i].beta_s = sc[i].beta_s;
    }
    return out;
}

} // namespace altair
