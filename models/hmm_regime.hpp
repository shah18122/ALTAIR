// models/hmm_regime.hpp -- the hidden Markov model's REGIME as an input,
// walk-forward, so a trader can lean on it without reading the future.
//
// analytics/hmm.hpp fits the model and the forecast curriculum already turns
// it into a direction call. What nothing used was the regime itself: the
// filtered probability that today is the high-variance ("stress") state. That
// is the number a volatility seller wants -- sell premium in calm, size down or
// stand aside in stress -- and the number Avellaneda-Stoikov quoting scales
// its risk aversion with (strategies/as_market_maker.hpp).
//
// WALK-FORWARD, AND WHY IT IS CAUSAL.
//   * Day t's row uses returns r_1..r_t: r_t is the close-to-close return
//     ending at day t's close, known at that close, and the row is stamped
//     there.
//   * The model is refitted every `refit_every` days on returns up to the
//     refit day only (an expanding window, or the last `window` days), then
//     the forward filter is rerun under the new parameters from the window's
//     start; between refits the filter takes one step per day.
//   * P(stress next) is the filtered vector times the transition matrix: a
//     one-step-ahead forecast, not a smoothed (backward-pass) estimate. The
//     backward pass would read the future; it is never used here.
//
// States are sorted by sigma inside fit_hmm, so the last state is the stress
// one by construction. The fit's `agreeing_restarts` is written with every
// row: a regime from a fit no two restarts agreed on is an accident.

#pragma once

#include <analytics/hmm.hpp>
#include <models/curriculum_feeds.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace altair {

struct RegimeParams {
    std::size_t min_history = 500;   ///< returns before the first fit
    std::size_t refit_every = 21;    ///< days between refits
    std::size_t window = 0;          ///< 0: expanding; else the last `window` returns
    std::uint64_t seed = 7;
    std::size_t restarts = 4;
    std::size_t iters = 80;
};

struct RegimeRow {
    std::int64_t day = 0;            ///< IST day of the close this row is known at
    double p_stress = 0.0;           ///< filtered P(stress state | r_1..r_t)
    double p_stress_next = 0.0;      ///< one step ahead
    double sigma_calm = 0.0, sigma_stress = 0.0;   ///< daily, of the current fit
    double dwell_stress = 0.0;       ///< expected days in stress, 1 / (1 - a_ss)
    std::int64_t fitted_on = 0;      ///< the day the current fit used data up to
    std::size_t agreeing = 0, restarts = 0;
};

/// `day[i]` is the IST day whose close ends return `ret[i]`. Ascending.
[[nodiscard]] inline std::vector<RegimeRow>
walk_forward_regimes(const std::vector<std::int64_t>& day, const std::vector<double>& ret, const RegimeParams& p = {}) {
    std::vector<RegimeRow> out;
    const std::size_t n = std::min(day.size(), ret.size());
    Hmm h{};
    std::vector<double> alpha, next;
    std::size_t last_fit = 0;
    std::int64_t fitted_on = 0;
    bool have = false;

    const auto step = [&](double x) {
        const std::size_t k = h.k;
        double tot = 0.0;
        for (std::size_t j = 0; j < k; ++j) {
            double pr = 0.0;
            for (std::size_t s = 0; s < k; ++s) { pr += alpha[s] * h.trans(s, j); }
            next[j] = pr * detail::gauss(x, h.mu[j], h.sigma[j]);
            tot += next[j];
        }
        if (!(tot > 0.0) || !std::isfinite(tot)) {
            next.assign(k, 1.0 / static_cast<double>(k));
            tot = 1.0;
        }
        for (std::size_t j = 0; j < k; ++j) { alpha[j] = next[j] / tot; }
    };

    for (std::size_t t = 0; t < n; ++t) {
        const bool due = t + 1 >= p.min_history && (!have || t - last_fit >= p.refit_every);
        if (due) {
            const std::size_t b = p.window > 0 && t + 1 > p.window ? t + 1 - p.window : 0;
            std::vector<double> x(ret.begin() + static_cast<std::ptrdiff_t>(b),
                                  ret.begin() + static_cast<std::ptrdiff_t>(t + 1));
            auto m = fit_hmm(x, 2, p.seed, p.restarts, p.iters);
            if (m) {
                h = std::move(*m);
                alpha.assign(h.pi.begin(), h.pi.end());
                next.assign(h.k, 0.0);
                // The filter is rerun from the window's start under the new
                // parameters; the first observation enters through pi.
                for (std::size_t j = 0; j < h.k; ++j) { alpha[j] *= detail::gauss(x[0], h.mu[j], h.sigma[j]); }
                double tot = 0.0;
                for (const double a : alpha) { tot += a; }
                if (tot > 0.0 && std::isfinite(tot)) { for (double& a : alpha) { a /= tot; } }
                else { alpha.assign(h.k, 1.0 / static_cast<double>(h.k)); }
                for (std::size_t i = 1; i < x.size(); ++i) { step(x[i]); }
                have = true;
                last_fit = t;
                fitted_on = day[t];
            } else if (have) {
                step(ret[t]);
                last_fit = t;   // try again a period later, not every day
            }
        } else if (have) {
            step(ret[t]);
        }
        if (!have) { continue; }
        const std::size_t s = h.k - 1;
        RegimeRow r;
        r.day = day[t];
        r.p_stress = alpha[s];
        double pn = 0.0;
        for (std::size_t i = 0; i < h.k; ++i) { pn += alpha[i] * h.trans(i, s); }
        r.p_stress_next = pn;
        r.sigma_calm = h.sigma[0];
        r.sigma_stress = h.sigma[s];
        r.dwell_stress = h.expected_dwell(s);
        r.fitted_on = fitted_on;
        r.agreeing = h.agreeing_restarts;
        r.restarts = h.restarts;
        out.push_back(r);
    }
    return out;
}

/// Close-to-close log returns by IST day from bars of any timeframe: `t` in
/// IST seconds (bar start), ascending; a day's close is its last bar's.
/// `day[i]` is the day whose close ends `ret[i]`.
inline void daily_log_returns(const std::vector<std::int64_t>& t, const std::vector<double>& close,
                              std::vector<std::int64_t>& day, std::vector<double>& ret) {
    constexpr std::int64_t kDaySec = 86'400;
    day.clear();
    ret.clear();
    std::int64_t cur = std::numeric_limits<std::int64_t>::min();
    double last = 0.0, prev_close = 0.0;
    bool have_prev = false;
    const auto flush = [&]() {
        if (cur == std::numeric_limits<std::int64_t>::min() || !(last > 0.0)) { return; }
        if (have_prev) {
            day.push_back(cur);
            ret.push_back(std::log(last / prev_close));
        }
        prev_close = last;
        have_prev = true;
    };
    for (std::size_t i = 0; i < t.size() && i < close.size(); ++i) {
        const std::int64_t d = t[i] >= 0 ? t[i] / kDaySec : -((-t[i] + kDaySec - 1) / kDaySec);
        if (d != cur) { flush(); cur = d; last = 0.0; }
        if (close[i] > 0.0 && std::isfinite(close[i])) { last = close[i]; }
    }
    flush();
}

/// The regime as a daily ModelFeed (models/curriculum_feeds.hpp), stamped at
/// each day's 15:30 close: P(stress), P(stress next), and log(sigma_stress /
/// sigma_calm) -- how far apart the two states are.
[[nodiscard]] inline ModelFeed regime_daily_feed(const std::string& source, const std::vector<RegimeRow>& rows) {
    constexpr std::int64_t kDaySec = 86'400, kCloseSec = 15 * 3600 + 30 * 60;
    ModelFeed f;
    f.source = source;
    f.names = {"HMM P(stress)", "HMM P(stress) next", "HMM stress/calm sigma"};
    f.intraday = false;
    f.required = false;
    for (const auto& r : rows) {
        if (!(r.sigma_calm > 0.0) || !(r.sigma_stress > 0.0)) { continue; }
        f.t.push_back(r.day * kDaySec + kCloseSec);
        f.v.push_back(static_cast<float>(r.p_stress));
        f.v.push_back(static_cast<float>(r.p_stress_next));
        f.v.push_back(static_cast<float>(std::log(r.sigma_stress / r.sigma_calm)));
    }
    return f;
}

} // namespace altair
