// models/hawkes.hpp -- M22 stable univariate exponential Hawkes process.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <vector>

namespace altair {

enum class HawkesError : std::uint8_t {
    TooFewEvents,
    BadTimestamp,
    BadParameter,
    Unstable,
    NotConverged
};

struct HawkesModel {
    double baseline = 0.0; // events / time
    double alpha = 0.0;    // intensity jump
    double beta = 0.0;     // decay / time
    double log_likelihood = -std::numeric_limits<double>::infinity();

    [[nodiscard]] double branching_ratio() const noexcept { return alpha / beta; }
    [[nodiscard]] double stationary_rate() const noexcept {
        return baseline / (1.0 - branching_ratio());
    }

    [[nodiscard]] double intensity(double time,
                                   std::span<const double> events) const noexcept {
        double value = baseline;
        for (const double event : events) {
            if (event >= time) break;
            value += alpha * std::exp(-beta * (time - event));
        }
        return value;
    }

    /// Time-rescaling residuals: integrated conditional intensity between
    /// events. A correctly specified process maps these to Exp(1).
    [[nodiscard]] std::vector<double>
    residuals(std::span<const double> events) const {
        std::vector<double> out;
        if (events.size() < 2) return out;
        out.reserve(events.size() - 1);
        double excitation_after_previous = alpha;
        for (std::size_t i = 1; i < events.size(); ++i) {
            const double dt = events[i] - events[i - 1];
            const double decay = std::exp(-beta * dt);
            out.push_back(baseline * dt + excitation_after_previous
                         * (1.0 - decay) / beta);
            excitation_after_previous = excitation_after_previous * decay + alpha;
        }
        return out;
    }
};

namespace hawkes_detail {

[[nodiscard]] inline std::expected<double, HawkesError>
log_likelihood(std::span<const double> events, double end,
               double baseline, double alpha, double beta) noexcept {
    if (!(baseline > 0.0) || !(alpha >= 0.0) || !(beta > 0.0)
        || !(alpha < beta) || events.empty() || !(end > events.back()))
        return std::unexpected(alpha >= beta ? HawkesError::Unstable
                                             : HawkesError::BadParameter);
    double ll = 0.0, recursion = 0.0;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (!std::isfinite(events[i]) || events[i] < 0.0
            || (i != 0 && !(events[i] > events[i - 1])))
            return std::unexpected(HawkesError::BadTimestamp);
        if (i != 0) {
            const double dt = events[i] - events[i - 1];
            recursion = std::exp(-beta * dt) * (1.0 + recursion);
        }
        const double lambda = baseline + alpha * recursion;
        if (!(lambda > 0.0) || !std::isfinite(lambda))
            return std::unexpected(HawkesError::BadParameter);
        ll += std::log(lambda);
    }
    double compensator = baseline * end;
    for (const double event : events)
        compensator += (alpha / beta) * (1.0 - std::exp(-beta * (end - event)));
    return ll - compensator;
}

} // namespace hawkes_detail

/// Deterministic bounded grid MLE. It is intentionally modest: offline model
/// selection is reproducible and cannot wander into branching ratio >=1.
[[nodiscard]] inline std::expected<HawkesModel, HawkesError>
fit_hawkes(std::span<const double> events, double end,
           std::size_t beta_points = 32, std::size_t branching_points = 40) {
    if (events.size() < 20 || !(end > 0.0) || beta_points < 4
        || branching_points < 4 || beta_points > 512 || branching_points > 512)
        return std::unexpected(HawkesError::TooFewEvents);
    for (std::size_t i = 0; i < events.size(); ++i)
        if (!std::isfinite(events[i]) || events[i] < 0.0
            || (i != 0 && !(events[i] > events[i - 1])) || events[i] >= end)
            return std::unexpected(HawkesError::BadTimestamp);
    const double rate = static_cast<double>(events.size()) / end;
    HawkesModel best;
    for (std::size_t bi = 0; bi < beta_points; ++bi) {
        const double log_beta = std::log(std::max(rate * 0.05, 1e-4))
            + (std::log(std::max(rate * 20.0, 2e-4))
               - std::log(std::max(rate * 0.05, 1e-4)))
              * static_cast<double>(bi) / static_cast<double>(beta_points - 1);
        const double beta = std::exp(log_beta);
        for (std::size_t ni = 0; ni < branching_points; ++ni) {
            const double branching = 0.95 * static_cast<double>(ni)
                                   / static_cast<double>(branching_points - 1);
            const double baseline = std::max(1e-9, rate * (1.0 - branching));
            const double alpha = branching * beta;
            const auto ll = hawkes_detail::log_likelihood(
                events, end, baseline, alpha, beta);
            if (ll && *ll > best.log_likelihood)
                best = HawkesModel{baseline, alpha, beta, *ll};
        }
    }
    if (!std::isfinite(best.log_likelihood))
        return std::unexpected(HawkesError::NotConverged);
    return best;
}

} // namespace altair
