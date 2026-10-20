// Tests for models/meta_learners.hpp -- the CNN, the analogs and the selector.

#include <models/meta_learners.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

using namespace altair::meta;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

struct Gen {
    std::uint64_t s;
    double u() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0); }
    double n() { double a = 0; for (int k = 0; k < 12; ++k) a += u(); return a - 6.0; }
};

void test_gradient() {
    TcnConfig c;
    c.window = 5; c.hidden = 3; c.channels = 2; c.kernel = 2; c.l2 = 1e-2;
    MetaTcn net(c);
    net.init(4, 11);
    Gen g{5};
    std::vector<double> win(5 * 4);
    for (double& v : win) v = g.n();
    // Push the hidden units off zero so ReLU kinks do not sit on the probe.
    std::vector<double> grad, tmp;
    const double t = 0.7;
    (void)net.loss_and_grad(win, t, grad);
    double worst = 0.0;
    auto& w = net.weights();
    for (std::size_t i = 0; i < w.size(); ++i) {
        const double h = 1e-6, keep = w[i];
        w[i] = keep + h;
        const double up = net.loss_and_grad(win, t, tmp);
        w[i] = keep - h;
        const double dn = net.loss_and_grad(win, t, tmp);
        w[i] = keep;
        const double fd = (up - dn) / (2 * h);
        worst = std::max(worst, std::fabs(fd - grad[i]) / std::max(1.0, std::fabs(fd)));
    }
    std::printf("        CNN: %zu parameters, worst gradient error %.2e\n", net.params(), worst);
    check(worst < 1e-5, "the CNN's backpropagation matches finite differences, parameter by parameter");
}

/// A sequence where the move follows yesterday's input 1 and today's input 0.
struct Seq { std::vector<std::vector<double>> x; std::vector<double> y; };
Seq lagged(std::size_t n, std::uint64_t seed, double signal) {
    Gen g{seed};
    Seq s;
    std::vector<double> prev(6, 0.0);
    for (std::size_t t = 0; t < n; ++t) {
        std::vector<double> x(6);
        for (double& v : x) v = g.n();
        s.y.push_back(signal * (30.0 * x[0] - 25.0 * prev[1]) + 20.0 * g.n());
        s.x.push_back(x);
        prev = x;
    }
    return s;
}

std::unique_ptr<Selector> make(std::size_t features) {
    std::vector<std::unique_ptr<Learner>> c;
    c.push_back(std::make_unique<NoChange>());
    MetaConfig mc;
    mc.min_days = 100;
    mc.score_days = 30;
    c.push_back(std::make_unique<RidgeLearner>(features, mc));
    TcnConfig tc;
    tc.min_steps = 120; tc.refit_every = 60; tc.epochs = 30;
    c.push_back(std::make_unique<MetaTcn>(tc));
    AnalogConfig ac;
    ac.min_steps = 120; ac.refit_every = 60; ac.components = 4;
    c.push_back(std::make_unique<AnalogLearner>(ac));
    return std::make_unique<Selector>(std::move(c), 200.0, 40);
}

struct Scores { std::vector<double> sse; double zero = 0.0; std::size_t n = 0; std::size_t last_chosen = 0; std::vector<double> calls; };
Scores run(const Seq& s, std::size_t score_from) {
    auto sel = make(s.x[0].size());
    Scores out;
    out.sse.assign(sel->size() + 1, 0.0);
    for (std::size_t t = 0; t < s.y.size(); ++t) {
        const auto c = sel->predict(s.x[t]);
        out.calls.push_back(c.mu);
        if (t >= score_from) {
            for (std::size_t k = 0; k < sel->size(); ++k) {
                const double d = sel->last_prediction(k) - s.y[t];
                out.sse[k] += d * d;
            }
            out.sse[sel->size()] += (c.mu - s.y[t]) * (c.mu - s.y[t]);
            out.zero += s.y[t] * s.y[t];
            ++out.n;
        }
        out.last_chosen = c.chosen;
        sel->learn(s.y[t]);
    }
    return out;
}

void test_learning() {
    const Seq s = lagged(700, 3, 1.0);
    const auto r = run(s, 400);
    std::printf("        signal: MSE ridge %.0f, CNN %.0f, analogs %.0f, selected %.0f, no change %.0f\n", r.sse[1] / r.n,
                r.sse[2] / r.n, r.sse[3] / r.n, r.sse[4] / r.n, r.zero / r.n);
    check(r.sse[2] < 0.6 * r.zero, "the CNN finds a move that follows today's and yesterday's inputs");
    check(r.sse[2] < r.sse[1], "the CNN beats the ridge where the signal needs yesterday's inputs");
    check(r.sse[4] < 0.75 * r.zero && r.last_chosen != 0, "the selector trades a learner, not no change, when one is better");
}

void test_noise() {
    const Seq s = lagged(700, 9, 0.0);
    const auto r = run(s, 400);
    std::printf("        noise: MSE ridge %.0f, CNN %.0f, analogs %.0f, selected %.0f, no change %.0f\n", r.sse[1] / r.n,
                r.sse[2] / r.n, r.sse[3] / r.n, r.sse[4] / r.n, r.zero / r.n);
    check(r.sse[4] < 1.05 * r.zero, "on pure noise the selected calls cost at most 5 % over no change");
}

void test_no_lookahead() {
    Seq a = lagged(600, 21, 1.0);
    Seq b = a;
    const std::size_t cut = 420;
    for (std::size_t t = cut; t < b.y.size(); ++t) {   // the cut step's own outcome changes too
        b.y[t] = -b.y[t] + 7.0;
        if (t > cut) for (double& v : b.x[t]) v = -v;
    }
    const auto ra = run(a, 0), rb = run(b, 0);
    bool same = true;
    for (std::size_t t = 0; t <= cut; ++t) same = same && ra.calls[t] == rb.calls[t];
    bool differs_after = false;
    for (std::size_t t = cut + 1; t < ra.calls.size(); ++t) differs_after = differs_after || ra.calls[t] != rb.calls[t];
    check(same, "every call up to a step is bit for bit the same whatever happens from that step on");
    check(differs_after, "and the calls after it do respond to the other future (the check can fail)");
}

} // namespace

int main() {
    std::printf("Meta learners\n");
    test_gradient();
    test_learning();
    test_noise();
    test_no_lookahead();
    std::printf("Meta learners: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
