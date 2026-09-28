// models/deep_rl.hpp -- M25-M27 deterministic reference agents.
// Research only: these return actions; they do not include or call OMS code.
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

enum class DeepRlError : std::uint8_t {
    BadShape,
    BadParameter,
    InvalidAction,
    EmptyReplay,
    NonFinite,
    CheckpointMismatch
};

struct DeepTransition {
    std::vector<double> state;
    std::size_t action = 0;
    double reward = 0.0;
    std::vector<double> next_state;
    bool terminal = false;
};

struct DqnConfig {
    std::size_t state_size = 0;
    std::size_t actions = 0;
    std::size_t hidden = 0;
    std::size_t replay_capacity = 0;
    std::size_t target_sync_updates = 0;
    double learning_rate = 0.0;
    double gamma = 0.0;
    std::uint64_t seed = 0;
};

class DqnAgent {
public:
    [[nodiscard]] static std::expected<DqnAgent, DeepRlError>
    create(DqnConfig config) {
        if (config.state_size == 0 || config.actions < 2 || config.hidden == 0
            || config.replay_capacity == 0 || config.target_sync_updates == 0
            || !(config.learning_rate > 0.0) || !(config.gamma >= 0.0)
            || !(config.gamma <= 1.0) || config.state_size > 1024
            || config.actions > 128 || config.hidden > 4096)
            return std::unexpected(DeepRlError::BadParameter);
        DqnAgent out;
        out.config_ = config;
        out.online_.resize(config);
        out.initialise(out.online_, config.seed);
        out.target_ = out.online_;
        out.replay_.reserve(config.replay_capacity);
        return out;
    }

    [[nodiscard]] std::expected<void, DeepRlError>
    remember(DeepTransition transition) {
        if (!valid_transition(transition))
            return std::unexpected(DeepRlError::BadShape);
        if (replay_.size() < config_.replay_capacity)
            replay_.push_back(std::move(transition));
        else {
            replay_[replay_cursor_] = std::move(transition);
            replay_cursor_ = (replay_cursor_ + 1) % replay_.size();
        }
        return {};
    }

    [[nodiscard]] std::expected<double, DeepRlError> train_replay() {
        if (replay_.empty()) return std::unexpected(DeepRlError::EmptyReplay);
        double loss = 0.0;
        for (const auto& transition : replay_) {
            std::vector<double> hidden(config_.hidden), q(config_.actions);
            forward(online_, transition.state, hidden, q);
            std::vector<double> target_hidden(config_.hidden), next_q(config_.actions);
            forward(target_, transition.next_state, target_hidden, next_q);
            const double bootstrap = transition.terminal ? 0.0
                : config_.gamma * *std::max_element(next_q.begin(), next_q.end());
            const double target = transition.reward + bootstrap;
            const double error = q[transition.action] - target;
            loss += 0.5 * error * error;

            std::vector<double> hidden_gradient(config_.hidden, 0.0);
            for (std::size_t h = 0; h < config_.hidden; ++h) {
                const std::size_t index = transition.action * config_.hidden + h;
                hidden_gradient[h] = error * online_.w2[index]
                                   * (1.0 - hidden[h] * hidden[h]);
                online_.w2[index] -= config_.learning_rate
                                   * clipped(error * hidden[h]);
            }
            online_.b2[transition.action] -= config_.learning_rate * clipped(error);
            for (std::size_t h = 0; h < config_.hidden; ++h) {
                for (std::size_t j = 0; j < config_.state_size; ++j)
                    online_.w1[h * config_.state_size + j] -= config_.learning_rate
                        * clipped(hidden_gradient[h] * transition.state[j]);
                online_.b1[h] -= config_.learning_rate * clipped(hidden_gradient[h]);
            }
        }
        ++updates_;
        if (updates_ % config_.target_sync_updates == 0) target_ = online_;
        return loss / static_cast<double>(replay_.size());
    }

    [[nodiscard]] std::expected<std::vector<double>, DeepRlError>
    values(std::span<const double> state) const {
        if (!valid_state(state)) return std::unexpected(DeepRlError::BadShape);
        std::vector<double> hidden(config_.hidden), q(config_.actions);
        forward(online_, state, hidden, q);
        return q;
    }
    [[nodiscard]] std::expected<std::size_t, DeepRlError>
    greedy(std::span<const double> state) const {
        const auto q = values(state);
        if (!q) return std::unexpected(q.error());
        return static_cast<std::size_t>(std::max_element(q->begin(), q->end())
                                        - q->begin());
    }

    [[nodiscard]] std::vector<double> checkpoint() const {
        std::vector<double> out;
        out.reserve(online_.w1.size() + online_.b1.size()
                    + online_.w2.size() + online_.b2.size());
        out.insert(out.end(), online_.w1.begin(), online_.w1.end());
        out.insert(out.end(), online_.b1.begin(), online_.b1.end());
        out.insert(out.end(), online_.w2.begin(), online_.w2.end());
        out.insert(out.end(), online_.b2.begin(), online_.b2.end());
        return out;
    }
    [[nodiscard]] std::expected<void, DeepRlError>
    load_checkpoint(std::span<const double> checkpoint) {
        if (checkpoint.size() != checkpoint_size())
            return std::unexpected(DeepRlError::CheckpointMismatch);
        auto cursor = checkpoint.begin();
        auto load = [&cursor](std::vector<double>& destination) {
            std::copy(cursor, cursor + static_cast<std::ptrdiff_t>(destination.size()),
                      destination.begin());
            cursor += static_cast<std::ptrdiff_t>(destination.size());
        };
        load(online_.w1); load(online_.b1); load(online_.w2); load(online_.b2);
        target_ = online_;
        return {};
    }

private:
    struct Network {
        std::vector<double> w1, b1, w2, b2;
        void resize(const DqnConfig& c) {
            w1.resize(c.hidden * c.state_size); b1.resize(c.hidden);
            w2.resize(c.actions * c.hidden); b2.resize(c.actions);
        }
    };
    void initialise(Network& network, std::uint64_t seed) noexcept {
        std::uint64_t state = seed != 0 ? seed : 0xD0A6E17ull;
        auto draw = [&state]() {
            state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
            return 2.0 * static_cast<double>(
                (state * 2685821657736338717ull) >> 11)
                / static_cast<double>(1ull << 53) - 1.0;
        };
        const double s1 = std::sqrt(6.0 / static_cast<double>(
            config_.state_size + config_.hidden));
        const double s2 = std::sqrt(6.0 / static_cast<double>(
            config_.hidden + config_.actions));
        for (double& value : network.w1) value = s1 * draw();
        for (double& value : network.w2) value = s2 * draw();
        std::fill(network.b1.begin(), network.b1.end(), 0.0);
        std::fill(network.b2.begin(), network.b2.end(), 0.0);
    }
    void forward(const Network& network, std::span<const double> state,
                 std::span<double> hidden, std::span<double> q) const noexcept {
        for (std::size_t h = 0; h < config_.hidden; ++h) {
            double value = network.b1[h];
            for (std::size_t j = 0; j < config_.state_size; ++j)
                value += network.w1[h * config_.state_size + j] * state[j];
            hidden[h] = std::tanh(value);
        }
        for (std::size_t action = 0; action < config_.actions; ++action) {
            double value = network.b2[action];
            for (std::size_t h = 0; h < config_.hidden; ++h)
                value += network.w2[action * config_.hidden + h] * hidden[h];
            q[action] = value;
        }
    }
    [[nodiscard]] bool valid_state(std::span<const double> state) const noexcept {
        if (state.size() != config_.state_size) return false;
        for (const double value : state) if (!std::isfinite(value)) return false;
        return true;
    }
    [[nodiscard]] bool valid_transition(const DeepTransition& t) const noexcept {
        return valid_state(t.state) && valid_state(t.next_state)
            && t.action < config_.actions && std::isfinite(t.reward);
    }
    [[nodiscard]] std::size_t checkpoint_size() const noexcept {
        return online_.w1.size() + online_.b1.size()
             + online_.w2.size() + online_.b2.size();
    }
    [[nodiscard]] static double clipped(double gradient) noexcept {
        return std::clamp(gradient, -10.0, 10.0);
    }

    DqnConfig config_{};
    Network online_, target_;
    std::vector<DeepTransition> replay_;
    std::size_t replay_cursor_ = 0, updates_ = 0;
};

struct PolicySample {
    std::vector<double> state;
    std::size_t action = 0;
    double reward = 0.0;
    std::vector<double> next_state;
    bool terminal = false;
    double old_probability = 0.0;
    std::vector<std::uint8_t> allowed;
};

struct PolicyConfig {
    std::size_t state_size = 0;
    std::size_t actions = 0;
    double actor_learning_rate = 0.0;
    double critic_learning_rate = 0.0;
    double gamma = 0.0;
    double gae_lambda = 0.0;
    double clip_ratio = 0.0;
};

/// Linear softmax actor plus linear critic. This is a neural policy with no
/// hidden layer: deliberately the smallest function approximator that makes
/// PPO clipping and actor/critic gradients executable and inspectable.
class PpoAgent {
public:
    [[nodiscard]] static std::expected<PpoAgent, DeepRlError>
    create(PolicyConfig config) {
        if (config.state_size == 0 || config.actions < 2
            || !(config.actor_learning_rate > 0.0)
            || !(config.critic_learning_rate > 0.0)
            || !(config.gamma >= 0.0 && config.gamma <= 1.0)
            || !(config.gae_lambda >= 0.0 && config.gae_lambda <= 1.0)
            || !(config.clip_ratio > 0.0 && config.clip_ratio < 1.0))
            return std::unexpected(DeepRlError::BadParameter);
        PpoAgent out; out.config_ = config;
        out.actor_.assign(config.actions * config.state_size, 0.0);
        out.critic_.assign(config.state_size, 0.0);
        return out;
    }

    [[nodiscard]] std::expected<std::vector<double>, DeepRlError>
    probabilities(std::span<const double> state,
                  std::span<const std::uint8_t> allowed = {}) const {
        if (!valid_state(state) || (!allowed.empty() && allowed.size() != config_.actions))
            return std::unexpected(DeepRlError::BadShape);
        std::vector<double> logits(config_.actions,
                                   -std::numeric_limits<double>::infinity());
        double maximum = -std::numeric_limits<double>::infinity();
        for (std::size_t action = 0; action < config_.actions; ++action) {
            if (!allowed.empty() && allowed[action] == 0) continue;
            double value = 0.0;
            for (std::size_t j = 0; j < config_.state_size; ++j)
                value += actor_[action * config_.state_size + j] * state[j];
            logits[action] = value;
            maximum = std::max(maximum, value);
        }
        if (!std::isfinite(maximum)) return std::unexpected(DeepRlError::InvalidAction);
        double total = 0.0;
        for (double& value : logits) if (std::isfinite(value)) {
            value = std::exp(value - maximum); total += value;
        } else value = 0.0;
        for (double& value : logits) value /= total;
        return logits;
    }

    [[nodiscard]] std::expected<double, DeepRlError>
    update(std::span<const PolicySample> samples, std::size_t epochs = 4) {
        if (samples.empty() || epochs == 0)
            return std::unexpected(DeepRlError::BadShape);
        std::vector<double> value(samples.size()), next_value(samples.size());
        for (std::size_t i = 0; i < samples.size(); ++i) {
            if (!valid_sample(samples[i])) return std::unexpected(DeepRlError::BadShape);
            value[i] = critic_value(samples[i].state);
            next_value[i] = samples[i].terminal ? 0.0
                                                : critic_value(samples[i].next_state);
        }
        std::vector<double> advantage(samples.size(), 0.0), target(samples.size(), 0.0);
        double next_advantage = 0.0;
        for (std::size_t i = samples.size(); i-- > 0;) {
            const double continuation = samples[i].terminal ? 0.0 : 1.0;
            const double delta = samples[i].reward
                + config_.gamma * next_value[i] * continuation - value[i];
            advantage[i] = delta + config_.gamma * config_.gae_lambda
                                   * continuation * next_advantage;
            target[i] = advantage[i] + value[i];
            next_advantage = advantage[i];
        }
        double objective = 0.0;
        for (std::size_t epoch = 0; epoch < epochs; ++epoch) {
            for (std::size_t i = 0; i < samples.size(); ++i) {
                const auto probability = probabilities(samples[i].state,
                                                        samples[i].allowed);
                if (!probability) return std::unexpected(probability.error());
                const double old = samples[i].old_probability > 0.0
                    ? samples[i].old_probability : 1.0 / static_cast<double>(config_.actions);
                const double ratio = (*probability)[samples[i].action] / old;
                const double clipped_ratio = std::clamp(
                    ratio, 1.0 - config_.clip_ratio, 1.0 + config_.clip_ratio);
                const double raw = ratio * advantage[i];
                const double clipped_objective = clipped_ratio * advantage[i];
                objective += std::min(raw, clipped_objective);
                const bool clipped = (advantage[i] >= 0.0 && ratio > 1.0 + config_.clip_ratio)
                                  || (advantage[i] < 0.0 && ratio < 1.0 - config_.clip_ratio);
                if (!clipped) {
                    const double scale = config_.actor_learning_rate
                                       * advantage[i] * ratio;
                    for (std::size_t action = 0; action < config_.actions; ++action) {
                        const double derivative = (action == samples[i].action ? 1.0 : 0.0)
                                                - (*probability)[action];
                        for (std::size_t j = 0; j < config_.state_size; ++j)
                            actor_[action * config_.state_size + j] += scale
                                * derivative * samples[i].state[j];
                    }
                }
                const double critic_error = critic_value(samples[i].state) - target[i];
                for (std::size_t j = 0; j < config_.state_size; ++j)
                    critic_[j] -= config_.critic_learning_rate
                                * critic_error * samples[i].state[j];
            }
        }
        return objective / static_cast<double>(samples.size() * epochs);
    }

    [[nodiscard]] std::vector<double> checkpoint() const {
        std::vector<double> out = actor_;
        out.insert(out.end(), critic_.begin(), critic_.end());
        return out;
    }
    [[nodiscard]] std::expected<void, DeepRlError>
    load_checkpoint(std::span<const double> values) {
        if (values.size() != actor_.size() + critic_.size())
            return std::unexpected(DeepRlError::CheckpointMismatch);
        std::copy(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(actor_.size()),
                  actor_.begin());
        std::copy(values.begin() + static_cast<std::ptrdiff_t>(actor_.size()),
                  values.end(), critic_.begin());
        return {};
    }

private:
    [[nodiscard]] bool valid_state(std::span<const double> state) const noexcept {
        if (state.size() != config_.state_size) return false;
        for (const double value : state) if (!std::isfinite(value)) return false;
        return true;
    }
    [[nodiscard]] bool valid_sample(const PolicySample& sample) const noexcept {
        return valid_state(sample.state) && valid_state(sample.next_state)
            && sample.action < config_.actions && std::isfinite(sample.reward)
            && (sample.allowed.empty()
                || (sample.allowed.size() == config_.actions
                    && sample.allowed[sample.action] != 0));
    }
    [[nodiscard]] double critic_value(std::span<const double> state) const noexcept {
        double value = 0.0;
        for (std::size_t j = 0; j < config_.state_size; ++j)
            value += critic_[j] * state[j];
        return value;
    }
    PolicyConfig config_{};
    std::vector<double> actor_, critic_;
};

/// Actor-critic uses the same explicit actor/value losses without PPO clipping
/// or GAE carry. Keeping it as a named wrapper prevents UI/config code from
/// silently treating the two objectives as interchangeable.
class ActorCriticAgent {
public:
    [[nodiscard]] static std::expected<ActorCriticAgent, DeepRlError>
    create(PolicyConfig config) {
        config.clip_ratio = 0.999;
        config.gae_lambda = 0.0;
        const auto policy = PpoAgent::create(config);
        if (!policy) return std::unexpected(policy.error());
        return ActorCriticAgent{*policy};
    }
    [[nodiscard]] std::expected<double, DeepRlError>
    update(std::span<const PolicySample> samples) { return policy_.update(samples, 1); }
    [[nodiscard]] auto probabilities(std::span<const double> state) const {
        return policy_.probabilities(state);
    }
    [[nodiscard]] std::vector<double> checkpoint() const { return policy_.checkpoint(); }
private:
    explicit ActorCriticAgent(PpoAgent policy) : policy_(std::move(policy)) {}
    PpoAgent policy_;
};

} // namespace altair
