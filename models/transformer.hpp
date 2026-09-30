// models/transformer.hpp -- fixed-capacity causal, trainable Transformer.
//
// This is a small reference/training backend, not a claim that finite-
// difference optimisation should replace LibTorch for large searches.  It
// closes the architectural gap: multiple heads, sinusoidal positions, two
// layer-normalised residual sublayers, a configurable stack, a trainable
// feed-forward network and a full-parameter checkpoint.  The numerical
// gradient is deliberately slow but exact and deterministic; it is kept as the
// reference that analytic_gradients() (backpropagation, what train_step uses)
// is checked against. Live inference performs no allocation.

#pragma once

#include <models/tensor.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

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
        positional(sequence, T, a);
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

    /// Exact gradient by backpropagation: the same loss as compute_gradients
    /// at the cost of about three forward passes instead of 2 * kParams.
    /// compute_gradients stays as the reference it is checked against.
    [[nodiscard]] double analytic_gradients(const double* sequence, std::size_t steps,
                                            double target, double* gradient) const {
        for (std::size_t i = 0; i < kParams; ++i) gradient[i] = 0.0;
        const std::size_t T = std::min(steps, MaxT);
        if (T == 0) return target * target;
        std::vector<Cache> cache(Layers);
        double a[MaxT * D] = {};
        positional(sequence, T, a);
        for (std::size_t layer = 0; layer < Layers; ++layer) {
            forward_cached(a, T, p_ + layer * kBlockParams, cache[layer]);
            for (std::size_t i = 0; i < T * D; ++i) a[i] = cache[layer].out[i];
        }
        double y = p_[kReadout + D];
        for (std::size_t d = 0; d < D; ++d) y += p_[kReadout + d] * a[(T - 1) * D + d];
        const double e = y - target;
        const double dy = 2.0 * e;
        double g[MaxT * D] = {};
        for (std::size_t d = 0; d < D; ++d) {
            gradient[kReadout + d] = dy * a[(T - 1) * D + d];
            g[(T - 1) * D + d] = dy * p_[kReadout + d];
        }
        gradient[kReadout + D] = dy;
        for (std::size_t layer = Layers; layer-- > 0;) {
            backward_block(cache[layer], T, p_ + layer * kBlockParams,
                           gradient + layer * kBlockParams, g);
        }
        return e * e;
    }

    double train_step(const double* sequence, std::size_t steps, double target,
                      double learning_rate, double gradient_clip = 2.0) {
        double g[kParams];
        const double value = analytic_gradients(sequence, steps, target, g);
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
    /// Input plus the sinusoidal position code, as encode() builds it.
    static void positional(const double* sequence, std::size_t T, double* a) noexcept {
        for (std::size_t t = 0; t < T; ++t) {
            for (std::size_t d = 0; d < D; ++d) {
                const double denom = std::pow(10000.0,
                    static_cast<double>(2 * (d / 2)) / static_cast<double>(D));
                const double angle = static_cast<double>(t) / denom;
                const double pos = (d % 2 == 0) ? std::sin(angle) : std::cos(angle);
                a[t * D + d] = sequence[t * D + d] + pos;
            }
        }
    }

    /// Everything one block's backward pass needs from its forward pass.
    struct Cache {
        double in[MaxT * D], xhat1[MaxT * D], inv1[MaxT], n1[MaxT * D];
        double q[MaxT * D], k[MaxT * D], v[MaxT * D];
        double prob[Heads * MaxT * MaxT];   // [head][t][s], s <= t
        double attended[MaxT * D], residual[MaxT * D];
        double xhat2[MaxT * D], inv2[MaxT], n2[MaxT * D];
        double pre[MaxT * FF], hidden[MaxT * FF], out[MaxT * D];
    };

    static void normalise_cached(const double* input, const double* gamma, const double* beta,
                                 double* xhat, double& inv, double* output) noexcept {
        double mean = 0.0;
        for (std::size_t d = 0; d < D; ++d) mean += input[d];
        mean /= static_cast<double>(D);
        double variance = 0.0;
        for (std::size_t d = 0; d < D; ++d) {
            const double q = input[d] - mean; variance += q * q;
        }
        variance /= static_cast<double>(D);
        inv = 1.0 / std::sqrt(variance + 1e-6);
        for (std::size_t d = 0; d < D; ++d) {
            xhat[d] = (input[d] - mean) * inv;
            output[d] = gamma[d] * xhat[d] + beta[d];
        }
    }

    /// Layer-norm backward: accumulates into gamma/beta gradients and ADDS
    /// the input gradient to `gin`.
    static void normalise_backward(const double* gout, const double* xhat, double inv,
                                   const double* gamma, double* ggamma, double* gbeta,
                                   double* gin) noexcept {
        double gx[D], mean_g = 0.0, mean_gx = 0.0;
        for (std::size_t d = 0; d < D; ++d) {
            ggamma[d] += gout[d] * xhat[d];
            gbeta[d] += gout[d];
            gx[d] = gout[d] * gamma[d];
            mean_g += gx[d];
            mean_gx += gx[d] * xhat[d];
        }
        mean_g /= static_cast<double>(D);
        mean_gx /= static_cast<double>(D);
        for (std::size_t d = 0; d < D; ++d) gin[d] += inv * (gx[d] - mean_g - xhat[d] * mean_gx);
    }

    /// block() with its intermediates kept. Same arithmetic, same order.
    static void forward_cached(const double* input, std::size_t T, const double* b, Cache& c) noexcept {
        constexpr std::size_t Hd = D / Heads;
        const double scale = std::sqrt(static_cast<double>(Hd));
        for (std::size_t t = 0; t < T; ++t) {
            for (std::size_t d = 0; d < D; ++d) c.in[t * D + d] = input[t * D + d];
            normalise_cached(input + t * D, b + kLn1Gamma, b + kLn1Beta, c.xhat1 + t * D, c.inv1[t],
                             c.n1 + t * D);
            for (std::size_t o = 0; o < D; ++o) {
                c.q[t * D + o] = c.k[t * D + o] = c.v[t * D + o] = 0.0;
                for (std::size_t d = 0; d < D; ++d) {
                    const double x = c.n1[t * D + d];
                    c.q[t * D + o] += b[kWq + o * D + d] * x;
                    c.k[t * D + o] += b[kWk + o * D + d] * x;
                    c.v[t * D + o] += b[kWv + o * D + d] * x;
                }
            }
        }
        for (std::size_t t = 0; t < T; ++t) {
            for (std::size_t head = 0; head < Heads; ++head) {
                double* scores = c.prob + (head * MaxT + t) * MaxT;
                for (std::size_t s = 0; s <= t; ++s) {
                    scores[s] = 0.0;
                    for (std::size_t d = 0; d < Hd; ++d)
                        scores[s] += c.q[t * D + head * Hd + d] * c.k[s * D + head * Hd + d];
                    scores[s] /= scale;
                }
                softmax_row(scores, t + 1);
                for (std::size_t d = 0; d < Hd; ++d) {
                    double sum = 0.0;
                    for (std::size_t s = 0; s <= t; ++s) sum += scores[s] * c.v[s * D + head * Hd + d];
                    c.attended[t * D + head * Hd + d] = sum;
                }
            }
            for (std::size_t o = 0; o < D; ++o) {
                double projection = 0.0;
                for (std::size_t d = 0; d < D; ++d) projection += b[kWo + o * D + d] * c.attended[t * D + d];
                c.residual[t * D + o] = input[t * D + o] + projection;
            }
            normalise_cached(c.residual + t * D, b + kLn2Gamma, b + kLn2Beta, c.xhat2 + t * D, c.inv2[t],
                             c.n2 + t * D);
            for (std::size_t f = 0; f < FF; ++f) {
                double sum = b[kB1 + f];
                for (std::size_t d = 0; d < D; ++d) sum += b[kW1 + f * D + d] * c.n2[t * D + d];
                c.pre[t * FF + f] = sum;
                c.hidden[t * FF + f] = relu(sum);
            }
            for (std::size_t d = 0; d < D; ++d) {
                double sum = b[kB2 + d];
                for (std::size_t f = 0; f < FF; ++f) sum += b[kW2 + d * FF + f] * c.hidden[t * FF + f];
                c.out[t * D + d] = c.residual[t * D + d] + sum;
            }
        }
    }

    /// `g` holds d(loss)/d(block output) on entry and d(loss)/d(block input)
    /// on exit; parameter gradients are ADDED to `gb`.
    static void backward_block(const Cache& c, std::size_t T, const double* b, double* gb,
                               double* g) noexcept {
        constexpr std::size_t Hd = D / Heads;
        const double scale = std::sqrt(static_cast<double>(Hd));
        double g_res[MaxT * D] = {}, g_att[MaxT * D] = {};
        double g_q[MaxT * D] = {}, g_k[MaxT * D] = {}, g_v[MaxT * D] = {};
        double g_in[MaxT * D] = {};
        for (std::size_t t = 0; t < T; ++t) {
            const double* go = g + t * D;
            double g_hidden[FF] = {}, g_n2[D] = {};
            for (std::size_t d = 0; d < D; ++d) {
                g_res[t * D + d] = go[d];
                gb[kB2 + d] += go[d];
                for (std::size_t f = 0; f < FF; ++f) {
                    gb[kW2 + d * FF + f] += go[d] * c.hidden[t * FF + f];
                    g_hidden[f] += b[kW2 + d * FF + f] * go[d];
                }
            }
            for (std::size_t f = 0; f < FF; ++f) {
                const double gp = c.pre[t * FF + f] > 0.0 ? g_hidden[f] : 0.0;
                gb[kB1 + f] += gp;
                for (std::size_t d = 0; d < D; ++d) {
                    gb[kW1 + f * D + d] += gp * c.n2[t * D + d];
                    g_n2[d] += b[kW1 + f * D + d] * gp;
                }
            }
            normalise_backward(g_n2, c.xhat2 + t * D, c.inv2[t], b + kLn2Gamma, gb + kLn2Gamma,
                               gb + kLn2Beta, g_res + t * D);
            for (std::size_t o = 0; o < D; ++o) {
                const double gr = g_res[t * D + o];
                g_in[t * D + o] += gr;   // the residual path
                for (std::size_t d = 0; d < D; ++d) {
                    gb[kWo + o * D + d] += gr * c.attended[t * D + d];
                    g_att[t * D + d] += b[kWo + o * D + d] * gr;
                }
            }
        }
        for (std::size_t t = 0; t < T; ++t) {
            for (std::size_t head = 0; head < Heads; ++head) {
                const double* prob = c.prob + (head * MaxT + t) * MaxT;
                double g_prob[MaxT] = {}, dot = 0.0;
                for (std::size_t s = 0; s <= t; ++s) {
                    for (std::size_t d = 0; d < Hd; ++d) {
                        const double ga = g_att[t * D + head * Hd + d];
                        g_prob[s] += ga * c.v[s * D + head * Hd + d];
                        g_v[s * D + head * Hd + d] += prob[s] * ga;
                    }
                    dot += prob[s] * g_prob[s];
                }
                for (std::size_t s = 0; s <= t; ++s) {
                    const double gs = prob[s] * (g_prob[s] - dot) / scale;
                    for (std::size_t d = 0; d < Hd; ++d) {
                        g_q[t * D + head * Hd + d] += gs * c.k[s * D + head * Hd + d];
                        g_k[s * D + head * Hd + d] += gs * c.q[t * D + head * Hd + d];
                    }
                }
            }
        }
        for (std::size_t t = 0; t < T; ++t) {
            double g_n1[D] = {};
            for (std::size_t o = 0; o < D; ++o) {
                for (std::size_t d = 0; d < D; ++d) {
                    const double x = c.n1[t * D + d];
                    gb[kWq + o * D + d] += g_q[t * D + o] * x;
                    gb[kWk + o * D + d] += g_k[t * D + o] * x;
                    gb[kWv + o * D + d] += g_v[t * D + o] * x;
                    g_n1[d] += b[kWq + o * D + d] * g_q[t * D + o] + b[kWk + o * D + d] * g_k[t * D + o]
                             + b[kWv + o * D + d] * g_v[t * D + o];
                }
            }
            normalise_backward(g_n1, c.xhat1 + t * D, c.inv1[t], b + kLn1Gamma, gb + kLn1Gamma,
                               gb + kLn1Beta, g_in + t * D);
        }
        for (std::size_t i = 0; i < T * D; ++i) g[i] = g_in[i];
    }

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
