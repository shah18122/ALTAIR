// models/aggregator.hpp -- per-model, per-timeframe aggregation, and the
// confidence interval a size is derived from.
//
// P8-11 and P8-12. The last two cards of the ML stack, and the ones that
// decide what any of the rest of it is worth.
//
// TWO FORECASTS AT DIFFERENT HORIZONS ARE NOT TWO OPINIONS ABOUT ONE THING.
//
// This is the card. A model trained on a 10-minute forward return and one
// trained on a 1-day forward return produce numbers of the same type, in the
// same units, on the same instrument, at the same instant. Averaging them is
// one line of code and it is meaningless: they are estimates of DIFFERENT
// QUANTITIES, and the average is an estimate of neither.
//
// The size of the error is not subtle. Under a random walk a return scales
// with the square root of the horizon, so a 1-day forecast on a 375-minute
// session is about six times the magnitude of a 10-minute one for the same
// underlying signal strength. An unweighted mean is therefore dominated by the
// longest-horizon member regardless of which member is any good, and adding a
// short-horizon model to the ensemble barely moves the answer. `tests/`
// measures that.
//
// So every forecast carries its horizon, `Aggregator` refuses to combine
// mismatched ones, and combining across horizons requires an explicit
// rescaling that says what it is doing.
//
// FIVE MODELS ON THE SAME FEATURES ARE NOT FIVE VOTES.
//
// P6-04 measured this for signals: ten members of an EMA stack carried 1.25
// signals' worth of information. Model outputs are worse, because the models
// share not only their features but their training data and their labels. The
// aggregator therefore takes the same measured mean correlation and applies
// the same n_eff correction -- an ensemble's standard error is not
// sigma/sqrt(k).
//
// SIZE ON THE LOWER BOUND, AND THE BOUND INCLUDES THE WEIGHTS' OWN ERROR.
//
// P8-12. ROADMAP section 3 sizes on the lower confidence bound of edge, and
// there are two sources of uncertainty, not one: each model's own standard
// error, and the estimation error of the ensemble WEIGHTS, which were fitted
// on a finite sample like everything else. An interval that includes only the
// first is too tight by however much the weights were fitted on.

#pragma once

#include <models/serving.hpp>
#include <strategies/score.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

inline constexpr std::size_t kMaxEnsemble = 16;

enum class AggregateError : std::uint8_t {
    /// Members disagree about the forecast horizon.
    HorizonMismatch,
    /// No members were ready.
    NoMembers,
    /// The correlation among members was not supplied.
    NoCorrelation,
    /// The weights do not sum to something usable.
    BadWeights,
    /// The weights' own estimation error was not supplied.
    NoWeightError,
    /// Members disagree about the feature registry.
    FeatureVersionMismatch
};

/// One member's contribution.
struct Member {
    Forecast forecast{};
    /// The horizon this model was TRAINED on, in bars. Not the horizon anyone
    /// wants to trade -- the one the labels were built with (P8-01).
    std::size_t horizon = 0;
    double weight = 0.0;
    bool ready = false;
};

/// The ensemble's answer.
struct Aggregate {
    double value = 0.0;
    /// Standard error WITH the correlation inflation and the weight-estimation
    /// term. Never sigma/sqrt(k).
    double std_error = 0.0;
    /// What a naive independent-members calculation would have claimed.
    /// Carried for comparison; never decided on.
    double naive_std_error = 0.0;
    double n_effective = 0.0;
    std::size_t members = 0;
    std::size_t horizon = 0;
    std::uint64_t feature_version = 0;

    /// value - k * std_error, toward zero. THE number a size derives from.
    [[nodiscard]] double lower_bound(double k) const noexcept {
        const double mag = std::fabs(value) - k * std_error;
        return mag > 0.0 ? mag : 0.0;
    }
    [[nodiscard]] int direction() const noexcept {
        return value > 0.0 ? 1 : (value < 0.0 ? -1 : 0);
    }
};

/// Rescale a forecast from one horizon to another under a square-root rule.
///
/// EXPLICIT, and the only way to combine across horizons. The square-root rule
/// assumes independent increments, which is exactly the assumption a
/// forecasting model exists to violate -- so this is an approximation, it is
/// named as one, and using it silently inside the aggregator would hide that.
[[nodiscard]] inline std::expected<double, AggregateError>
rescale_horizon(double value, std::size_t from, std::size_t to) noexcept {
    if (from == 0 || to == 0) {
        return std::unexpected(AggregateError::HorizonMismatch);
    }
    return value * std::sqrt(static_cast<double>(to)
                             / static_cast<double>(from));
}

/// Combine members that agree on horizon and feature registry.
///
/// `mean_correlation` is MEASURED across the members' historical forecasts,
/// exactly as in P6-04. `weight_std_error` is the standard error of the fitted
/// ensemble weights; both are required because assuming either is zero
/// produces an interval that is too tight in a way nothing downstream can
/// detect.
[[nodiscard]] inline std::expected<Aggregate, AggregateError>
aggregate(const Member* m, std::size_t k, double mean_correlation,
          double weight_std_error) noexcept {
    if (k == 0 || k > kMaxEnsemble) {
        return std::unexpected(AggregateError::NoMembers);
    }
    if (!(mean_correlation >= 0.0) || !(mean_correlation < 1.0)) {
        return std::unexpected(AggregateError::NoCorrelation);
    }
    if (!(weight_std_error >= 0.0)) {
        return std::unexpected(AggregateError::NoWeightError);
    }

    std::size_t ready = 0, horizon = 0;
    std::uint64_t fv = 0;
    double sw = 0.0, acc = 0.0, var = 0.0;
    for (std::size_t i = 0; i < k; ++i) {
        if (!m[i].ready) { continue; }
        if (m[i].horizon == 0) {
            return std::unexpected(AggregateError::HorizonMismatch);
        }
        if (ready == 0) {
            horizon = m[i].horizon;
            fv = m[i].forecast.feature_version;
        } else {
            // THE CHECK. Two models at different horizons estimate different
            // quantities; the mean of them estimates neither.
            if (m[i].horizon != horizon) {
                return std::unexpected(AggregateError::HorizonMismatch);
            }
            if (m[i].forecast.feature_version != fv) {
                return std::unexpected(AggregateError::FeatureVersionMismatch);
            }
        }
        const double w = m[i].weight;
        sw += w < 0.0 ? -w : w;
        acc += w * m[i].forecast.value;
        const double c = w * m[i].forecast.std_error;
        var += c * c;
        ++ready;
    }
    if (ready == 0) { return std::unexpected(AggregateError::NoMembers); }
    if (!(sw > 0.0)) { return std::unexpected(AggregateError::BadWeights); }

    Aggregate out{};
    out.members = ready;
    out.horizon = horizon;
    out.feature_version = fv;
    out.value = acc / sw;
    out.naive_std_error = std::sqrt(var) / sw;
    // Correlation inflation, exactly as P6-04: sqrt(1 + (n-1)rho).
    const double inflated = out.naive_std_error
                          * correlation_inflation(ready, mean_correlation);
    // AND the weights' own estimation error, added in quadrature. Two
    // independent sources of uncertainty; an interval carrying only the first
    // is too tight by however much data the weights were fitted on.
    out.std_error = std::sqrt(inflated * inflated
                              + weight_std_error * weight_std_error
                                    * out.value * out.value);
    out.n_effective = effective_signals(ready, mean_correlation);
    return out;
}

/// Turn an aggregate into the number a trade handler is allowed to see.
///
/// Net of the round-trip cost (rule 5) and taken at the lower bound
/// (ROADMAP section 3). There is no accessor that returns the point estimate
/// in paise, for the same reason P6-04's Score has none.
[[nodiscard]] inline std::expected<Notional, AggregateError>
net_edge_lower_bound(const Aggregate& a, double paise_per_unit,
                     double confidence_k, Notional round_trip) noexcept {
    if (!(paise_per_unit > 0.0) || !(confidence_k > 0.0)) {
        return std::unexpected(AggregateError::BadWeights);
    }
    const double gross = a.lower_bound(confidence_k) * paise_per_unit;
    const double net = gross - static_cast<double>(round_trip.raw());
    return Notional{static_cast<std::int64_t>(net >= 0.0 ? net + 0.5
                                                         : net - 0.5)};
}

} // namespace altair
