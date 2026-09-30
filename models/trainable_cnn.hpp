// models/trainable_cnn.hpp -- a trained temporal CNN (dilated causal stack).
//
// The Atlas's CNN row is models/attention.hpp's dilated causal convolution.
// This is that convolution with its kernels LEARNED: `Layers` convolutions of
// width K at dilations 1, 2, 4 ..., each followed by a ReLU, read out at the
// last step by a logistic unit. The forward pass is attention.hpp's
// dilated_conv with Padding::CausalLeft, so output t never reads an input after
// t; training is analytic backpropagation (binary cross-entropy) with Adam,
// checked against a finite difference of the same loss in
// models/tests/test_trainable_cnn.cpp.
//
// Receptive field of the last step: 1 + (K - 1) * (2^Layers - 1) steps.

#pragma once

#include <models/attention.hpp>
#include <models/tensor.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace altair {

template <std::size_t Cin, std::size_t C, std::size_t Layers, std::size_t K = 3,
          std::size_t MaxT = kMaxSeq>
class TrainableTcn {
public:
    static_assert(Cin > 0 && C > 0 && Layers > 0 && K > 0 && MaxT > 0);

    [[nodiscard]] static constexpr std::size_t inputs(std::size_t layer) noexcept {
        return layer == 0 ? Cin : C;
    }
    [[nodiscard]] static constexpr std::size_t offset(std::size_t layer) noexcept {
        std::size_t o = 0;
        for (std::size_t l = 0; l < layer; ++l) o += C * inputs(l) * K + C;
        return o;
    }
    static constexpr std::size_t kReadout = offset(Layers);
    static constexpr std::size_t kParams = kReadout + C + 1;

    void reset(std::uint64_t seed) noexcept {
        Init init{seed};
        for (std::size_t l = 0; l < Layers; ++l) {
            Matrix w{p_.data() + offset(l), C, inputs(l) * K};
            init.xavier(w, inputs(l) * K, 0);   // fan-in only: He-style for ReLU
            init.zeros(p_.data() + offset(l) + C * inputs(l) * K, C);
        }
        Matrix r{p_.data() + kReadout, 1, C};
        init.xavier(r, C, 1);
        p_[kReadout + C] = 0.0;
        m_.fill(0.0);
        v_.fill(0.0);
        t_ = 0;
    }

    /// P(label = 1) from a [steps x Cin] sequence, oldest first; the last
    /// MaxT steps are used.
    [[nodiscard]] double probability(const double* sequence, std::size_t steps) const {
        Acts a;
        const std::size_t T = forward(sequence, steps, a);
        return T == 0 ? 0.5 : sigmoid(logit(a, T));
    }

    [[nodiscard]] double loss(const double* sequence, std::size_t steps, int label) const {
        return cross_entropy(probability(sequence, steps), label);
    }

    /// Exact gradient of loss() by backpropagation; returns the loss.
    [[nodiscard]] double gradients(const double* sequence, std::size_t steps, int label,
                                   double* grad) const {
        for (std::size_t i = 0; i < kParams; ++i) grad[i] = 0.0;
        Acts a;
        const std::size_t T = forward(sequence, steps, a);
        if (T == 0) return cross_entropy(0.5, label);
        const double p = sigmoid(logit(a, T));
        const double dz = p - (label != 0 ? 1.0 : 0.0);
        std::vector<double> g(MaxT * C, 0.0);   // d loss / d post-ReLU output of the top layer
        const double* top = a.post[Layers - 1].data();
        for (std::size_t c = 0; c < C; ++c) {
            grad[kReadout + c] = dz * top[(T - 1) * C + c];
            g[(T - 1) * C + c] = dz * p_[kReadout + c];
        }
        grad[kReadout + C] = dz;
        for (std::size_t l = Layers; l-- > 0;) {
            const std::size_t cin = inputs(l), dil = std::size_t{1} << l;
            const double* in = l == 0 ? a.input.data() : a.post[l - 1].data();
            const double* w = p_.data() + offset(l);
            double* gw = grad + offset(l);
            double* gbias = gw + C * cin * K;
            std::vector<double> gin(MaxT * cin, 0.0);
            for (std::size_t t = 0; t < T; ++t) {
                for (std::size_t f = 0; f < C; ++f) {
                    if (!(a.pre[l][t * C + f] > 0.0)) continue;   // ReLU gate
                    const double gp = g[t * C + f];
                    if (gp == 0.0) continue;
                    gbias[f] += gp;
                    for (std::size_t m = 0; m < K; ++m) {
                        const std::size_t back = (K - 1 - m) * dil;
                        if (back > t) continue;
                        const std::size_t src = t - back;
                        for (std::size_t ci = 0; ci < cin; ++ci) {
                            gw[f * cin * K + ci * K + m] += gp * in[src * cin + ci];
                            gin[src * cin + ci] += w[f * cin * K + ci * K + m] * gp;
                        }
                    }
                }
            }
            g.assign(gin.begin(), gin.end());
        }
        return cross_entropy(p, label);
    }

    /// One Adam step on one sequence; returns the loss before the step.
    double train_step(const double* sequence, std::size_t steps, int label,
                      double learning_rate) {
        std::vector<double> g(kParams);
        const double value = gradients(sequence, steps, label, g.data());
        ++t_;
        const double b1 = 0.9, b2 = 0.999;
        const double c1 = 1.0 - std::pow(b1, static_cast<double>(t_));
        const double c2 = 1.0 - std::pow(b2, static_cast<double>(t_));
        for (std::size_t i = 0; i < kParams; ++i) {
            m_[i] = b1 * m_[i] + (1.0 - b1) * g[i];
            v_[i] = b2 * v_[i] + (1.0 - b2) * g[i] * g[i];
            p_[i] -= learning_rate * (m_[i] / c1) / (std::sqrt(v_[i] / c2) + 1e-8);
        }
        return value;
    }

    [[nodiscard]] static constexpr std::size_t param_count() noexcept { return kParams; }
    [[nodiscard]] const double* params() const noexcept { return p_.data(); }
    [[nodiscard]] double* mutable_params() noexcept { return p_.data(); }

private:
    struct Acts {
        std::vector<double> input = std::vector<double>(MaxT * Cin, 0.0);
        std::vector<std::vector<double>> pre = std::vector<std::vector<double>>(Layers, std::vector<double>(MaxT * C, 0.0));
        std::vector<std::vector<double>> post = std::vector<std::vector<double>>(Layers, std::vector<double>(MaxT * C, 0.0));
    };

    static double sigmoid(double z) noexcept { return 1.0 / (1.0 + std::exp(-z)); }
    static double cross_entropy(double p, int label) noexcept {
        const double q = std::clamp(p, 1e-12, 1.0 - 1e-12);
        return label != 0 ? -std::log(q) : -std::log(1.0 - q);
    }

    double logit(const Acts& a, std::size_t T) const noexcept {
        double z = p_[kReadout + C];
        for (std::size_t c = 0; c < C; ++c) z += p_[kReadout + c] * a.post[Layers - 1][(T - 1) * C + c];
        return z;
    }

    /// Runs attention.hpp's causal dilated convolution layer by layer.
    std::size_t forward(const double* sequence, std::size_t steps, Acts& a) const {
        const std::size_t T = std::min(steps, MaxT);
        if (T == 0) return 0;
        const double* from = sequence + (steps - T) * Cin;
        std::copy(from, from + T * Cin, a.input.begin());
        for (std::size_t l = 0; l < Layers; ++l) {
            const std::size_t cin = inputs(l);
            double* in_data = l == 0 ? a.input.data() : a.post[l - 1].data();
            Matrix in{in_data, T, cin};
            Matrix w{const_cast<double*>(p_.data() + offset(l)), C, cin * K};
            Matrix out{a.pre[l].data(), T, C};
            if (!dilated_conv(in, w, p_.data() + offset(l) + C * cin * K, K, std::size_t{1} << l, out,
                              Padding::CausalLeft)) {
                return 0;
            }
            for (std::size_t i = 0; i < T * C; ++i) a.post[l][i] = relu(a.pre[l][i]);
        }
        return T;
    }

    std::array<double, kParams> p_{};
    std::array<double, kParams> m_{}, v_{};
    std::uint64_t t_ = 0;
};

} // namespace altair
