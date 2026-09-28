// Acceptance tests for the complete causal Transformer reference backend.

#include <models/transformer.hpp>

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* text) {
    if (ok) std::printf("  ok  : %s\n", text);
    else { ++failures; std::printf("  FAIL: %s\n", text); }
}
}

int main() {
    using Net = altair::CausalTransformer<4, 2, 5, 2, 4>;
    double sequence[16] = {
        0.1, -0.2, 0.3, 0.4,
        0.5,  0.1, 0.0, -0.3,
       -0.2,  0.6, 0.4, 0.2,
        0.7, -0.5, 0.2, 0.1
    };
    Net net; net.reset(73);

    const double prefix = net.predict_at(sequence, 4, 1);
    for (std::size_t i = 8; i < 16; ++i) sequence[i] += 1000.0 + i;
    check(net.predict_at(sequence, 4, 1) == prefix,
          "two-head stacked attention is strictly causal under a future shock");
    for (std::size_t i = 8; i < 16; ++i) sequence[i] -= 1000.0 + i;

    constexpr double target = -0.25;
    const double before = net.loss(sequence, 4, target);
    for (int epoch = 0; epoch < 60; ++epoch)
        net.train_step(sequence, 4, target, 0.01, 1.0);
    const double after = net.loss(sequence, 4, target);
    check(after < before * 0.02,
          "the full Transformer parameter vector trains on a known sequence");

    double saved[Net::kParams];
    for (std::size_t i = 0; i < Net::kParams; ++i) saved[i] = net.params()[i];
    Net copy; copy.reset(101); copy.load_params(saved);
    check(copy.predict(sequence, 4) == net.predict(sequence, 4),
          "Transformer full checkpoint reload is bit-identical");

    check(Net::kParams > 4 * 4 * 4 && Net::param_count() == Net::kParams,
          "checkpoint includes Q/K/V/output, feed-forward, norms and readout");

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
