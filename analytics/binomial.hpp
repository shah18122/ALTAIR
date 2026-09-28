// analytics/binomial.hpp -- bounded Cox-Ross-Rubinstein option lattice.
//
// The lattice is a transparent benchmark for the closed-form pricers. It is
// also the missing exercise-style model for contracts where an American
// early-exercise decision matters. Bounds on the step count are deliberate:
// a UI or feed-controlled value must not turn into an unbounded allocation.
#pragma once

#include <analytics/greeks.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

namespace altair {

struct BinomialValue {
    double price = 0.0;
    std::size_t steps = 0;
    bool american = false;
};

[[nodiscard]] inline std::expected<BinomialValue, GreekError>
binomial_price(OptionRight right, Price spot, Price strike, Years maturity,
               Vol volatility, double rate, std::size_t steps,
               bool american = false) {
    const double S = static_cast<double>(spot.raw());
    const double K = static_cast<double>(strike.raw());
    const double T = maturity.raw();
    const double sigma = volatility.raw();
    if (!(S > 0.0) || !(K > 0.0)) return std::unexpected(GreekError::NonPositivePrice);
    if (!(T > 0.0)) return std::unexpected(GreekError::Expired);
    if (!(sigma > 0.0)) return std::unexpected(GreekError::NonPositiveVol);
    if (!std::isfinite(S) || !std::isfinite(K) || !std::isfinite(T)
        || !std::isfinite(sigma) || !std::isfinite(rate)
        || steps == 0 || steps > 4096) return std::unexpected(GreekError::NotFinite);

    const double dt = T / static_cast<double>(steps);
    const double up = std::exp(sigma * std::sqrt(dt));
    const double down = 1.0 / up;
    const double disc = std::exp(-rate * dt);
    const double p = (std::exp(rate * dt) - down) / (up - down);
    if (!(p >= 0.0 && p <= 1.0) || !std::isfinite(p))
        return std::unexpected(GreekError::NotFinite);
    std::vector<double> values(steps + 1, 0.0);
    for (std::size_t j = 0; j <= steps; ++j) {
        const double terminal = S * std::pow(up, static_cast<double>(j))
                               * std::pow(down, static_cast<double>(steps - j));
        values[j] = right == OptionRight::Call
            ? std::max(terminal - K, 0.0) : std::max(K - terminal, 0.0);
    }
    for (std::size_t i = steps; i-- > 0;) {
        for (std::size_t j = 0; j <= i; ++j) {
            const double continuation = disc * (p * values[j + 1]
                                               + (1.0 - p) * values[j]);
            if (!american) { values[j] = continuation; continue; }
            const double node = S * std::pow(up, static_cast<double>(j))
                              * std::pow(down, static_cast<double>(i - j));
            const double exercise = right == OptionRight::Call
                ? std::max(node - K, 0.0) : std::max(K - node, 0.0);
            values[j] = std::max(continuation, exercise);
        }
    }
    if (!std::isfinite(values[0])) return std::unexpected(GreekError::NotFinite);
    return BinomialValue{values[0], steps, american};
}

} // namespace altair
