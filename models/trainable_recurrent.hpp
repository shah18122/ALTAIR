// models/trainable_recurrent.hpp -- fully trainable fixed-capacity LSTM/GRU.
//
// These models are the offline-training counterparts to recurrent.hpp's
// allocation-free reservoir/readout path.  Every gate matrix, gate bias and
// readout weight participates in the checkpoint and is trained by analytic
// backpropagation through time (BPTT).  Serving remains allocation-free.

#pragma once

#include <models/tensor.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace altair {

namespace recurrent_detail {

inline double clipped(double v, double limit) noexcept {
    return std::max(-limit, std::min(limit, v));
}

template <std::size_t N>
inline void zero(double (&v)[N]) noexcept {
    for (double& x : v) x = 0.0;
}

} // namespace recurrent_detail

/// Scalar sequence regressor with a trainable LSTM core.
/// Parameter layout: Wi,Wf,Wg,Wo, bi,bf,bg,bo, readout+bias.
template <std::size_t H, std::size_t X, std::size_t MaxT = kMaxSeq>
class TrainableLstm {
public:
    static_assert(H > 0 && X > 0 && H <= kMaxHidden && X <= kMaxInput);
    static constexpr std::size_t kCat = X + H;
    static constexpr std::size_t kGateMatrix = H * kCat;
    static constexpr std::size_t kGateBlock = kGateMatrix + H;
    static constexpr std::size_t kReadout = 4 * kGateBlock;
    static constexpr std::size_t kParams = kReadout + H + 1;

    void reset(std::uint64_t seed, double forget_bias = 1.0) noexcept {
        Init init{seed};
        for (std::size_t gate = 0; gate < 4; ++gate) {
            Matrix w{p_ + gate * kGateBlock, H, kCat};
            init.xavier(w, kCat, H);
            init.zeros(p_ + gate * kGateBlock + kGateMatrix, H);
        }
        for (std::size_t i = 0; i < H; ++i) bf()[i] = forget_bias;
        Matrix r{readout(), 1, H};
        init.xavier(r, H, 1);
        readout()[H] = 0.0;
    }

    [[nodiscard]] double predict(const double* sequence, std::size_t steps) const noexcept {
        Cache cache{};
        const std::size_t t = forward(sequence, steps, cache);
        double y = readout()[H];
        for (std::size_t j = 0; j < H; ++j) y += readout()[j] * cache.h[t][j];
        return y;
    }

    /// MSE and analytic gradient for one sequence. Sequence is [steps x X].
    [[nodiscard]] double compute_gradients(const double* sequence, std::size_t steps,
                                           double target, double* grad) const noexcept {
        for (std::size_t i = 0; i < kParams; ++i) grad[i] = 0.0;
        Cache c{};
        const std::size_t T = forward(sequence, steps, c);
        if (T == 0) return 0.0;

        double y = readout()[H];
        for (std::size_t j = 0; j < H; ++j) y += readout()[j] * c.h[T][j];
        const double e = y - target;
        const double dy = 2.0 * e;
        for (std::size_t j = 0; j < H; ++j) grad[kReadout + j] = dy * c.h[T][j];
        grad[kReadout + H] = dy;

        double dh_next[H] = {}, dc_next[H] = {};
        for (std::size_t j = 0; j < H; ++j) dh_next[j] = dy * readout()[j];

        for (std::size_t tr = T; tr-- > 0;) {
            double dai[H], daf[H], dag[H], dao[H];
            for (std::size_t a = 0; a < H; ++a) {
                const double tc = std::tanh(c.c[tr + 1][a]);
                const double dc = dc_next[a]
                    + dh_next[a] * c.o[tr][a] * (1.0 - tc * tc);
                dao[a] = dh_next[a] * tc * c.o[tr][a] * (1.0 - c.o[tr][a]);
                dai[a] = dc * c.g[tr][a] * c.i[tr][a] * (1.0 - c.i[tr][a]);
                daf[a] = dc * c.c[tr][a] * c.f[tr][a] * (1.0 - c.f[tr][a]);
                dag[a] = dc * c.i[tr][a] * (1.0 - c.g[tr][a] * c.g[tr][a]);
                dc_next[a] = dc * c.f[tr][a];
            }

            double cat[kCat];
            for (std::size_t j = 0; j < X; ++j) cat[j] = sequence[tr * X + j];
            for (std::size_t j = 0; j < H; ++j) cat[X + j] = c.h[tr][j];
            const double* da[4] = {dai, daf, dag, dao};
            double dh_prev[H] = {};
            for (std::size_t gate = 0; gate < 4; ++gate) {
                const double* wg = p_ + gate * kGateBlock;
                double* gw = grad + gate * kGateBlock;
                double* gb = gw + kGateMatrix;
                for (std::size_t a = 0; a < H; ++a) {
                    gb[a] += da[gate][a];
                    for (std::size_t j = 0; j < kCat; ++j) {
                        gw[a * kCat + j] += da[gate][a] * cat[j];
                        if (j >= X) dh_prev[j - X] += wg[a * kCat + j] * da[gate][a];
                    }
                }
            }
            for (std::size_t j = 0; j < H; ++j) dh_next[j] = dh_prev[j];
        }
        return e * e;
    }

    double train_step(const double* sequence, std::size_t steps, double target,
                      double learning_rate, double gradient_clip = 5.0) noexcept {
        double g[kParams];
        const double loss = compute_gradients(sequence, steps, target, g);
        for (std::size_t i = 0; i < kParams; ++i)
            p_[i] -= learning_rate * recurrent_detail::clipped(g[i], gradient_clip);
        return loss;
    }

    [[nodiscard]] static constexpr std::size_t param_count() noexcept { return kParams; }
    [[nodiscard]] const double* params() const noexcept { return p_; }
    [[nodiscard]] double* mutable_params() noexcept { return p_; }
    void load_params(const double* src) noexcept {
        for (std::size_t i = 0; i < kParams; ++i) p_[i] = src[i];
    }

private:
    struct Cache {
        double h[MaxT + 1][H] = {};
        double c[MaxT + 1][H] = {};
        double i[MaxT][H] = {}, f[MaxT][H] = {};
        double g[MaxT][H] = {}, o[MaxT][H] = {};
    };

    std::size_t forward(const double* sequence, std::size_t steps, Cache& c) const noexcept {
        const std::size_t T = std::min(steps, MaxT);
        for (std::size_t t = 0; t < T; ++t) {
            for (std::size_t a = 0; a < H; ++a) {
                double ai = bi()[a], af = bf()[a], ag = bg()[a], ao = bo()[a];
                for (std::size_t j = 0; j < X; ++j) {
                    const double x = sequence[t * X + j];
                    ai += wi()[a * kCat + j] * x; af += wf()[a * kCat + j] * x;
                    ag += wg()[a * kCat + j] * x; ao += wo()[a * kCat + j] * x;
                }
                for (std::size_t j = 0; j < H; ++j) {
                    const double h = c.h[t][j];
                    ai += wi()[a * kCat + X + j] * h; af += wf()[a * kCat + X + j] * h;
                    ag += wg()[a * kCat + X + j] * h; ao += wo()[a * kCat + X + j] * h;
                }
                c.i[t][a] = sigmoid(ai); c.f[t][a] = sigmoid(af);
                c.g[t][a] = std::tanh(ag); c.o[t][a] = sigmoid(ao);
                c.c[t + 1][a] = c.f[t][a] * c.c[t][a] + c.i[t][a] * c.g[t][a];
                c.h[t + 1][a] = c.o[t][a] * std::tanh(c.c[t + 1][a]);
            }
        }
        return T;
    }

    const double* wi() const noexcept { return p_; }
    const double* wf() const noexcept { return p_ + kGateBlock; }
    const double* wg() const noexcept { return p_ + 2 * kGateBlock; }
    const double* wo() const noexcept { return p_ + 3 * kGateBlock; }
    const double* bi() const noexcept { return p_ + kGateMatrix; }
    const double* bf() const noexcept { return p_ + kGateBlock + kGateMatrix; }
    const double* bg() const noexcept { return p_ + 2 * kGateBlock + kGateMatrix; }
    const double* bo() const noexcept { return p_ + 3 * kGateBlock + kGateMatrix; }
    double* bf() noexcept { return p_ + kGateBlock + kGateMatrix; }
    const double* readout() const noexcept { return p_ + kReadout; }
    double* readout() noexcept { return p_ + kReadout; }

    double p_[kParams] = {};
};

/// Scalar sequence regressor with a trainable GRU core.
/// Parameter layout: Wz,Wr,Wn, bz,br,bn, readout+bias.
template <std::size_t H, std::size_t X, std::size_t MaxT = kMaxSeq>
class TrainableGru {
public:
    static_assert(H > 0 && X > 0 && H <= kMaxHidden && X <= kMaxInput);
    static constexpr std::size_t kCat = X + H;
    static constexpr std::size_t kGateMatrix = H * kCat;
    static constexpr std::size_t kGateBlock = kGateMatrix + H;
    static constexpr std::size_t kReadout = 3 * kGateBlock;
    static constexpr std::size_t kParams = kReadout + H + 1;

    void reset(std::uint64_t seed, double update_bias = -1.0) noexcept {
        Init init{seed};
        for (std::size_t gate = 0; gate < 3; ++gate) {
            Matrix w{p_ + gate * kGateBlock, H, kCat};
            init.xavier(w, kCat, H);
            init.zeros(p_ + gate * kGateBlock + kGateMatrix, H);
        }
        for (std::size_t i = 0; i < H; ++i) bz()[i] = update_bias;
        Matrix r{readout(), 1, H}; init.xavier(r, H, 1); readout()[H] = 0.0;
    }

    [[nodiscard]] double predict(const double* sequence, std::size_t steps) const noexcept {
        Cache c{}; const std::size_t T = forward(sequence, steps, c);
        double y = readout()[H];
        for (std::size_t j = 0; j < H; ++j) y += readout()[j] * c.h[T][j];
        return y;
    }

    [[nodiscard]] double compute_gradients(const double* sequence, std::size_t steps,
                                           double target, double* grad) const noexcept {
        for (std::size_t i = 0; i < kParams; ++i) grad[i] = 0.0;
        Cache c{}; const std::size_t T = forward(sequence, steps, c);
        if (T == 0) return 0.0;
        double y = readout()[H];
        for (std::size_t j = 0; j < H; ++j) y += readout()[j] * c.h[T][j];
        const double e = y - target, dy = 2.0 * e;
        for (std::size_t j = 0; j < H; ++j) grad[kReadout + j] = dy * c.h[T][j];
        grad[kReadout + H] = dy;
        double dh_next[H];
        for (std::size_t j = 0; j < H; ++j) dh_next[j] = dy * readout()[j];

        for (std::size_t tr = T; tr-- > 0;) {
            double daz[H], dar[H], dan[H], dh_prev[H] = {}, dr[H] = {};
            for (std::size_t a = 0; a < H; ++a) {
                const double dh = dh_next[a];
                const double dz = dh * (c.n[tr][a] - c.h[tr][a]);
                const double dn = dh * c.z[tr][a];
                dh_prev[a] += dh * (1.0 - c.z[tr][a]);
                daz[a] = dz * c.z[tr][a] * (1.0 - c.z[tr][a]);
                dan[a] = dn * (1.0 - c.n[tr][a] * c.n[tr][a]);
            }

            double cat[kCat], catn[kCat];
            for (std::size_t j = 0; j < X; ++j) cat[j] = catn[j] = sequence[tr * X + j];
            for (std::size_t j = 0; j < H; ++j) {
                cat[X + j] = c.h[tr][j]; catn[X + j] = c.r[tr][j] * c.h[tr][j];
            }
            double* gwn = grad + 2 * kGateBlock;
            double* gbn = gwn + kGateMatrix;
            for (std::size_t a = 0; a < H; ++a) {
                gbn[a] += dan[a];
                for (std::size_t j = 0; j < kCat; ++j) {
                    gwn[a * kCat + j] += dan[a] * catn[j];
                    if (j >= X) {
                        const double dcat = wn()[a * kCat + j] * dan[a];
                        dr[j - X] += dcat * c.h[tr][j - X];
                        dh_prev[j - X] += dcat * c.r[tr][j - X];
                    }
                }
            }
            for (std::size_t a = 0; a < H; ++a)
                dar[a] = dr[a] * c.r[tr][a] * (1.0 - c.r[tr][a]);

            const double* da[2] = {daz, dar};
            for (std::size_t gate = 0; gate < 2; ++gate) {
                const double* w = p_ + gate * kGateBlock;
                double* gw = grad + gate * kGateBlock;
                double* gb = gw + kGateMatrix;
                for (std::size_t a = 0; a < H; ++a) {
                    gb[a] += da[gate][a];
                    for (std::size_t j = 0; j < kCat; ++j) {
                        gw[a * kCat + j] += da[gate][a] * cat[j];
                        if (j >= X) dh_prev[j - X] += w[a * kCat + j] * da[gate][a];
                    }
                }
            }
            for (std::size_t j = 0; j < H; ++j) dh_next[j] = dh_prev[j];
        }
        return e * e;
    }

    double train_step(const double* sequence, std::size_t steps, double target,
                      double learning_rate, double gradient_clip = 5.0) noexcept {
        double g[kParams]; const double loss = compute_gradients(sequence, steps, target, g);
        for (std::size_t i = 0; i < kParams; ++i)
            p_[i] -= learning_rate * recurrent_detail::clipped(g[i], gradient_clip);
        return loss;
    }

    [[nodiscard]] static constexpr std::size_t param_count() noexcept { return kParams; }
    [[nodiscard]] const double* params() const noexcept { return p_; }
    [[nodiscard]] double* mutable_params() noexcept { return p_; }
    void load_params(const double* src) noexcept {
        for (std::size_t i = 0; i < kParams; ++i) p_[i] = src[i];
    }

private:
    struct Cache {
        double h[MaxT + 1][H] = {};
        double z[MaxT][H] = {}, r[MaxT][H] = {}, n[MaxT][H] = {};
    };
    std::size_t forward(const double* sequence, std::size_t steps, Cache& c) const noexcept {
        const std::size_t T = std::min(steps, MaxT);
        for (std::size_t t = 0; t < T; ++t) {
            for (std::size_t a = 0; a < H; ++a) {
                double az = bz()[a], ar = br()[a];
                for (std::size_t j = 0; j < X; ++j) {
                    const double x = sequence[t * X + j];
                    az += wz()[a * kCat + j] * x; ar += wr()[a * kCat + j] * x;
                }
                for (std::size_t j = 0; j < H; ++j) {
                    az += wz()[a * kCat + X + j] * c.h[t][j];
                    ar += wr()[a * kCat + X + j] * c.h[t][j];
                }
                c.z[t][a] = sigmoid(az); c.r[t][a] = sigmoid(ar);
            }
            for (std::size_t a = 0; a < H; ++a) {
                double an = bn()[a];
                for (std::size_t j = 0; j < X; ++j) an += wn()[a * kCat + j] * sequence[t * X + j];
                for (std::size_t j = 0; j < H; ++j)
                    an += wn()[a * kCat + X + j] * c.r[t][j] * c.h[t][j];
                c.n[t][a] = std::tanh(an);
                c.h[t + 1][a] = (1.0 - c.z[t][a]) * c.h[t][a]
                              + c.z[t][a] * c.n[t][a];
            }
        }
        return T;
    }
    const double* wz() const noexcept { return p_; }
    const double* wr() const noexcept { return p_ + kGateBlock; }
    const double* wn() const noexcept { return p_ + 2 * kGateBlock; }
    const double* bz() const noexcept { return p_ + kGateMatrix; }
    const double* br() const noexcept { return p_ + kGateBlock + kGateMatrix; }
    const double* bn() const noexcept { return p_ + 2 * kGateBlock + kGateMatrix; }
    double* bz() noexcept { return p_ + kGateMatrix; }
    const double* readout() const noexcept { return p_ + kReadout; }
    double* readout() noexcept { return p_ + kReadout; }
    double p_[kParams] = {};
};

} // namespace altair
