// models/meta_learners.hpp -- altair_trader's candidates beside the ridge
// stack: a temporal CNN over every model's recent calls, an analog search for
// the most similar past moments, and the selector that trades whichever has
// been closest so far -- "no change" among them.
//
// THE STEP CONTRACT (the same as models/meta_trader.hpp). predict(x) makes
// this step's call from what was learned before it; learn(y) adds the step's
// outcome once it is known. Every refit happens inside learn(), from learned
// steps only, so nothing a learner fits has seen an outcome it is about to
// call. The CNN and the analogs refit every `refit_every` steps on every step
// learned so far and use the last fit in between.
//
// WHAT "SIMILAR" MEANS HERE.
//   * The CNN (a 1 x 1 convolution, then a causal convolution over time)
//     first mixes the models' calls into a few learned patterns -- models
//     that tend to agree, or to be right together, load on the same channel
//     -- then reads how those patterns moved over the last `window` steps.
//   * The analogs standardise today's calls and their 5-step average,
//     compress them to the principal components of the past, and find the
//     k nearest past moments in that space; the call is what followed them.
// Each holds back the newest 20 % of its training steps at every refit, and
// forecasts no change until its fit beats no change on them: a learner that
// cannot beat doing nothing on its own recent past is not let near a trade.
//
// THE SELECTOR scores every candidate prequentially -- on the call it made
// before the outcome -- with exponentially decayed squared error, and passes
// on the call of the one with the least, among those scored long enough.
// "No change" is always a candidate, so if none of them has been closer than
// doing nothing, nothing is traded. Nothing is chosen by looking at the step
// being traded.

#pragma once

#include <models/meta_trader.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace altair::meta {

class Learner {
public:
    virtual ~Learner() = default;
    [[nodiscard]] virtual std::string name() const = 0;
    /// This step's expected move (bp) from inputs `x`; 0 is "no change".
    [[nodiscard]] virtual double predict(const std::vector<double>& x) = 0;
    /// The outcome (bp) of the step last predicted.
    virtual void learn(double y) = 0;
    [[nodiscard]] virtual bool ready() const = 0;
    /// What the last fit decided, for the report.
    [[nodiscard]] virtual std::string note() const { return {}; }
};

/// The random walk: no move, always.
class NoChange final : public Learner {
public:
    std::string name() const override { return "No change"; }
    double predict(const std::vector<double>&) override { return 0.0; }
    void learn(double) override {}
    bool ready() const override { return true; }
};

/// models/meta_trader.hpp's ridge stack with forgetting, as a candidate.
class RidgeLearner final : public Learner {
public:
    RidgeLearner(std::size_t features, MetaConfig cfg) : m_(features, std::move(cfg)) {}
    std::string name() const override { return "Ridge stack"; }
    double predict(const std::vector<double>& x) override {
        last_ = m_.predict(x);
        return last_.ready ? last_.mu : 0.0;
    }
    void learn(double y) override { m_.learn(y); }
    bool ready() const override { return last_.ready; }
    [[nodiscard]] const MetaModel& model() const noexcept { return m_; }

private:
    MetaModel m_;
    MetaCall last_;
};

namespace learner_detail {

/// Deterministic generator: the same seed, the same fit.
struct LearnerRng {
    std::uint64_t s;
    double uniform() noexcept {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0);
    }
    std::size_t below(std::size_t n) noexcept { return static_cast<std::size_t>(uniform() * static_cast<double>(n)) % n; }
};

/// Per-feature mean and spread of the learned steps, for standardising.
struct LearnerScaler {
    std::vector<double> mean, inv_sd;
    void fit(const std::vector<std::vector<double>>& xs, std::size_t n) {
        const std::size_t p = xs.empty() ? 0 : xs[0].size();
        mean.assign(p, 0.0);
        inv_sd.assign(p, 1.0);
        if (n == 0) return;
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = 0; j < p; ++j) mean[j] += xs[i][j];
        for (double& m : mean) m /= static_cast<double>(n);
        std::vector<double> var(p, 0.0);
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = 0; j < p; ++j) var[j] += (xs[i][j] - mean[j]) * (xs[i][j] - mean[j]);
        for (std::size_t j = 0; j < p; ++j) {
            const double sd = std::sqrt(var[j] / static_cast<double>(n));
            inv_sd[j] = sd > 1e-12 ? 1.0 / sd : 0.0;   // a constant column carries nothing
        }
    }
    [[nodiscard]] double z(double v, std::size_t j) const noexcept {
        const double s = (v - mean[j]) * inv_sd[j];
        return std::clamp(s, -6.0, 6.0);   // RULE 11: a winsor, on purpose -- one wild input must not own the fit
    }
};

} // namespace learner_detail

struct TcnConfig {
    std::size_t window = 10;      ///< steps the convolution reads
    std::size_t hidden = 8;       ///< patterns the 1 x 1 convolution mixes the models into
    std::size_t channels = 8;     ///< channels of the causal convolution over time
    std::size_t kernel = 3;       ///< its width in steps
    std::size_t epochs = 30;
    std::size_t batch = 32;
    std::size_t max_passes = 60'000;   ///< samples x epochs per refit, at most
    std::size_t min_steps = 250;  ///< no fit before this many learned steps
    std::size_t refit_every = 63; ///< steps between refits
    double lr = 3e-3, l2 = 1e-3;
    double holdout = 0.2;         ///< the newest share of the training steps, held back
    std::uint64_t seed = 7;
};

/// A temporal CNN over the models' calls, regression on the next move, by
/// analytic backpropagation and Adam (checked against finite differences in
/// models/tests/test_meta_learners.cpp).
class MetaTcn final : public Learner {
public:
    explicit MetaTcn(TcnConfig cfg = {}) : c_(cfg) {}
    std::string name() const override { return "CNN"; }

    double predict(const std::vector<double>& x) override {
        xs_.push_back(x);
        pending_ = true;
        if (!fitted_ || !useful_ || xs_.size() < c_.window) { called_ = false; return 0.0; }
        called_ = true;
        build_window(xs_.size() - 1, win_);
        return forward(win_, act_) * scale_;
    }
    void learn(double y) override {
        if (!pending_) return;
        pending_ = false;
        ys_.push_back(std::isfinite(y) ? y : 0.0);
        const std::size_t n = ys_.size();
        if (n >= c_.min_steps && (n - last_fit_ >= c_.refit_every || !fitted_)) refit();
    }
    bool ready() const override { return fitted_; }
    std::string note() const override { return note_; }

    // ---- exposed for the gradient check -----------------------------------------
    [[nodiscard]] std::size_t params() const noexcept { return w_.size(); }
    void init(std::size_t features, std::uint64_t seed) {
        m_ = features;
        layout();
        learner_detail::LearnerRng r{seed * 0x9E3779B97F4A7C15ULL + 1};
        for (std::size_t i = 0; i < w_.size(); ++i) w_[i] = 0.0;
        const auto fill = [&](std::size_t at, std::size_t n, std::size_t fan_in) {
            const double a = std::sqrt(6.0 / static_cast<double>(fan_in));
            for (std::size_t i = 0; i < n; ++i) w_[at + i] = (2.0 * r.uniform() - 1.0) * a;
        };
        fill(oa_, c_.hidden * m_, m_);
        fill(ob_, c_.channels * c_.hidden * c_.kernel, c_.hidden * c_.kernel);
        fill(ov_, 2 * c_.channels, 2 * c_.channels);
    }
    /// Loss 0.5 (f(x) - t)^2 + 0.5 l2 |w|^2 and its gradient, for one window
    /// (window x features, row-major, already standardised).
    double loss_and_grad(const std::vector<double>& win, double t, std::vector<double>& g) {
        const double y = forward(win, act_);
        g.assign(w_.size(), 0.0);
        backward(win, act_, y - t, g);
        double reg = 0.0;
        for (std::size_t i = 0; i < w_.size(); ++i) {
            if (!is_bias(i)) { reg += w_[i] * w_[i]; g[i] += c_.l2 * w_[i]; }
        }
        return 0.5 * (y - t) * (y - t) + 0.5 * c_.l2 * reg;
    }
    [[nodiscard]] std::vector<double>& weights() noexcept { return w_; }

private:
    struct Acts {
        std::vector<double> h1, z1, h2, z2;
    };

    void layout() {
        oa_ = 0;
        oba_ = oa_ + c_.hidden * m_;
        ob_ = oba_ + c_.hidden;
        obb_ = ob_ + c_.channels * c_.hidden * c_.kernel;
        ov_ = obb_ + c_.channels;
        ob0_ = ov_ + 2 * c_.channels;
        w_.assign(ob0_ + 1, 0.0);
    }
    [[nodiscard]] bool is_bias(std::size_t i) const noexcept {
        return (i >= oba_ && i < ob_) || (i >= obb_ && i < ov_) || i == ob0_;
    }

    double forward(const std::vector<double>& win, Acts& a) const {
        const std::size_t L = c_.window, H = c_.hidden, C = c_.channels, K = c_.kernel;
        a.z1.assign(L * H, 0.0);
        a.h1.assign(L * H, 0.0);
        for (std::size_t t = 0; t < L; ++t)
            for (std::size_t h = 0; h < H; ++h) {
                double s = w_[oba_ + h];
                const double* wr = &w_[oa_ + h * m_];
                const double* xr = &win[t * m_];
                for (std::size_t j = 0; j < m_; ++j) s += wr[j] * xr[j];
                a.z1[t * H + h] = s;
                a.h1[t * H + h] = s > 0.0 ? s : 0.0;
            }
        const std::size_t T2 = L - K + 1;
        a.z2.assign(T2 * C, 0.0);
        a.h2.assign(T2 * C, 0.0);
        for (std::size_t u = 0; u < T2; ++u) {
            const std::size_t t = u + K - 1;
            for (std::size_t c = 0; c < C; ++c) {
                double s = w_[obb_ + c];
                for (std::size_t h = 0; h < H; ++h)
                    for (std::size_t k = 0; k < K; ++k) s += w_[ob_ + (c * H + h) * K + k] * a.h1[(t - k) * H + h];
                a.z2[u * C + c] = s;
                a.h2[u * C + c] = s > 0.0 ? s : 0.0;
            }
        }
        double y = w_[ob0_];
        for (std::size_t c = 0; c < C; ++c) {
            double avg = 0.0;
            for (std::size_t u = 0; u < T2; ++u) avg += a.h2[u * C + c];
            avg /= static_cast<double>(T2);
            y += w_[ov_ + c] * avg + w_[ov_ + C + c] * a.h2[(T2 - 1) * C + c];
        }
        return y;
    }

    void backward(const std::vector<double>& win, const Acts& a, double dy, std::vector<double>& g) const {
        const std::size_t L = c_.window, H = c_.hidden, C = c_.channels, K = c_.kernel, T2 = L - K + 1;
        g[ob0_] += dy;
        std::vector<double> dz2(T2 * C, 0.0);
        for (std::size_t c = 0; c < C; ++c) {
            double avg = 0.0;
            for (std::size_t u = 0; u < T2; ++u) avg += a.h2[u * C + c];
            avg /= static_cast<double>(T2);
            g[ov_ + c] += dy * avg;
            g[ov_ + C + c] += dy * a.h2[(T2 - 1) * C + c];
            for (std::size_t u = 0; u < T2; ++u) {
                double d = dy * w_[ov_ + c] / static_cast<double>(T2);
                if (u == T2 - 1) d += dy * w_[ov_ + C + c];
                dz2[u * C + c] = a.z2[u * C + c] > 0.0 ? d : 0.0;
            }
        }
        std::vector<double> dh1(L * H, 0.0);
        for (std::size_t u = 0; u < T2; ++u) {
            const std::size_t t = u + K - 1;
            for (std::size_t c = 0; c < C; ++c) {
                const double d = dz2[u * C + c];
                if (d == 0.0) continue;
                g[obb_ + c] += d;
                for (std::size_t h = 0; h < H; ++h)
                    for (std::size_t k = 0; k < K; ++k) {
                        g[ob_ + (c * H + h) * K + k] += d * a.h1[(t - k) * H + h];
                        dh1[(t - k) * H + h] += d * w_[ob_ + (c * H + h) * K + k];
                    }
            }
        }
        for (std::size_t t = 0; t < L; ++t)
            for (std::size_t h = 0; h < H; ++h) {
                const double d = a.z1[t * H + h] > 0.0 ? dh1[t * H + h] : 0.0;
                if (d == 0.0) continue;
                g[oba_ + h] += d;
                double* gr = &g[oa_ + h * m_];
                const double* xr = &win[t * m_];
                for (std::size_t j = 0; j < m_; ++j) gr[j] += d * xr[j];
            }
    }

    /// The standardised window ending at step `s` (window x features).
    void build_window(std::size_t s, std::vector<double>& out) const {
        out.assign(c_.window * m_, 0.0);
        for (std::size_t t = 0; t < c_.window; ++t) {
            const auto& x = xs_[s + 1 - c_.window + t];
            for (std::size_t j = 0; j < m_ && j < x.size(); ++j) out[t * m_ + j] = sc_.z(x[j], j);
        }
    }

    void refit() {
        const std::size_t n = ys_.size();   // learned steps: xs_[0 .. n-1] with outcomes
        last_fit_ = n;
        ++fits_;
        m_ = xs_.empty() ? 0 : xs_[0].size();
        if (m_ == 0 || n < c_.window + 20) return;
        sc_.fit(xs_, n);
        // Samples: windows ending at s = window-1 .. n-1, in time order.
        const std::size_t first = c_.window - 1, total = n - first;
        const std::size_t hold = std::max<std::size_t>(10, static_cast<std::size_t>(c_.holdout * static_cast<double>(total)));
        if (total <= hold + 20) return;
        const std::size_t train = total - hold;
        double ss = 0.0;
        for (std::size_t i = 0; i < train; ++i) ss += ys_[first + i] * ys_[first + i];
        scale_ = std::sqrt(ss / static_cast<double>(train));
        if (!(scale_ > 0.0)) return;
        init(m_, c_.seed + fits_);
        std::vector<std::vector<double>> wins(total);
        for (std::size_t i = 0; i < total; ++i) build_window(first + i, wins[i]);
        const auto holdout_mse = [&]() {
            double e = 0.0;
            for (std::size_t i = train; i < total; ++i) {
                const double d = forward(wins[i], act_) - ys_[first + i] / scale_;
                e += d * d;
            }
            return e / static_cast<double>(hold);
        };
        double zero = 0.0;
        for (std::size_t i = train; i < total; ++i) zero += (ys_[first + i] / scale_) * (ys_[first + i] / scale_);
        zero /= static_cast<double>(hold);
        std::vector<double> best = w_, mo(w_.size(), 0.0), vo(w_.size(), 0.0), g, gsum(w_.size());
        double best_e = holdout_mse();
        learner_detail::LearnerRng r{c_.seed * 7919 + fits_};
        std::vector<std::size_t> order(train);
        std::iota(order.begin(), order.end(), std::size_t{0});
        const std::size_t epochs = std::max<std::size_t>(1, std::min(c_.epochs, c_.max_passes / std::max<std::size_t>(1, train)));
        std::size_t step = 0;
        std::size_t best_epoch = 0;
        for (std::size_t ep = 0; ep < epochs; ++ep) {
            for (std::size_t i = train; i-- > 1;) std::swap(order[i], order[r.below(i + 1)]);
            for (std::size_t b = 0; b < train; b += c_.batch) {
                const std::size_t e = std::min(train, b + c_.batch);
                std::fill(gsum.begin(), gsum.end(), 0.0);
                for (std::size_t k = b; k < e; ++k) {
                    (void)loss_and_grad(wins[order[k]], ys_[first + order[k]] / scale_, g);
                    for (std::size_t q = 0; q < g.size(); ++q) gsum[q] += g[q];
                }
                const double inv = 1.0 / static_cast<double>(e - b);
                ++step;
                const double b1 = 0.9, b2 = 0.999;
                const double c1 = 1.0 - std::pow(b1, static_cast<double>(step)), c2 = 1.0 - std::pow(b2, static_cast<double>(step));
                for (std::size_t q = 0; q < w_.size(); ++q) {
                    const double gq = gsum[q] * inv;
                    mo[q] = b1 * mo[q] + (1 - b1) * gq;
                    vo[q] = b2 * vo[q] + (1 - b2) * gq * gq;
                    w_[q] -= c_.lr * (mo[q] / c1) / (std::sqrt(vo[q] / c2) + 1e-8);
                }
            }
            const double he = holdout_mse();
            if (he < best_e) { best_e = he; best = w_; best_epoch = ep + 1; }
        }
        w_ = best;
        fitted_ = true;
        useful_ = best_e < zero;
        note_ = "fit " + std::to_string(fits_) + " on " + std::to_string(train) + " steps (" + std::to_string(best_epoch)
              + " epochs kept): held-back MSE " + std::to_string(best_e).substr(0, 6) + " vs no change " + std::to_string(zero).substr(0, 6)
              + (useful_ ? "" : " -- forecasts no change");
    }

    TcnConfig c_;
    std::size_t m_ = 0;
    std::size_t oa_ = 0, oba_ = 0, ob_ = 0, obb_ = 0, ov_ = 0, ob0_ = 0;
    std::vector<double> w_;
    learner_detail::LearnerScaler sc_;
    std::vector<std::vector<double>> xs_;
    std::vector<double> ys_;
    std::vector<double> win_;
    Acts act_;
    double scale_ = 1.0;
    bool pending_ = false, fitted_ = false, useful_ = false, called_ = false;
    std::size_t last_fit_ = 0, fits_ = 0;
    std::string note_;
};

struct AnalogConfig {
    std::size_t components = 8;   ///< principal components the patterns are compressed to
    std::size_t smooth = 5;       ///< the pattern is today's inputs and their mean over this many steps
    std::size_t min_steps = 250;
    std::size_t refit_every = 63;
    std::size_t pca_rows = 4000;  ///< the newest learned steps the components are fitted on
    double holdout = 0.2;
};

/// The k nearest past moments by the models' calls; the call is the mean of
/// what followed them.
class AnalogLearner final : public Learner {
public:
    explicit AnalogLearner(AnalogConfig cfg = {}) : c_(cfg) {}
    std::string name() const override { return "Analogs"; }

    double predict(const std::vector<double>& x) override {
        xs_.push_back(x);
        pending_ = true;
        last_neighbours_.clear();
        if (!fitted_ || !useful_) return 0.0;
        const auto q = project(pattern(xs_.size() - 1));
        return neighbours_mean(q, proj_.size(), &last_neighbours_);
    }
    void learn(double y) override {
        if (!pending_) return;
        pending_ = false;
        ys_.push_back(std::isfinite(y) ? y : 0.0);
        const std::size_t n = ys_.size();
        if (n >= c_.min_steps && (n - last_fit_ >= c_.refit_every || !fitted_)) refit();
    }
    bool ready() const override { return fitted_; }
    std::string note() const override { return note_; }

    /// The neighbours of the last call: (step index, distance, what followed).
    struct Neighbour { std::size_t step; double distance; double outcome; };
    [[nodiscard]] const std::vector<Neighbour>& last_neighbours() const noexcept { return last_neighbours_; }

private:
    [[nodiscard]] std::vector<double> pattern(std::size_t s) const {
        const std::size_t m = xs_[s].size();
        std::vector<double> p(2 * m, 0.0);
        const std::size_t from = s + 1 >= c_.smooth ? s + 1 - c_.smooth : 0;
        for (std::size_t j = 0; j < m; ++j) {
            p[j] = sc_.z(xs_[s][j], j);
            double a = 0.0;
            for (std::size_t t = from; t <= s; ++t) a += sc_.z(xs_[t][j], j);
            p[m + j] = a / static_cast<double>(s + 1 - from);
        }
        return p;
    }
    [[nodiscard]] std::vector<double> project(const std::vector<double>& p) const {
        std::vector<double> q(basis_.size(), 0.0);
        for (std::size_t c = 0; c < basis_.size(); ++c)
            for (std::size_t j = 0; j < p.size(); ++j) q[c] += (p[j] - pmean_[j]) * basis_[c][j];
        return q;
    }
    /// Mean outcome of the k nearest of the first `upto` stored projections.
    double neighbours_mean(const std::vector<double>& q, std::size_t upto, std::vector<Neighbour>* keep) const {
        if (upto == 0) return 0.0;
        const std::size_t k = std::clamp<std::size_t>(static_cast<std::size_t>(std::sqrt(static_cast<double>(upto))), 10, 100);
        std::vector<std::pair<double, std::size_t>> d(upto);
        for (std::size_t i = 0; i < upto; ++i) {
            double s = 0.0;
            for (std::size_t c = 0; c < q.size(); ++c) s += (proj_[i][c] - q[c]) * (proj_[i][c] - q[c]);
            d[i] = {s, i};
        }
        const std::size_t kk = std::min(k, upto);
        std::partial_sort(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(kk), d.end());
        double m = 0.0;
        for (std::size_t i = 0; i < kk; ++i) {
            m += out_[d[i].second];
            if (keep != nullptr) keep->push_back({step_[d[i].second], std::sqrt(d[i].first), out_[d[i].second]});
        }
        return m / static_cast<double>(kk);
    }

    void refit() {
        const std::size_t n = ys_.size();
        last_fit_ = n;
        ++fits_;
        if (n < 40 || xs_.empty()) return;
        sc_.fit(xs_, n);
        std::vector<std::vector<double>> pats(n);
        for (std::size_t s = 0; s < n; ++s) pats[s] = pattern(s);
        const std::size_t dim = pats[0].size();
        const std::size_t from = n > c_.pca_rows ? n - c_.pca_rows : 0;
        pmean_.assign(dim, 0.0);
        for (std::size_t s = from; s < n; ++s)
            for (std::size_t j = 0; j < dim; ++j) pmean_[j] += pats[s][j];
        for (double& v : pmean_) v /= static_cast<double>(n - from);
        // Principal components by power iteration with deflation, on X'X
        // applied implicitly (X: the centred patterns), never forming it.
        basis_.clear();
        learner_detail::LearnerRng r{0xA5A5ULL + fits_};
        for (std::size_t c = 0; c < c_.components && c < dim; ++c) {
            std::vector<double> v(dim);
            for (double& e : v) e = r.uniform() - 0.5;
            for (int it = 0; it < 30; ++it) {
                std::vector<double> w(dim, 0.0);
                for (std::size_t s = from; s < n; ++s) {
                    double dot = 0.0;
                    for (std::size_t j = 0; j < dim; ++j) dot += (pats[s][j] - pmean_[j]) * v[j];
                    for (std::size_t j = 0; j < dim; ++j) w[j] += dot * (pats[s][j] - pmean_[j]);
                }
                for (const auto& b : basis_) {
                    double dot = 0.0;
                    for (std::size_t j = 0; j < dim; ++j) dot += w[j] * b[j];
                    for (std::size_t j = 0; j < dim; ++j) w[j] -= dot * b[j];
                }
                double norm = 0.0;
                for (double e : w) norm += e * e;
                norm = std::sqrt(norm);
                if (!(norm > 0.0)) break;
                for (std::size_t j = 0; j < dim; ++j) v[j] = w[j] / norm;
            }
            basis_.push_back(v);
        }
        proj_.assign(n, {});
        out_.assign(n, 0.0);
        step_.assign(n, 0);
        for (std::size_t s = 0; s < n; ++s) { proj_[s] = project(pats[s]); out_[s] = ys_[s]; step_[s] = s; }
        // Held back: the newest 20 % called from the older steps alone.
        const std::size_t hold = std::max<std::size_t>(10, static_cast<std::size_t>(c_.holdout * static_cast<double>(n)));
        const std::size_t train = n - hold;
        double e = 0.0, zero = 0.0;
        for (std::size_t s = train; s < n; ++s) {
            const double f = neighbours_mean(proj_[s], train, nullptr);
            e += (f - ys_[s]) * (f - ys_[s]);
            zero += ys_[s] * ys_[s];
        }
        fitted_ = true;
        useful_ = e < zero;
        note_ = "fit " + std::to_string(fits_) + " on " + std::to_string(n) + " steps, " + std::to_string(basis_.size())
              + " components: held-back MSE " + std::to_string(e / static_cast<double>(hold)).substr(0, 8) + " vs no change "
              + std::to_string(zero / static_cast<double>(hold)).substr(0, 8) + (useful_ ? "" : " -- forecasts no change");
    }

    AnalogConfig c_;
    learner_detail::LearnerScaler sc_;
    std::vector<std::vector<double>> xs_, basis_, proj_;
    std::vector<double> ys_, pmean_, out_;
    std::vector<std::size_t> step_;
    std::vector<Neighbour> last_neighbours_;
    bool pending_ = false, fitted_ = false, useful_ = false;
    std::size_t last_fit_ = 0, fits_ = 0;
    std::string note_;
};

/// Every candidate called on every step; the one with the least decayed
/// prequential squared error is the one whose call is passed on.
class Selector {
public:
    Selector(std::vector<std::unique_ptr<Learner>> c, double half_life, std::size_t score_min)
        : c_(std::move(c)), decay_(std::pow(0.5, 1.0 / std::max(1.0, half_life))), score_min_(score_min),
          sse_(c_.size(), 0.0), sw_(c_.size(), 0.0), scored_(c_.size(), 0), pred_(c_.size(), 0.0), live_(c_.size(), false) {}

    struct Call { double mu = 0.0; std::size_t chosen = 0; };

    Call predict(const std::vector<double>& x) {
        for (std::size_t k = 0; k < c_.size(); ++k) {
            pred_[k] = c_[k]->predict(x);
            live_[k] = c_[k]->ready();
        }
        pending_ = true;
        std::size_t best = 0;   // "No change" first: the default
        double e = std::numeric_limits<double>::infinity();
        for (std::size_t k = 0; k < c_.size(); ++k) {
            if (scored_[k] < score_min_ || !(sw_[k] > 0.0)) continue;
            const double m = sse_[k] / sw_[k];
            if (m < e) { e = m; best = k; }
        }
        return {pred_[best], best};
    }
    void learn(double y) {
        if (!pending_) return;
        pending_ = false;
        for (std::size_t k = 0; k < c_.size(); ++k) {
            if (live_[k] && std::isfinite(y)) {
                const double d = pred_[k] - y;
                sse_[k] = sse_[k] * decay_ + d * d;
                sw_[k] = sw_[k] * decay_ + 1.0;
                ++scored_[k];
            }
            c_[k]->learn(y);
        }
    }
    [[nodiscard]] std::size_t size() const noexcept { return c_.size(); }
    [[nodiscard]] const Learner& candidate(std::size_t k) const { return *c_[k]; }
    [[nodiscard]] Learner& candidate(std::size_t k) { return *c_[k]; }
    [[nodiscard]] double last_prediction(std::size_t k) const { return pred_[k]; }
    [[nodiscard]] bool last_live(std::size_t k) const { return live_[k]; }

private:
    std::vector<std::unique_ptr<Learner>> c_;
    double decay_;
    std::size_t score_min_;
    std::vector<double> sse_, sw_;
    std::vector<std::size_t> scored_;
    std::vector<double> pred_;
    std::vector<bool> live_;
    bool pending_ = false;
};

} // namespace altair::meta
