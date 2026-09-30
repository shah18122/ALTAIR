// Tests for models/trainable_cnn.hpp -- the trained dilated causal CNN.

#include <models/trainable_cnn.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;
void check(bool ok, const char* text) {
    if (ok) std::printf("  ok  : %s\n", text);
    else { ++failures; std::printf("  FAIL: %s\n", text); }
}

struct Rng {
    std::uint64_t s;
    double uniform() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0);
    }
    double signed_unit() { return 2.0 * uniform() - 1.0; }
};

using Net = altair::TrainableTcn<4, 6, 3, 3, 16>;

/// Backpropagation against the central finite difference of the same loss.
double worst_gradient_error(std::uint64_t seed, std::size_t steps, int label) {
    Rng r{seed};
    std::vector<double> seq(16 * 4);
    for (double& v : seq) v = r.signed_unit();
    Net net; net.reset(seed);
    std::vector<double> analytic(Net::kParams);
    const double l0 = net.gradients(seq.data(), steps, label, analytic.data());
    double worst = std::fabs(l0 - net.loss(seq.data(), steps, label));
    for (std::size_t i = 0; i < Net::kParams; ++i) {
        double* p = net.mutable_params() + i;
        const double saved = *p, eps = 1e-6 * (1.0 + std::fabs(saved));
        *p = saved + eps;
        const double plus = net.loss(seq.data(), steps, label);
        *p = saved - eps;
        const double minus = net.loss(seq.data(), steps, label);
        *p = saved;
        const double numeric = (plus - minus) / (2.0 * eps);
        worst = std::max(worst, std::fabs(numeric - analytic[i]) / (1e-4 + std::fabs(numeric)));
    }
    return worst;
}

} // namespace

int main() {
    std::printf("Trainable CNN\n");
    const double e1 = worst_gradient_error(3, 16, 1);
    const double e2 = worst_gradient_error(4, 9, 0);
    const double e3 = worst_gradient_error(5, 2, 1);
    std::printf("        worst relative gradient error: %.2e %.2e %.2e\n", e1, e2, e3);
    check(e1 < 1e-5 && e2 < 1e-5 && e3 < 1e-5,
          "backpropagation matches the finite-difference gradient on every parameter");

    {
        // Receptive field of three layers of width 3: 1 + 2 * (1 + 2 + 4) = 15 steps.
        Rng r{9};
        std::vector<double> seq(16 * 4);
        for (double& v : seq) v = r.signed_unit();
        Net net; net.reset(9);
        const double p = net.probability(seq.data(), 16);
        for (std::size_t c = 0; c < 4; ++c) seq[c] += 50.0;   // step 0: 16 steps back from the last
        check(net.probability(seq.data(), 16) == p, "a step outside the receptive field cannot move the output");
        seq[15 * 4] += 1.0;                                    // the last step
        check(net.probability(seq.data(), 16) != p, "the last step moves it");
    }

    {
        // Learnable pattern: up when the step 3 back and the last step agree in sign.
        Rng r{21};
        const std::size_t n = 4000, steps = 16;
        std::vector<double> x(n * steps * 4);
        std::vector<int> y(n);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t k = 0; k < steps * 4; ++k) x[i * steps * 4 + k] = r.signed_unit();
            const double a = x[i * steps * 4 + (steps - 4) * 4], b = x[i * steps * 4 + (steps - 1) * 4];
            y[i] = (a * b > 0.0) ? 1 : 0;
        }
        Net net; net.reset(21);
        for (int epoch = 0; epoch < 12; ++epoch) {
            for (std::size_t i = 0; i < 3000; ++i) (void)net.train_step(&x[i * steps * 4], steps, y[i], 3e-3);
        }
        std::size_t right = 0;
        for (std::size_t i = 3000; i < n; ++i) {
            if ((net.probability(&x[i * steps * 4], steps) > 0.5) == (y[i] == 1)) ++right;
        }
        const double acc = static_cast<double>(right) / 1000.0;
        std::printf("        held-out accuracy on a sign-agreement pattern: %.3f\n", acc);
        check(acc > 0.85, "the trained kernels learn a pattern no linear readout of the raw steps can");
    }

    std::printf("Trainable CNN: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
