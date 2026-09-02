// flagging/scorecard.hpp -- per-model, per-horizon, per-regime scorecards and
// the session-close weight update.
//
// P9-01 and P9-02.
//
// AN AGGREGATE SCORE HIDES THE FAILURE IT IS AVERAGING OVER.
//
// This is the card, and CLAUDE.md states it as a rule: "Report per regime,
// never only in aggregate."
//
// A model with an information coefficient of +0.6 in trending markets and
// -0.6 in ranging ones has an aggregate IC of approximately zero. Read as one
// number it looks like a model with no edge, which is a shrug. Read per regime
// it is a model with a large edge and a large ANTI-edge, which is two
// actionable facts: trade it when the regime detector says trending, and
// either invert or disable it otherwise.
//
// Measured on one model over 800 observations split across two regimes:
//
//     IC when TRENDING        +0.7375
//     IC when MEAN-REVERTING  -0.7561
//     AGGREGATE IC            +0.0131   <- the number most scorecards report
//     IC SPREAD                1.4937
//
// The aggregate is not a summary of those two facts. It is the arithmetic that
// destroys them, and no amount of staring at it recovers what it averaged. The
// SPREAD is the one number that says an aggregate is hiding something: a
// uniformly mediocre model has the same +0.01 and a small spread.
//
// So every score is filed under (model, horizon, regime) and there is no
// method that returns an all-regime number without also returning the cell
// counts -- because the second thing an aggregate hides is how thin the cells
// are.
//
// THE THIN CELLS ARE THE ONES YOU MOST NEED.
//
// Split a hundred observations across four regimes and the extreme ones get
// ten each. Those are the regimes where a model's behaviour matters most and
// where its score is least certain, and an update rule that treats a ten-
// observation cell like a thousand-observation one will thrash on noise.
//
// So the weight update is SHRUNK BY SAMPLE SIZE: the step is scaled by
// n / (n + k), which is zero at no evidence and approaches one as the cell
// fills. Measured with k = 50, on the SAME terrible IC from two cells:
//
//     5 observations      shrinkage 0.0909   weight 0.500 -> 0.4548
//     2000 observations   shrinkage 0.9756   weight 0.500 -> 0.0240
//
// The thin cell barely moves and the full one moves a long way. That is what
// stops the first three observations of a new regime from halving a weight.

#pragma once

#include <strategies/regime.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

inline constexpr std::size_t kMaxScoredModels = 32;
inline constexpr std::size_t kMaxHorizons = 4;
/// Trend x vol, the two axes a scorecard splits on. Liquidity is carried on
/// the regime but not split on, because splitting on three axes turns a
/// hundred observations into cells of eight.
inline constexpr std::size_t kScoreRegimes = 16;

enum class ScoreError : std::uint8_t {
    /// The model or horizon index is out of range.
    OutOfRange,
    /// The regime was not fully decided -- P6-03 refuses to guess and so does
    /// this.
    RegimeIncomplete,
    /// Fewer observations than the statistic needs.
    TooFewObservations,
    /// The shrinkage constant was not supplied.
    NoShrinkage,
    /// The horizon was never registered.
    UnknownHorizon
};

/// One (model, horizon, regime) cell.
///
/// Everything is streaming: no observation is stored, so a session's worth of
/// scoring costs a fixed number of bytes per cell and the cost does not grow
/// with the session.
struct ScoreCell {
    /// Sum of forecast*realised, and the two second moments. Enough for a
    /// correlation without keeping the samples.
    double sum_fr = 0.0;
    double sum_ff = 0.0;
    double sum_rr = 0.0;
    double sum_f = 0.0;
    double sum_r = 0.0;
    /// How often the sign was right.
    std::size_t hits = 0;
    std::size_t n = 0;

    void observe(double forecast, double realised) noexcept {
        sum_fr += forecast * realised;
        sum_ff += forecast * forecast;
        sum_rr += realised * realised;
        sum_f += forecast;
        sum_r += realised;
        if ((forecast > 0.0) == (realised > 0.0)) { ++hits; }
        ++n;
    }

    /// Information coefficient: the correlation between forecast and realised.
    ///
    /// Refuses below four observations rather than returning the +/-1 that two
    /// points always produce -- a correlation from two points is a line through
    /// two points, exactly as P5-04 says of a beta.
    [[nodiscard]] std::expected<double, ScoreError> ic() const noexcept {
        if (n < 4) { return std::unexpected(ScoreError::TooFewObservations); }
        const double dn = static_cast<double>(n);
        const double cov = sum_fr / dn - (sum_f / dn) * (sum_r / dn);
        const double vf = sum_ff / dn - (sum_f / dn) * (sum_f / dn);
        const double vr = sum_rr / dn - (sum_r / dn) * (sum_r / dn);
        if (!(vf > 0.0) || !(vr > 0.0)) {
            return std::unexpected(ScoreError::TooFewObservations);
        }
        return cov / std::sqrt(vf * vr);
    }

    /// The IC's own standard error, approximately 1/sqrt(n-1).
    ///
    /// Carried because a scorecard without it invites comparing a 0.15 from
    /// twelve observations against a 0.10 from nine hundred.
    [[nodiscard]] double ic_std_error() const noexcept {
        return n < 2 ? 1.0 : 1.0 / std::sqrt(static_cast<double>(n) - 1.0);
    }
    [[nodiscard]] std::expected<double, ScoreError> hit_rate() const noexcept {
        if (n == 0) { return std::unexpected(ScoreError::TooFewObservations); }
        return static_cast<double>(hits) / static_cast<double>(n);
    }
};

/// Which cell a (regime) pair maps to. Trend x vol, both from P6-03.
[[nodiscard]] inline std::expected<std::size_t, ScoreError>
regime_cell(const MarketRegime& r) noexcept {
    if (r.trend == TrendRegime::Unknown || r.vol == VolRegime::Unknown) {
        return std::unexpected(ScoreError::RegimeIncomplete);
    }
    return static_cast<std::size_t>(r.trend) * 4u
         + static_cast<std::size_t>(r.vol);
}

/// The scorecard store.
///
/// Fixed size, no allocation, and indexed by (model, horizon, regime cell).
class Scorecards {
public:
    [[nodiscard]] std::expected<void, ScoreError>
    observe(std::size_t model, std::size_t horizon, const MarketRegime& r,
            double forecast, double realised) noexcept {
        if (model >= kMaxScoredModels || horizon >= kMaxHorizons) {
            return std::unexpected(ScoreError::OutOfRange);
        }
        const auto cell = regime_cell(r);
        if (!cell) { return std::unexpected(cell.error()); }
        at(model, horizon, *cell).observe(forecast, realised);
        // The aggregate is ALSO maintained, because it is genuinely wanted --
        // just never on its own. `aggregate()` returns it with the cell counts
        // attached so a reader cannot quote it without seeing what it hid.
        agg_[model][horizon].observe(forecast, realised);
        return {};
    }

    [[nodiscard]] const ScoreCell& at(std::size_t model, std::size_t horizon,
                                      std::size_t cell) const noexcept {
        return cells_[model][horizon][cell];
    }
    [[nodiscard]] ScoreCell& at(std::size_t model, std::size_t horizon,
                                std::size_t cell) noexcept {
        return cells_[model][horizon][cell];
    }

    /// The all-regime number, WITH the per-cell counts.
    ///
    /// The counts are not decoration. Returning them is what makes it
    /// impossible to quote the aggregate without the reader seeing that it was
    /// built from cells of 412 and 9.
    struct Aggregate {
        ScoreCell overall{};
        std::size_t cell_n[kScoreRegimes] = {};
        std::size_t populated = 0;
        std::size_t thinnest = 0;
    };
    [[nodiscard]] Aggregate aggregate(std::size_t model,
                                      std::size_t horizon) const noexcept {
        Aggregate a{};
        a.overall = agg_[model][horizon];
        a.thinnest = static_cast<std::size_t>(-1);
        for (std::size_t c = 0; c < kScoreRegimes; ++c) {
            a.cell_n[c] = cells_[model][horizon][c].n;
            if (a.cell_n[c] > 0) {
                ++a.populated;
                if (a.cell_n[c] < a.thinnest) { a.thinnest = a.cell_n[c]; }
            }
        }
        if (a.populated == 0) { a.thinnest = 0; }
        return a;
    }

    /// The widest gap between any two populated cells' ICs.
    ///
    /// The single number that says "this aggregate is hiding something". A
    /// model with +0.6 and -0.6 has a spread of 1.2 and an aggregate near
    /// zero; a model that is uniformly mediocre has a small spread and the
    /// same aggregate.
    [[nodiscard]] std::expected<double, ScoreError>
    ic_spread(std::size_t model, std::size_t horizon) const noexcept {
        double lo = 1e308, hi = -1e308;
        std::size_t found = 0;
        for (std::size_t c = 0; c < kScoreRegimes; ++c) {
            const auto v = cells_[model][horizon][c].ic();
            if (!v) { continue; }
            if (*v < lo) { lo = *v; }
            if (*v > hi) { hi = *v; }
            ++found;
        }
        if (found < 2) {
            return std::unexpected(ScoreError::TooFewObservations);
        }
        return hi - lo;
    }

private:
    ScoreCell cells_[kMaxScoredModels][kMaxHorizons][kScoreRegimes] = {};
    ScoreCell agg_[kMaxScoredModels][kMaxHorizons] = {};
};

// ---------------------------------------------------------------------------
// P9-02: the session-close weight update
// ---------------------------------------------------------------------------

/// How aggressively a session's evidence moves a weight.
struct UpdatePolicy {
    /// Step size at full confidence. Zero means no update, which is a choice.
    double rate = 0.0;
    /// The shrinkage constant k in n/(n+k). Larger means more evidence is
    /// needed before a cell moves its weight at all. No default: how much
    /// evidence is enough depends on how expensive being wrong is, which this
    /// file does not know.
    double shrink_k = 0.0;
    /// Weights are clamped here. A model is never given more than this share
    /// or driven below the floor, because a de-weighted model that reaches
    /// exactly zero can never earn its way back -- it stops being scored.
    double min_weight = 0.0;
    double max_weight = 1.0;

    [[nodiscard]] bool valid() const noexcept {
        return rate > 0.0 && shrink_k > 0.0 && max_weight > min_weight
            && min_weight > 0.0;
    }
};

/// One session's update to one (model, horizon, regime) weight.
///
/// The step is `rate * shrinkage * ic`, where shrinkage is n/(n+k). At n=0 the
/// step is exactly zero; at n=k it is half; it approaches the full rate only
/// as the cell fills. That is what stops the first three observations of a new
/// regime from halving a weight.
[[nodiscard]] inline std::expected<double, ScoreError>
update_weight(double current, const ScoreCell& cell,
              const UpdatePolicy& p) noexcept {
    if (!p.valid()) { return std::unexpected(ScoreError::NoShrinkage); }
    const auto ic = cell.ic();
    // No usable IC means NO UPDATE. Not a downward one: an absence of evidence
    // is not evidence of failure, and treating it as such de-weights every
    // model on the first day of a regime it has never seen.
    if (!ic) { return current; }

    const double n = static_cast<double>(cell.n);
    const double shrink = n / (n + p.shrink_k);
    double next = current + p.rate * shrink * *ic;
    if (next < p.min_weight) { next = p.min_weight; }
    if (next > p.max_weight) { next = p.max_weight; }
    return next;
}

/// The shrinkage factor on its own, so a caller can see how much of a session
/// actually landed.
[[nodiscard]] inline double shrinkage(std::size_t n, double k) noexcept {
    if (!(k > 0.0)) { return 0.0; }
    const double dn = static_cast<double>(n);
    return dn / (dn + k);
}

} // namespace altair
