// backtest/agent_market.hpp -- M24 deterministic conserved agent simulation.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <vector>

namespace altair {

enum class SimAgentKind : std::uint8_t {
    Fundamental,
    Momentum,
    Noise,
    LiquidityProvider
};

struct SimAgent {
    SimAgentKind kind = SimAgentKind::Noise;
    double cash = 0.0;
    std::int64_t inventory = 0;
    double sensitivity = 1.0;
    std::int64_t max_order = 1;
};

struct AgentMarketConfig {
    double initial_price = 0.0;
    double fundamental = 0.0;
    double log_impact_per_unit = 0.0;
    std::uint64_t seed = 0;
};

enum class AgentMarketError : std::uint8_t {
    BadConfig,
    NoLiquidityProvider,
    LiquidityCapacity,
    CashUnavailable,
    Overflow
};

struct AgentMarketStep {
    double price = 0.0;
    std::int64_t volume = 0;
    std::int64_t outside_demand = 0;
};

class AgentMarket {
public:
    [[nodiscard]] static std::expected<AgentMarket, AgentMarketError>
    create(AgentMarketConfig config, std::vector<SimAgent> agents) {
        if (!(config.initial_price > 0.0) || !(config.fundamental > 0.0)
            || !(config.log_impact_per_unit >= 0.0)
            || !std::isfinite(config.initial_price)
            || !std::isfinite(config.fundamental) || agents.size() < 2)
            return std::unexpected(AgentMarketError::BadConfig);
        std::size_t providers = 0;
        for (const auto& agent : agents) {
            if (!std::isfinite(agent.cash) || !std::isfinite(agent.sensitivity)
                || agent.max_order <= 0)
                return std::unexpected(AgentMarketError::BadConfig);
            if (agent.kind == SimAgentKind::LiquidityProvider) ++providers;
        }
        if (providers != 1)
            return std::unexpected(AgentMarketError::NoLiquidityProvider);
        AgentMarket out;
        out.config_ = config;
        out.agents_ = std::move(agents);
        out.price_ = config.initial_price;
        out.previous_price_ = config.initial_price;
        out.rng_ = config.seed != 0 ? config.seed : 0xA6E17A17ull;
        for (const auto& agent : out.agents_) {
            out.initial_cash_ += agent.cash;
            out.initial_inventory_ += agent.inventory;
        }
        return out;
    }

    [[nodiscard]] std::expected<AgentMarketStep, AgentMarketError> step() {
        std::vector<std::int64_t> order(agents_.size(), 0);
        std::size_t provider = agents_.size();
        std::int64_t outside = 0;
        for (std::size_t i = 0; i < agents_.size(); ++i) {
            const auto& agent = agents_[i];
            if (agent.kind == SimAgentKind::LiquidityProvider) {
                provider = i;
                continue;
            }
            double signal = 0.0;
            if (agent.kind == SimAgentKind::Fundamental)
                signal = (config_.fundamental - price_) / price_;
            else if (agent.kind == SimAgentKind::Momentum)
                signal = (price_ - previous_price_) / previous_price_;
            else
                signal = 2.0 * uniform() - 1.0;
            const double raw = signal * agent.sensitivity;
            const double bounded = std::clamp(raw,
                -static_cast<double>(agent.max_order),
                 static_cast<double>(agent.max_order));
            order[i] = static_cast<std::int64_t>(std::llround(bounded));
            if ((order[i] > 0 && outside > std::numeric_limits<std::int64_t>::max() - order[i])
                || (order[i] < 0 && outside < std::numeric_limits<std::int64_t>::min() - order[i]))
                return std::unexpected(AgentMarketError::Overflow);
            outside += order[i];
        }
        if (provider == agents_.size())
            return std::unexpected(AgentMarketError::NoLiquidityProvider);
        if (outside == std::numeric_limits<std::int64_t>::min()
            || std::llabs(outside) > agents_[provider].max_order)
            return std::unexpected(AgentMarketError::LiquidityCapacity);
        order[provider] = -outside;

        previous_price_ = price_;
        price_ *= std::exp(config_.log_impact_per_unit
                           * static_cast<double>(outside));
        if (!(price_ > 0.0) || !std::isfinite(price_))
            return std::unexpected(AgentMarketError::Overflow);

        std::int64_t volume = 0;
        for (std::size_t buyer = 0; buyer < order.size(); ++buyer) {
            while (order[buyer] > 0) {
                std::size_t seller = 0;
                while (seller < order.size() && order[seller] >= 0) ++seller;
                if (seller == order.size()) return std::unexpected(AgentMarketError::Overflow);
                const std::int64_t quantity = std::min(order[buyer], -order[seller]);
                const double notional = price_ * static_cast<double>(quantity);
                if (agents_[buyer].cash + 1e-9 < notional)
                    return std::unexpected(AgentMarketError::CashUnavailable);
                agents_[buyer].cash -= notional;
                agents_[seller].cash += notional;
                agents_[buyer].inventory += quantity;
                agents_[seller].inventory -= quantity;
                order[buyer] -= quantity;
                order[seller] += quantity;
                volume += quantity;
            }
        }
        return AgentMarketStep{price_, volume, outside};
    }

    [[nodiscard]] bool conserved(double tolerance = 1e-8) const noexcept {
        double cash = 0.0;
        std::int64_t inventory = 0;
        for (const auto& agent : agents_) {
            cash += agent.cash;
            inventory += agent.inventory;
        }
        return std::fabs(cash - initial_cash_) <= tolerance
            * (1.0 + std::fabs(initial_cash_))
            && inventory == initial_inventory_;
    }
    [[nodiscard]] double price() const noexcept { return price_; }
    [[nodiscard]] const std::vector<SimAgent>& agents() const noexcept { return agents_; }

private:
    [[nodiscard]] double uniform() noexcept {
        rng_ ^= rng_ >> 12; rng_ ^= rng_ << 25; rng_ ^= rng_ >> 27;
        return static_cast<double>((rng_ * 2685821657736338717ull) >> 11)
             / static_cast<double>(1ull << 53);
    }

    AgentMarketConfig config_{};
    std::vector<SimAgent> agents_;
    double price_ = 0.0, previous_price_ = 0.0;
    double initial_cash_ = 0.0;
    std::int64_t initial_inventory_ = 0;
    std::uint64_t rng_ = 0;
};

} // namespace altair
