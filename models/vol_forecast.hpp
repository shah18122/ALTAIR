// models/vol_forecast.hpp -- how big the next move is, on every timeframe,
// judged on RMSE and MAE against a random walk and proved not overfitted.
//
// WHY THIS TARGET. The 2026-11-10 nightly (docs/MODEL_AUDIT.md) shows no
// model forecasts the PRICE better than "no change" on any timeframe (best
// +0.13 % RMSE skill). The SIZE of the next move is a different matter:
// volatility clusters and follows the clock (the open and the close move most),
// so it can be forecast, and an option seller needs exactly that. The target
// here is the next bar's absolute log return |r| in basis points (for a daily
// bar, the next session's close-to-close move).
//
// THE MODELS. Each forecast for bar i is made at the close of bar i-1 from
// bars 0..i-1 only. Nothing is fitted on a bar it is then scored on.
//   Random walk     |r| of the last bar
//   Mean 22         the average |r| per bar over the last 22 sessions
//   EWMA            exponentially weighted |r|; the decay (0.90 / 0.97 / 0.99 /
//                   0.995) is the one with the lowest error SO FAR
//   Seasonal        the average |r| at this time of day over the last 22
//                   sessions (intraday only)
//   HAR-seasonal    ridge regression on the last bar, the last 6 bars, the
//                   last session, the last 5 and 22 sessions, the time-of-day
//                   average, the previous INDIA VIX close and the overnight
//                   gap flag (Corsi's HAR with a clock); sums discounted with
//                   a half-life of `half_life_sessions`, refit every session
//   Combination     the models above weighted by 1 / their recent MSE
//
// HOW IT IS JUDGED. Scored only after a burn-in (`burn_sessions`). RMSE and
// MAE in bp; skill = 1 - RMSE / RMSE(random walk) and against Mean 22; a
// Diebold-Mariano test on squared errors with a Newey-West variance; and the
// skill in each third of the scored period separately, so a model that won
// once and then stopped shows it. Overfitting is checked twice: in-sample
// RMSE of the last HAR fit against the RMSE its real forecasts had on the
// same (discounted, recent) rows, and the thirds.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace altair::vol_forecast {

inline constexpr std::int64_t kVfDaySec = 86'400;

struct Bar {
    std::int64_t t = 0;   ///< bar start, IST seconds since epoch
    double c = 0.0;       ///< close
};

/// A daily close known at `known_t` (e.g. INDIA VIX, stamped at its 15:30 close).
struct Known {
    std::int64_t known_t = 0;
    double v = 0.0;
};

struct Options {
    bool intraday = true;
    std::size_t burn_sessions = 120;      ///< no score before this many sessions
    double half_life_sessions = 250.0;    ///< HAR sums' discount
    double ridge = 1e-3;                  ///< on standardised features
    std::size_t mse_window = 2000;        ///< rows behind the EWMA choice and the combination weights
};

inline const std::vector<std::string>& model_names() {
    static const std::vector<std::string> n{"Random walk", "Mean 22", "EWMA", "Seasonal", "HAR-seasonal", "Combination"};
    return n;
}
inline constexpr std::size_t kModels = 6;
inline constexpr std::size_t kRw = 0, kMean = 1, kEwma = 2, kSeas = 3, kHar = 4, kComb = 5;

/// One scored row: the forecasts made before the bar, and what it did.
struct Row {
    std::int64_t t = 0;
    double actual = 0.0;
    std::array<double, kModels> f{};
};

struct Run {
    std::vector<Row> rows;                ///< scored rows, in time order
    double har_in_sample_rmse = std::numeric_limits<double>::quiet_NaN();   ///< final fit, on its own (discounted) rows
    double har_oos_same_weights = std::numeric_limits<double>::quiet_NaN(); ///< its forecasts' error, same weights: compare
    std::size_t bars = 0, sessions = 0;
};

namespace detail {

inline constexpr std::size_t kF = 9;   // bias + 8 features

/// Solve (A + lambda I) x = b for a small symmetric positive system.
inline bool solve(std::array<std::array<double, kF>, kF> a, std::array<double, kF> b, std::array<double, kF>& x) {
    for (std::size_t c = 0; c < kF; ++c) {
        std::size_t p = c;
        for (std::size_t r = c + 1; r < kF; ++r) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12) return false;
        std::swap(a[p], a[c]);
        std::swap(b[p], b[c]);
        for (std::size_t r = 0; r < kF; ++r) {
            if (r == c) continue;
            const double m = a[r][c] / a[c][c];
            for (std::size_t k = c; k < kF; ++k) a[r][k] -= m * a[c][k];
            b[r] -= m * b[c];
        }
    }
    for (std::size_t c = 0; c < kF; ++c) x[c] = b[c] / a[c][c];
    return true;
}

inline double mean_back(const std::vector<double>& v, std::size_t n) {
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    const std::size_t k = std::min(n, v.size());
    double s = 0.0;
    for (std::size_t i = v.size() - k; i < v.size(); ++i) s += v[i];
    return s / static_cast<double>(k);
}

} // namespace detail

/// Walk forward over `bars` (one instrument, one timeframe, time order).
/// `vix` (optional) must be sorted by known_t.
[[nodiscard]] inline Run run(const std::vector<Bar>& bars, const std::vector<Known>& vix, const Options& o) {
    using detail::kF;
    Run out;
    out.bars = bars.size();
    if (bars.size() < 3) return out;
    const double nan = std::numeric_limits<double>::quiet_NaN();

    // Per-session state.
    std::vector<double> session_mean;            // mean |r| of each finished session
    double cur_sum = 0.0;
    std::size_t cur_n = 0;
    std::int64_t cur_day = bars[0].t / kVfDaySec;
    // Seasonal: per minute-of-day, the last 22 sessions' |r|.
    std::map<int, std::deque<double>> slot;
    std::map<int, double> slot_sum;
    // Recent bars.
    std::deque<double> last6;
    double last6_sum = 0.0;
    // EWMA candidates and their running squared errors.
    constexpr std::array<double, 4> kLam{0.90, 0.97, 0.99, 0.995};
    std::array<double, 4> ew{};
    bool ew_init = false;
    std::array<std::deque<double>, 4> ew_se;
    std::array<double, 4> ew_se_sum{};
    // Combination: the other models' recent squared errors.
    std::array<std::deque<double>, kModels - 1> m_se;
    std::array<double, kModels - 1> m_se_sum{};
    // HAR: discounted normal equations, refit once a session.
    std::array<std::array<double, kF>, kF> xtx{};
    std::array<double, kF> xty{};
    std::array<double, kF> beta{};
    bool have_beta = false;
    std::size_t rows_in_fit = 0;
    double yty = 0.0, rows_w = 0.0;   // discounted, for the in-sample error
    double oos_se = 0.0, oos_w = 0.0; // the same discount on HAR's real forecast errors
    std::size_t bars_per_session_est = 1;
    std::size_t vix_at = 0;
    double vix_now = nan;

    for (std::size_t i = 1; i < bars.size(); ++i) {
        const auto& b = bars[i];
        const std::int64_t day = b.t / kVfDaySec;
        const int minute = static_cast<int>((b.t % kVfDaySec) / 60);
        if (day != cur_day) {
            if (cur_n > 0) {
                session_mean.push_back(cur_sum / static_cast<double>(cur_n));
                bars_per_session_est = std::max<std::size_t>(1, cur_n);
            }
            cur_sum = 0.0;
            cur_n = 0;
            cur_day = day;
            // Refit HAR at each session start, on rows from before it.
            if (rows_in_fit > 2 * kF) {
                auto a = xtx;
                for (std::size_t k = 1; k < kF; ++k) a[k][k] += o.ridge * (xtx[k][k] > 0.0 ? xtx[k][k] : 1.0);
                std::array<double, kF> nb{};
                if (detail::solve(a, xty, nb)) { beta = nb; have_beta = true; }
            }
        }
        const bool first_of_session = cur_n == 0;
        // The previous INDIA VIX close known before this bar starts.
        while (vix_at < vix.size() && vix[vix_at].known_t <= b.t) vix_now = vix[vix_at++].v;

        const double prev_abs = std::fabs(std::log(bars[i - 1].c / (i >= 2 ? bars[i - 2].c : bars[i - 1].c))) * 1e4;
        // Features known at the close of bar i-1.
        const double a1 = prev_abs;
        const double a6 = last6.empty() ? a1 : last6_sum / static_cast<double>(last6.size());
        const double aS = session_mean.empty() ? a6 : session_mean.back();
        const double aW = session_mean.empty() ? a6 : detail::mean_back(session_mean, 5);
        const double aM = session_mean.empty() ? a6 : detail::mean_back(session_mean, 22);
        double seas = aM;
        if (o.intraday) {
            const auto it = slot.find(minute);
            if (it != slot.end() && !it->second.empty()) seas = slot_sum[minute] / static_cast<double>(it->second.size());
        }
        const double vx = std::isfinite(vix_now) ? vix_now : 0.0;
        const std::array<double, kF> x{1.0, a1, a6, aS, aW, aM, seas, vx, first_of_session ? 1.0 : 0.0};

        // The forecasts.
        std::array<double, kModels> f{};
        f[kRw] = a1;
        f[kMean] = aM;
        if (!ew_init) { ew.fill(a1); ew_init = true; }
        std::size_t best_l = 2;
        for (std::size_t k = 0; k < kLam.size(); ++k)
            if (!ew_se[k].empty() && ew_se_sum[k] < ew_se_sum[best_l]) best_l = k;
        f[kEwma] = ew[best_l];
        f[kSeas] = o.intraday ? seas : aM;
        double har = aM;
        if (have_beta) {
            har = 0.0;
            for (std::size_t k = 0; k < kF; ++k) har += beta[k] * x[k];
            har = std::max(0.0, har);
        }
        f[kHar] = har;
        {
            double wsum = 0.0, fsum = 0.0;
            for (std::size_t m = 0; m < kModels - 1; ++m) {
                if (m_se[m].empty()) continue;
                const double mse = m_se_sum[m] / static_cast<double>(m_se[m].size());
                const double w = 1.0 / std::max(mse, 1e-12);
                wsum += w;
                fsum += w * f[m];
            }
            f[kComb] = wsum > 0.0 ? fsum / wsum : aM;
        }

        // The outcome.
        const double y = std::fabs(std::log(b.c / bars[i - 1].c)) * 1e4;
        if (!std::isfinite(y)) continue;
        if (session_mean.size() >= o.burn_sessions) out.rows.push_back({b.t, y, f});

        // Learn from it (after the forecast was made and recorded).
        for (std::size_t m = 0; m < kModels - 1; ++m) {
            const double e = (f[m] - y) * (f[m] - y);
            m_se[m].push_back(e);
            m_se_sum[m] += e;
            if (m_se[m].size() > o.mse_window) { m_se_sum[m] -= m_se[m].front(); m_se[m].pop_front(); }
        }
        for (std::size_t k = 0; k < kLam.size(); ++k) {
            const double e = (ew[k] - y) * (ew[k] - y);
            ew_se[k].push_back(e);
            ew_se_sum[k] += e;
            if (ew_se[k].size() > o.mse_window) { ew_se_sum[k] -= ew_se[k].front(); ew_se[k].pop_front(); }
            ew[k] = kLam[k] * ew[k] + (1.0 - kLam[k]) * y;
        }
        {
            const double per_row = std::pow(0.5, 1.0 / (o.half_life_sessions * static_cast<double>(bars_per_session_est)));
            for (std::size_t r = 0; r < kF; ++r) {
                xty[r] = per_row * xty[r] + x[r] * y;
                for (std::size_t c = 0; c < kF; ++c) xtx[r][c] = per_row * xtx[r][c] + x[r] * x[c];
            }
            yty = per_row * yty + y * y;
            rows_w = per_row * rows_w + 1.0;
            oos_se *= per_row;
            oos_w *= per_row;
            if (have_beta) { oos_se += (f[kHar] - y) * (f[kHar] - y); oos_w += 1.0; }
            ++rows_in_fit;
        }
        last6.push_back(y);
        last6_sum += y;
        if (last6.size() > 6) { last6_sum -= last6.front(); last6.pop_front(); }
        if (o.intraday) {
            auto& q = slot[minute];
            q.push_back(y);
            slot_sum[minute] += y;
            if (q.size() > 22) { slot_sum[minute] -= q.front(); q.pop_front(); }
        }
        cur_sum += y;
        ++cur_n;
    }
    out.sessions = session_mean.size() + (cur_n > 0 ? 1 : 0);
    // In-sample: the HAR fit on all the (discounted) rows, scored on those same
    // rows. Far below the out-of-sample RMSE would mean it learned noise.
    if (rows_in_fit > 2 * kF && rows_w > 0.0) {
        auto a = xtx;
        for (std::size_t k = 1; k < kF; ++k) a[k][k] += o.ridge * (xtx[k][k] > 0.0 ? xtx[k][k] : 1.0);
        std::array<double, kF> bb{};
        if (detail::solve(a, xty, bb)) {
            double sse = yty;
            for (std::size_t r = 0; r < kF; ++r) {
                sse -= 2.0 * bb[r] * xty[r];
                for (std::size_t c = 0; c < kF; ++c) sse += bb[r] * xtx[r][c] * bb[c];
            }
            out.har_in_sample_rmse = std::sqrt(std::max(0.0, sse) / rows_w);
            if (oos_w > 0.0) out.har_oos_same_weights = std::sqrt(oos_se / oos_w);
        }
    }
    return out;
}

/// Diebold-Mariano on squared errors (model a against b): positive t means a
/// is WORSE. Newey-West variance with lag ceil(n^(1/3)).
[[nodiscard]] inline double dm_t(const std::vector<Row>& rows, std::size_t a, std::size_t b) {
    const std::size_t n = rows.size();
    if (n < 10) return 0.0;
    std::vector<double> d(n);
    double mean = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double ea = rows[i].f[a] - rows[i].actual, eb = rows[i].f[b] - rows[i].actual;
        d[i] = ea * ea - eb * eb;
        mean += d[i];
    }
    mean /= static_cast<double>(n);
    const std::size_t L = static_cast<std::size_t>(std::ceil(std::cbrt(static_cast<double>(n))));
    double v = 0.0;
    for (std::size_t i = 0; i < n; ++i) v += (d[i] - mean) * (d[i] - mean);
    for (std::size_t l = 1; l <= L && l < n; ++l) {
        double c = 0.0;
        for (std::size_t i = l; i < n; ++i) c += (d[i] - mean) * (d[i - l] - mean);
        v += 2.0 * (1.0 - static_cast<double>(l) / static_cast<double>(L + 1)) * c;
    }
    v /= static_cast<double>(n);
    return v > 0.0 ? mean / std::sqrt(v / static_cast<double>(n)) : 0.0;
}

struct VolScore {
    std::size_t n = 0;
    double rmse = 0.0, mae = 0.0;
    double skill_rw = 0.0, skill_mean = 0.0;   ///< 1 - RMSE / RMSE(baseline)
    double dm_rw = 0.0, dm_mean = 0.0;         ///< negative: better than the baseline
    std::array<double, 3> skill_rw_thirds{};   ///< the same skill in each third of the scored period
};

[[nodiscard]] inline VolScore score(const std::vector<Row>& rows, std::size_t m) {
    VolScore s;
    s.n = rows.size();
    if (rows.empty()) return s;
    const auto rmse_of = [&](std::size_t k, std::size_t from, std::size_t to) {
        double se = 0.0;
        for (std::size_t i = from; i < to; ++i) se += (rows[i].f[k] - rows[i].actual) * (rows[i].f[k] - rows[i].actual);
        return std::sqrt(se / static_cast<double>(std::max<std::size_t>(1, to - from)));
    };
    double ae = 0.0;
    for (const auto& r : rows) ae += std::fabs(r.f[m] - r.actual);
    s.rmse = rmse_of(m, 0, rows.size());
    s.mae = ae / static_cast<double>(rows.size());
    s.skill_rw = 1.0 - s.rmse / rmse_of(kRw, 0, rows.size());
    s.skill_mean = 1.0 - s.rmse / rmse_of(kMean, 0, rows.size());
    s.dm_rw = m == kRw ? 0.0 : dm_t(rows, m, kRw);
    s.dm_mean = m == kMean ? 0.0 : dm_t(rows, m, kMean);
    const std::size_t third = rows.size() / 3;
    for (std::size_t k = 0; k < 3; ++k) {
        const std::size_t from = k * third, to = k == 2 ? rows.size() : (k + 1) * third;
        s.skill_rw_thirds[k] = 1.0 - rmse_of(m, from, to) / rmse_of(kRw, from, to);
    }
    return s;
}

} // namespace altair::vol_forecast
