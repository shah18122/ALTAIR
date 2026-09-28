// models/transformer.hpp -- fixed-capacity causal, trainable Transformer.
//
// This is a small reference/training backend, not a claim that finite-
// difference optimisation should replace LibTorch for large searches.  It
// closes the architectural gap: multiple heads, sinusoidal positions, two
// layer-normalised residual sublayers, a configurable stack, a trainable
// feed-forward network and a full-parameter checkpoint.  The numerical
// gradient is deliberately slow but exact and deterministic; live inference
// performs no allocation and never uses it.

#pragma once

#include <models/tensor.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace altair {

template <std::size_t D, std::size_t Heads, std::size_t FF,
          std::size_t Layers = 2, std::size_t MaxT = kMaxSeq>
class CausalTransformer {
public:
    static_assert(D > 0 && Heads > 0 && FF > 0 && Layers > 0);
    static_assert(D % Heads == 0, "model width must divide evenly into heads");
    static_assert(D <= kMaxHidden && FF <= kMaxHidden && MaxT <= kMaxSeq);

    static constexpr std::size_t kD2 = D * D;
    static constexpr std::size_t kWq = 0;
    static constexpr std::size_t kWk = kWq + kD2;
    static constexpr std::size_t kWv = kWk + kD2;
    static constexpr std::size_t kWo = kWv + kD2;
    static constexpr std::size_t kW1 = kWo + kD2;
    static constexpr std::size_t kB1 = kW1 + FF * D;
    static constexpr std::size_t kW2 = kB1 + FF;
    static constexpr std::size_t kB2 = kW2 + D * FF;
    static constexpr std::size_t kLn1Gamma = kB2 + D;
    static constexpr std::size_t kLn1Beta = kLn1Gamma + D;
    static constexpr std::size_t kLn2Gamma = kLn1Beta + D;
    static constexpr std::size_t kLn2Beta = kLn2Gamma + D;
    static constexpr std::size_t kBlockParams = kLn2Beta + D;
    static constexpr std::size_t kReadout = Layers * kBlockParams;
    static constexpr std::size_t kParams = kReadout + D + 1;

    void reset(std::uint64_t seed) noexcept {
        Init init{seed};
        for (std::size_t layer = 0; layer < Layers; ++layer) {
            double* b = p_ + layer * kBlockParams;
            Matrix q{b + kWq, D, D}, k{b + kWk, D, D};
            Matrix v{b + kWv, D, D}, o{b + kWo, D, D};
            Matrix w1{b + kW1, FF, D}, w2{b + kW2, D, FF};
            init.xavier(q, D, D); init.xavier(k, D, D);
            init.xavier(v, D, D); init.xavier(o, D, D);
            init.xavier(w1, D, FF); init.xavier(w2, FF, D);
            init.zeros(b + kB1, FF); init.zeros(b + kB2, D);
            init.ones(b + kLn1Gamma, D); init.zeros(b + kLn1Beta, D);
            init.ones(b + kLn2Gamma, D); init.zeros(b + kLn2Beta, D);
        }
        Matrix r{p_ + kReadout, 1, D}; init.xavier(r, D, 1);
        p_[kReadout + D] = 0.0;
    }

    /// Encode [steps x D] into caller storage of the same shape.
    void encode(const double* sequence, std::size_t steps, double* output) const noexcept {
        const std::size_t T = std::min(steps, MaxT);
        if (T == 0) return;
        double a[MaxT * D] = {}, z[MaxT * D] = {};
        for (std::size_t t = 0; t < T; ++t) {
            for (std::size_t d = 0; d < D; ++d) {
                const double denom = std::pow(10000.0,
                    static_cast<double>(2 * (d / 2)) / static_cast<double>(D));
                const double angle = static_cast<double>(t) / denom;
                const double pos = (d % 2 == 0) ? std::sin(angle) : std::cos(angle);
                a[t * D + d] = sequence[t * D + d] + pos;
            }
        }
        for (std::size_t layer = 0; layer < Layers; ++layer) {
            block(a, T, p_ + layer * kBlockParams, z);
            for (std::size_t i = 0; i < T * D; ++i) a[i] = z[i];
        }
        for (std::size_t i = 0; i < T * D; ++i) output[i] = a[i];
    }

    [[nodiscard]] double predict_at(const double* sequence, std::size_t steps,
                                    std::size_t position) const noexcept {
        const std::size_t T = std::min(steps, MaxT);
        if (T == 0 || position >= T) return 0.0;
        double encoded[MaxT * D] = {};
        encode(sequence, T, encoded);
        double y = p_[kReadout + D];
        for (std::size_t d = 0; d < D; ++d)
            y += p_[kReadout + d] * encoded[position * D + d];
        return y;
    }

    [[nodiscard]] double predict(const double* sequence, std::size_t steps) const noexcept {
        const std::size_t T = std::min(steps, MaxT);
        return T == 0 ? 0.0 : predict_at(sequence, T, T - 1);
    }

    [[nodiscard]] double loss(const double* sequence, std::size_t steps,
                              double target) const noexcept {
        const double e = predict(sequence, steps) - target;
        return e * e;
    }

    /// Central finite-difference gradient of the complete parameter vector.
    /// This is the deterministic CPU reference used to validate architecture
    /// and checkpoints. Practical jobs should replace only this training
    /// backend with autograd; encode()/predict() remain the serving contract.
    [[nodiscard]] double compute_gradients(const double* sequence, std::size_t steps,
                                           double target, double* gradient) noexcept {
        const double value = loss(sequence, steps, target);
        for (std::size_t i = 0; i < kParams; ++i) {
            const double saved = p_[i];
            const double eps = 1e-5 * (1.0 + std::fabs(saved));
            p_[i] = saved + eps;
            const double plus = loss(sequence, steps, target);
            p_[i] = saved - eps;
            const double minus = loss(sequence, steps, target);
            p_[i] = saved;
            gradient[i] = (plus - minus) / (2.0 * eps);
        }
        return value;
    }

    double train_step(const double* sequence, std::size_t steps, double target,
                      double learning_rate, double gradient_clip = 2.0) noexcept {
        double g[kParams];
        const double value = compute_gradients(sequence, steps, target, g);
        for (std::size_t i = 0; i < kParams; ++i) {
            const double gi = std::max(-gradient_clip, std::min(gradient_clip, g[i]));
            p_[i] -= learning_rate * gi;
        }
        return value;
    }

    [[nodiscard]] static constexpr std::size_t param_count() noexcept { return kParams; }
    [[nodiscard]] const double* params() const noexcept { return p_; }
    [[nodiscard]] double* mutable_params() noexcept { return p_; }
    void load_params(const double* source) noexcept {
        for (std::size_t i = 0; i < kParams; ++i) p_[i] = source[i];
    }

private:
    static void normalise(const double* input, const double* gamma,
                          const double* beta, double* output) noexcept {
        double mean = 0.0;
        for (std::size_t d = 0; d < D; ++d) mean += input[d];
        mean /= static_cast<double>(D);
        double variance = 0.0;
        for (std::size_t d = 0; d < D; ++d) {
            const double q = input[d] - mean; variance += q * q;
        }
        variance /= static_cast<double>(D);
        const double inv = 1.0 / std::sqrt(variance + 1e-6);
        for (std::size_t d = 0; d < D; ++d)
            output[d] = gamma[d] * (input[d] - mean) * inv + beta[d];
    }

    static void block(const double* input, std::size_t T, const double* b,
                      double* output) noexcept {
        constexpr std::size_t Hd = D / Heads;
        double n1[MaxT * D], q[MaxT * D], k[MaxT * D], v[MaxT * D];
        double attended[MaxT * D] = {}, residual[MaxT * D], n2[MaxT * D];
        for (std::size_t t = 0; t < T; ++t) {
            normalise(input + t * D, b + kLn1Gamma, b + kLn1Beta, n1 + t * D);
            for (std::size_t o = 0; o < D; ++o) {
                q[t * D + o] = k[t * D + o] = v[t * D + o] = 0.0;
                for (std::size_t d = 0; d < D; ++d) {
                    const double x = n1[t * D + d];
                    q[t * D + o] += b[kWq + o * D + d] * x;
                    k[t * D + o] += b[kWk + o * D + d] * x;
                    v[t * D + o] += b[kWv + o * D + d] * x;
                }
            }
        }
        for (std::size_t t = 0; t < T; ++t) {
            for (std::size_t head = 0; head < Heads; ++head) {
                double scores[MaxT] = {};
                for (std::size_t s = 0; s <= t; ++s) {
                    for (std::size_t d = 0; d < Hd; ++d)
                        scores[s] += q[t * D + head * Hd + d]
                                   * k[s * D + head * Hd + d];
                    scores[s] /= std::sqrt(static_cast<double>(Hd));
                }
                softmax_row(scores, t + 1); // causal: future positions do not enter normalisation
                for (std::size_t d = 0; d < Hd; ++d) {
                    double sum = 0.0;
                    for (std::size_t s = 0; s <= t; ++s)
                        sum += scores[s] * v[s * D + head * Hd + d];
                    attended[t * D + head * Hd + d] = sum;
                }
            }
            for (std::size_t o = 0; o < D; ++o) {
                double projection = 0.0;
                for (std::size_t d = 0; d < D; ++d)
                    projection += b[kWo + o * D + d] * attended[t * D + d];
                residual[t * D + o] = input[t * D + o] + projection;
            }
            normalise(residual + t * D, b + kLn2Gamma, b + kLn2Beta, n2 + t * D);
            double hidden[FF];
            for (std::size_t f = 0; f < FF; ++f) {
                double sum = b[kB1 + f];
                for (std::size_t d = 0; d < D; ++d)
                    sum += b[kW1 + f * D + d] * n2[t * D + d];
                hidden[f] = relu(sum);
            }
            for (std::size_t d = 0; d < D; ++d) {
                double sum = b[kB2 + d];
                for (std::size_t f = 0; f < FF; ++f)
                    sum += b[kW2 + d * FF + f] * hidden[f];
                output[t * D + d] = residual[t * D + d] + sum;
            }
        }
    }

    double p_[kParams] = {};
};

} // namespace altair
