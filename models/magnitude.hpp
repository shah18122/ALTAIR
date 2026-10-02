// models/magnitude.hpp -- direction is half a forecast; the size of the move is the other half.
//
// WHY. A call right 55 % of the time can still lose: if it is right on the
// small days and wrong on the big ones, the money goes the other way. The
// curriculum's accuracy counts every call alike. This file measures what the
// accuracy hides, and gates calls on what a trade actually earns.
//
// MEASURES, per model:
//   * magnitude-weighted accuracy: sum |r| over right calls / sum |r| over all
//     -- the share of the market's movement the model was on the right side of;
//   * mean |r| on right calls against wrong ones, and the payoff ratio;
//   * gross edge: mean(direction x r), in bp -- what accuracy is worth.
//
// THE GATE. A call right with probability q earns, before costs,
//     q x E[gain | right]  -  (1 - q) x E[loss | wrong].
// The older form, (2q - 1) E|r|, assumed a right call gains exactly what a
// wrong one loses. That is the assumption a model that is right on the small
// days and wrong on the big ones breaks -- the very failure this file exists
// to catch. So the two magnitudes are measured separately, PER MODEL, from
// its own walk-forward calls, in units of the volatility forecast made with
// each call (`WalkForwardPayoff`), and scaled by today's forecast:
//   * q from a walk-forward calibration of the model's own probabilities
//     (`WalkForwardCalibrator`: binned, shrunk toward 1/2, updated only with
//     outcomes already known);
//   * E[gain | right] and E[loss | wrong] as multiples of sigma, shrunk toward
//     sqrt(2/pi) -- the mean absolute value of a normal move, which is what
//     the old gate assumed for both -- by the same pseudo-count.
// AND IT KNOWS HOW SURE IT IS. Each estimate carries a standard error (q's
// from its bin's count, the magnitudes' from their spread); `gate_value`
// combines them by the delta method and the gate opens only when the value
// is positive by `z` standard errors. A thin history keeps the gate shut.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>

namespace altair {

struct MagCall {
    int dir = 0;       ///< +1 up, -1 down, 0 no call
    double r = 0.0;    ///< the realised log return over the call's horizon
};

struct MagnitudeStats {
    std::size_t calls = 0, right = 0, wrong = 0;
    double accuracy = 0.0;            ///< right / (right + wrong); flat outcomes are neither
    double weighted_accuracy = 0.0;   ///< sum |r| right / sum |r| (right + wrong)
    double mean_abs_right_bp = 0.0, mean_abs_wrong_bp = 0.0;
    double payoff_ratio = 0.0;        ///< mean gain on a right call / mean loss on a wrong one
    double gross_bp = 0.0;            ///< mean(dir x r) x 1e4 over every call
};

[[nodiscard]] inline MagnitudeStats magnitude_stats(std::span<const MagCall> calls) noexcept {
    MagnitudeStats s;
    double abs_right = 0.0, abs_wrong = 0.0, edge = 0.0;
    for (const MagCall& c : calls) {
        if (c.dir == 0 || !std::isfinite(c.r)) { continue; }
        ++s.calls;
        edge += static_cast<double>(c.dir) * c.r;
        if (c.r == 0.0) { continue; }
        if ((c.r > 0.0) == (c.dir > 0)) { ++s.right; abs_right += std::fabs(c.r); }
        else { ++s.wrong; abs_wrong += std::fabs(c.r); }
    }
    const double decided = static_cast<double>(s.right + s.wrong);
    if (decided > 0.0) { s.accuracy = static_cast<double>(s.right) / decided; }
    if (abs_right + abs_wrong > 0.0) { s.weighted_accuracy = abs_right / (abs_right + abs_wrong); }
    if (s.right > 0) { s.mean_abs_right_bp = 1e4 * abs_right / static_cast<double>(s.right); }
    if (s.wrong > 0) { s.mean_abs_wrong_bp = 1e4 * abs_wrong / static_cast<double>(s.wrong); }
    if (s.mean_abs_wrong_bp > 0.0) { s.payoff_ratio = s.mean_abs_right_bp / s.mean_abs_wrong_bp; }
    if (s.calls > 0) { s.gross_bp = 1e4 * edge / static_cast<double>(s.calls); }
    return s;
}

/// The probability a call is right, learned only from calls whose outcome is
/// already known. Probabilities are binned 0.50-0.55, ..., 0.95-1.00; each
/// bin's rate is shrunk toward 1/2 by `prior` pseudo-calls, so a bin with five
/// lucky calls does not read as 100 %. Without a probability (a model that
/// only votes), its running overall rate is used, shrunk the same way.
class WalkForwardCalibrator {
public:
    explicit WalkForwardCalibrator(double prior = 20.0) noexcept : prior_(prior) {}

    /// `q`: the model's stated probability that its call is right (NaN: none).
    [[nodiscard]] double calibrated(double q) const noexcept {
        if (!valid(q)) { return shrink(all_right_, all_n_); }
        const std::size_t b = bin(q);
        return shrink(right_[b], n_[b]);
    }
    /// Standard error of calibrated(q): its bin's binomial error at the shrunk count.
    [[nodiscard]] double calibrated_se(double q) const noexcept {
        const double p = calibrated(q);
        const double n = (valid(q) ? n_[bin(q)] : all_n_) + prior_;
        return std::sqrt(p * (1.0 - p) / n);
    }
    void add(double q, bool right) noexcept {
        all_n_ += 1.0;
        all_right_ += right ? 1.0 : 0.0;
        if (!valid(q)) { return; }
        const std::size_t b = bin(q);
        n_[b] += 1.0;
        right_[b] += right ? 1.0 : 0.0;
    }

private:
    /// Not a probability (NaN, or outside [0, 1]): treated as none, never clamped into a bin.
    [[nodiscard]] static bool valid(double q) noexcept { return q >= 0.0 && q <= 1.0; }
    [[nodiscard]] static std::size_t bin(double q) noexcept {
        const double c = q < 0.5 ? 1.0 - q : q;   // a probability below 1/2 is the other call's
        const auto b = static_cast<std::size_t>((c - 0.5) / 0.05);
        // RULE 11: proven -- valid() bounds c to [0.5, 1], so b <= 10, and 10 only at
        // c == 1 exactly, which belongs in the top bin (0.95-1.00).
        return b >= kBins ? kBins - 1 : b;
    }
    [[nodiscard]] double shrink(double right, double n) const noexcept { return (right + 0.5 * prior_) / (n + prior_); }

    static constexpr std::size_t kBins = 10;
    double prior_;
    std::array<double, kBins> n_{}, right_{};
    double all_n_ = 0.0, all_right_ = 0.0;
};

/// The mean absolute value of a normal move with standard deviation `sigma`.
[[nodiscard]] inline double expected_abs_move(double sigma) noexcept {
    return sigma * std::sqrt(2.0 / 3.14159265358979323846);
}

/// The symmetric value (2q - 1) x E|r| - cost, in bp: what a call is worth
/// IF right and wrong calls move alike. Kept for comparison; the gate is
/// `gate_value`, which does not assume it.
[[nodiscard]] inline double call_value_bp(double q, double expected_abs_bp, double cost_bp) noexcept {
    return (2.0 * q - 1.0) * expected_abs_bp - cost_bp;
}

/// What a model's right calls gained and its wrong calls lost, as multiples
/// of the volatility forecast made with each call, learned only from outcomes
/// already known. Each mean is shrunk toward sqrt(2/pi) -- E|z| for a normal
/// move -- by `prior` pseudo-calls, with that prior's variance (1 - 2/pi), so
/// a model with ten calls reads as "a normal move, roughly" rather than as
/// whatever its ten days happened to be.
class WalkForwardPayoff {
public:
    static constexpr double kNormalAbs = 0.79788456080286535588;   ///< sqrt(2/pi)
    static constexpr double kNormalAbsVar = 0.36338022763241865693;   ///< 1 - 2/pi

    explicit WalkForwardPayoff(double prior = 20.0) noexcept : prior_(prior) {}

    /// One resolved call: right or wrong, and |r| / sigma (the forecast at the call).
    void add(bool right, double abs_move_in_sigmas) noexcept {
        if (!std::isfinite(abs_move_in_sigmas) || abs_move_in_sigmas < 0.0) { return; }
        Side& s = right ? right_ : wrong_;
        s.n += 1.0;
        s.sum += abs_move_in_sigmas;
        s.ss += abs_move_in_sigmas * abs_move_in_sigmas;
    }

    struct Estimate {
        double gain = 0.0, loss = 0.0;         ///< in sigmas: E[|r|/sigma | right], E[|r|/sigma | wrong]
        double gain_se = 0.0, loss_se = 0.0;   ///< their standard errors
        double n_right = 0.0, n_wrong = 0.0;   ///< resolved calls behind them
    };
    [[nodiscard]] Estimate estimate() const noexcept {
        Estimate e;
        shrunk(right_, e.gain, e.gain_se);
        shrunk(wrong_, e.loss, e.loss_se);
        e.n_right = right_.n;
        e.n_wrong = wrong_.n;
        return e;
    }

private:
    struct Side { double n = 0.0, sum = 0.0, ss = 0.0; };
    void shrunk(const Side& s, double& mean, double& se) const noexcept {
        const double n = s.n + prior_;
        mean = (s.sum + prior_ * kNormalAbs) / n;
        // Pooled second moment, prior included: E[x^2] of |z| is 1.
        const double m2 = (s.ss + prior_ * 1.0) / n;
        const double var = std::max(m2 - mean * mean, 0.0);
        se = std::sqrt(var / n);
    }
    double prior_;
    Side right_{}, wrong_{};
};

/// What the gate saw, in bp.
struct GateValue {
    double q = 0.5, q_se = 0.0;
    double gain_bp = 0.0, loss_bp = 0.0;   ///< expected gain if right, loss if wrong, at today's sigma
    double cost_bp = 0.0;
    double value_bp = 0.0;                 ///< q x gain - (1 - q) x loss - cost
    double se_bp = 0.0;                    ///< its standard error (delta method)
    double lower_bp = 0.0;                 ///< value - z x se: the gate opens only when this is > 0
    [[nodiscard]] bool open() const noexcept { return lower_bp > 0.0; }
};

/// The conditional value of a call and its lower bound at `z` standard errors.
/// `sigma_bp` is today's volatility forecast for the horizon, in bp.
[[nodiscard]] inline GateValue gate_value(double q, double q_se, const WalkForwardPayoff::Estimate& pay,
                                          double sigma_bp, double cost_bp, double z = 1.0) noexcept {
    GateValue g;
    g.q = q;
    g.q_se = q_se;
    g.gain_bp = pay.gain * sigma_bp;
    g.loss_bp = pay.loss * sigma_bp;
    g.cost_bp = cost_bp;
    g.value_bp = q * g.gain_bp - (1.0 - q) * g.loss_bp - cost_bp;
    const double dq = g.gain_bp + g.loss_bp;   // d value / d q
    const double var = dq * dq * q_se * q_se + q * q * pay.gain_se * pay.gain_se * sigma_bp * sigma_bp
                     + (1.0 - q) * (1.0 - q) * pay.loss_se * pay.loss_se * sigma_bp * sigma_bp;
    g.se_bp = std::sqrt(var);
    g.lower_bp = g.value_bp - z * g.se_bp;
    return g;
}

} // namespace altair
