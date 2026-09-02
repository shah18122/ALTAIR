// models/tensor.hpp -- the minimal tensor every model in this directory uses.
//
// P8-02 (shared by P8-03 through P8-12).
//
// WHY THIS EXISTS RATHER THAN AT::TENSOR.
//
// The ROADMAP names LibTorch for training and ONNX for serving, and that is
// still the plan. LibTorch is not vendored yet -- it is about 2.5 GB, and the
// `net` preset already caps at four parallel jobs because Beast headers
// exhausted the compiler's heap on this box. So the architectures in this
// directory are built against this type, behind a stable interface, and a
// LibTorch backend drops in behind the same interface without touching a
// caller.
//
// That is not a workaround for its own sake. Everything downstream -- the
// training loop, the registry, the aggregator, the confidence intervals -- is
// framework-independent, and writing it against a type we control means the
// framework choice stays reversible instead of being cemented into every
// signature.
//
// SHAPES ARE CHECKED, NOT ASSUMED.
//
// A shape mismatch in a matrix multiply is the single most common bug in model
// code and it is silent when the dimensions happen to be compatible for the
// wrong reason -- a batch of 64 features against 64 hidden units multiplies
// fine and means nothing. Every operation here returns `std::expected` and
// checks. The cost is a branch per call, on a path that then does O(n^3) work.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class TensorError : std::uint8_t {
    /// The shapes are not compatible for this operation.
    ShapeMismatch,
    /// A dimension was zero.
    EmptyDimension,
    /// The tensor does not fit its storage.
    TooLarge,
    /// A non-finite value reached an operation that cannot carry one.
    NotFinite
};

/// A 2-D row-major tensor over caller-provided storage.
///
/// A VIEW, not an owner. Allocation is the caller's decision and happens once
/// at setup, never inside a training step -- the same discipline as the hot
/// path, for a different reason: a training loop that allocates per batch
/// spends its time in the allocator rather than in the arithmetic.
class Matrix {
public:
    Matrix() = default;
    Matrix(double* data, std::size_t rows, std::size_t cols) noexcept
        : d_(data), r_(rows), c_(cols) {}

    [[nodiscard]] std::size_t rows() const noexcept { return r_; }
    [[nodiscard]] std::size_t cols() const noexcept { return c_; }
    [[nodiscard]] std::size_t size() const noexcept { return r_ * c_; }
    [[nodiscard]] double* data() noexcept { return d_; }
    [[nodiscard]] const double* data() const noexcept { return d_; }

    [[nodiscard]] double& at(std::size_t i, std::size_t j) noexcept {
        return d_[i * c_ + j];
    }
    [[nodiscard]] double at(std::size_t i, std::size_t j) const noexcept {
        return d_[i * c_ + j];
    }
    [[nodiscard]] double* row(std::size_t i) noexcept { return d_ + i * c_; }
    [[nodiscard]] const double* row(std::size_t i) const noexcept {
        return d_ + i * c_;
    }

    void fill(double v) noexcept {
        for (std::size_t i = 0; i < r_ * c_; ++i) { d_[i] = v; }
    }
    [[nodiscard]] bool valid() const noexcept {
        return d_ != nullptr && r_ > 0 && c_ > 0;
    }
    [[nodiscard]] bool all_finite() const noexcept {
        for (std::size_t i = 0; i < r_ * c_; ++i) {
            if (!std::isfinite(d_[i])) { return false; }
        }
        return true;
    }

private:
    double* d_ = nullptr;
    std::size_t r_ = 0;
    std::size_t c_ = 0;
};

/// out = a * b. Shapes checked.
[[nodiscard]] inline std::expected<void, TensorError>
matmul(const Matrix& a, const Matrix& b, Matrix& out) noexcept {
    if (!a.valid() || !b.valid() || !out.valid()) {
        return std::unexpected(TensorError::EmptyDimension);
    }
    if (a.cols() != b.rows() || out.rows() != a.rows()
        || out.cols() != b.cols()) {
        return std::unexpected(TensorError::ShapeMismatch);
    }
    for (std::size_t i = 0; i < a.rows(); ++i) {
        double* o = out.row(i);
        for (std::size_t j = 0; j < b.cols(); ++j) { o[j] = 0.0; }
        for (std::size_t k = 0; k < a.cols(); ++k) {
            const double aik = a.at(i, k);
            if (aik == 0.0) { continue; }
            const double* bk = b.row(k);
            for (std::size_t j = 0; j < b.cols(); ++j) { o[j] += aik * bk[j]; }
        }
    }
    return {};
}

/// out += bias, broadcast across rows.
[[nodiscard]] inline std::expected<void, TensorError>
add_bias(Matrix& out, const double* bias, std::size_t n) noexcept {
    if (!out.valid() || bias == nullptr) {
        return std::unexpected(TensorError::EmptyDimension);
    }
    if (n != out.cols()) { return std::unexpected(TensorError::ShapeMismatch); }
    for (std::size_t i = 0; i < out.rows(); ++i) {
        double* o = out.row(i);
        for (std::size_t j = 0; j < n; ++j) { o[j] += bias[j]; }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Activations
// ---------------------------------------------------------------------------

/// Numerically stable logistic.
///
/// The textbook 1/(1+exp(-x)) overflows for x below about -745 and returns a
/// denormal long before that. Splitting on the sign keeps the exponent
/// negative in both branches, which is exactly the same reasoning as
/// analytics/greeks.hpp using erfc rather than erf.
[[nodiscard]] inline double sigmoid(double x) noexcept {
    if (x >= 0.0) { return 1.0 / (1.0 + std::exp(-x)); }
    const double e = std::exp(x);
    return e / (1.0 + e);
}

[[nodiscard]] inline double tanh_act(double x) noexcept { return std::tanh(x); }

[[nodiscard]] inline double relu(double x) noexcept {
    return x > 0.0 ? x : 0.0;
}

/// Softmax over one row, IN PLACE, with the max subtracted first.
///
/// Subtracting the max is not an optimisation. exp(800) is infinity and
/// infinity/infinity is NaN, so a row of large logits produces a NaN
/// probability vector that then propagates into a position size. The
/// subtraction is algebraically a no-op and numerically the whole function.
inline void softmax_row(double* x, std::size_t n) noexcept {
    if (n == 0) { return; }
    double m = x[0];
    for (std::size_t i = 1; i < n; ++i) { if (x[i] > m) { m = x[i]; } }
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) { x[i] = std::exp(x[i] - m); s += x[i]; }
    if (s > 0.0) {
        for (std::size_t i = 0; i < n; ++i) { x[i] /= s; }
    }
}

/// Deterministic parameter initialisation.
///
/// Xavier/Glorot: uniform on +-sqrt(6/(fan_in+fan_out)). SEEDED, because rule
/// 10 wants every decision reproducible and a model whose initial weights
/// cannot be regenerated cannot be re-trained to the same place.
class Init {
public:
    /// xorshift needs a non-zero state. Replace ONLY a zero seed -- an
    /// earlier `seed | 1` collapsed every even seed onto the odd one above
    /// it, so 42 and 43 produced bit-identical streams and half of any seed
    /// sweep was a duplicate of the other half.
    explicit Init(std::uint64_t seed) noexcept
        : s_(seed != 0 ? seed : 0x9E3779B97F4A7C15ull) {}

    void xavier(Matrix& m, std::size_t fan_in, std::size_t fan_out) noexcept {
        const double lim = std::sqrt(6.0 / static_cast<double>(fan_in + fan_out));
        for (std::size_t i = 0; i < m.size(); ++i) {
            m.data()[i] = (2.0 * next() - 1.0) * lim;
        }
    }
    void zeros(double* v, std::size_t n) noexcept {
        for (std::size_t i = 0; i < n; ++i) { v[i] = 0.0; }
    }
    /// LSTM and GRU forget gates are conventionally biased toward 1 so the
    /// cell remembers by default and has to learn to forget. Starting at zero
    /// makes the gradient vanish through long sequences before training has a
    /// chance to open the gate.
    void ones(double* v, std::size_t n) noexcept {
        for (std::size_t i = 0; i < n; ++i) { v[i] = 1.0; }
    }

private:
    [[nodiscard]] double next() noexcept {
        s_ ^= s_ >> 12; s_ ^= s_ << 25; s_ ^= s_ >> 27;
        return static_cast<double>((s_ * 2685821657736338717ull) >> 11)
             / static_cast<double>(1ull << 53);
    }
    std::uint64_t s_;
};

} // namespace altair
