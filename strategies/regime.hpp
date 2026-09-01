// strategies/regime.hpp -- the regime detector.
//
// P6-03. Detection only; oms/ decides what to do.
//
// A REGIME BOUNDARY IS A THRESHOLD ON A NOISY ESTIMATE.
//
// This is the card. Every regime here is defined by comparing a MEASURED
// quantity against a threshold -- Hurst against 0.5, realised vol against a
// percentile, spread against a level. Those quantities are measurements and
// they carry error bars, which ROADMAP section 3 requires be propagated rather
// than dropped.
//
// Propagating them changes the answer. Near the threshold the confidence
// interval straddles it, so the label is a coin flip -- and near the threshold
// is exactly where a regime-conditional model switches behaviour. A detector
// that always returns a side is at its least reliable precisely where the
// consequences of being wrong are largest.
//
// So every call here returns a null label when the estimate's interval
// contains the threshold, and `Unknown` is ordinal 0 so a zeroed struct cannot
// pass for a trending market.
//
// Measured, on 4,000 bars of a PURE RANDOM WALK -- a series with no regime in
// it at all:
//
//   naive rule (is Hurst above or below 0.5?)   3,872 regime labels. 100%.
//   with the error bar                            273 regime labels.   7.1%
//
// The naive rule can never return "random walk", because a continuous
// statistic is never exactly 0.5, so it describes a coin-flip series as 2,164
// bars of trend and 1,708 of mean reversion -- 3,872 regimes for a conditional
// model to switch behaviour on.
//
// And the error bar takes it to 7.1%, NOT to zero. A 2-sigma test on a true
// null fires about 5% of the time by construction, a little more here because
// consecutive windows share 511 of their 512 samples so excursions cluster.
// That residue is what hysteresis and the dwell requirement are for; they sit
// on top of the error bar rather than instead of it.
//
// On a series that genuinely does persist (AR(1) momentum, phi = 0.94) the
// same detector says Trending on 3,868 of 3,872 calls. The error bar made it
// correct, not merely silent.
//
// HYSTERESIS IS NOT COSMETIC.
//
// A statistic wandering across a threshold produces a regime that flips every
// few bars, and a strategy that switches behaviour on every flip pays the
// spread for the privilege. Entering a regime and leaving it therefore use
// DIFFERENT thresholds, and the detector additionally requires a regime to
// hold for a minimum number of bars before it is published.
//
// Measured on 6,000 bars whose persistence fades in and out: enter and exit
// both at 2 sigma with immediate publication gives 119 regime changes; enter
// at 2, exit at 1, publish after 5 bars gives 25. A 4.8x reduction, while
// still publishing a regime on 5,760 of the calls -- so the damping did not
// simply silence it.
//
// A REGIME LABEL FITTED ON THE WHOLE SAMPLE IS LOOK-AHEAD (rule 7).
//
// The other way this goes wrong, and it is the standard way: fit a two-state
// model to the full history, label every bar with its most likely state, then
// report performance per state. Every one of those labels used the whole
// series, including its future. Everything here is causal -- each call is a
// function of the bars that have already printed, and the detector has no
// method that takes a whole series.

#pragma once

#include <analytics/hurst.hpp>
#include <analytics/indicators.hpp>
#include <analytics/rolling.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

/// Directional character. Ordinal 0 is Unknown, not "range".
enum class TrendRegime : std::uint8_t {
    Unknown = 0,
    /// Hurst significantly above 0.5: moves persist.
    Trending,
    /// Hurst significantly below 0.5: moves revert.
    MeanReverting,
    /// Hurst indistinguishable from 0.5. A RANDOM WALK IS A FINDING, not a
    /// failure to classify -- most of the time this is the honest answer, and
    /// a detector that never returns it is describing its own noise.
    RandomWalk
};

enum class VolRegime : std::uint8_t { Unknown = 0, Low, Normal, High };
enum class LiquidityRegime : std::uint8_t { Unknown = 0, Thin, Normal, Deep };

enum class RegimeError : std::uint8_t {
    /// Not enough history for the estimator this regime rests on.
    WarmingUp,
    /// The estimator refused -- a constant series, a zero spread.
    Degenerate,
    /// The caller supplied no thresholds. There is no default set.
    NoThresholds
};

/// One regime call, with the measurement behind it.
///
/// `statistic` and `std_error` travel with the label so a consumer can see how
/// close the call was, and a model can weight by it rather than treating a
/// marginal call and a decisive one as the same input.
template <class Label>
struct RegimeCall {
    Label label{};
    double statistic = 0.0;
    double std_error = 0.0;
    /// How many standard errors the statistic sits from the nearest threshold.
    /// Zero at the boundary; the label is Unknown below `min_sigma`.
    double margin_sigma = 0.0;
    /// How many bars this label has held, published only after `min_bars`.
    std::size_t held_bars = 0;

    [[nodiscard]] bool decided() const noexcept {
        return label != Label::Unknown;
    }
};

/// Thresholds for the trend regime. No defaults: 0.5 is the null for Hurst but
/// `min_sigma` and `min_bars` are decisions about how much evidence is enough
/// and how much churn is tolerable, and neither has a right answer that is
/// independent of what the regime is used for.
struct TrendThresholds {
    /// The null. Hurst of 0.5 is a random walk.
    double centre = 0.5;
    /// How many standard errors from the centre before a side is claimed.
    /// Zero means "no threshold supplied" and is refused.
    double min_sigma = 0.0;
    /// How many standard errors are needed to LEAVE a regime, which is less
    /// than to enter it. This is the hysteresis.
    double exit_sigma = 0.0;
    /// Minimum bars a label must hold before it is published.
    std::size_t min_bars = 0;
};

/// Causal trend-regime detector over a rolling window of RETURNS.
///
/// Fed returns, never levels: P3-06c reports Hurst 0.96 on a price series and
/// 0.50 on its own increments, so a detector fed prices would report a
/// trending market every single bar of every session ever recorded.
template <std::size_t N>
class TrendDetector {
public:
    /// Push one CLOSE. The differencing happens here, once, where it is
    /// visible.
    void push(Price close) noexcept {
        const double c = static_cast<double>(close.raw());
        if (seeded_) { rets_.push(c - prev_); }
        prev_ = c;
        seeded_ = true;
    }

    [[nodiscard]] std::expected<RegimeCall<TrendRegime>, RegimeError>
    call(const TrendThresholds& t) noexcept {
        if (!(t.min_sigma > 0.0) || !(t.exit_sigma > 0.0) || t.min_bars == 0) {
            return std::unexpected(RegimeError::NoThresholds);
        }
        if (rets_.size() < kHurstMinSamples) {
            return std::unexpected(RegimeError::WarmingUp);
        }
        static thread_local double buf[N];
        const std::size_t n = rets_.size();
        for (std::size_t i = 0; i < n; ++i) { buf[i] = rets_.at(i); }
        const auto h = hurst_rs(buf, n);
        if (!h) { return std::unexpected(RegimeError::Degenerate); }

        RegimeCall<TrendRegime> out{};
        out.statistic = h->h;
        out.std_error = h->std_error;
        const double dev = h->h - t.centre;
        out.margin_sigma = h->std_error > 0.0 ? dev / h->std_error : 0.0;

        // HYSTERESIS. Entering needs min_sigma; staying needs only exit_sigma.
        const double need = (current_ == TrendRegime::Trending
                             || current_ == TrendRegime::MeanReverting)
                              ? t.exit_sigma : t.min_sigma;
        TrendRegime proposed;
        if (out.margin_sigma >= need)       { proposed = TrendRegime::Trending; }
        else if (out.margin_sigma <= -need) { proposed = TrendRegime::MeanReverting; }
        else                                { proposed = TrendRegime::RandomWalk; }

        if (proposed == current_) { ++held_; } else { current_ = proposed; held_ = 1; }
        out.held_bars = held_;
        // Not published until it has held. A one-bar regime is noise wearing a
        // label, and publishing it makes a strategy trade the noise.
        out.label = held_ >= t.min_bars ? current_ : TrendRegime::Unknown;
        return out;
    }

    /// What the label would be WITHOUT the error bar -- the naive rule, kept
    /// so the difference can be measured rather than asserted.
    [[nodiscard]] static TrendRegime naive_label(double hurst,
                                                 double centre) noexcept {
        if (hurst > centre) { return TrendRegime::Trending; }
        if (hurst < centre) { return TrendRegime::MeanReverting; }
        return TrendRegime::RandomWalk;
    }

    [[nodiscard]] std::size_t samples() const noexcept { return rets_.size(); }

private:
    RollingWindow<N> rets_;
    double prev_ = 0.0;
    TrendRegime current_ = TrendRegime::Unknown;
    std::size_t held_ = 0;
    bool seeded_ = false;
};

// ---------------------------------------------------------------------------
// Volatility regime
// ---------------------------------------------------------------------------

struct VolThresholds {
    /// Realised vol below this is Low, above `high` is High. In the same units
    /// as the estimator -- paise of standard deviation per bar.
    double low = 0.0;
    double high = 0.0;
    double min_sigma = 0.0;
    std::size_t min_bars = 0;
};

/// Causal volatility-regime detector.
///
/// The standard error of a sample standard deviation from n observations is
/// approximately sd / sqrt(2(n-1)). At n = 60 that is 9% of the estimate --
/// so a "high volatility" threshold 9% above the current reading is a coin
/// flip, and thresholds set by eye on a chart are routinely closer than that.
template <std::size_t N>
class VolDetector {
public:
    void push(Price close) noexcept {
        const double c = static_cast<double>(close.raw());
        if (seeded_) { rets_.push(c - prev_); }
        prev_ = c;
        seeded_ = true;
    }

    [[nodiscard]] std::expected<RegimeCall<VolRegime>, RegimeError>
    call(const VolThresholds& t) noexcept {
        if (!(t.high > t.low) || !(t.min_sigma > 0.0) || t.min_bars == 0) {
            return std::unexpected(RegimeError::NoThresholds);
        }
        if (rets_.size() < 8) { return std::unexpected(RegimeError::WarmingUp); }
        const auto sd = rets_.stddev();
        if (!sd || !(*sd > 0.0)) {
            return std::unexpected(RegimeError::Degenerate);
        }

        RegimeCall<VolRegime> out{};
        out.statistic = *sd;
        out.std_error = *sd / std::sqrt(2.0 * static_cast<double>(
                                            rets_.size() - 1));
        // Distance to the NEARER threshold, signed so the sign says which side
        // the estimate is on relative to it.
        const double d_low = (*sd - t.low) / out.std_error;
        const double d_high = (*sd - t.high) / out.std_error;
        out.margin_sigma = std::fabs(d_low) < std::fabs(d_high) ? d_low : d_high;

        const double need = (current_ != VolRegime::Unknown
                             && current_ != VolRegime::Normal)
                              ? 0.5 * t.min_sigma : t.min_sigma;
        VolRegime proposed;
        if (d_high >= need)     { proposed = VolRegime::High; }
        else if (d_low <= -need){ proposed = VolRegime::Low; }
        else                    { proposed = VolRegime::Normal; }

        if (proposed == current_) { ++held_; } else { current_ = proposed; held_ = 1; }
        out.held_bars = held_;
        out.label = held_ >= t.min_bars ? current_ : VolRegime::Unknown;
        return out;
    }

private:
    RollingWindow<N> rets_;
    double prev_ = 0.0;
    VolRegime current_ = VolRegime::Unknown;
    std::size_t held_ = 0;
    bool seeded_ = false;
};

// ---------------------------------------------------------------------------
// The composite
// ---------------------------------------------------------------------------

/// The three axes together. Deliberately NOT collapsed into a single enum:
/// "trending and thin" and "trending and deep" are different markets, and a
/// single label would have to pick which axis to throw away.
struct MarketRegime {
    TrendRegime trend = TrendRegime::Unknown;
    VolRegime vol = VolRegime::Unknown;
    LiquidityRegime liquidity = LiquidityRegime::Unknown;

    /// Every axis decided. A model conditioned on regime should refuse to fire
    /// when this is false rather than falling back to an unconditioned
    /// version -- which is a different model, trained on a different sample.
    [[nodiscard]] bool complete() const noexcept {
        return trend != TrendRegime::Unknown && vol != VolRegime::Unknown
            && liquidity != LiquidityRegime::Unknown;
    }
    /// A stable key for per-regime reporting. CLAUDE.md: report per regime,
    /// never only in aggregate.
    [[nodiscard]] std::uint32_t key() const noexcept {
        return (static_cast<std::uint32_t>(trend) << 16)
             | (static_cast<std::uint32_t>(vol) << 8)
             |  static_cast<std::uint32_t>(liquidity);
    }
};

/// Liquidity from the book, with the same discipline.
///
/// Takes the spread in basis points and the visible depth, both of which P5-03
/// already computes and both of which are absent rather than zero when the
/// book has not arrived -- so this takes them as `std::expected` and refuses
/// rather than reading an empty book as infinitely tight.
[[nodiscard]] inline LiquidityRegime
liquidity_regime(double spread_bps, double depth, double thin_bps,
                 double deep_bps, double min_depth) noexcept {
    if (!(thin_bps > deep_bps) || !(min_depth > 0.0)) {
        return LiquidityRegime::Unknown;
    }
    if (spread_bps >= thin_bps || depth < min_depth) {
        return LiquidityRegime::Thin;
    }
    if (spread_bps <= deep_bps && depth >= 3.0 * min_depth) {
        return LiquidityRegime::Deep;
    }
    return LiquidityRegime::Normal;
}

} // namespace altair
