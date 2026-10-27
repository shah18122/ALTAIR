// models/combo_search.hpp -- altair_trader's combination search: every model's
// output is a variable, and the search tries every way of combining them with
// every strategy at every time of day, walk-forward.
//
// THE PIECES.
//   * A STEP is one decision: a moment (a time-of-day slot on a session) for
//     one underlying. Steps of the same underlying and slot form a GROUP.
//   * Each step carries the SIGNALS known at its moment -- every model's
//     output (a volatility forecast, a direction model's conviction, India
//     VIX, the IV-RV gap, days to expiry, ...) -- and, per strategy of the
//     book, the OUTCOME of trading it there: net rupees for one lot, and the
//     moment that outcome became final (a stop, the square-off, an expiry).
//   * A CONDITION is one signal above, or below, its running median in that
//     group -- the median of the values seen on EARLIER steps only.
//   * A RULE is (group, strategy, up to `depth` conditions). Depth 0 is
//     "always". With n signals there are 1 + 2n singles and pairs of
//     conditions on different signals; pairs are formed among the first
//     `pair_signals` signals only (the caller puts the core ones first), so
//     hundreds of model columns still fit in memory.
//
// WALK-FORWARD, AND NOTHING ELSE. Steps are processed in time order. Before
// each step, every outcome that became final at or before its moment is
// credited to every rule that fired on its step (exponentially forgotten:
// `half_life_days`). At the first step of each calendar month after the
// warm-up, the book is re-picked: per (group, strategy) the rule with the
// highest trailing t-statistic (at least `min_trades` effective trades), then
// the `top_k` best of those across the book. Until the next pick, every pick
// that fires on a step is traded, one lot each -- no limit on the count. No
// parameter here is tuned on the outcomes: they are fixed before the run.
//
// MULTIPLE TESTING. Trying hundreds of thousands of rules finds lucky ones.
// Three guards, all reported:
//   * the walk-forward result itself is out of sample (a pick trades only
//     steps after it was made);
//   * a PLACEBO: the same search on outcomes shuffled across sessions (each
//     session's outcomes kept together, signals left where they were); the
//     share of placebo runs that do as well is the p-value of the real run;
//   * the DEFLATED SHARPE RATIO (Bailey & Lopez de Prado 2014) of the best
//     in-sample rule, deflated by the number of rules tried and the spread
//     of their Sharpe ratios: what the best rule is worth after the search.
//
// No threads: one search runs on one thread, so there is nothing to race.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <queue>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace altair::combo {

inline constexpr std::int64_t kComboDaySec = 86'400;
inline constexpr double kComboPi = 3.14159265358979323846;
inline constexpr double kComboE = 2.71828182845904523536;

[[nodiscard]] inline double nan() noexcept { return std::numeric_limits<double>::quiet_NaN(); }

/// One strategy's result at one step.
struct Outcome {
    double pnl = nan();            ///< net rupees, one lot; NaN: not tradable at this step
    std::int64_t known_at = 0;     ///< IST seconds when the result is final
};

struct Step {
    std::int64_t t = 0;            ///< decision moment, IST seconds
    int group = 0;                 ///< (underlying, slot)
    std::vector<double> x;         ///< signals, NaN when unknown
    std::vector<Outcome> y;        ///< one per strategy
};

struct SearchConfig {
    int depth = 2;                     ///< conditions per rule, 0..2
    std::size_t pair_signals = 40;     ///< pairs only among the first this many signals
    std::size_t top_k = 10;            ///< rules traded after each pick
    double half_life_days = 365.0;     ///< an outcome this old counts half
    double min_trades = 30.0;          ///< effective trades before a rule can be picked
    std::int64_t warmup_days = 365;    ///< no pick before this many calendar days of steps
};

/// A rule: group, strategy, conditions (code = 2 * signal + (0 above, 1 below); -1 none).
struct Rule {
    int group = 0, strategy = 0, c1 = -1, c2 = -1;
};

/// Trailing record of one rule.
struct RuleStat {
    double w = 0, s = 0, q = 0, w2 = 0;   ///< forgotten: sum of weights, weighted sum, weighted squares, squared weights
    double n = 0, us = 0, uq = 0;         ///< plain: count, sum, squares

    void add(double y, double wt) noexcept {
        w += wt; s += wt * y; q += wt * y * y; w2 += wt * wt;
        n += 1; us += y; uq += y * y;
    }
    [[nodiscard]] double n_eff() const noexcept { return w2 > 0 ? w * w / w2 : 0.0; }
    [[nodiscard]] double mean() const noexcept { return w > 0 ? s / w : 0.0; }
    /// Trailing t-statistic of the mean outcome; 0 when undefined.
    [[nodiscard]] double t() const noexcept {
        const double ne = n_eff();
        if (w <= 0 || ne < 2) return 0.0;
        const double m = s / w;
        const double var = std::max(0.0, q / w - m * m) * ne / (ne - 1.0);
        return var > 0 ? m / std::sqrt(var / ne) : 0.0;
    }
    [[nodiscard]] double plain_t() const noexcept {
        if (n < 2) return 0.0;
        const double m = us / n, var = std::max(0.0, uq / n - m * m) * n / (n - 1.0);
        return var > 0 ? m / std::sqrt(var / n) : 0.0;
    }
};

/// One walk-forward trade.
struct ComboTrade {
    std::size_t step = 0;
    std::size_t rule = 0;        ///< index into Search::rules()
    double pnl = 0;
    std::int64_t known_at = 0;
    double t_at_pick = 0, mean_at_pick = 0, n_at_pick = 0;   ///< why it was picked
};

/// One pick: when, and which rules.
struct Pick {
    std::int64_t t = 0;
    std::vector<std::size_t> rules;
    std::vector<double> t_stat, mean, n_eff;   ///< each rule's trailing record when picked
};

class Search {
public:
    Search(std::size_t groups, std::size_t strategies, std::size_t signals, SearchConfig cfg = {})
        : cfg_(cfg), groups_(groups), strategies_(strategies), n_(signals) {
        cfg_.depth = std::clamp(cfg_.depth, 0, 2);
        const std::size_t ps = std::min(cfg_.pair_signals, n_);
        // condition codes 0 .. 2n-1; pair index table among the first `ps` signals
        pair_index_.assign(4 * ps * ps, -1);
        std::vector<std::pair<int, int>> conds;   // per (g, s) template: (c1, c2)
        conds.emplace_back(-1, -1);
        if (cfg_.depth >= 1) for (std::size_t c = 0; c < 2 * n_; ++c) conds.emplace_back(static_cast<int>(c), -1);
        if (cfg_.depth >= 2) {
            for (std::size_t a = 0; a < 2 * ps; ++a)
                for (std::size_t b = a + 1; b < 2 * ps; ++b) {
                    if (a / 2 == b / 2) continue;
                    pair_index_[a * 2 * ps + b] = static_cast<int>(conds.size());
                    conds.emplace_back(static_cast<int>(a), static_cast<int>(b));
                }
        }
        ps_ = ps;
        per_ = conds.size();
        rules_.reserve(groups_ * strategies_ * per_);
        for (std::size_t g = 0; g < groups_; ++g)
            for (std::size_t s = 0; s < strategies_; ++s)
                for (const auto& [a, b] : conds)
                    rules_.push_back(Rule{static_cast<int>(g), static_cast<int>(s), a, b});
        stats_.assign(rules_.size(), RuleStat{});
    }

    [[nodiscard]] const std::vector<Rule>& rules() const noexcept { return rules_; }
    [[nodiscard]] const std::vector<RuleStat>& stats() const noexcept { return stats_; }
    [[nodiscard]] const std::vector<ComboTrade>& trades() const noexcept { return trades_; }
    [[nodiscard]] const std::vector<Pick>& picks() const noexcept { return picks_; }
    /// Conditions true at each step (codes), as decided at that step.
    [[nodiscard]] const std::vector<std::vector<int>>& fired() const noexcept { return fired_; }
    /// The threshold each signal was compared with at each step (NaN: none yet).
    [[nodiscard]] const std::vector<std::vector<double>>& thresholds() const noexcept { return thr_; }

    /// Runs the whole walk-forward over `steps` (time order; ties keep input order).
    void run(const std::vector<Step>& steps) {
        const std::size_t ns = steps.size();
        fired_.assign(ns, {});
        thr_.assign(ns, {});
        trades_.clear();
        picks_.clear();
        std::fill(stats_.begin(), stats_.end(), RuleStat{});
        medians_.assign(groups_ * n_, RunningMedian{});
        if (ns == 0) return;
        origin_day_ = steps.front().t / kComboDaySec;

        // pending outcomes: (known_at, step, strategy), earliest first
        using Item = std::tuple<std::int64_t, std::size_t, std::size_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<>> pending;
        std::vector<std::size_t> picked;
        std::vector<double> pick_t(rules_.size(), 0), pick_m(rules_.size(), 0), pick_n(rules_.size(), 0);
        int last_month = -1;
        for (std::size_t j = 0; j < ns; ++j) {
            const Step& st = steps[j];
            // 1. every outcome final by now is credited
            while (!pending.empty() && std::get<0>(pending.top()) <= st.t) {
                const auto [ka, sj, s] = pending.top();
                pending.pop();
                credit(steps[sj], sj, s, ka);
            }
            // 2. this step's conditions, from medians of earlier steps only
            const std::size_t g = static_cast<std::size_t>(st.group);
            auto& f = fired_[j];
            auto& th = thr_[j];
            th.assign(n_, nan());
            for (std::size_t i = 0; i < n_; ++i) {
                auto& med = medians_[g * n_ + i];
                const double m = med.median();
                th[i] = m;
                const double v = i < st.x.size() ? st.x[i] : nan();
                if (std::isfinite(v) && std::isfinite(m)) {
                    if (v > m) f.push_back(static_cast<int>(2 * i));
                    else if (v < m) f.push_back(static_cast<int>(2 * i + 1));
                }
            }
            // 3. monthly pick, after the warm-up
            const int month = month_of(st.t);
            if (month != last_month) {
                last_month = month;
                if (st.t / kComboDaySec - origin_day_ >= cfg_.warmup_days) {
                    picked = pick(st.t);
                    Pick pk{st.t, picked, {}, {}, {}};
                    for (std::size_t r : picked) {
                        pick_t[r] = stats_[r].t();
                        pick_m[r] = stats_[r].mean();
                        pick_n[r] = stats_[r].n_eff();
                        pk.t_stat.push_back(pick_t[r]);
                        pk.mean.push_back(pick_m[r]);
                        pk.n_eff.push_back(pick_n[r]);
                    }
                    picks_.push_back(std::move(pk));
                }
            }
            // 4. trade every pick that fires here
            for (std::size_t r : picked) {
                const Rule& ru = rules_[r];
                if (ru.group != st.group || !fires(ru, f)) continue;
                const auto s = static_cast<std::size_t>(ru.strategy);
                if (s >= st.y.size() || !std::isfinite(st.y[s].pnl)) continue;
                trades_.push_back(ComboTrade{j, r, st.y[s].pnl, st.y[s].known_at, pick_t[r], pick_m[r], pick_n[r]});
            }
            // 5. this step's outcomes wait until they are final; its signals join the medians
            for (std::size_t s = 0; s < st.y.size() && s < strategies_; ++s)
                if (std::isfinite(st.y[s].pnl)) pending.emplace(std::max(st.y[s].known_at, st.t + 1), j, s);
            for (std::size_t i = 0; i < n_ && i < st.x.size(); ++i)
                if (std::isfinite(st.x[i])) medians_[g * n_ + i].push(st.x[i]);
        }
        // the rest is credited so the final table is the full in-sample record
        while (!pending.empty()) {
            const auto [ka, sj, s] = pending.top();
            pending.pop();
            credit(steps[sj], sj, s, ka);
        }
    }

    [[nodiscard]] static bool fires(const Rule& r, const std::vector<int>& f) noexcept {
        const auto has = [&](int c) { return c < 0 || std::find(f.begin(), f.end(), c) != f.end(); };
        return has(r.c1) && has(r.c2);
    }

    [[nodiscard]] std::size_t index_of(std::size_t g, std::size_t s, int c1, int c2) const noexcept {
        const std::size_t base = (g * strategies_ + s) * per_;
        if (c1 < 0) return base;
        if (c2 < 0) return base + 1 + static_cast<std::size_t>(c1);
        if (c1 > c2) std::swap(c1, c2);
        const auto a = static_cast<std::size_t>(c1), b = static_cast<std::size_t>(c2);
        if (a >= 2 * ps_ || b >= 2 * ps_) return static_cast<std::size_t>(-1);
        const int k = pair_index_[a * 2 * ps_ + b];
        return k < 0 ? static_cast<std::size_t>(-1) : base + static_cast<std::size_t>(k);
    }

    /// Calendar month id (y * 12 + m) of IST seconds.
    [[nodiscard]] static int month_of(std::int64_t t) noexcept {
        std::int64_t z = (t >= 0 ? t : t - (kComboDaySec - 1)) / kComboDaySec + 719468;
        const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const auto doe = static_cast<unsigned>(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        const unsigned m = mp < 10 ? mp + 3 : mp - 9;
        return static_cast<int>((y + (m <= 2 ? 1 : 0)) * 12 + m - 1);
    }

private:
    /// Exact running median: two heaps.
    struct RunningMedian {
        std::priority_queue<double> lo;                                      ///< lower half, max on top
        std::priority_queue<double, std::vector<double>, std::greater<>> hi; ///< upper half, min on top
        void push(double v) {
            if (lo.empty() || v <= lo.top()) lo.push(v); else hi.push(v);
            if (lo.size() > hi.size() + 1) { hi.push(lo.top()); lo.pop(); }
            else if (hi.size() > lo.size()) { lo.push(hi.top()); hi.pop(); }
        }
        [[nodiscard]] double median() const {
            if (lo.empty()) return nan();
            return lo.size() > hi.size() ? lo.top() : 0.5 * (lo.top() + hi.top());
        }
    };

    void credit(const Step& st, std::size_t j, std::size_t s, std::int64_t known_at) {
        const double y = st.y[s].pnl;
        const double days = static_cast<double>(known_at / kComboDaySec - origin_day_);
        const double wt = std::exp2(days / std::max(1.0, cfg_.half_life_days));
        const auto g = static_cast<std::size_t>(st.group);
        const auto& f = fired_[j];
        stats_[index_of(g, s, -1, -1)].add(y, wt);
        if (cfg_.depth < 1) return;
        for (std::size_t a = 0; a < f.size(); ++a) {
            stats_[index_of(g, s, f[a], -1)].add(y, wt);
            if (cfg_.depth < 2) continue;
            for (std::size_t b = a + 1; b < f.size(); ++b) {
                const std::size_t k = index_of(g, s, f[a], f[b]);
                if (k != static_cast<std::size_t>(-1)) stats_[k].add(y, wt);
            }
        }
    }

    /// Per (group, strategy) the best rule by trailing t, then the top_k of those.
    [[nodiscard]] std::vector<std::size_t> pick(std::int64_t) const {
        std::vector<std::pair<double, std::size_t>> best;
        for (std::size_t gs = 0; gs < groups_ * strategies_; ++gs) {
            double bt = 0;
            std::size_t br = static_cast<std::size_t>(-1);
            for (std::size_t k = 0; k < per_; ++k) {
                const std::size_t r = gs * per_ + k;
                const RuleStat& st = stats_[r];
                if (st.n_eff() < cfg_.min_trades || st.mean() <= 0) continue;
                const double tt = st.t();
                if (tt > bt) { bt = tt; br = r; }
            }
            if (br != static_cast<std::size_t>(-1)) best.emplace_back(bt, br);
        }
        std::sort(best.begin(), best.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < best.size() && i < cfg_.top_k; ++i) out.push_back(best[i].second);
        return out;
    }

    SearchConfig cfg_;
    std::size_t groups_, strategies_, n_, ps_ = 0, per_ = 0;
    std::vector<int> pair_index_;
    std::vector<Rule> rules_;
    std::vector<RuleStat> stats_;
    std::vector<RunningMedian> medians_;
    std::vector<std::vector<int>> fired_;
    std::vector<std::vector<double>> thr_;
    std::vector<ComboTrade> trades_;
    std::vector<Pick> picks_;
    std::int64_t origin_day_ = 0;
};

// ---- statistics ------------------------------------------------------------------------

[[nodiscard]] inline double norm_cdf(double x) noexcept { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

/// Inverse normal CDF (Acklam), |error| < 1.2e-9 after one Newton step.
[[nodiscard]] inline double norm_inv(double p) noexcept {
    if (!(p > 0.0 && p < 1.0)) return p <= 0.0 ? -INFINITY : INFINITY;
    static constexpr double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                                   1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
    static constexpr double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                                   6.680131188771972e+01, -1.328068155288572e+01};
    static constexpr double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                                   -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
    static constexpr double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                                   3.754408661907416e+00};
    double x;
    if (p < 0.02425) {
        const double q = std::sqrt(-2 * std::log(p));
        x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    } else if (p > 1 - 0.02425) {
        const double q = std::sqrt(-2 * std::log(1 - p));
        x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    } else {
        const double q = p - 0.5, r = q * q;
        x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
            (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
    }
    const double e = norm_cdf(x) - p;
    const double u = e * std::sqrt(2 * kComboPi) * std::exp(x * x / 2);
    return x - u / (1 + x * u / 2);
}

/// Moments of a series: mean, sd, skew, kurtosis (not excess).
struct Moments { double n = 0, mean = 0, sd = 0, skew = 0, kurt = 3; };

[[nodiscard]] inline Moments moments(const std::vector<double>& v) {
    Moments m;
    m.n = static_cast<double>(v.size());
    if (v.size() < 2) return m;
    m.mean = std::accumulate(v.begin(), v.end(), 0.0) / m.n;
    double m2 = 0, m3 = 0, m4 = 0;
    for (double x : v) { const double d = x - m.mean; m2 += d * d; m3 += d * d * d; m4 += d * d * d * d; }
    m2 /= m.n; m3 /= m.n; m4 /= m.n;
    m.sd = std::sqrt(m2);
    if (m2 > 0) { m.skew = m3 / std::pow(m2, 1.5); m.kurt = m4 / (m2 * m2); }
    return m;
}

/// Probabilistic Sharpe ratio: P(true SR > sr0) given an observed per-period
/// `sr` over `t` periods with the series' skew and kurtosis (Bailey & Lopez de Prado 2012).
[[nodiscard]] inline double probabilistic_sharpe(double sr, double sr0, double t, double skew, double kurt) noexcept {
    if (t < 2) return nan();
    const double den = 1.0 - skew * sr + (kurt - 1.0) / 4.0 * sr * sr;
    if (!(den > 0)) return nan();
    return norm_cdf((sr - sr0) * std::sqrt(t - 1.0) / std::sqrt(den));
}

/// The Sharpe ratio the best of `trials` independent tries with per-period
/// Sharpe variance `var_sr` reaches by luck alone (the deflation benchmark).
[[nodiscard]] inline double expected_max_sharpe(double trials, double var_sr) noexcept {
    if (trials < 2 || !(var_sr > 0)) return 0.0;
    constexpr double gamma = 0.5772156649015329;
    return std::sqrt(var_sr) * ((1 - gamma) * norm_inv(1 - 1 / trials) + gamma * norm_inv(1 - 1 / (trials * kComboE)));
}

/// Session-block shuffle of outcomes: every step of session d takes the
/// outcomes of the same group's step on session perm(d) (NaN if that session
/// has no such step); a known_at keeps its distance from the step's moment.
/// Signals are untouched.
[[nodiscard]] inline std::vector<Step> shuffle_outcomes(const std::vector<Step>& steps, std::uint64_t seed) {
    std::vector<std::int64_t> days;
    for (const Step& s : steps) days.push_back(s.t / kComboDaySec);
    std::sort(days.begin(), days.end());
    days.erase(std::unique(days.begin(), days.end()), days.end());
    std::vector<std::size_t> perm(days.size());
    std::iota(perm.begin(), perm.end(), std::size_t{0});
    std::mt19937_64 rng(seed);
    std::shuffle(perm.begin(), perm.end(), rng);
    // (day index, group) -> step
    std::vector<std::vector<std::size_t>> at(days.size());
    const auto day_index = [&](std::int64_t d) {
        return static_cast<std::size_t>(std::lower_bound(days.begin(), days.end(), d) - days.begin());
    };
    for (std::size_t j = 0; j < steps.size(); ++j) at[day_index(steps[j].t / kComboDaySec)].push_back(j);
    std::vector<Step> out = steps;
    for (std::size_t j = 0; j < steps.size(); ++j) {
        const std::size_t src_day = perm[day_index(steps[j].t / kComboDaySec)];
        const Step* src = nullptr;
        for (std::size_t k : at[src_day]) {
            const Step& c = steps[k];
            // same group, same time of day
            if (c.group == steps[j].group && c.t % kComboDaySec == steps[j].t % kComboDaySec) { src = &c; break; }
        }
        for (std::size_t s = 0; s < out[j].y.size(); ++s) {
            if (src == nullptr || s >= src->y.size()) { out[j].y[s] = Outcome{}; continue; }
            out[j].y[s].pnl = src->y[s].pnl;
            out[j].y[s].known_at = steps[j].t + (src->y[s].known_at - src->t);
        }
    }
    return out;
}

} // namespace altair::combo
