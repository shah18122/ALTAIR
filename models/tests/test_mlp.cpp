// models/tests/test_mlp.cpp -- M12 acceptance tests for the trainable MLP.
//
// WHAT THIS FILE IS FOR.
//
// `Mlp` shipped as a random-features model: the hidden layer is seeded once and
// frozen, and only the linear readout is solved. That is a real method and it
// was honestly labelled, but it cannot represent a target a frozen random basis
// does not happen to span -- which is why the Atlas card read `Partial`. M12
// adds backpropagation, and this file is the evidence.
//
// THE GRADIENT IS CHECKED, NOT TRUSTED.
//
// The first check compares the analytic gradient against a central finite
// difference of the very objective `compute_gradients` returns. A gradient that
// is never compared to a numerical one is an assertion about nothing -- and a
// sign error in the ReLU derivative or the readout transpose produces code that
// trains, badly, forever, while every loss-decreases check still passes.
//
// THE CHECKPOINT IS THE WHOLE VECTOR IN Backprop MODE.
//
// The second check reloads a checkpoint into a fresh model and requires
// bit-identical predictions. A readout-only checkpoint reloaded into a
// backprop-trained model loads weights that belong to a different model, and
// the failure is quiet: the predictions are wrong, not invalid.
//
// No check description here may contain the substring "F" "AIL" joined.
// Nothing here is a market claim. The toy targets are synthetic and separable
// or not by construction, and a synthetic fixture never becomes market
// validation (P6-05's acceptance contract, item 5).

#include <models/mlp.hpp>
#include <models/dataset.hpp>

#include <backtest/validation.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

/// A synthetic dataset held in owned vectors, viewed by a `Dataset`.
///
/// Built directly rather than through labels/features: this card is about the
/// model's arithmetic, and pulling in the label and feature machinery would
/// make a gradient error indistinguishable from a labelling error.
struct Toy {
    std::size_t n = 0, k = 0;
    std::vector<double> xs, ys, ws;
    altair::Dataset d{};

    /// Deterministic pseudo-random inputs in [-1, 1] and a caller-supplied
    /// target. No <random> and no clock: two runs must be bit-identical.
    Toy(std::size_t rows, std::size_t cols, bool xor_target)
        : n(rows), k(cols), xs(rows * cols), ys(rows), ws(rows, 1.0)
    {
        std::uint64_t s = 0x9E3779B97F4A7C15ull;
        auto next = [&s]() {
            s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
            return static_cast<double>((s * 2685821657736338717ull) >> 11)
                 / static_cast<double>(1ull << 53);
        };
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < k; ++j) {
                xs[i * k + j] = 2.0 * next() - 1.0;
            }
            if (xor_target) {
                // XOR in the first two coordinates: linearly inseparable, so a
                // frozen random basis plus a linear readout has no shortcut.
                const bool a = xs[i * k + 0] > 0.0;
                const bool b = k > 1 ? xs[i * k + 1] > 0.0 : false;
                ys[i] = (a != b) ? 1.0 : -1.0;
            } else {
                // relu(x0 + x1) - relu(x0 - x1): piecewise linear, so NOT
                // linearly separable, and EXACTLY representable by two ReLU
                // hidden units with readout weights +1 and -1. That exactness
                // is the point -- "did backprop learn the hidden layer?" has a
                // real answer (loss near zero) rather than an opinion about a
                // threshold. It also has a ground truth to compare against,
                // which XOR with full-batch gradient descent does not.
                const double hi = xs[i * k + 0] + (k > 1 ? xs[i * k + 1] : 0.0);
                const double lo = xs[i * k + 0] - (k > 1 ? xs[i * k + 1] : 0.0);
                ys[i] = (hi > 0.0 ? hi : 0.0) - (lo > 0.0 ? lo : 0.0);
            }
        }
        d.x = altair::Matrix{xs.data(), n, k};
        d.y = ys.data();
        d.weight = ws.data();
        d.rows = n;
    }

    [[nodiscard]] altair::Block all() const noexcept {
        return altair::Block{0, n};
    }
};

/// 1. The analytic gradient must equal a central finite difference of the
/// objective it claims to differentiate.
void gradients_match_finite_differences()
{
    constexpr std::size_t H = 8, X = 4;
    using Net = altair::Mlp<H, X>;
    Toy toy(24, X, true);

    Net m(1e-3, altair::MlpTrainer::Backprop);
    m.reset(0xA17A1Bu);

    const std::size_t P = Net::kFullParams;
    std::vector<double> grad(P, 0.0);
    (void)m.compute_gradients(toy.d, toy.all(), grad.data());

    double* p = const_cast<double*>(m.params());
    const double eps = 1e-6;
    double worst_abs = 0.0, worst_rel = 0.0;
    bool finite = true;
    std::vector<double> scratch(P, 0.0);

    for (std::size_t i = 0; i < P; ++i) {
        const double orig = p[i];

        p[i] = orig + eps;
        const double plus = m.compute_gradients(toy.d, toy.all(), scratch.data());

        p[i] = orig - eps;
        const double minus = m.compute_gradients(toy.d, toy.all(), scratch.data());

        p[i] = orig;

        const double numeric = (plus - minus) / (2.0 * eps);
        const double analytic = grad[i];
        if (!std::isfinite(numeric) || !std::isfinite(analytic)) { finite = false; }

        const double abs_err = std::fabs(numeric - analytic);
        const double rel_err = abs_err / (1.0 + std::fabs(analytic));
        if (abs_err > worst_abs) { worst_abs = abs_err; }
        if (rel_err > worst_rel) { worst_rel = rel_err; }
    }

    std::printf("        max |numeric - analytic| = %.3e, max rel = %.3e "
                "(over %zu parameters, eps = %.0e)\n",
                worst_abs, worst_rel, P, eps);
    check(finite, "every analytic and numeric gradient entry is finite");
    check(worst_rel < 1e-4,
          "the analytic gradient matches a central finite difference of the "
          "same objective to 1e-4 relative -- the ReLU derivative and the "
          "readout transpose are both correct");
}

/// 2. A Backprop checkpoint is the whole parameter vector, and reloading it
/// must not change the model.
void checkpoint_round_trip_is_bit_identical()
{
    constexpr std::size_t H = 6, X = 3;
    using Net = altair::Mlp<H, X>;
    Toy toy(40, X, true);

    Net trained(1e-3, altair::MlpTrainer::Backprop);
    trained.reset(0x5EEDu);
    for (int e = 0; e < 50; ++e) { (void)trained.train_epoch(toy.d, toy.all(), 0.05); }

    std::vector<double> snapshot(Net::kFullParams, 0.0);
    std::memcpy(snapshot.data(), trained.params(), snapshot.size() * sizeof(double));

    Net reloaded(1e-3, altair::MlpTrainer::Backprop);
    reloaded.reset(0x1111u);              // deliberately a DIFFERENT seed
    reloaded.load_params(snapshot.data());

    std::vector<double> a(toy.n, 0.0), b(toy.n, 0.0);
    trained.predict(toy.d, toy.all(), a.data());
    reloaded.predict(toy.d, toy.all(), b.data());

    bool identical = true;
    for (std::size_t i = 0; i < toy.n; ++i) {
        if (std::memcmp(&a[i], &b[i], sizeof(double)) != 0) { identical = false; }
    }
    check(Net::kFullParams == H * X + H + H + 1,
          "the full parameter vector is W1 (H*X) plus b1 (H) plus the readout "
          "and its intercept (H+1)");
    check(trained.param_count() == Net::kFullParams,
          "a Backprop model reports the whole vector as its checkpoint");
    check(identical,
          "a checkpoint reloaded into a differently seeded model predicts "
          "bit-identically -- the hidden layer travels with the readout");
}

/// 3. Determinism: the same seed and the same steps must reach the same place.
void same_seed_same_params()
{
    constexpr std::size_t H = 5, X = 3;
    using Net = altair::Mlp<H, X>;
    Toy toy(30, X, true);

    Net a(1e-3, altair::MlpTrainer::Backprop), b(1e-3, altair::MlpTrainer::Backprop);
    a.reset(0xBEEFu);
    b.reset(0xBEEFu);
    for (int e = 0; e < 25; ++e) {
        (void)a.train_epoch(toy.d, toy.all(), 0.03);
        (void)b.train_epoch(toy.d, toy.all(), 0.03);
    }
    check(std::memcmp(a.params(), b.params(),
                      Net::kFullParams * sizeof(double)) == 0,
          "two runs with the same seed take bit-identical steps (rule 10)");
}

/// 4. A zero learning rate is a no-op, and an empty block touches nothing.
void degenerate_steps_change_nothing()
{
    constexpr std::size_t H = 4, X = 2;
    using Net = altair::Mlp<H, X>;
    Toy toy(20, X, true);

    Net m(1e-3, altair::MlpTrainer::Backprop);
    m.reset(0x1234u);
    std::vector<double> before(Net::kFullParams, 0.0);
    std::memcpy(before.data(), m.params(), before.size() * sizeof(double));

    (void)m.train_epoch(toy.d, toy.all(), 0.0);
    check(std::memcmp(before.data(), m.params(),
                      before.size() * sizeof(double)) == 0,
          "a zero learning rate leaves every parameter unchanged");

    const double empty = m.train_epoch(toy.d, altair::Block{7, 7}, 0.5);
    check(empty == 0.0
              && std::memcmp(before.data(), m.params(),
                             before.size() * sizeof(double)) == 0,
          "an empty block returns zero and steps nothing");
}

/// 5. A label with zero weight carries no gradient.
void zero_weight_rows_carry_no_gradient()
{
    constexpr std::size_t H = 4, X = 2;
    using Net = altair::Mlp<H, X>;
    Toy toy(16, X, true);
    for (double& w : toy.ws) { w = 0.0; }

    Net m(1e-3, altair::MlpTrainer::Backprop);
    m.reset(0x77u);
    std::vector<double> grad(Net::kFullParams, 1.0);   // pre-filled, must be zeroed
    const double loss = m.compute_gradients(toy.d, toy.all(), grad.data());

    bool all_zero = true;
    for (double g : grad) { if (g != 0.0) { all_zero = false; } }
    check(all_zero,
          "every sample weighted zero produces no gradient at all -- an "
          "overlapping label is not an observation");
    check(loss == 0.0, "and the weighted objective is zero, not undefined");
}

/// 6. The point of the card: backprop trains the hidden layer and reaches a
/// solution a frozen random basis is not handed. The random-features baseline
/// is PRINTED rather than asserted, because gating "backprop beats random
/// features" would be asserting a property of one seed.
void backprop_learns_a_nonlinear_target()
{
    constexpr std::size_t H = 8, X = 2;
    using Net = altair::Mlp<H, X>;
    Toy toy(64, X, false);         // relu(x0+x1) - relu(x0-x1): not linear

    Net bp(1e-3, altair::MlpTrainer::Backprop);
    bp.reset(0xC0FFEEu);
    const double bp_start = bp.train_epoch(toy.d, toy.all(), 0.0);
    // 6,000 full-batch steps. The epoch count is a property of this fixture,
    // not a tuned threshold: it is the number at which the run settles.
    for (int e = 0; e < 6000; ++e) { (void)bp.train_epoch(toy.d, toy.all(), 0.10); }
    const double bp_end = bp.train_epoch(toy.d, toy.all(), 0.0);

    // The frozen-basis baseline, for contrast only.
    Net rf(1e-3, altair::MlpTrainer::RandomFeatures);
    rf.reset(0xC0FFEEu);
    (void)rf.train_epoch(toy.d, toy.all(), 0.0);   // one ridge solve
    const double rf_end = rf.train_epoch(toy.d, toy.all(), 0.0);

    std::printf("        piecewise train MSE  backprop %.6f (start %.6f), "
                "frozen random basis %.6f\n",
                bp_end, bp_start, rf_end);
    check(std::isfinite(bp_end) && bp_end < 0.01,
          "backprop drives an exactly-representable piecewise-linear target "
          "below 0.01 train MSE -- the hidden layer learned, not just the "
          "readout");
    check(bp_end < 0.25 * bp_start,
          "and the improvement is real -- at least four times below its own "
          "starting loss, not a rounding difference");
    check(rf.param_count() == H + 1,
          "the random-features mode still reports its readout-only checkpoint, "
          "so existing callers and their recorded numbers do not move");
}

}  // namespace

int main()
{
    std::printf("M12 trainable MLP\n\n");

    gradients_match_finite_differences();
    checkpoint_round_trip_is_bit_identical();
    same_seed_same_params();
    degenerate_steps_change_nothing();
    zero_weight_rows_carry_no_gradient();
    backprop_learns_a_nonlinear_target();

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : "THERE WERE FAILURES");
    return failures == 0 ? 0 : 1;
}
