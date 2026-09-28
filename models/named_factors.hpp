// models/named_factors.hpp -- M19 explicit point-in-time factor definitions.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace altair {

struct FactorInput {
    std::uint64_t instrument = 0;
    std::uint64_t information_available_ns = 0;
    double market_cap = 0.0;
    double book_equity = 0.0;
    double operating_income = 0.0;
    double assets = 0.0;
    double trailing_return_ex_recent = 0.0; // e.g. months 12 through 2
    double next_period_return = 0.0;        // evaluation only
};

enum class NamedFactor : std::uint8_t { Value, Quality, Momentum };
enum class NamedFactorError : std::uint8_t {
    TooFewInstruments,
    BadParameter,
    NonFinite,
    NoPointInTimeCoverage
};

struct FactorPortfolio {
    NamedFactor factor = NamedFactor::Value;
    std::vector<std::uint64_t> long_instruments;
    std::vector<std::uint64_t> short_instruments;
    double long_return = 0.0;
    double short_return = 0.0;
    double long_short_return = 0.0;
    double coverage = 0.0;
};

/// Equal-weight top-minus-bottom quantile. Inputs not known by
/// formation_ns-rebalance_lag_ns are excluded rather than backfilled.
[[nodiscard]] inline std::expected<FactorPortfolio, NamedFactorError>
named_factor_portfolio(std::span<const FactorInput> universe, NamedFactor factor,
                       std::uint64_t formation_ns,
                       std::uint64_t rebalance_lag_ns,
                       double quantile = 0.3) {
    if (universe.size() < 6 || !(quantile > 0.0 && quantile <= 0.5)
        || formation_ns < rebalance_lag_ns)
        return std::unexpected(NamedFactorError::BadParameter);
    struct Scored { const FactorInput* row; double score; };
    std::vector<Scored> eligible;
    const std::uint64_t cutoff = formation_ns - rebalance_lag_ns;
    for (const auto& row : universe) {
        if (row.information_available_ns > cutoff) continue;
        if (!(row.market_cap > 0.0) || !(row.assets > 0.0)
            || !std::isfinite(row.book_equity)
            || !std::isfinite(row.operating_income)
            || !std::isfinite(row.trailing_return_ex_recent)
            || !std::isfinite(row.next_period_return))
            return std::unexpected(NamedFactorError::NonFinite);
        double score = 0.0;
        if (factor == NamedFactor::Value) score = row.book_equity / row.market_cap;
        else if (factor == NamedFactor::Quality)
            score = row.operating_income / row.assets;
        else score = row.trailing_return_ex_recent;
        eligible.push_back({&row, score});
    }
    if (eligible.size() < 6)
        return std::unexpected(NamedFactorError::NoPointInTimeCoverage);
    std::sort(eligible.begin(), eligible.end(), [](const auto& a, const auto& b) {
        return a.score != b.score ? a.score < b.score
                                  : a.row->instrument < b.row->instrument;
    });
    const std::size_t side = std::max<std::size_t>(1,
        static_cast<std::size_t>(std::floor(quantile * eligible.size())));
    FactorPortfolio out;
    out.factor = factor;
    out.coverage = static_cast<double>(eligible.size())
                 / static_cast<double>(universe.size());
    for (std::size_t i = 0; i < side; ++i) {
        out.short_instruments.push_back(eligible[i].row->instrument);
        out.short_return += eligible[i].row->next_period_return;
        const auto& high = eligible[eligible.size() - side + i];
        out.long_instruments.push_back(high.row->instrument);
        out.long_return += high.row->next_period_return;
    }
    out.long_return /= static_cast<double>(side);
    out.short_return /= static_cast<double>(side);
    out.long_short_return = out.long_return - out.short_return;
    return out;
}

} // namespace altair
