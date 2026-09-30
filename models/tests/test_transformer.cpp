// Acceptance tests for the complete causal Transformer reference backend.

#include <models/transformer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* text) {
    if (ok) std::printf("  ok  : %s\n", text);
    else { ++failures; std::printf("  FAIL: %s\n", text); }
}

/// Backpropagation against the central finite difference of the same loss,
/// on every parameter, at a random point and for a sequence shorter than MaxT.
template <class Net>
double worst_gradient_error(std::uint64_t seed, std::size_t steps) {
    double sequence[64 * 64];   // steps x D, both at most 64
    std::uint64_t s = seed;
    for (double& v : sequence) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        v = static_cast<double>(s >> 11) * (2.0 / 9007199254740992.0) - 1.0;
    }
    Net net; net.reset(seed);
    for (std::size_t i = 0; i < Net::kParams; ++i) {   // off the init point: LN gains and biases non-trivial
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        net.mutable_params()[i] += 0.1 * (static_cast<double>(s >> 11) * (2.0 / 9007199254740992.0) - 1.0);
    }
    static double numeric[Net::kParams], analytic[Net::kParams];
    const double l1 = net.compute_gradients(sequence, steps, 0.3, numeric);
    const double l2 = net.analytic_gradients(sequence, steps, 0.3, analytic);
    double worst = std::fabs(l1 - l2);
    for (std::size_t i = 0; i < Net::kParams; ++i) {
        worst = std::max(worst, std::fabs(numeric[i] - analytic[i]) / (1e-4 + std::fabs(numeric[i])));
    }
    return worst;
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

    const double e1 = worst_gradient_error<Net>(11, 4);
    const double e2 = worst_gradient_error<altair::CausalTransformer<4, 2, 8, 1, 8>>(12, 8);
    const double e3 = worst_gradient_error<altair::CausalTransformer<4, 2, 8, 1, 8>>(13, 5);
    const double e4 = worst_gradient_error<altair::CausalTransformer<6, 3, 7, 3, 6>>(14, 6);
    std::printf("        worst relative gradient error: %.2e %.2e %.2e %.2e\n", e1, e2, e3, e4);
    check(e1 < 1e-5 && e2 < 1e-5 && e3 < 1e-5 && e4 < 1e-5,
          "backpropagation matches the finite-difference gradient on every parameter");

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
