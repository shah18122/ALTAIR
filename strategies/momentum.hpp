// strategies/momentum.hpp -- time-series momentum, and the shared vocabulary
// every directional strategy in this directory reports in.
//
// P21-01.
//
// THE STAGE 3 HOLE.
//
// `ROADMAP_GAP.md` names it: Altair has the risk layer, the execution
// algorithms, the cost model, the walk-forward harness and six model families,
// and had nothing in `strategies/` that emits a directional signal. Everything
// downstream was being evaluated against model FORECASTS rather than against a
// strategy, so the capacity ceiling, the shortfall attribution and the
// optimiser comparison were machinery with nothing in them.
//
// This is the first thing to put in them.
//
// TURNOVER IS THE WHOLE STORY, AND IT IS REPORTED BESIDE EVERY RETURN.
//
// QUANTLAB's Phase 3 ran seven alpha families and its verdict on momentum and
// mean reversion was the same both times: a REAL GROSS EDGE, KILLED BY
// TURNOVER. That is the finding this file is built around, so `StrategyResult`
// carries gross, net, AND the turnover that separates them -- and `net` is
// computed from the actual position changes rather than assumed.
//
// A momentum strategy reporting a Sharpe without a turnover figure is not
// reporting a strategy. It is reporting the half of one that survives before
// the broker is paid.
//
// NO FITTING. THESE ARE RULES, AND THE PARAMETERS ARE CHOICES.
//
// The lookback and the vol target are not estimated from the data -- they are
// stated. That matters for how the result should be read: there is no
// in-sample optimism to deflate here, but there IS a parameter that somebody
// picked, and picking it by trying twelve and keeping the best would put the
// optimism straight back. The test therefore SWEEPS the lookback and shows the
// whole curve rather than the best point.
//
// SIGNAL AT t, RETURN FROM t TO t+1. NEVER THE SAME BAR.
//
// Rule 7. The position for bar t+1 is decided from returns up to and including
// t, and earns r[t+1]. Multiplying a signal by the return that produced it is
// the single most common backtest error and it produces a Sharpe of 8.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <vector>

namespace altair {

enum class StratError : std::uint8_t {
    TooFewSamples,
    /// A lookback, window or threshold outside its admissible range.
    BadParameter,
    /// The series has no variation, so no signal can be formed.
    Degenerate
};

/// What a directional strategy produced. GROSS, NET, and the turnover between.
struct StrategyResult {
    /// Position held into each bar, in units of the vol target. Index i is the
    /// position EARNING r[i], decided from data through i-1.
    std::vector<double> position;

    std::size_t bars = 0;
    /// Bars where a position was actually held.
    std::size_t active = 0;
    /// Times the position changed sign or returned to flat.
    ///
    /// Reported because a strategy that entered ONCE and held for 36 years is
    /// not a strategy, and turnover alone does not say so loudly enough --
    /// 0.0001 reads as "admirably cheap" when it means "never traded again".
    std::size_t round_trips = 0;

    /// Mean per-bar return, in bps. Gross of cost.
    double gross_bps = 0.0;
    /// The same, less turnover * cost. THE number.
    double net_bps = 0.0;
    /// Mean |Δposition| per bar. One full flip from +1 to -1 is 2.0.
    double turnover = 0.0;
    /// Per-bar standard deviation of the GROSS return stream, bps.
    ///
    /// Both Sharpe ratios below divide by this one figure, including the net
    /// one. That is deliberate: the cost charged on a bar is turnover times a
    /// constant, so the net stream differs from the gross stream by a term
    /// with very little variance of its own, and using two different
    /// denominators would make gross and net Sharpe incomparable for the sake
    /// of a third decimal place. Stated here rather than left to be inferred
    /// from the arithmetic.
    double vol_bps = 0.0;
    /// Annualised, on the caller's bars-per-year.
    double gross_sharpe = 0.0;
    double net_sharpe = 0.0;
    /// Largest peak-to-trough of the NET cumulative return, as a fraction.
    double max_drawdown = 0.0;

    /// How much cost the turnover actually consumed, bps per bar. Reported
    /// separately because "gross 3.2, net -1.8" invites the question and the
    /// answer should not need arithmetic.
    [[nodiscard]] double cost_drag_bps() const noexcept {
        return gross_bps - net_bps;
    }
    /// Standard error of the mean net return, bps. A net figure without this
    /// is a point estimate, and ROADMAP section 3 forbids sizing on one.
    [[nodiscard]] double net_se_bps() const noexcept {
        return bars > 1 && vol_bps > 0.0
            ? vol_bps / std::sqrt(static_cast<double>(bars)) : 0.0;
    }
    /// Net mean over its own standard error. Below about 2 there is no result.
    [[nodiscard]] double net_t() const noexcept {
        const double se = net_se_bps();
        return se > 0.0 ? net_bps / se : 0.0;
    }
};

/// Score a position series against the returns it earned.
///
/// `position[i]` is held INTO bar i and earns `r[i]`. The caller is
/// responsible for having built it causally; this function cannot check that
/// and says so rather than pretending to.
///
/// `cost_bps` is charged per UNIT of turnover: moving from +1 to -1 is 2.0
/// units and costs 2 * cost_bps. That is the right accounting -- a full
/// reversal is two trades -- and getting it wrong halves the cost of exactly
/// the strategies that trade most.
[[nodiscard]] inline std::expected<StrategyResult, StratError>
evaluate(const std::vector<double>& position, const std::vector<double>& r,
         double cost_bps, double bars_per_year) {
    if (position.size() != r.size() || r.size() < 100) {
        return std::unexpected(StratError::TooFewSamples);
    }
    if (!(cost_bps >= 0.0) || !(bars_per_year > 0.0)) {
        return std::unexpected(StratError::BadParameter);
    }
    StrategyResult out;
    out.position = position;
    out.bars = r.size();

    std::vector<double> gross(r.size(), 0.0), net(r.size(), 0.0);
    double prev = 0.0, sum_g = 0.0, sum_n = 0.0, sum_to = 0.0;
    for (std::size_t i = 0; i < r.size(); ++i) {
        const double p = position[i];
        if (std::fabs(p) > 1e-12) { ++out.active; }
        const double to = std::fabs(p - prev);
        if ((p == 0.0) != (prev == 0.0) || p * prev < 0.0) {
            ++out.round_trips;
        }
        gross[i] = p * r[i];
        net[i] = gross[i] - to * cost_bps;
        sum_g += gross[i];
        sum_n += net[i];
        sum_to += to;
        prev = p;
    }
    const double n = static_cast<double>(r.size());
    out.gross_bps = sum_g / n;
    out.net_bps = sum_n / n;
    out.turnover = sum_to / n;

    double s2 = 0.0;
    for (const double g : gross) {
        const double d = g - out.gross_bps;
        s2 += d * d;
    }
    out.vol_bps = std::sqrt(s2 / (n - 1.0));
    if (out.vol_bps > 0.0) {
        const double ann = std::sqrt(bars_per_year);
        out.gross_sharpe = out.gross_bps / out.vol_bps * ann;
        out.net_sharpe = out.net_bps / out.vol_bps * ann;
    }

    double cum = 0.0, peak = 0.0, worst = 0.0;
    for (const double v : net) {
        cum += v;
        peak = std::max(peak, cum);
        worst = std::max(worst, peak - cum);
    }
    out.max_drawdown = 1.0 - std::exp(-worst / 10000.0);
    return out;
}

/// Always long, one unit, from the first bar. THE CONTROL.
///
/// Every result in this project ships beside the dumb thing it has to beat,
/// and for a directional strategy on an index that dumb thing is HOLDING THE
/// INDEX. It is not optional and it is not a footnote: NIFTY compounded from
/// 279 to 24,080 between 1990 and 2026, which is +5.09 bps per bar of drift
/// sitting there for free. A long-biased strategy on that series earns some of
/// it automatically, so a positive net proves nothing on its own -- the
/// question is always whether it beat holding.
[[nodiscard]] inline std::vector<double> buy_and_hold(std::size_t n) {
    return std::vector<double>(n, 1.0);
}

/// Mean net return of one position series OVER another, and its t-statistic.
///
/// PAIRED, on the same bars, which is the whole point: the strategy and the
/// benchmark see identical returns, so the difference series removes the
/// market move that both of them shared. An unpaired comparison of two Sharpe
/// ratios computed on the same series would credit the strategy with the
/// index's drift and then test it against zero.
struct Excess {
    double mean_bps = 0.0;
    double se_bps = 0.0;
    double t = 0.0;
};

[[nodiscard]] inline std::expected<Excess, StratError>
excess_over(const std::vector<double>& position,
            const std::vector<double>& benchmark,
            const std::vector<double>& r, double cost_bps) {
    if (position.size() != r.size() || benchmark.size() != r.size()
        || r.size() < 100) {
        return std::unexpected(StratError::TooFewSamples);
    }
    if (!(cost_bps >= 0.0)) { return std::unexpected(StratError::BadParameter); }

    std::vector<double> d(r.size(), 0.0);
    double pa = 0.0, pb = 0.0, sum = 0.0;
    for (std::size_t i = 0; i < r.size(); ++i) {
        const double net_a = position[i] * r[i]
                           - std::fabs(position[i] - pa) * cost_bps;
        const double net_b = benchmark[i] * r[i]
                           - std::fabs(benchmark[i] - pb) * cost_bps;
        d[i] = net_a - net_b;
        sum += d[i];
        pa = position[i];
        pb = benchmark[i];
    }
    const double n = static_cast<double>(r.size());
    Excess e;
    e.mean_bps = sum / n;
    double s2 = 0.0;
    for (const double v : d) {
        const double q = v - e.mean_bps;
        s2 += q * q;
    }
    const double sd = std::sqrt(s2 / (n - 1.0));
    e.se_bps = sd / std::sqrt(n);
    e.t = e.se_bps > 0.0 ? e.mean_bps / e.se_bps : 0.0;
    return e;
}

// ---------------------------------------------------------------------------
// Time-series momentum
// ---------------------------------------------------------------------------

struct MomentumSpec {
    /// Bars of lookback. The ONE parameter, and it is a choice rather than a
    /// fit -- see the header.
    std::size_t lookback = 60;
    /// Bars used to estimate volatility for scaling. Longer than the lookback
    /// on purpose: a vol estimate as noisy as the signal adds variance rather
    /// than removing it.
    std::size_t vol_window = 120;
    /// Target per-bar volatility in bps. Positions are scaled so that a
    /// quiet regime and a violent one carry the same risk -- QUANTLAB's
    /// second hedge-fund model, and the reason many funds avoid crisis
    /// drawdowns.
    double vol_target_bps = 100.0;
    /// Cap on |position|, in vol-target units. Without it a quiet patch
    /// divides by a tiny vol and asks for 40x leverage.
    double max_position = 2.0;

    [[nodiscard]] bool valid() const noexcept {
        return lookback >= 2 && vol_window >= 10 && vol_target_bps > 0.0
            && max_position > 0.0;
    }
};

/// Position series for time-series momentum.
///
/// The rule: be long when the trailing `lookback` return is positive, short
/// when negative, scaled to a constant volatility. That is the whole strategy,
/// and it is deliberately the plainest form -- the roadmap's Stage 3 asks for
/// one trend-following system, not a good one, and a plain rule is the only
/// kind whose failure can be attributed to the idea rather than the tuning.
[[nodiscard]] inline std::expected<std::vector<double>, StratError>
momentum_positions(const std::vector<double>& r, const MomentumSpec& spec) {
    if (!spec.valid()) { return std::unexpected(StratError::BadParameter); }
    const std::size_t warm = std::max(spec.lookback, spec.vol_window);
    if (r.size() < warm + 100) {
        return std::unexpected(StratError::TooFewSamples);
    }
    std::vector<double> pos(r.size(), 0.0);

    for (std::size_t i = warm; i < r.size(); ++i) {
        // EVERYTHING BELOW USES r[..i-1]. The position is held INTO bar i and
        // earns r[i], which it has not seen. Multiplying a signal by the
        // return that produced it is the error that yields a Sharpe of 8.
        double mom = 0.0;
        for (std::size_t k = i - spec.lookback; k < i; ++k) { mom += r[k]; }

        double mu = 0.0;
        for (std::size_t k = i - spec.vol_window; k < i; ++k) { mu += r[k]; }
        mu /= static_cast<double>(spec.vol_window);
        double s2 = 0.0;
        for (std::size_t k = i - spec.vol_window; k < i; ++k) {
            const double d = r[k] - mu;
            s2 += d * d;
        }
        const double vol = std::sqrt(s2 / static_cast<double>(spec.vol_window - 1));
        if (!(vol > 1e-9)) { continue; }        // no variation: no position

        const double sign = mom > 0.0 ? 1.0 : (mom < 0.0 ? -1.0 : 0.0);
        double p = sign * spec.vol_target_bps / vol;
        if (p > spec.max_position) { p = spec.max_position; }
        if (p < -spec.max_position) { p = -spec.max_position; }
        pos[i] = p;
    }
    return pos;
}

} // namespace altair
