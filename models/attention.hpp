// models/attention.hpp -- attention, gated residual networks, variable
// selection, and dilated causal convolution.
//
// P8-06 (TFT components) and P8-08 (temporal CNN + attention).
//
// ATTENTION WITHOUT A CAUSAL MASK IS A LOOK-AHEAD BUG WITH GOOD ACCURACY.
//
// This is the card, and it is rule 7 arriving in the one place the type system
// cannot help. Self-attention lets every position see every other position.
// That is the point of it, and on a sentence it is correct. On a time series
// it means position t attends to t+1, and the model is reading the answer.
//
// Nothing about the shapes objects. The tensors are the right size, the
// softmax sums to one, the loss falls, and the validation score is excellent
// because the validation set has the same structure. It only fails in
// production, where t+1 has not happened.
//
// So `attention` takes a `Causality` argument with no default, and
// `Causality::Unmasked` exists only so the cost can be measured.
//
// Measured, with the target for position t hidden in position t+1's value:
//
//     UNMASKED   mean |output - the future value|   0.0010
//     MASKED     mean |output - the future value|   4.4948
//
// The unmasked model recovers a value it was never given, to three decimal
// places, purely by attending to the position that holds it.
//
// The mask is applied BEFORE the softmax, as -infinity, not by zeroing weights
// after it. Zeroing after leaves the future in the normalising sum, so every
// VISIBLE weight is wrong even though no future value is read directly.
//
// A DILATED CONVOLUTION MUST BE LEFT-PADDED, AND "SAME" PADDING IS CENTRED.
//
// The same defect one layer down and easier to miss, because the padding
// argument in every library defaults to something reasonable for images.
// "Same" padding puts (k-1)/2 zeros on each side so the output length matches
// the input -- which centres the kernel, so output t is a function of inputs
// t-(k-1)/2 through t+(k-1)/2. Half the receptive field is the future.
//
// Causal padding puts all k-1 zeros on the LEFT. Output t then depends on
// inputs t-(k-1)*d through t, and nothing later.
//
// Measured by perturbing ONE input and seeing which outputs move -- kernel 5,
// dilation 2, spike at input 12:
//
//     CausalLeft   0 outputs before the spike move,  4 after
//     Same         2 outputs BEFORE the spike move,  2 after
//
// Two outputs that are functions of an input which had not arrived yet. The
// forward reach is (k-1)/2 * dilation = 4 samples, and exactly zero for
// causal.
//
// A SOFTMAX OVER FEATURES IS NOT FEATURE IMPORTANCE.
//
// The TFT's variable-selection network produces a softmax over inputs and it
// is routinely read as "the model says feature 3 matters most". It says
// feature 3's gate logit is largest, which is a RELATIVE statement among the
// features offered and carries no information about whether any of them
// matter. Two useless features give 50/50, exactly as two decisive ones do.
// `VariableSelection` therefore reports the softmax AND the magnitude of the
// gated output, because only the second distinguishes those cases.

#pragma once

#include <models/tensor.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

inline constexpr std::size_t kMaxAttnSeq = 128;

enum class AttentionError : std::uint8_t {
    /// The causality argument was not supplied.
    NoCausality,
    /// A dimension exceeded capacity or was zero.
    BadShape,
    /// The dilation or kernel width was zero.
    BadKernel
};

/// Whether a position may attend to positions after it.
///
/// No default. On a sentence, unmasked is right; on a time series it is a
/// look-ahead bug that improves every offline metric.
enum class Causality : std::uint8_t {
    Unspecified = 0,
    /// Position t sees only 0..t. The only correct choice for a forecast.
    Masked,
    /// Position t sees everything. Present so its cost can be measured.
    Unmasked
};

/// Scaled dot-product attention over one sequence.
///
/// `q`, `k`, `v` are [T x D]. `out` is [T x D]. `scores` is scratch, [T x T].
///
/// The 1/sqrt(D) scaling is not cosmetic: without it the dot products grow
/// with D, the softmax saturates, and the gradient through it vanishes -- the
/// same numerical failure as an unscaled logit, one layer up.
[[nodiscard]] inline std::expected<void, AttentionError>
attention(const Matrix& q, const Matrix& k, const Matrix& v, Matrix& out,
          double* scores, Causality c) noexcept {
    if (c == Causality::Unspecified) {
        return std::unexpected(AttentionError::NoCausality);
    }
    if (!q.valid() || !k.valid() || !v.valid() || !out.valid()) {
        return std::unexpected(AttentionError::BadShape);
    }
    const std::size_t T = q.rows(), D = q.cols();
    if (k.rows() != T || v.rows() != T || k.cols() != D || out.rows() != T
        || out.cols() != v.cols() || T > kMaxAttnSeq) {
        return std::unexpected(AttentionError::BadShape);
    }

    const double scale = 1.0 / std::sqrt(static_cast<double>(D));
    for (std::size_t i = 0; i < T; ++i) {
        double* row = scores + i * T;
        // THE MASK. Positions after i are set to -infinity BEFORE the softmax
        // rather than zeroed after it -- zeroing after leaves the future in
        // the normalising sum, so the visible weights are wrong even though
        // no future value is directly read.
        const std::size_t limit = c == Causality::Masked ? i + 1 : T;
        for (std::size_t j = 0; j < T; ++j) {
            if (j >= limit) { row[j] = -1e300; continue; }
            double dot = 0.0;
            for (std::size_t d = 0; d < D; ++d) { dot += q.at(i, d) * k.at(j, d); }
            row[j] = dot * scale;
        }
        softmax_row(row, T);
        for (std::size_t d = 0; d < v.cols(); ++d) {
            double acc = 0.0;
            for (std::size_t j = 0; j < T; ++j) { acc += row[j] * v.at(j, d); }
            out.at(i, d) = acc;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Dilated causal convolution (P8-08)
// ---------------------------------------------------------------------------

/// How the input is padded.
enum class Padding : std::uint8_t {
    Unspecified = 0,
    /// All k-1 zeros on the LEFT. Output t depends on inputs at or before t.
    CausalLeft,
    /// (k-1)/2 zeros on each side. The library default, and half the
    /// receptive field is the future. Present so it can be measured.
    Same
};

/// One dilated 1-D convolution over a [T x C] input.
///
/// `w` is [C_out x (C_in * K)], `out` is [T x C_out].
[[nodiscard]] inline std::expected<void, AttentionError>
dilated_conv(const Matrix& in, const Matrix& w, const double* bias,
             std::size_t kernel, std::size_t dilation, Matrix& out,
             Padding p) noexcept {
    if (p == Padding::Unspecified) {
        return std::unexpected(AttentionError::BadKernel);
    }
    if (kernel == 0 || dilation == 0) {
        return std::unexpected(AttentionError::BadKernel);
    }
    if (!in.valid() || !w.valid() || !out.valid()) {
        return std::unexpected(AttentionError::BadShape);
    }
    const std::size_t T = in.rows(), C = in.cols(), F = out.cols();
    if (out.rows() != T || w.rows() != F || w.cols() != C * kernel) {
        return std::unexpected(AttentionError::BadShape);
    }

    // The ONLY difference between the two paddings: where the kernel sits
    // relative to t. Left-padded, tap m reaches back (K-1-m)*dilation. Same-
    // padded, it is centred and half the taps reach FORWARD.
    const std::int64_t centre = p == Padding::Same
        ? static_cast<std::int64_t>((kernel - 1) / 2 * dilation)
        : 0;

    for (std::size_t t = 0; t < T; ++t) {
        for (std::size_t f = 0; f < F; ++f) {
            double acc = bias != nullptr ? bias[f] : 0.0;
            for (std::size_t m = 0; m < kernel; ++m) {
                const std::int64_t src = static_cast<std::int64_t>(t)
                    - static_cast<std::int64_t>((kernel - 1 - m) * dilation)
                    + centre;
                if (src < 0 || src >= static_cast<std::int64_t>(T)) { continue; }
                for (std::size_t ci = 0; ci < C; ++ci) {
                    acc += w.at(f, ci * kernel + m)
                         * in.at(static_cast<std::size_t>(src), ci);
                }
            }
            out.at(t, f) = acc;
        }
    }
    return {};
}

/// How far forward, in samples, an output at t can see under a padding.
///
/// Zero for CausalLeft. Positive for Same, which is the whole problem stated
/// as a number a test can assert on.
[[nodiscard]] inline std::size_t forward_reach(std::size_t kernel,
                                               std::size_t dilation,
                                               Padding p) noexcept {
    if (p != Padding::Same || kernel < 2) { return 0; }
    return (kernel - 1) / 2 * dilation;
}

// ---------------------------------------------------------------------------
// TFT components (P8-06)
// ---------------------------------------------------------------------------

/// A gated linear unit: the gate that lets a TFT skip a whole sub-network.
///
/// out = sigmoid(gate) * value. The point is that a layer which is not useful
/// can be switched off entirely rather than learned around, which is what
/// makes the architecture work on small data -- and small data is what a
/// trading series always is.
inline void glu(const double* value, const double* gate, double* out,
                std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) { out[i] = sigmoid(gate[i]) * value[i]; }
}

/// A gated residual network: x + GLU(f(x)), with the residual carrying the
/// input past the transform untouched.
inline void gated_residual(const double* x, const double* value,
                           const double* gate, double* out,
                           std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = x[i] + sigmoid(gate[i]) * value[i];
    }
}

/// The result of a variable-selection network.
struct Selection {
    /// The softmax over features. Sums to one. RELATIVE, and therefore not
    /// feature importance -- see the header.
    double weight[kMaxInput] = {};
    /// The magnitude of the gated, weighted output. THIS is what distinguishes
    /// "feature 3 matters most among these" from "none of them matter".
    double output_magnitude = 0.0;
    std::size_t n = 0;

    /// The largest weight, and how much it exceeds an even split. A value near
    /// 1.0 means the softmax is not selecting anything.
    [[nodiscard]] double concentration() const noexcept {
        if (n == 0) { return 0.0; }
        double m = 0.0;
        for (std::size_t i = 0; i < n; ++i) { if (weight[i] > m) { m = weight[i]; } }
        return m * static_cast<double>(n);
    }
};

/// Softmax-weighted feature selection.
///
/// Reports BOTH the weights and the output magnitude. A caller reading only
/// the weights learns which feature won a contest, not whether the contest was
/// about anything.
[[nodiscard]] inline std::expected<Selection, AttentionError>
variable_selection(const double* features, const double* logits,
                   std::size_t n) noexcept {
    if (n == 0 || n > kMaxInput) {
        return std::unexpected(AttentionError::BadShape);
    }
    Selection s{};
    s.n = n;
    for (std::size_t i = 0; i < n; ++i) { s.weight[i] = logits[i]; }
    softmax_row(s.weight, n);
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) { acc += s.weight[i] * features[i]; }
    s.output_magnitude = std::fabs(acc);
    return s;
}

} // namespace altair
