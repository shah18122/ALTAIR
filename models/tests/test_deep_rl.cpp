#include <models/deep_rl.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* text) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    if (!ok) ++failures;
}
}

int main() {
    using namespace altair;
    // M25: a two-state contextual bandit, with replay and target network.
    DqnConfig dc{2, 2, 8, 128, 5, 0.03, 0.0, 7};
    auto dqn = DqnAgent::create(dc);
    check(dqn.has_value(), "DQN creates a seeded online/target network pair");
    if (dqn) {
        for (int repeat = 0; repeat < 20; ++repeat) {
            for (std::size_t state = 0; state < 2; ++state)
                for (std::size_t action = 0; action < 2; ++action) {
                    std::vector<double> s{state == 0 ? 1.0 : 0.0,
                                          state == 1 ? 1.0 : 0.0};
                    (void)dqn->remember({s, action, action == state ? 1.0 : -1.0,
                                         s, true});
                }
        }
        for (int i = 0; i < 150; ++i) (void)dqn->train_replay();
        check(dqn->greedy(std::array<double,2>{1,0}) == 0
                  && dqn->greedy(std::array<double,2>{0,1}) == 1,
              "DQN replay learns the reward-maximising action in both states");
        const auto saved = dqn->checkpoint();
        auto restored = DqnAgent::create(dc);
        check(restored && restored->load_checkpoint(saved)
                  && restored->values(std::array<double,2>{1,0})
                     == dqn->values(std::array<double,2>{1,0}),
              "DQN checkpoint round-trip is bit-deterministic");
    }

    // M26: PPO clipped policy gradient with GAE and an action mask.
    PolicyConfig pc{1, 2, 0.08, 0.05, 0.0, 0.95, 0.2};
    auto ppo = PpoAgent::create(pc);
    check(ppo.has_value(), "PPO creates an explicit actor and critic");
    if (ppo) {
        std::vector<PolicySample> samples;
        for (int i = 0; i < 64; ++i) {
            const std::size_t action = static_cast<std::size_t>(i % 2);
            samples.push_back({{1.0}, action, action == 0 ? 1.0 : -1.0,
                               {1.0}, true, 0.5, {1,1}});
        }
        for (int i = 0; i < 10; ++i) (void)ppo->update(samples, 4);
        const auto probability = ppo->probabilities(std::array<double,1>{1.0});
        check(probability && (*probability)[0] > (*probability)[1],
              "PPO gradient raises probability of the positively rewarded action");
        const auto masked = ppo->probabilities(std::array<double,1>{1.0},
                                               std::array<std::uint8_t,2>{0,1});
        check(masked && (*masked)[0] == 0.0 && (*masked)[1] == 1.0,
              "PPO action constraints assign zero probability before sampling");
        const auto saved = ppo->checkpoint();
        auto restored = PpoAgent::create(pc);
        check(restored && restored->load_checkpoint(saved)
                  && restored->probabilities(std::array<double,1>{1.0}) == probability,
              "PPO actor/critic checkpoint round-trip is deterministic");
    }

    // M27: one-step actor-critic is a separately named objective.
    auto actor_critic = ActorCriticAgent::create(pc);
    if (actor_critic) {
        std::vector<PolicySample> samples(32,
            PolicySample{{1.0}, 1, 1.0, {1.0}, true, 0.5, {1,1}});
        for (int i = 0; i < 20; ++i) (void)actor_critic->update(samples);
        const auto probability = actor_critic->probabilities(
            std::array<double,1>{1.0});
        check(probability && (*probability)[1] > 0.5,
              "actor-critic actor follows the sign of its TD advantage");
    } else check(false, "actor-critic creates");

    std::printf("Deep RL: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
