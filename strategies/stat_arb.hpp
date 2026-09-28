// strategies/stat_arb.hpp -- M17 constrained, costed multi-leg stat-arb plan.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace altair {

enum class StatArbError : std::uint8_t {
    TooFewLegs,
    BadInput,
    NoLongSide,
    NoShortSide,
    ZeroExposure,
    NoNetEdge
};

struct StatArbCandidateLeg {
    std::uint64_t instrument = 0;
    double score = 0.0;             // signed expected residual return
    double price = 0.0;             // same currency for every leg
    double beta = 0.0;              // exposure to the neutralised factor
    double max_notional = 0.0;      // hard per-leg capacity
    double round_trip_cost_bps = 0.0;
};

struct StatArbLeg {
    std::uint64_t instrument = 0;
    double notional = 0.0;          // positive long, negative short
    double expected_gross = 0.0;
    double round_trip_cost = 0.0;
};

struct StatArbPortfolio {
    std::vector<StatArbLeg> legs;
    double gross_notional = 0.0;
    double net_notional = 0.0;
    double factor_exposure = 0.0;
    double expected_gross = 0.0;
    double expected_cost = 0.0;
    double expected_net = 0.0;
};

/// Build a market-neutral, single-factor-neutral long/short basket.
///
/// Long and short sides receive exactly half the gross budget. Within a side,
/// absolute score is the allocation weight, capped by declared capacity. The
/// short side is then split between its two strongest distinct betas so its
/// factor exposure exactly offsets the long side. Inputs outside this narrow,
/// auditable contract are refused; no optimiser silently relaxes neutrality.
[[nodiscard]] inline std::expected<StatArbPortfolio, StatArbError>
build_stat_arb(std::span<const StatArbCandidateLeg> candidates,
               double gross_budget,
               double max_abs_factor_exposure = 1e-9) {
    if (candidates.size() < 3 || !(gross_budget > 0.0)
        || !(max_abs_factor_exposure >= 0.0))
        return std::unexpected(StatArbError::BadInput);
    std::vector<const StatArbCandidateLeg*> longs, shorts;
    for (const auto& c : candidates) {
        if (c.instrument == 0 || !std::isfinite(c.score)
            || !(c.price > 0.0) || !std::isfinite(c.beta)
            || !(c.max_notional > 0.0) || !(c.round_trip_cost_bps >= 0.0)
            || !std::isfinite(c.round_trip_cost_bps))
            return std::unexpected(StatArbError::BadInput);
        if (c.score > 0.0) longs.push_back(&c);
        else if (c.score < 0.0) shorts.push_back(&c);
    }
    if (longs.empty()) return std::unexpected(StatArbError::NoLongSide);
    if (shorts.size() < 2) return std::unexpected(StatArbError::NoShortSide);

    const double side_budget = gross_budget * 0.5;
    double long_weight = 0.0;
    for (const auto* c : longs) long_weight += c->score;
    if (!(long_weight > 0.0)) return std::unexpected(StatArbError::ZeroExposure);

    StatArbPortfolio out;
    double long_beta_dollars = 0.0;
    for (const auto* c : longs) {
        const double n = std::min(c->max_notional,
                                  side_budget * c->score / long_weight);
        if (!(n > 0.0)) continue;
        out.legs.push_back({c->instrument, n, n * c->score,
                            n * c->round_trip_cost_bps / 10'000.0});
        long_beta_dollars += n * c->beta;
    }
    double actual_long = 0.0;
    for (const auto& l : out.legs) actual_long += l.notional;
    if (std::fabs(actual_long - side_budget) > 1e-7 * gross_budget)
        return std::unexpected(StatArbError::BadInput); // capacity cannot fill

    // Choose a bracketing beta pair. For short notionals x,y:
    // x+y=side_budget and beta_x*x+beta_y*y=long_beta_dollars.
    const StatArbCandidateLeg* a = nullptr;
    const StatArbCandidateLeg* b = nullptr;
    double x = 0.0, y = 0.0;
    for (std::size_t i = 0; i < shorts.size() && !a; ++i) {
        for (std::size_t j = i + 1; j < shorts.size(); ++j) {
            const double den = shorts[i]->beta - shorts[j]->beta;
            if (std::fabs(den) < 1e-12) continue;
            const double xi = (long_beta_dollars
                              - shorts[j]->beta * side_budget) / den;
            const double yi = side_budget - xi;
            if (xi >= 0.0 && yi >= 0.0
                && xi <= shorts[i]->max_notional
                && yi <= shorts[j]->max_notional) {
                a = shorts[i]; b = shorts[j]; x = xi; y = yi;
                break;
            }
        }
    }
    if (!a || !b) return std::unexpected(StatArbError::ZeroExposure);
    for (const auto [c, n] : {std::pair{a, x}, std::pair{b, y}}) {
        if (!(n > 0.0)) continue;
        out.legs.push_back({c->instrument, -n, n * -c->score,
                            n * c->round_trip_cost_bps / 10'000.0});
    }
    for (const auto& l : out.legs) {
        out.gross_notional += std::fabs(l.notional);
        out.net_notional += l.notional;
        out.expected_gross += l.expected_gross;
        out.expected_cost += l.round_trip_cost;
        const auto it = std::find_if(candidates.begin(), candidates.end(),
            [&](const auto& c) { return c.instrument == l.instrument; });
        out.factor_exposure += l.notional * it->beta;
    }
    out.expected_net = out.expected_gross - out.expected_cost;
    if (std::fabs(out.net_notional) > 1e-7 * gross_budget
        || std::fabs(out.factor_exposure) > max_abs_factor_exposure)
        return std::unexpected(StatArbError::ZeroExposure);
    if (!(out.expected_net > 0.0))
        return std::unexpected(StatArbError::NoNetEdge);
    return out;
}

} // namespace altair
