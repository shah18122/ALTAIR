// Acceptance tests for analytic BPTT in TrainableLstm and TrainableGru.

#include <models/trainable_recurrent.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

int failures = 0;
void check(bool ok, const char* text) {
    if (ok) std::printf("  ok  : %s\n", text);
    else { ++failures; std::printf("  FAIL: %s\n", text); }
}

template <class Net>
void gradient_check(const char* name, Net& net, const double* x,
                    std::size_t steps, double target) {
    double analytic[Net::kParams];
    (void)net.compute_gradients(x, steps, target, analytic);
    constexpr double eps = 1e-6;
    double worst = 0.0;
    for (std::size_t i = 0; i < Net::kParams; ++i) {
        double* p = net.mutable_params();
        const double saved = p[i];
        p[i] = saved + eps;
        const double lp = std::pow(net.predict(x, steps) - target, 2.0);
        p[i] = saved - eps;
        const double lm = std::pow(net.predict(x, steps) - target, 2.0);
        p[i] = saved;
        const double numeric = (lp - lm) / (2.0 * eps);
        worst = std::max(worst, std::fabs(numeric - analytic[i]));
    }
    std::printf("    %s worst absolute gradient error %.3e\n", name, worst);
    check(worst < 2e-6, name);
}

template <class Net>
void trains_and_checkpoints(const char* train_text, const char* checkpoint_text,
                            Net& net, const double* x, std::size_t steps,
                            double target) {
    const double before = std::pow(net.predict(x, steps) - target, 2.0);
    for (int i = 0; i < 600; ++i) net.train_step(x, steps, target, 0.02, 2.0);
    const double after = std::pow(net.predict(x, steps) - target, 2.0);
    check(after < before * 1e-3, train_text);

    double saved[Net::kParams];
    for (std::size_t i = 0; i < Net::kParams; ++i) saved[i] = net.params()[i];
    Net copy; copy.reset(999); copy.load_params(saved);
    check(copy.predict(x, steps) == net.predict(x, steps), checkpoint_text);
}

} // namespace

int main() {
    constexpr double sequence[] = {0.2, -0.4, 0.7, 0.1};
    constexpr double target = -0.35;

    altair::TrainableLstm<2, 1, 4> lstm;
    lstm.reset(41, 1.0);
    gradient_check("LSTM analytic BPTT matches central differences", lstm,
                   sequence, 4, target);
    trains_and_checkpoints("LSTM trains every gate and overfits a known sequence",
                           "LSTM full checkpoint reload is bit-identical",
                           lstm, sequence, 4, target);

    altair::TrainableGru<2, 1, 4> gru;
    gru.reset(43, -1.0);
    gradient_check("GRU analytic BPTT matches central differences", gru,
                   sequence, 4, target);
    trains_and_checkpoints("GRU trains every gate and overfits a known sequence",
                           "GRU full checkpoint reload is bit-identical",
                           gru, sequence, 4, target);

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
