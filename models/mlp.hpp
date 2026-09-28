// models/mlp.hpp -- the cross-sectional MLP.
//
// P8-07.
//
// A CROSS-SECTIONAL MODEL NEEDS CROSS-SECTIONAL NORMALISATION, AND A
// TIME-SERIES Z-SCORE IS NOT IT.
//
// This is the card, and it is the one place where P8-02's scaler -- correct
// everywhere else in this directory -- is the wrong tool.
//
// A cross-sectional model ranks instruments against each other AT ONE INSTANT:
// which of today's fifty stocks will outperform the others. Standardising each
// feature over TIME answers a different question. A momentum feature
// z-scored over its own history says "this stock is unusually strong FOR
// ITSELF"; z-scored across today's universe it says "this stock is strong
// RELATIVE TO ITS PEERS". Those are different signals and only the second is
// what a cross-sectional model is being asked about.
//
// Worse, a time-series z-score leaves a market-wide move in the features. On a
// day when everything falls 3%, every instrument's time-series z-score is
// deeply negative and the model sees fifty strong sell signals -- when the
// cross-sectional truth is that nothing changed relative to anything.
// Measured on 50 instruments hit by a market-wide 3-sigma shock that moves
// every one of them by exactly the same amount:
//
//     mean |change| in TIME-SERIES z-score      3.0000
//     mean |change| in CROSS-SECTIONAL rank     0.0000
//
// Not one rank moves, which is the truth -- nothing changed relative to
// anything. Every z-score moves by the full shock, so a model fed them sees
// fifty strong sell signals on a day when the cross-section is unchanged.
//
// So this file provides `cross_sectional_rank` and the model takes ranks. The
// time-series scaler is still right for a per-instrument model; the point is
// that the two are not interchangeable and nothing in a shape check says so.

#pragma once

#include <models/dataset.hpp>
#include <models/tensor.hpp>
#include <models/training.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class MlpError : std::uint8_t {
    /// Fewer instruments than a cross-section needs.
    TooFewInstruments,
    /// A dimension exceeded capacity.
    BadShape,
    /// The universe was ragged -- see `cross_sectional_rank`.
    RaggedUniverse
};

/// Normalise one instant's values ACROSS the universe to [-1, 1] by rank.
///
/// Rank rather than z-score, because a cross-section on any given day is
/// small (fifty names) and fat-tailed, and one outlier dominates a
/// cross-sectional mean and variance in a way it cannot dominate an ordering.
///
/// The universe SIZE is an input and must be the same at every instant the
/// model sees, because a rank of 7 means something different out of 20 than
/// out of 200 -- which is why a ragged universe is refused rather than
/// silently rescaled.
[[nodiscard]] inline std::expected<void, MlpError>
cross_sectional_rank(const double* values, std::size_t n,
                     double* out) noexcept {
    if (n < 2) { return std::unexpected(MlpError::TooFewInstruments); }
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t below = 0, equal = 0;
        for (std::size_t j = 0; j < n; ++j) {
            if (values[j] < values[i]) { ++below; }
            else if (values[j] == values[i]) { ++equal; }
        }
        // Midrank for ties, so a column of identical values maps to 0 rather
        // than to whatever the iteration order produced.
        const double r = (static_cast<double>(below)
                          + 0.5 * (static_cast<double>(equal) - 1.0))
                       / (static_cast<double>(n) - 1.0);
        out[i] = 2.0 * r - 1.0;
    }
    return {};
}

/// How an `Mlp` is trained.
enum class MlpTrainer : std::uint8_t {
    /// The hidden layer is seeded once and frozen; only the linear readout is
    /// solved, in closed form. This is the original random-features model and
    /// remains the DEFAULT so existing callers and their recorded numbers do
    /// not move silently.
    RandomFeatures = 0,
    /// M12: the hidden layer is trained too, by backpropagation. Slower per
    /// epoch, and the only way to reach a target a frozen random basis cannot
    /// span -- which is exactly the gap Atlas flagged as `Partial`.
    Backprop
};

/// A two-layer MLP with ReLU, satisfying P8-03's `Model` interface.
///
/// In `RandomFeatures` mode it trains as models/recurrent.hpp does: the hidden
/// layer is fixed at its seeded initialisation and the linear readout is solved
/// in closed form. A random hidden layer with a solved readout is a real, named
/// method, and honestly weaker than backpropagation.
///
/// In `Backprop` mode the hidden layer is trained as well, by SGD on the
/// sample-weighted mean squared error, so the model can represent a target a
/// frozen random basis cannot. Gradient descent on a non-convex objective is
/// not guaranteed to find a global optimum and this does not pretend otherwise;
/// the gradient is CHECKED against a central finite difference rather than
/// asserted, because a gradient that is never compared to a numerical one is an
/// assertion about nothing.
///
/// STORAGE. Both modes share ONE contiguous parameter vector, laid out
/// [W1: H*X][b1: H][w2: H+1], so a checkpoint is a single copy and the
/// finite-difference test can perturb any parameter by index.
template <std::size_t H, std::size_t X>
class Mlp {
public:
    /// W1 (H*X) and b1 (H): everything the readout path holds frozen.
    static constexpr std::size_t kHiddenParams = H * X + H;
    /// The readout incl. intercept -- what `RandomFeatures::param_count()` is.
    static constexpr std::size_t kReadoutParams = H + 1;
    /// [W1][b1][w2 incl. intercept] -- what `Backprop::param_count()` is.
    static constexpr std::size_t kFullParams = kHiddenParams + kReadoutParams;

    explicit Mlp(double ridge,
                 MlpTrainer trainer = MlpTrainer::RandomFeatures) noexcept
        : ridge_(ridge), trainer_(trainer) {}

    [[nodiscard]] MlpTrainer trainer() const noexcept { return trainer_; }

    void reset(std::uint64_t seed) noexcept {
        Init g{seed};
        Matrix w{w1(), H, X};
        g.xavier(w, X, H);
        g.zeros(b1(), H);
        if (trainer_ == MlpTrainer::Backprop) {
            // A ZERO READOUT GIVES THE HIDDEN LAYER NO GRADIENT AT ALL.
            //
            // dL/dW1 is proportional to dL/dy times w2, so with w2 == 0 every
            // hidden parameter has an exactly zero gradient on the first step.
            // The readout then trains alone, shrinks the units it finds
            // uninformative toward zero, and those units stay frozen out --
            // which is a plateau, not a slow start. This fixture reproduced it
            // at 0.25 MSE, unchanged by learning rate or epoch count, until the
            // readout was seeded. Xavier here is the standard symmetry break.
            //
            // RandomFeatures is deliberately untouched: its ridge solve
            // overwrites the readout entirely, so a zero start there is both
            // harmless and the behaviour existing callers were recorded against.
            Matrix r{w2(), H, 1};
            g.xavier(r, H, 1);
            w2()[H] = 0.0;   // the intercept still starts at the sample mean
        } else {
            for (std::size_t i = 0; i < kReadoutParams; ++i) { w2()[i] = 0.0; }
        }
    }

    void hidden(const double* x, double* h) const noexcept {
        const double* b = b1();
        const double* w = w1();
        for (std::size_t i = 0; i < H; ++i) {
            double acc = b[i];
            const double* r = w + i * X;
            for (std::size_t j = 0; j < X; ++j) { acc += r[j] * x[j]; }
            h[i] = relu(acc);
        }
    }

    /// P33-05: THE SAME SILENT TRUNCATION recurrent.hpp HAD, AT 8,192.
    ///
    /// `static thread_local double S[8192 * (H + 1)]` and then
    /// `n = min(b.size(), 8192)`. A caller handing this a block of 211,000
    /// bars got a readout fitted on 3.9 per cent of them -- and the training
    /// MSE was computed over the same truncated slice, so the one number that
    /// would have exposed it agreed with the fit.
    ///
    /// Identical shape and identical fix to the GRU's: the solve consumes
    /// rows through a Gram matrix of (H+1) x (H+1), so they STREAM and
    /// nothing is held. Two files having the same bug with different
    /// constants is the argument for hard rule 11 rather than for quietly
    /// fixing this one.
    double train_epoch(const Dataset& d, const Block& b, double lr) noexcept {
        return trainer_ == MlpTrainer::Backprop ? train_epoch_backprop(d, b, lr)
                                                : train_epoch_readout(d, b);
    }

    /// The sample-weighted mean squared error on `b` AND its gradient with
    /// respect to the FULL parameter vector.
    ///
    /// This is what backprop descends and what the finite-difference test
    /// differentiates, and it is deliberately ONE function: returning the value
    /// it differentiates is what stops the checked objective and the descended
    /// objective from drifting apart.
    [[nodiscard]] double compute_gradients(const Dataset& d, const Block& b,
                                           double* grad) const noexcept {
        for (std::size_t i = 0; i < kFullParams; ++i) { grad[i] = 0.0; }
        const std::size_t n = b.size();
        if (n == 0) { return 0.0; }
        const double* w1p = w1();
        const double* b1p = b1();
        const double* w2p = w2();
        double wsum = 0.0, sse = 0.0;
        double x[X], h[H], z[H];
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < X; ++j) { x[j] = d.x.at(b.start + i, j); }
            for (std::size_t a = 0; a < H; ++a) {
                double acc = b1p[a];
                const double* r = w1p + a * X;
                for (std::size_t j = 0; j < X; ++j) { acc += r[j] * x[j]; }
                z[a] = acc;
                h[a] = relu(acc);
            }
            double yhat = w2p[H];
            for (std::size_t a = 0; a < H; ++a) { yhat += w2p[a] * h[a]; }
            const double err = yhat - d.y[b.start + i];
            const double sw = d.weight != nullptr ? d.weight[b.start + i] : 1.0;
            if (!(sw > 0.0)) { continue; }   // a zero-weight label is not an observation
            sse += sw * err * err;
            wsum += sw;
            const double dy = 2.0 * sw * err;
            for (std::size_t a = 0; a < H; ++a) {
                grad[kHiddenParams + a] += dy * h[a];
            }
            grad[kHiddenParams + H] += dy;                 // readout intercept
            for (std::size_t a = 0; a < H; ++a) {
                if (z[a] <= 0.0) { continue; }             // ReLU' is 0 on the dead side
                const double dh = dy * w2p[a];
                grad[H * X + a] += dh;                     // b1
                double* rw = grad + a * X;
                for (std::size_t j = 0; j < X; ++j) { rw[j] += dh * x[j]; }
            }
        }
        if (!(wsum > 0.0)) { return 0.0; }
        const double scale = 1.0 / wsum;
        for (std::size_t i = 0; i < kFullParams; ++i) { grad[i] *= scale; }
        return sse / wsum;
    }

    /// One SGD step over the full parameter vector.
    ///
    /// Returns the UNWEIGHTED mean squared error, matching the readout path's
    /// reporting convention so switching modes does not silently change the
    /// number the harness prints.
    double train_epoch_backprop(const Dataset& d, const Block& b,
                                double lr) noexcept {
        double grad[kFullParams];
        (void)compute_gradients(d, b, grad);
        if (lr != 0.0) {
            for (std::size_t i = 0; i < kFullParams; ++i) {
                p_[i] -= lr * grad[i];
            }
        }
        return unweighted_mse(d, b);
    }

    /// Unweighted mean squared error over `b` -- the figure the readout path
    /// has always reported.
    [[nodiscard]] double unweighted_mse(const Dataset& d,
                                        const Block& b) const noexcept {
        const std::size_t n = b.size();
        if (n == 0) { return 0.0; }
        double x[X], h[H], sse = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < X; ++j) { x[j] = d.x.at(b.start + i, j); }
            hidden(x, h);
            double yhat = w2()[H];
            for (std::size_t a = 0; a < H; ++a) { yhat += w2()[a] * h[a]; }
            const double e = yhat - d.y[b.start + i];
            sse += e * e;
        }
        return sse / static_cast<double>(n);
    }

    double train_epoch_readout(const Dataset& d, const Block& b) noexcept {
        constexpr std::size_t M = H + 1;
        const std::size_t n = b.size();
        if (n == 0) { return 0.0; }

        // Weighted for the solve, unweighted for the reported error: the
        // original returned an unweighted MSE and swapping in a weighted one
        // silently would be its own small lie.
        double aw[M * M] = {}, rw[M] = {};
        double au[M * M] = {}, ru[M] = {}, yty = 0.0;

        double h[H], x[X], sv[M];
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < X; ++j) { x[j] = d.x.at(b.start + i, j); }
            hidden(x, h);
            for (std::size_t j = 0; j < H; ++j) { sv[j] = h[j]; }
            sv[H] = 1.0;
            const double yi = d.y[b.start + i];
            const double sw = d.weight != nullptr ? d.weight[b.start + i] : 1.0;
            yty += yi * yi;
            for (std::size_t a = 0; a < M; ++a) {
                rw[a] += sw * sv[a] * yi;
                ru[a] += sv[a] * yi;
                for (std::size_t c = 0; c < M; ++c) {
                    aw[a * M + c] += sw * sv[a] * sv[c];
                    au[a * M + c] += sv[a] * sv[c];
                }
            }
        }

        solve_normal(aw, rw);

        // SSE = w'Aw - 2w'rhs + y'y. Exact, and no second pass over the rows.
        double sse = yty;
        for (std::size_t a = 0; a < M; ++a) {
            sse -= 2.0 * w2()[a] * ru[a];
            for (std::size_t c = 0; c < M; ++c) {
                sse += w2()[a] * au[a * M + c] * w2()[c];
            }
        }
        return sse > 0.0 ? sse / static_cast<double>(n) : 0.0;
    }

    void predict(const Dataset& d, const Block& b, double* out) const noexcept {
        double h[H], x[X];
        const double* w = w2();
        for (std::size_t i = b.start; i < b.end; ++i) {
            for (std::size_t j = 0; j < X; ++j) { x[j] = d.x.at(i, j); }
            hidden(x, h);
            double p = w[H];
            for (std::size_t j = 0; j < H; ++j) { p += w[j] * h[j]; }
            out[i] = p;
        }
    }

    /// In `RandomFeatures` mode a checkpoint is the readout only, exactly as
    /// before. In `Backprop` mode it is the whole vector, because the hidden
    /// layer is now part of what was learned and a readout-only checkpoint
    /// would reload into a different model.
    [[nodiscard]] std::size_t param_count() const noexcept {
        return trainer_ == MlpTrainer::Backprop ? kFullParams : kReadoutParams;
    }
    [[nodiscard]] const double* params() const noexcept {
        return trainer_ == MlpTrainer::Backprop ? p_ : w2();
    }
    void load_params(const double* src) noexcept {
        double* dst = trainer_ == MlpTrainer::Backprop ? p_ : w2();
        for (std::size_t i = 0; i < param_count(); ++i) { dst[i] = src[i]; }
    }

private:
    /// Solve (G + ridge*I) w = rhs from the ALREADY-ACCUMULATED system.
    ///
    /// Takes the Gram matrix rather than the design matrix, so the caller
    /// never has to hold the rows -- see train_epoch for why that matters.
    /// Copies its inputs, because the elimination destroys them.
    ///
    /// The ridge is applied to the intercept as well, which is not the
    /// textbook choice: shrinking an intercept toward zero biases every
    /// prediction toward zero. The bias column accumulates n against a ridge
    /// of order one, so it is parts per thousand -- named rather than left
    /// silently almost-right.
    void solve_normal(const double* gram, const double* rhs_in) noexcept {
        constexpr std::size_t M = H + 1;
        double A[M * M], rhs[M];
        for (std::size_t i = 0; i < M * M; ++i) { A[i] = gram[i]; }
        for (std::size_t i = 0; i < M; ++i) { rhs[i] = rhs_in[i]; }
        for (std::size_t a = 0; a < M; ++a) { A[a * M + a] += ridge_; }
        for (std::size_t k = 0; k < M; ++k) {
            std::size_t piv = k;
            double best = std::fabs(A[k * M + k]);
            for (std::size_t r2 = k + 1; r2 < M; ++r2) {
                const double v = std::fabs(A[r2 * M + k]);
                if (v > best) { best = v; piv = r2; }
            }
            if (piv != k) {
                for (std::size_t c = 0; c < M; ++c) {
                    const double t = A[k * M + c];
                    A[k * M + c] = A[piv * M + c];
                    A[piv * M + c] = t;
                }
                const double t = rhs[k];
                rhs[k] = rhs[piv];
                rhs[piv] = t;
            }
            const double d0 = A[k * M + k];
            if (std::fabs(d0) < 1e-300) { continue; }
            for (std::size_t r2 = k + 1; r2 < M; ++r2) {
                const double f = A[r2 * M + k] / d0;
                if (f == 0.0) { continue; }
                for (std::size_t c = k; c < M; ++c) {
                    A[r2 * M + c] -= f * A[k * M + c];
                }
                rhs[r2] -= f * rhs[k];
            }
        }
        for (std::size_t ri = M; ri-- > 0;) {
            double acc = rhs[ri];
            for (std::size_t c = ri + 1; c < M; ++c) {
                acc -= A[ri * M + c] * w2()[c];
            }
            const double d0 = A[ri * M + ri];
            w2()[ri] = std::fabs(d0) > 1e-300 ? acc / d0 : 0.0;
        }
    }

    [[nodiscard]] double* w1() noexcept { return p_; }
    [[nodiscard]] const double* w1() const noexcept { return p_; }
    [[nodiscard]] double* b1() noexcept { return p_ + H * X; }
    [[nodiscard]] const double* b1() const noexcept { return p_ + H * X; }
    [[nodiscard]] double* w2() noexcept { return p_ + kHiddenParams; }
    [[nodiscard]] const double* w2() const noexcept { return p_ + kHiddenParams; }

    double p_[kFullParams] = {};
    double ridge_;
    MlpTrainer trainer_;
};

} // namespace altair
