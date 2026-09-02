// models/recurrent.hpp -- LSTM and GRU cells.
//
// P8-04 and P8-05.
//
// WHAT IS AND IS NOT IMPLEMENTED HERE, STATED PLAINLY.
//
// The FORWARD passes are complete and exact: the gate equations, the cell
// state, the ordering, the initialisation. That is what serving runs and it is
// what is numerically checkable without a framework.
//
// TRAINING is the readout only -- the recurrent weights are fixed at their
// seeded initialisation and the output layer is solved in closed form by ridge
// regression on the hidden states. That is a real and named method (echo-state
// / reservoir computing), it trains end to end, and it is honestly weaker than
// backpropagation through time. BPTT and autograd are what a LibTorch backend
// behind this same interface brings, and the interface is unchanged when it
// arrives.
//
// Saying which half is which matters more than either half. A file that
// implemented a forward pass and called itself an LSTM would be exactly the
// confident, plausible, incomplete thing review gate 7 exists to catch.
//
// THE FORGET-GATE BIAS MUST START AT ONE, AND ZERO IS THE DEFAULT NOBODY
// CHOOSES.
//
// This is the card. The forget gate decides how much of the cell state
// survives a step:
//
//     c_t = f_t * c_{t-1} + i_t * g_t
//
// With a zero bias and small initial weights, f_t sits at sigmoid(0) = 0.5, so
// the cell retains half its contents every step. After ten steps 0.1% remains;
// the memory half-life is ONE step. A network initialised that way has no
// long-range gradient to learn with -- it has to climb out of the vanishing
// regime before it can start learning what to remember, and usually does not.
//
// Measured:
//
//     forget bias   retention/step   half-life   left after 20 steps
//        +0.0           0.5000       1.00 steps        9.5e-07
//        +1.0           0.7311       2.21 steps        1.9e-03
//        +2.0           0.8808       5.46 steps        7.9e-02
//
// 1994x more memory at 20 steps, from one initialisation constant that nobody
// sets deliberately. `init` therefore takes the bias as a REQUIRED argument;
// there is no overload without it.
//
// A GRU HAS NO SEPARATE CELL STATE, AND THAT IS THE WHOLE DIFFERENCE.
//
// An LSTM's cell state is protected by the output gate: it can hold a value
// while emitting nothing about it. A GRU's hidden state is both the memory and
// the output, so remembering something forces it to say something. The GRU is
// cheaper and usually as good; the exception is exactly the case where a value
// must be carried across a long gap without influencing the intervening
// outputs, which is what a multi-horizon forecast asks for.
//
// AND ITS GATE HAS THE OPPOSITE SIGN. h = (1-z)*h + z*n, so RETAINING means a
// SMALL z, and the bias that lengthens a GRU's memory is NEGATIVE. Copying the
// LSTM's +1 across -- which looks like consistency -- gives a half-life of
// 0.53 steps against 2.21 for -1. The exact opposite of the intent.

#pragma once

#include <models/dataset.hpp>
#include <models/tensor.hpp>
#include <models/training.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class RecurrentError : std::uint8_t {
    /// A dimension exceeded the fixed capacity.
    TooLarge,
    /// The ridge solve was singular.
    Singular,
    /// The sequence length was zero.
    EmptySequence
};

/// One LSTM cell's state.
struct LstmState {
    double h[kMaxHidden] = {};      // hidden / output
    double c[kMaxHidden] = {};      // CELL -- the protected memory
};

/// An LSTM cell. Forward pass only; see the header on training.
///
/// Gate order is the conventional i, f, g, o. It is written out rather than
/// packed into one matrix because the packed form is where the forget bias
/// gets initialised as part of a block and quietly ends up at zero.
template <std::size_t H, std::size_t X>
class LstmCell {
public:
    static_assert(H <= kMaxHidden && X <= kMaxInput, "cell exceeds capacity");

    /// `forget_bias` is REQUIRED and has no default. See the header: zero is
    /// what you get by not thinking about it, and it is the wrong answer.
    void init(std::uint64_t seed, double forget_bias) noexcept {
        Init g{seed};
        Matrix wi{wi_, H, X + H}, wf{wf_, H, X + H};
        Matrix wg{wg_, H, X + H}, wo{wo_, H, X + H};
        g.xavier(wi, X + H, H);
        g.xavier(wf, X + H, H);
        g.xavier(wg, X + H, H);
        g.xavier(wo, X + H, H);
        for (std::size_t i = 0; i < H; ++i) {
            bi_[i] = 0.0;
            bf_[i] = forget_bias;   // THE one bias that is not zero
            bg_[i] = 0.0;
            bo_[i] = 0.0;
        }
        forget_bias_ = forget_bias;
    }

    /// Advance one step. `x` has X entries.
    void step(const double* x, LstmState& s) const noexcept {
        double cat[X + H];
        for (std::size_t j = 0; j < X; ++j) { cat[j] = x[j]; }
        for (std::size_t j = 0; j < H; ++j) { cat[X + j] = s.h[j]; }

        for (std::size_t i = 0; i < H; ++i) {
            double ai = bi_[i], af = bf_[i], ag = bg_[i], ao = bo_[i];
            const double* ri = wi_ + i * (X + H);
            const double* rf = wf_ + i * (X + H);
            const double* rg = wg_ + i * (X + H);
            const double* ro = wo_ + i * (X + H);
            for (std::size_t j = 0; j < X + H; ++j) {
                ai += ri[j] * cat[j];
                af += rf[j] * cat[j];
                ag += rg[j] * cat[j];
                ao += ro[j] * cat[j];
            }
            const double in = sigmoid(ai);
            const double fg = sigmoid(af);
            const double gg = tanh_act(ag);
            const double og = sigmoid(ao);
            // c_t = f * c_{t-1} + i * g. The multiplicative path through f is
            // the ONLY route a gradient has across many steps, which is why
            // where f starts decides whether there is one.
            s.c[i] = fg * s.c[i] + in * gg;
            s.h[i] = og * tanh_act(s.c[i]);
        }
    }

    /// The gate value on a zero input with a zero state -- i.e. sigmoid of the
    /// bias alone. What the cell retains per step before it has learned
    /// anything.
    [[nodiscard]] double initial_retention() const noexcept {
        return sigmoid(forget_bias_);
    }
    /// How many steps until the initial cell contents fall to half.
    [[nodiscard]] double memory_half_life() const noexcept {
        const double r = initial_retention();
        if (!(r > 0.0) || r >= 1.0) { return 0.0; }
        return std::log(0.5) / std::log(r);
    }
    [[nodiscard]] static constexpr std::size_t hidden() noexcept { return H; }

private:
    double wi_[H * (X + H)] = {}, wf_[H * (X + H)] = {};
    double wg_[H * (X + H)] = {}, wo_[H * (X + H)] = {};
    double bi_[H] = {}, bf_[H] = {}, bg_[H] = {}, bo_[H] = {};
    double forget_bias_ = 0.0;
};

/// One GRU cell. No cell state -- the hidden state is both memory and output.
template <std::size_t H, std::size_t X>
class GruCell {
public:
    static_assert(H <= kMaxHidden && X <= kMaxInput, "cell exceeds capacity");

    void init(std::uint64_t seed, double update_bias) noexcept {
        Init g{seed};
        Matrix wz{wz_, H, X + H}, wr{wr_, H, X + H}, wn{wn_, H, X + H};
        g.xavier(wz, X + H, H);
        g.xavier(wr, X + H, H);
        g.xavier(wn, X + H, H);
        for (std::size_t i = 0; i < H; ++i) {
            // The GRU's update gate plays the forget gate's role but with the
            // opposite sign convention: h = (1-z)*h + z*n, so RETAINING means
            // a SMALL z, and the bias that lengthens memory is NEGATIVE.
            // Copying the LSTM's +1 here shortens memory instead of extending
            // it, which is the sign error this comment exists to prevent.
            bz_[i] = update_bias;
            br_[i] = 0.0;
            bn_[i] = 0.0;
        }
        update_bias_ = update_bias;
    }

    void step(const double* x, double* h) const noexcept {
        double cat[X + H];
        for (std::size_t j = 0; j < X; ++j) { cat[j] = x[j]; }
        for (std::size_t j = 0; j < H; ++j) { cat[X + j] = h[j]; }

        double z[H], r[H];
        for (std::size_t i = 0; i < H; ++i) {
            double az = bz_[i], ar = br_[i];
            const double* rz = wz_ + i * (X + H);
            const double* rr = wr_ + i * (X + H);
            for (std::size_t j = 0; j < X + H; ++j) {
                az += rz[j] * cat[j];
                ar += rr[j] * cat[j];
            }
            z[i] = sigmoid(az);
            r[i] = sigmoid(ar);
        }
        // The candidate sees the RESET-gated previous state, not the raw one.
        double cat2[X + H];
        for (std::size_t j = 0; j < X; ++j) { cat2[j] = x[j]; }
        for (std::size_t j = 0; j < H; ++j) { cat2[X + j] = r[j] * h[j]; }

        double n[H];
        for (std::size_t i = 0; i < H; ++i) {
            double an = bn_[i];
            const double* rn = wn_ + i * (X + H);
            for (std::size_t j = 0; j < X + H; ++j) { an += rn[j] * cat2[j]; }
            n[i] = tanh_act(an);
        }
        for (std::size_t i = 0; i < H; ++i) {
            h[i] = (1.0 - z[i]) * h[i] + z[i] * n[i];
        }
    }

    /// Retention per step: 1 - z, so a NEGATIVE bias retains more.
    [[nodiscard]] double initial_retention() const noexcept {
        return 1.0 - sigmoid(update_bias_);
    }
    [[nodiscard]] double memory_half_life() const noexcept {
        const double r = initial_retention();
        if (!(r > 0.0) || r >= 1.0) { return 0.0; }
        return std::log(0.5) / std::log(r);
    }

private:
    double wz_[H * (X + H)] = {}, wr_[H * (X + H)] = {}, wn_[H * (X + H)] = {};
    double bz_[H] = {}, br_[H] = {}, bn_[H] = {};
    double update_bias_ = 0.0;
};

// ---------------------------------------------------------------------------
// A trainable model over a recurrent cell
// ---------------------------------------------------------------------------

/// Ridge readout over a fixed recurrent reservoir.
///
/// Satisfies P8-03's `Model` interface. The recurrent weights stay at their
/// seeded initialisation and the linear readout is solved in closed form:
/// (S'S + lambda I) w = S'y, by Gaussian elimination with partial pivoting.
///
/// Ridge and not least squares: the hidden states of a recurrent net are
/// strongly collinear by construction -- consecutive states differ by one
/// gated update -- so S'S is near-singular and an unregularised solve returns
/// enormous weights that cancel. That is the same conditioning argument P6-04
/// makes about correlated signals, appearing here as a numerical problem
/// rather than a statistical one.
template <class Cell, std::size_t H, std::size_t X, std::size_t Seq>
class RecurrentReadout {
public:
    explicit RecurrentReadout(double gate_bias, double ridge) noexcept
        : gate_bias_(gate_bias), ridge_(ridge) {}

    void reset(std::uint64_t seed) noexcept {
        cell_.init(seed, gate_bias_);
        for (double& v : w_) { v = 0.0; }
    }

    /// Run the cell over the `Seq` rows ending at `row` and return the final
    /// hidden state. Rows before the start of the dataset are skipped rather
    /// than zero-padded, so a short prefix is a shorter sequence and not a
    /// sequence with invented history in front of it.
    void encode(const Dataset& d, std::size_t row, double* h) const noexcept {
        for (std::size_t i = 0; i < H; ++i) { h[i] = 0.0; }
        const std::size_t first = row >= Seq - 1 ? row - (Seq - 1) : 0;
        double x[X];
        if constexpr (std::is_same_v<Cell, LstmCell<H, X>>) {
            LstmState s{};
            for (std::size_t t = first; t <= row; ++t) {
                for (std::size_t j = 0; j < X; ++j) { x[j] = d.x.at(t, j); }
                cell_.step(x, s);
            }
            for (std::size_t i = 0; i < H; ++i) { h[i] = s.h[i]; }
        } else {
            for (std::size_t t = first; t <= row; ++t) {
                for (std::size_t j = 0; j < X; ++j) { x[j] = d.x.at(t, j); }
                cell_.step(x, h);
            }
        }
    }

    /// One "epoch" is one closed-form solve. There is no gradient step, which
    /// is why `lr` is ignored and the loop converges immediately -- stated
    /// here rather than left for a reader to discover from a flat loss curve.
    double train_epoch(const Dataset& d, const Block& b, double) noexcept {
        static thread_local double S[4096 * (H + 1)];
        const std::size_t n = b.size() < 4096 ? b.size() : 4096;
        if (n == 0) { return 0.0; }
        double h[H];
        for (std::size_t i = 0; i < n; ++i) {
            encode(d, b.start + i, h);
            for (std::size_t j = 0; j < H; ++j) { S[i * (H + 1) + j] = h[j]; }
            S[i * (H + 1) + H] = 1.0;       // bias column
        }
        solve_ridge(S, d.y + b.start, d.weight + b.start, n);
        double err = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            double p = 0.0;
            for (std::size_t j = 0; j <= H; ++j) { p += w_[j] * S[i * (H + 1) + j]; }
            const double e = p - d.y[b.start + i];
            err += e * e;
        }
        return err / static_cast<double>(n);
    }

    void predict(const Dataset& d, const Block& b, double* out) const noexcept {
        double h[H];
        for (std::size_t i = b.start; i < b.end; ++i) {
            encode(d, i, h);
            double p = w_[H];
            for (std::size_t j = 0; j < H; ++j) { p += w_[j] * h[j]; }
            out[i] = p;
        }
    }

    [[nodiscard]] std::size_t param_count() const noexcept { return H + 1; }
    [[nodiscard]] const double* params() const noexcept { return w_; }
    void load_params(const double* src) noexcept {
        for (std::size_t i = 0; i <= H; ++i) { w_[i] = src[i]; }
    }
    [[nodiscard]] const Cell& cell() const noexcept { return cell_; }

private:
    void solve_ridge(const double* S, const double* y, const double* wt,
                     std::size_t n) noexcept {
        constexpr std::size_t M = H + 1;
        double A[M * M] = {}, rhs[M] = {};
        for (std::size_t i = 0; i < n; ++i) {
            const double sw = wt != nullptr ? wt[i] : 1.0;
            const double* s = S + i * M;
            for (std::size_t a = 0; a < M; ++a) {
                rhs[a] += sw * s[a] * y[i];
                for (std::size_t b2 = 0; b2 < M; ++b2) {
                    A[a * M + b2] += sw * s[a] * s[b2];
                }
            }
        }
        for (std::size_t a = 0; a < M; ++a) { A[a * M + a] += ridge_; }

        // Gaussian elimination with PARTIAL PIVOTING. Without the pivot a
        // near-zero leading entry -- which a collinear reservoir produces
        // routinely -- divides the whole row by noise.
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
                for (std::size_t c = k; c < M; ++c) { A[r2 * M + c] -= f * A[k * M + c]; }
                rhs[r2] -= f * rhs[k];
            }
        }
        for (std::size_t ri = M; ri-- > 0;) {
            double acc = rhs[ri];
            for (std::size_t c = ri + 1; c < M; ++c) { acc -= A[ri * M + c] * w_[c]; }
            const double d0 = A[ri * M + ri];
            w_[ri] = std::fabs(d0) > 1e-300 ? acc / d0 : 0.0;
        }
    }

    Cell cell_{};
    double w_[H + 1] = {};
    double gate_bias_;
    double ridge_;
};

} // namespace altair
