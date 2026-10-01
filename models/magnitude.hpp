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
// THE GATE. A call with probability q of being right, on a move of expected
// size E|r|, earns (2q - 1) E|r| - cost before anything else. So take it only
// when that is positive. The Tradability sheet applies this once, with the
// HISTORY's average E|r|; here both halves are forecast PER CALL, from data
// known when the call is made:
//   * q from a walk-forward calibration of the model's own probabilities
//     (`WalkForwardCalibrator`: binned, shrunk toward 1/2, updated only with
//     outcomes already known);
//   * E|r| from a volatility forecast (HAR, analytics/har_rv.hpp) as
//     sigma x sqrt(2/pi), the mean absolute value of a normal move.
// On a quiet day the gate demands near-certainty; on a volatile one, a modest
// edge is enough -- which is how a desk sizes a directional view.

#pragma once

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

/// What a call is worth before it is taken: (2q - 1) x E|r| - cost, all in bp.
[[nodiscard]] inline double call_value_bp(double q, double expected_abs_bp, double cost_bp) noexcept {
    return (2.0 * q - 1.0) * expected_abs_bp - cost_bp;
}

} // namespace altair
