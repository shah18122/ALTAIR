// models/horizon_eval.hpp -- is there a forecast at this horizon, and does it
// survive its own cost?
//
// P8-15.
//
// THE QUESTION IS NOT "WHAT DOES THE MODEL PREDICT". IT IS "IS THERE ANYTHING
// TO PREDICT, AND IS IT BIGGER THAN THE SPREAD".
//
// Those get answered in the wrong order almost every time. A model is fitted,
// it reports 54% directional accuracy, and the cost of acting on it is checked
// afterwards -- by which point the 54% has become the thing being defended.
// CLAUDE.md rule 5 puts the cost first: every signal is priced net of full
// cost BEFORE it exists. So this file measures the cost hurdle first, from the
// return distribution alone, and only then fits anything.
//
// THE HURDLE IS A PROPERTY OF THE DATA, NOT OF THE MODEL.
//
// If the typical move over the horizon is smaller than the round-trip cost,
// then NO forecast at ANY accuracy is tradeable at that horizon -- a perfect
// oracle still loses. That is one line of arithmetic and it settles more
// questions than a training run does. `frac_exceeding_cost` is that line.
//
// AN OVERNIGHT GAP IS NOT A ONE-HOUR RETURN.
//
// NSE hourly bars run 09:15 to 15:15, seven per session. The step from a day's
// last bar to the next day's first spans SEVENTEEN AND THREE QUARTER HOURS and
// carries every overnight event -- US close, Asian open, results, policy. Left
// in, it inflates the "hourly" volatility, and it inflates it in the direction
// that makes the cost hurdle look easy to clear. The caller therefore supplies
// a session id per bar and cross-session transitions are EXCLUDED and counted.
//
// WHY AN AR AND NOT AN LSTM.
//
// Not a weaker substitute -- the correct model class for the sample. There are
// about three thousand hourly bars. A small LSTM has tens of thousands of
// parameters, so it has more parameters than observations and will fit noise
// with great conviction; CLAUDE.md's own reality check ("anything claiming 70%
// is overfit") is that failure. A linear AR on a handful of lags is what three
// thousand observations can actually support, and if it finds nothing then the
// honest reading is that the linear structure is absent -- which is worth
// knowing before spending a GPU on the nonlinear question.
//
// EVERYTHING IS WALK-FORWARD. Coefficients come from the training block only,
// and the block after it is scored with them. Same discipline as P8-14.

#pragma once

#include <backtest/validation.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

namespace altair {

enum class HorizonError : std::uint8_t {
    TooFewSamples,
    BadParameter,
    /// The normal equations were singular -- collinear lags, or a training
    /// block with no variation. Refused rather than solved with a pseudo-
    /// inverse nobody asked for.
    Singular
};

struct HorizonSpec {
    std::size_t initial_train = 0;
    std::size_t test_len = 0;
    std::size_t step = 0;
    /// AR order. The number of past returns the forecast may use.
    std::size_t lags = 0;
    /// ROUND-TRIP cost in basis points of notional. Not one-way: a forecast
    /// has to pay for getting in AND out before it has earned anything.
    double cost_bps = 0.0;
    bool expanding = true;
};

struct HorizonReport {
    // ---- the distribution, before any model ------------------------------
    std::size_t bars = 0;
    std::size_t returns_used = 0;
    /// Cross-session steps dropped. An overnight gap is a different process.
    std::size_t overnight_excluded = 0;
    double mean_abs_bps = 0.0;
    double median_abs_bps = 0.0;
    double sd_bps = 0.0;
    /// THE DECISIVE NUMBER. Fraction of horizons whose absolute move exceeds
    /// the round-trip cost. An oracle that called every direction correctly
    /// would still lose money on the rest.
    double frac_exceeding_cost = 0.0;
    /// What a PERFECT oracle earns net of cost, in bps per bar, taking every
    /// move regardless of size. Negative means the horizon is untradeable at
    /// this cost no matter what any model does.
    double oracle_net_bps = 0.0;
    /// The same oracle, but only trading when the move exceeds the cost. This
    /// is the ceiling on any strategy at this horizon and cost.
    double oracle_selective_net_bps = 0.0;

    // ---- the model, walk-forward ----------------------------------------
    std::size_t folds = 0;
    std::size_t scored = 0;
    double rmse_model_bps = 0.0;
    double rmse_persistence_bps = 0.0;
    double rmse_mean_bps = 0.0;
    /// Directional accuracy against a CONSTANT predictor (the training
    /// majority), which is the bar that matters -- beating a coin is not the
    /// test when the series drifts.
    double dir_model = 0.0;
    double dir_const = 0.0;

    // ---- rule 5: net of cost, or it does not exist -----------------------
    /// Trades taken: only where the forecast exceeded the cost hurdle.
    std::size_t trades = 0;
    double gross_bps_per_trade = 0.0;
    double net_bps_per_trade = 0.0;
    double net_bps_total = 0.0;
    bool profitable_after_cost = false;
};

namespace detail {

/// Solve A x = b in place, Gaussian elimination with partial pivoting.
/// Returns false if the matrix is singular to working precision.
inline bool solve(std::vector<double>& a, std::vector<double>& b,
                  std::size_t n) {
    for (std::size_t c = 0; c < n; ++c) {
        std::size_t piv = c;
        for (std::size_t r = c + 1; r < n; ++r) {
            if (std::fabs(a[r * n + c]) > std::fabs(a[piv * n + c])) {
                piv = r;
            }
        }
        if (std::fabs(a[piv * n + c]) < 1e-12) {
            return false;
        }
        if (piv != c) {
            for (std::size_t k = 0; k < n; ++k) {
                std::swap(a[c * n + k], a[piv * n + k]);
            }
            std::swap(b[c], b[piv]);
        }
        for (std::size_t r = c + 1; r < n; ++r) {
            const double f = a[r * n + c] / a[c * n + c];
            if (f == 0.0) { continue; }
            for (std::size_t k = c; k < n; ++k) {
                a[r * n + k] -= f * a[c * n + k];
            }
            b[r] -= f * b[c];
        }
    }
    for (std::size_t i = n; i-- > 0;) {
        double s = b[i];
        for (std::size_t k = i + 1; k < n; ++k) {
            s -= a[i * n + k] * b[k];
        }
        b[i] = s / a[i * n + i];
    }
    return true;
}

} // namespace detail

/// Evaluate a one-step-ahead forecast at whatever horizon the bars represent.
///
/// `closes` and `session` are parallel and the same length; `session[i]` is an
/// opaque id that changes when the trading day does. A return is formed only
/// between bars in the SAME session -- see the header.
///
/// `spec.cost_bps` is the round-trip cost. Pass the real one from
/// `risk/cost.hpp`; passing zero measures a world that does not exist.
[[nodiscard]] inline std::expected<HorizonReport, HorizonError>
evaluate_horizon(const std::vector<double>& closes,
                 const std::vector<std::int64_t>& session,
                 const HorizonSpec& spec) {
    if (closes.size() != session.size() || spec.lags == 0
        || spec.initial_train == 0 || spec.test_len == 0 || spec.step == 0
        || spec.cost_bps < 0.0) {
        return std::unexpected(HorizonError::BadParameter);
    }

    HorizonReport rep{};
    rep.bars = closes.size();

    // Returns, intraday only. Index i of `r` is the return INTO bar i+1, and
    // `keep` records whether that step stayed inside one session.
    std::vector<double> r;
    r.reserve(closes.size());
    for (std::size_t i = 0; i + 1 < closes.size(); ++i) {
        if (session[i] != session[i + 1]) {
            ++rep.overnight_excluded;
            continue;
        }
        if (!(closes[i] > 0.0) || !(closes[i + 1] > 0.0)) {
            continue;
        }
        r.push_back(std::log(closes[i + 1] / closes[i]));
    }
    rep.returns_used = r.size();
    if (r.size() < spec.initial_train + spec.test_len + spec.lags + 2) {
        return std::unexpected(HorizonError::TooFewSamples);
    }

    // ---- the distribution, and the cost hurdle, BEFORE any model ---------
    {
        std::vector<double> a;
        a.reserve(r.size());
        double sum = 0.0, sum2 = 0.0;
        std::size_t over = 0;
        double oracle_all = 0.0, oracle_sel = 0.0;
        std::size_t sel = 0;
        for (double x : r) {
            const double bps = x * 10'000.0;
            a.push_back(std::fabs(bps));
            sum += bps;
            sum2 += bps * bps;
            // A perfect oracle takes sign(actual) and earns |actual|, paying
            // the round trip every time.
            oracle_all += std::fabs(bps) - spec.cost_bps;
            if (std::fabs(bps) > spec.cost_bps) {
                ++over;
                ++sel;
                oracle_sel += std::fabs(bps) - spec.cost_bps;
            }
        }
        const auto n = static_cast<double>(r.size());
        const double mean = sum / n;
        rep.sd_bps = std::sqrt(std::max(0.0, sum2 / n - mean * mean));
        double abs_sum = 0.0;
        for (double x : a) { abs_sum += x; }
        rep.mean_abs_bps = abs_sum / n;
        std::sort(a.begin(), a.end());
        rep.median_abs_bps = a[a.size() / 2];
        rep.frac_exceeding_cost = static_cast<double>(over) / n;
        rep.oracle_net_bps = oracle_all / n;
        rep.oracle_selective_net_bps =
            sel > 0 ? oracle_sel / static_cast<double>(sel) : 0.0;
    }

    // ---- walk-forward AR --------------------------------------------------
    // The design matrix starts `lags` in, so sample j predicts r[j + lags]
    // from r[j .. j+lags-1]. Folds are over the SAMPLE index, not the raw
    // return index, so a fold boundary cannot straddle a lag window.
    const std::size_t p = spec.lags;
    const std::size_t nsamp = r.size() - p;
    WalkForwardSpec wf{};
    wf.initial_train = spec.initial_train;
    wf.test_len = spec.test_len;
    wf.step = spec.step;
    wf.gap = 1;
    wf.expanding = spec.expanding;

    const auto k = walk_forward_count(nsamp, wf);
    if (!k || *k == 0) {
        return std::unexpected(HorizonError::TooFewSamples);
    }
    rep.folds = *k;

    double se_model = 0.0, se_pers = 0.0, se_mean = 0.0;
    std::size_t dir_hit = 0, dir_base_hit = 0;
    double gross = 0.0;

    for (std::size_t fi = 0; fi < *k; ++fi) {
        const auto fold = walk_forward_fold(nsamp, wf, fi);
        if (!fold) {
            return std::unexpected(HorizonError::TooFewSamples);
        }
        const std::size_t d = p + 1;             // lags + intercept
        std::vector<double> xtx(d * d, 0.0);
        std::vector<double> xty(d, 0.0);
        double ysum = 0.0;
        std::size_t up = 0, ntr = 0;

        for (std::size_t j = fold->train.start; j < fold->train.end; ++j) {
            double x[16];
            x[0] = 1.0;
            for (std::size_t l = 0; l < p; ++l) {
                x[l + 1] = r[j + l];
            }
            const double y = r[j + p];
            for (std::size_t a2 = 0; a2 < d; ++a2) {
                for (std::size_t b2 = 0; b2 < d; ++b2) {
                    xtx[a2 * d + b2] += x[a2] * x[b2];
                }
                xty[a2] += x[a2] * y;
            }
            ysum += y;
            if (y > 0.0) { ++up; }
            ++ntr;
        }
        if (ntr == 0) { continue; }
        const double train_mean = ysum / static_cast<double>(ntr);
        const bool base_up = up * 2 > ntr;

        std::vector<double> beta = xty;
        if (!detail::solve(xtx, beta, d)) {
            return std::unexpected(HorizonError::Singular);
        }

        for (std::size_t j = fold->test.start; j < fold->test.end; ++j) {
            double f = beta[0];
            for (std::size_t l = 0; l < p; ++l) {
                f += beta[l + 1] * r[j + l];
            }
            const double y = r[j + p];
            const double fb = f * 10'000.0;
            const double yb = y * 10'000.0;

            se_model += (fb - yb) * (fb - yb);
            // Persistence: tomorrow equals today. CLAUDE.md's stated bar.
            se_pers += (r[j + p - 1] * 10'000.0 - yb)
                     * (r[j + p - 1] * 10'000.0 - yb);
            se_mean += (train_mean * 10'000.0 - yb)
                     * (train_mean * 10'000.0 - yb);

            if ((f > 0.0) == (y > 0.0)) { ++dir_hit; }
            if (base_up == (y > 0.0)) { ++dir_base_hit; }

            // RULE 5. Trade only when the forecast clears the round trip; the
            // realised P&L is the actual move in the forecast's direction,
            // minus the cost, every time.
            if (std::fabs(fb) > spec.cost_bps) {
                const double pnl = (f > 0.0 ? yb : -yb);
                gross += pnl;
                rep.net_bps_total += pnl - spec.cost_bps;
                ++rep.trades;
            }
            ++rep.scored;
        }
    }

    if (rep.scored > 0) {
        const auto n = static_cast<double>(rep.scored);
        rep.rmse_model_bps = std::sqrt(se_model / n);
        rep.rmse_persistence_bps = std::sqrt(se_pers / n);
        rep.rmse_mean_bps = std::sqrt(se_mean / n);
        rep.dir_model = static_cast<double>(dir_hit) / n;
        rep.dir_const = static_cast<double>(dir_base_hit) / n;
    }
    if (rep.trades > 0) {
        const auto t = static_cast<double>(rep.trades);
        rep.gross_bps_per_trade = gross / t;
        rep.net_bps_per_trade = rep.net_bps_total / t;
    }
    rep.profitable_after_cost = rep.trades > 0 && rep.net_bps_total > 0.0;
    return rep;
}

} // namespace altair
