// models/classical.hpp -- deterministic classical model primitives.
//
// These models are research components, not a trading decision path. Fitting
// owns its vectors and may allocate; prediction is read-only and allocation
// free. Every fitter refuses malformed, non-finite or numerically singular
// input instead of returning a plausible-looking zero model.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace altair {

enum class ClassicalError : std::uint8_t {
    BadShape,
    TooFewSamples,
    NonFinite,
    InvalidLabel,
    Singular,
    NotConverged
};

namespace classical_detail {

[[nodiscard]] inline bool finite_row(const double* row, std::size_t n) noexcept {
    for (std::size_t j = 0; j < n; ++j) {
        if (!std::isfinite(row[j])) { return false; }
    }
    return true;
}

[[nodiscard]] inline double sigmoid(double x) noexcept {
    if (x >= 0.0) {
        const double e = std::exp(-x);
        return 1.0 / (1.0 + e);
    }
    const double e = std::exp(x);
    return e / (1.0 + e);
}

/// Solve a small dense system by pivoted Gaussian elimination.
[[nodiscard]] inline bool solve(std::vector<double>& a, std::vector<double>& b,
                                std::size_t n) noexcept {
    for (std::size_t k = 0; k < n; ++k) {
        std::size_t pivot = k;
        double best = std::fabs(a[k * n + k]);
        for (std::size_t i = k + 1; i < n; ++i) {
            const double v = std::fabs(a[i * n + k]);
            if (v > best) { best = v; pivot = i; }
        }
        if (!(best > 1e-12) || !std::isfinite(best)) { return false; }
        if (pivot != k) {
            for (std::size_t j = k; j < n; ++j)
                std::swap(a[k * n + j], a[pivot * n + j]);
            std::swap(b[k], b[pivot]);
        }
        const double d = a[k * n + k];
        for (std::size_t i = k + 1; i < n; ++i) {
            const double f = a[i * n + k] / d;
            if (f == 0.0) { continue; }
            a[i * n + k] = 0.0;
            for (std::size_t j = k + 1; j < n; ++j)
                a[i * n + j] -= f * a[k * n + j];
            b[i] -= f * b[k];
        }
    }
    for (std::size_t i = n; i-- > 0;) {
        double v = b[i];
        for (std::size_t j = i + 1; j < n; ++j) v -= a[i * n + j] * b[j];
        const double d = a[i * n + i];
        if (!(std::fabs(d) > 1e-12) || !std::isfinite(v)) { return false; }
        b[i] = v / d;
    }
    return true;
}

class ClassicalRng {
public:
    explicit ClassicalRng(std::uint64_t seed) noexcept
        : state_(seed != 0 ? seed : 0x9E3779B97F4A7C15ull) {}
    [[nodiscard]] std::uint64_t next() noexcept {
        state_ ^= state_ >> 12; state_ ^= state_ << 25; state_ ^= state_ >> 27;
        return state_ * 2685821657736338717ull;
    }
    [[nodiscard]] std::size_t index(std::size_t n) noexcept {
        return n == 0 ? 0 : static_cast<std::size_t>(next() % n);
    }
    [[nodiscard]] double unit() noexcept {
        return static_cast<double>(next() >> 11) / static_cast<double>(1ull << 53);
    }
private:
    std::uint64_t state_;
};

} // namespace classical_detail

// ---------------------------------------------------------------------------
// Logistic regression
// ---------------------------------------------------------------------------

struct LogisticParams {
    std::size_t epochs = 500;
    double learning_rate = 0.05;
    double l2 = 1e-4;
};

// ---------------------------------------------------------------------------
// M01 — deterministic binary support-vector machine
// ---------------------------------------------------------------------------

enum class SvmKernel : std::uint8_t { Linear, Rbf };

struct SvmParams {
    SvmKernel kernel = SvmKernel::Linear;
    double c = 1.0;
    double gamma = 1.0;
    double tolerance = 1e-4;
    std::size_t max_passes = 20;
    std::size_t max_samples = 8192;
};

/// Binary C-SVM trained with deterministic simplified SMO.
///
/// Labels are {-1,+1}. The dual is
///   max sum(alpha_i) - 1/2 sum(alpha_i alpha_j y_i y_j K_ij)
/// subject to 0<=alpha_i<=C and sum(alpha_i y_i)=0. RBF uses
/// exp(-gamma*||x-z||^2). Fitting is an offline research operation; prediction
/// is allocation-free and visits only support vectors.
class SupportVectorMachine {
public:
    [[nodiscard]] static std::expected<SupportVectorMachine, ClassicalError>
    fit(std::span<const double> x, std::size_t rows, std::size_t cols,
        std::span<const std::int8_t> labels, SvmParams params = {}) {
        if (rows < 2 || cols == 0 || rows > x.size() / cols
            || labels.size() != rows || rows > params.max_samples
            || !(params.c > 0.0) || !(params.gamma > 0.0)
            || !(params.tolerance > 0.0) || params.max_passes == 0
            || !std::isfinite(params.c) || !std::isfinite(params.gamma))
            return std::unexpected(ClassicalError::BadShape);
        bool negative = false, positive = false;
        for (std::size_t i = 0; i < rows; ++i) {
            if (!classical_detail::finite_row(x.data() + i * cols, cols))
                return std::unexpected(ClassicalError::NonFinite);
            if (labels[i] == -1) negative = true;
            else if (labels[i] == 1) positive = true;
            else return std::unexpected(ClassicalError::InvalidLabel);
        }
        if (!negative || !positive)
            return std::unexpected(ClassicalError::InvalidLabel);

        SupportVectorMachine out;
        out.cols_ = cols;
        out.params_ = params;
        std::vector<double> alpha(rows, 0.0);
        double bias = 0.0;
        const auto kernel = [&](std::size_t a, std::size_t b) noexcept {
            return out.kernel(x.data() + a * cols, x.data() + b * cols);
        };
        const auto margin = [&](std::size_t i) noexcept {
            double value = bias;
            for (std::size_t j = 0; j < rows; ++j)
                if (alpha[j] > 0.0)
                    value += alpha[j] * static_cast<double>(labels[j]) * kernel(j, i);
            return value;
        };

        std::size_t unchanged_passes = 0;
        while (unchanged_passes < params.max_passes) {
            std::size_t changed = 0;
            for (std::size_t i = 0; i < rows; ++i) {
                const double yi = static_cast<double>(labels[i]);
                const double ei = margin(i) - yi;
                if (!((yi * ei < -params.tolerance && alpha[i] < params.c)
                      || (yi * ei > params.tolerance && alpha[i] > 0.0)))
                    continue;

                // Deterministic maximal error separation; this is slower than
                // a random pair but makes model hashes reproducible.
                std::size_t j = i == 0 ? 1 : 0;
                double ej = margin(j) - static_cast<double>(labels[j]);
                double best = std::fabs(ei - ej);
                for (std::size_t candidate = 0; candidate < rows; ++candidate) {
                    if (candidate == i) continue;
                    const double e = margin(candidate)
                                   - static_cast<double>(labels[candidate]);
                    const double separation = std::fabs(ei - e);
                    if (separation > best) {
                        best = separation;
                        j = candidate;
                        ej = e;
                    }
                }
                const double yj = static_cast<double>(labels[j]);
                const double ai_old = alpha[i], aj_old = alpha[j];
                double lower = 0.0, upper = 0.0;
                if (labels[i] != labels[j]) {
                    lower = std::max(0.0, aj_old - ai_old);
                    upper = std::min(params.c, params.c + aj_old - ai_old);
                } else {
                    lower = std::max(0.0, ai_old + aj_old - params.c);
                    upper = std::min(params.c, ai_old + aj_old);
                }
                if (!(upper > lower)) continue;
                const double kii = kernel(i, i), kjj = kernel(j, j);
                const double kij = kernel(i, j);
                const double eta = 2.0 * kij - kii - kjj;
                if (!(eta < -1e-15)) continue;
                double aj = aj_old - yj * (ei - ej) / eta;
                aj = std::clamp(aj, lower, upper);
                if (std::fabs(aj - aj_old) < params.tolerance * 0.1) continue;
                const double ai = ai_old + yi * yj * (aj_old - aj);
                const double b1 = bias - ei - yi * (ai - ai_old) * kii
                                - yj * (aj - aj_old) * kij;
                const double b2 = bias - ej - yi * (ai - ai_old) * kij
                                - yj * (aj - aj_old) * kjj;
                alpha[i] = ai;
                alpha[j] = aj;
                bias = ai > 0.0 && ai < params.c ? b1
                     : (aj > 0.0 && aj < params.c ? b2 : 0.5 * (b1 + b2));
                ++changed;
            }
            unchanged_passes = changed == 0 ? unchanged_passes + 1 : 0;
        }

        out.bias_ = bias;
        for (std::size_t i = 0; i < rows; ++i) {
            if (alpha[i] <= params.tolerance * 0.1) continue;
            out.alpha_.push_back(alpha[i]);
            out.labels_.push_back(labels[i]);
            out.support_.insert(out.support_.end(), x.begin()
                + static_cast<std::ptrdiff_t>(i * cols), x.begin()
                + static_cast<std::ptrdiff_t>((i + 1) * cols));
        }
        if (out.alpha_.empty() || !std::isfinite(out.bias_))
            return std::unexpected(ClassicalError::NotConverged);
        return out;
    }

    [[nodiscard]] double margin(std::span<const double> features) const noexcept {
        if (features.size() != cols_ || !classical_detail::finite_row(
                features.data(), features.size()))
            return std::numeric_limits<double>::quiet_NaN();
        double value = bias_;
        for (std::size_t i = 0; i < alpha_.size(); ++i)
            value += alpha_[i] * static_cast<double>(labels_[i])
                   * kernel(support_.data() + i * cols_, features.data());
        return value;
    }
    [[nodiscard]] std::int8_t classify(std::span<const double> features) const noexcept {
        return margin(features) >= 0.0 ? 1 : -1;
    }
    [[nodiscard]] std::size_t support_vector_count() const noexcept {
        return alpha_.size();
    }

private:
    [[nodiscard]] double kernel(const double* a, const double* b) const noexcept {
        double value = 0.0;
        if (params_.kernel == SvmKernel::Linear) {
            for (std::size_t j = 0; j < cols_; ++j) value += a[j] * b[j];
            return value;
        }
        for (std::size_t j = 0; j < cols_; ++j) {
            const double d = a[j] - b[j];
            value += d * d;
        }
        return std::exp(-params_.gamma * value);
    }

    SvmParams params_{};
    std::vector<double> support_;
    std::vector<double> alpha_;
    std::vector<std::int8_t> labels_;
    std::size_t cols_ = 0;
    double bias_ = 0.0;
};

// ---------------------------------------------------------------------------
// M02 — exact k-nearest-neighbour classification/regression
// ---------------------------------------------------------------------------

struct KnnNeighbour {
    std::size_t training_row = 0;
    double squared_distance = 0.0;
    double target = 0.0;
};

class KnnModel {
public:
    [[nodiscard]] static std::expected<KnnModel, ClassicalError>
    fit(std::span<const double> x, std::size_t rows, std::size_t cols,
        std::span<const double> target, std::size_t k, bool classification) {
        if (rows < 2 || cols == 0 || rows > x.size() / cols
            || target.size() != rows || k == 0 || k > rows)
            return std::unexpected(ClassicalError::BadShape);
        KnnModel out;
        out.rows_ = rows; out.cols_ = cols; out.k_ = k;
        out.classification_ = classification;
        out.x_.assign(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(rows * cols));
        out.target_.assign(target.begin(), target.end());
        out.mean_.assign(cols, 0.0); out.scale_.assign(cols, 0.0);
        for (std::size_t i = 0; i < rows; ++i) {
            if (!classical_detail::finite_row(x.data() + i * cols, cols)
                || !std::isfinite(target[i]))
                return std::unexpected(ClassicalError::NonFinite);
            if (classification && !(target[i] == 0.0 || target[i] == 1.0))
                return std::unexpected(ClassicalError::InvalidLabel);
            for (std::size_t j = 0; j < cols; ++j) out.mean_[j] += x[i * cols + j];
        }
        for (double& v : out.mean_) v /= static_cast<double>(rows);
        for (std::size_t i = 0; i < rows; ++i)
            for (std::size_t j = 0; j < cols; ++j) {
                const double d = x[i * cols + j] - out.mean_[j];
                out.scale_[j] += d * d;
            }
        for (double& v : out.scale_) {
            v = std::sqrt(v / static_cast<double>(rows - 1));
            if (!(v > 1e-12)) v = 1.0; // constant feature contributes zero.
        }
        return out;
    }

    [[nodiscard]] std::expected<std::vector<KnnNeighbour>, ClassicalError>
    neighbours(std::span<const double> features) const {
        if (features.size() != cols_ || !classical_detail::finite_row(
                features.data(), features.size()))
            return std::unexpected(ClassicalError::BadShape);
        std::vector<KnnNeighbour> all;
        all.reserve(rows_);
        for (std::size_t i = 0; i < rows_; ++i) {
            double d2 = 0.0;
            for (std::size_t j = 0; j < cols_; ++j) {
                const double d = (features[j] - x_[i * cols_ + j]) / scale_[j];
                d2 += d * d;
            }
            all.push_back(KnnNeighbour{i, d2, target_[i]});
        }
        std::partial_sort(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(k_),
                          all.end(), [](const auto& a, const auto& b) {
            return a.squared_distance != b.squared_distance
                ? a.squared_distance < b.squared_distance
                : a.training_row < b.training_row;
        });
        all.resize(k_);
        return all;
    }

    [[nodiscard]] std::expected<double, ClassicalError>
    predict(std::span<const double> features) const {
        const auto nearest = neighbours(features);
        if (!nearest) return std::unexpected(nearest.error());
        double sum = 0.0;
        for (const auto& n : *nearest) sum += n.target;
        if (!classification_) return sum / static_cast<double>(nearest->size());
        const double half = 0.5 * static_cast<double>(nearest->size());
        if (sum != half) return sum > half ? 1.0 : 0.0;
        // Deterministic tie rule: the closest neighbour wins; training-row
        // order resolves exact distance ties above.
        return nearest->front().target;
    }

private:
    std::vector<double> x_, target_, mean_, scale_;
    std::size_t rows_ = 0, cols_ = 0, k_ = 0;
    bool classification_ = false;
};

class LogisticRegression {
public:
    [[nodiscard]] static std::expected<LogisticRegression, ClassicalError>
    fit(std::span<const double> x, std::size_t rows, std::size_t cols,
        std::span<const std::uint8_t> labels,
        LogisticParams params = {}) {
        if (rows == 0 || cols == 0 || rows > x.size() / cols
            || labels.size() != rows || params.epochs == 0
            || !(params.learning_rate > 0.0) || params.l2 < 0.0
            || !std::isfinite(params.learning_rate) || !std::isfinite(params.l2))
            return std::unexpected(ClassicalError::BadShape);
        LogisticRegression out;
        out.weights_.assign(cols, 0.0);
        for (std::size_t i = 0; i < rows; ++i) {
            if (!classical_detail::finite_row(x.data() + i * cols, cols)
                || labels[i] > 1) return std::unexpected(
                    labels[i] > 1 ? ClassicalError::InvalidLabel : ClassicalError::NonFinite);
        }
        for (std::size_t epoch = 0; epoch < params.epochs; ++epoch) {
            std::vector<double> grad(cols, 0.0);
            double bias_grad = 0.0;
            for (std::size_t i = 0; i < rows; ++i) {
                const double* row = x.data() + i * cols;
                double z = out.bias_;
                for (std::size_t j = 0; j < cols; ++j) z += out.weights_[j] * row[j];
                const double error = classical_detail::sigmoid(z)
                                   - static_cast<double>(labels[i]);
                bias_grad += error;
                for (std::size_t j = 0; j < cols; ++j) grad[j] += error * row[j];
            }
            const double inv = 1.0 / static_cast<double>(rows);
            out.bias_ -= params.learning_rate * bias_grad * inv;
            for (std::size_t j = 0; j < cols; ++j) {
                grad[j] = grad[j] * inv + params.l2 * out.weights_[j];
                out.weights_[j] -= params.learning_rate * grad[j];
            }
        }
        if (!std::isfinite(out.bias_)) return std::unexpected(ClassicalError::NotConverged);
        for (double v : out.weights_)
            if (!std::isfinite(v)) return std::unexpected(ClassicalError::NotConverged);
        out.cols_ = cols;
        return out;
    }

    [[nodiscard]] double probability(std::span<const double> features) const noexcept {
        if (features.size() != cols_) return std::numeric_limits<double>::quiet_NaN();
        double z = bias_;
        for (std::size_t j = 0; j < cols_; ++j) z += weights_[j] * features[j];
        return classical_detail::sigmoid(z);
    }
    [[nodiscard]] bool classify(std::span<const double> features,
                                double cutoff = 0.5) const noexcept {
        return probability(features) >= cutoff;
    }
    [[nodiscard]] std::span<const double> weights() const noexcept { return weights_; }
    [[nodiscard]] double bias() const noexcept { return bias_; }

private:
    std::vector<double> weights_;
    std::size_t cols_ = 0;
    double bias_ = 0.0;
};

// ---------------------------------------------------------------------------
// Deterministic shallow random forest
// ---------------------------------------------------------------------------

struct ForestParams {
    std::size_t trees = 32;
    std::size_t max_depth = 5;
    std::size_t min_leaf = 3;
    std::size_t features_per_split = 0; // zero means ceil(sqrt(cols))
    std::uint64_t seed = 0xA17A17ull;
    bool regression = false;
};

struct ForestNode {
    int feature = -1;
    double threshold = 0.0;
    double value = 0.0;
    int left = -1;
    int right = -1;
};

struct ForestTree { std::vector<ForestNode> nodes; };

class RandomForest {
public:
    [[nodiscard]] static std::expected<RandomForest, ClassicalError>
    fit(std::span<const double> x, std::size_t rows, std::size_t cols,
        std::span<const double> y, ForestParams params = {}) {
        if (rows < 2 || cols == 0 || rows > x.size() / cols || y.size() != rows
            || params.trees == 0 || params.max_depth == 0 || params.min_leaf == 0)
            return std::unexpected(ClassicalError::BadShape);
        if (params.features_per_split > cols)
            return std::unexpected(ClassicalError::BadShape);
        for (std::size_t i = 0; i < rows; ++i) {
            if (!classical_detail::finite_row(x.data() + i * cols, cols)
                || !std::isfinite(y[i])) return std::unexpected(ClassicalError::NonFinite);
            if (!params.regression && !(y[i] == 0.0 || y[i] == 1.0))
                return std::unexpected(ClassicalError::InvalidLabel);
        }
        RandomForest out;
        out.cols_ = cols; out.regression_ = params.regression;
        classical_detail::ClassicalRng rng{params.seed};
        out.trees_.reserve(params.trees);
        for (std::size_t t = 0; t < params.trees; ++t) {
            std::vector<std::size_t> sample(rows);
            for (auto& index : sample) index = rng.index(rows);
            ForestTree tree;
            out.build_node(tree, x, y, cols, sample, 0, params, rng);
            if (tree.nodes.empty()) return std::unexpected(ClassicalError::NotConverged);
            out.trees_.push_back(std::move(tree));
        }
        return out;
    }

    [[nodiscard]] double predict(std::span<const double> features) const noexcept {
        if (features.size() != cols_ || trees_.empty())
            return std::numeric_limits<double>::quiet_NaN();
        double sum = 0.0;
        for (const auto& tree : trees_) {
            int node = 0;
            while (tree.nodes[static_cast<std::size_t>(node)].feature >= 0) {
                const auto& n = tree.nodes[static_cast<std::size_t>(node)];
                node = features[static_cast<std::size_t>(n.feature)] <= n.threshold
                    ? n.left : n.right;
            }
            sum += tree.nodes[static_cast<std::size_t>(node)].value;
        }
        return sum / static_cast<double>(trees_.size());
    }
    [[nodiscard]] bool classify(std::span<const double> features,
                                double cutoff = 0.5) const noexcept {
        return predict(features) >= cutoff;
    }
    [[nodiscard]] std::size_t tree_count() const noexcept { return trees_.size(); }

private:
    static double impurity(std::span<const double> y, const std::vector<std::size_t>& rows,
                           bool regression) {
        double sum = 0.0, sum2 = 0.0;
        for (const auto i : rows) { sum += y[i]; sum2 += y[i] * y[i]; }
        const double n = static_cast<double>(rows.size());
        if (regression) return sum2 - sum * sum / n;
        const double p = sum / n;
        return n * p * (1.0 - p);
    }

    void build_node(ForestTree& tree, std::span<const double> x,
                    std::span<const double> y, std::size_t cols,
                    const std::vector<std::size_t>& rows, std::size_t depth,
                    const ForestParams& params, classical_detail::ClassicalRng& rng) {
        ForestNode leaf;
        double sum = 0.0;
        for (const auto i : rows) sum += y[i];
        leaf.value = sum / static_cast<double>(rows.size());
        const int here = static_cast<int>(tree.nodes.size());
        tree.nodes.push_back(leaf);
        if (depth >= params.max_depth || rows.size() < 2 * params.min_leaf) return;

        const double parent = impurity(y, rows, params.regression);
        double best_gain = 0.0; std::size_t best_feature = 0; double best_threshold = 0.0;
        const std::size_t want = params.features_per_split != 0
            ? params.features_per_split
            : std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(cols)))));
        std::vector<std::size_t> features(cols);
        for (std::size_t j = 0; j < cols; ++j) features[j] = j;
        for (std::size_t j = cols; j-- > 1;) std::swap(features[j], features[rng.index(j + 1)]);
        for (std::size_t fi = 0; fi < std::min(want, cols); ++fi) {
            const std::size_t feature = features[fi];
            double lo = x[rows[0] * cols + feature], hi = lo;
            for (const auto i : rows) {
                lo = std::min(lo, x[i * cols + feature]);
                hi = std::max(hi, x[i * cols + feature]);
            }
            if (!(hi > lo)) continue;
            for (int c = 1; c <= 8; ++c) {
                const double threshold = lo + (hi - lo) * static_cast<double>(c) / 9.0;
                std::vector<std::size_t> left, right;
                for (const auto i : rows)
                    (x[i * cols + feature] <= threshold ? left : right).push_back(i);
                if (left.size() < params.min_leaf || right.size() < params.min_leaf) continue;
                const double gain = parent - impurity(y, left, params.regression)
                                  - impurity(y, right, params.regression);
                if (gain > best_gain && std::isfinite(gain)) {
                    best_gain = gain; best_feature = feature; best_threshold = threshold;
                }
            }
        }
        if (!(best_gain > 0.0)) return;
        std::vector<std::size_t> left, right;
        for (const auto i : rows)
            (x[i * cols + best_feature] <= best_threshold ? left : right).push_back(i);
        tree.nodes[static_cast<std::size_t>(here)].feature = static_cast<int>(best_feature);
        tree.nodes[static_cast<std::size_t>(here)].threshold = best_threshold;
        const int left_root = static_cast<int>(tree.nodes.size());
        build_node(tree, x, y, cols, left, depth + 1, params, rng);
        const int right_root = static_cast<int>(tree.nodes.size());
        build_node(tree, x, y, cols, right, depth + 1, params, rng);
        tree.nodes[static_cast<std::size_t>(here)].left = left_root;
        tree.nodes[static_cast<std::size_t>(here)].right = right_root;
    }

    std::vector<ForestTree> trees_;
    std::size_t cols_ = 0;
    bool regression_ = false;
};

// ---------------------------------------------------------------------------
// PCA factor extraction and VAR(1)
// ---------------------------------------------------------------------------

struct PcaModel {
    std::vector<double> mean;
    std::vector<double> loadings; // component-major, components x columns
    std::vector<double> explained;
    std::size_t components = 0, columns = 0;

    [[nodiscard]] std::expected<std::vector<double>, ClassicalError>
    transform(std::span<const double> row) const {
        if (row.size() != columns) return std::unexpected(ClassicalError::BadShape);
        std::vector<double> out(components, 0.0);
        for (std::size_t k = 0; k < components; ++k)
            for (std::size_t j = 0; j < columns; ++j)
                out[k] += (row[j] - mean[j]) * loadings[k * columns + j];
        return out;
    }
};

[[nodiscard]] inline std::expected<PcaModel, ClassicalError>
fit_pca(std::span<const double> x, std::size_t rows, std::size_t columns,
        std::size_t components) {
    if (rows < 2 || columns == 0 || rows > x.size() / columns || components == 0
        || components > columns) return std::unexpected(ClassicalError::BadShape);
    PcaModel out; out.columns = columns; out.components = components;
    out.mean.assign(columns, 0.0);
    for (std::size_t i = 0; i < rows; ++i) {
        if (!classical_detail::finite_row(x.data() + i * columns, columns))
            return std::unexpected(ClassicalError::NonFinite);
        for (std::size_t j = 0; j < columns; ++j) out.mean[j] += x[i * columns + j];
    }
    for (double& v : out.mean) v /= static_cast<double>(rows);
    std::vector<double> cov(columns * columns, 0.0);
    for (std::size_t i = 0; i < rows; ++i)
        for (std::size_t a = 0; a < columns; ++a)
            for (std::size_t b = 0; b < columns; ++b)
                cov[a * columns + b] += (x[i * columns + a] - out.mean[a])
                                       * (x[i * columns + b] - out.mean[b]);
    for (double& v : cov) v /= static_cast<double>(rows - 1);
    out.loadings.assign(components * columns, 0.0);
    out.explained.assign(components, 0.0);
    for (std::size_t k = 0; k < components; ++k) {
        std::vector<double> v(columns, 0.0); v[k % columns] = 1.0;
        for (int it = 0; it < 100; ++it) {
            std::vector<double> next(columns, 0.0);
            for (std::size_t a = 0; a < columns; ++a)
                for (std::size_t b = 0; b < columns; ++b) next[a] += cov[a * columns + b] * v[b];
            for (std::size_t q = 0; q < k; ++q) {
                double dot = 0.0;
                for (std::size_t j = 0; j < columns; ++j) dot += next[j] * out.loadings[q * columns + j];
                for (std::size_t j = 0; j < columns; ++j) next[j] -= dot * out.loadings[q * columns + j];
            }
            double norm = 0.0; for (double z : next) norm += z * z;
            norm = std::sqrt(norm);
            if (!(norm > 1e-12) || !std::isfinite(norm)) return std::unexpected(ClassicalError::Singular);
            for (std::size_t j = 0; j < columns; ++j) v[j] = next[j] / norm;
        }
        double eigen = 0.0;
        for (std::size_t a = 0; a < columns; ++a)
            for (std::size_t b = 0; b < columns; ++b) eigen += v[a] * cov[a * columns + b] * v[b];
        if (!(eigen > 1e-12) || !std::isfinite(eigen)) return std::unexpected(ClassicalError::Singular);
        for (std::size_t j = 0; j < columns; ++j) out.loadings[k * columns + j] = v[j];
        out.explained[k] = eigen;
        for (std::size_t a = 0; a < columns; ++a)
            for (std::size_t b = 0; b < columns; ++b) cov[a * columns + b] -= eigen * v[a] * v[b];
    }
    return out;
}

// M03 — a trainable linear autoencoder. The optimum under squared
// reconstruction loss is the PCA subspace, so fitting is deterministic and
// globally optimal rather than an optimiser-dependent approximation.
struct LinearAutoencoder {
    PcaModel encoder;

    [[nodiscard]] static std::expected<LinearAutoencoder, ClassicalError>
    fit(std::span<const double> x, std::size_t rows, std::size_t columns,
        std::size_t bottleneck) {
        const auto pca = fit_pca(x, rows, columns, bottleneck);
        if (!pca) return std::unexpected(pca.error());
        return LinearAutoencoder{*pca};
    }

    [[nodiscard]] std::expected<std::vector<double>, ClassicalError>
    encode(std::span<const double> row) const {
        return encoder.transform(row);
    }

    [[nodiscard]] std::expected<std::vector<double>, ClassicalError>
    decode(std::span<const double> code) const {
        if (code.size() != encoder.components)
            return std::unexpected(ClassicalError::BadShape);
        std::vector<double> out = encoder.mean;
        for (std::size_t k = 0; k < encoder.components; ++k)
            for (std::size_t j = 0; j < encoder.columns; ++j)
                out[j] += code[k] * encoder.loadings[k * encoder.columns + j];
        return out;
    }

    [[nodiscard]] std::expected<double, ClassicalError>
    reconstruction_error(std::span<const double> row) const {
        const auto code = encode(row);
        if (!code) return std::unexpected(code.error());
        const auto reconstructed = decode(*code);
        if (!reconstructed) return std::unexpected(reconstructed.error());
        double loss = 0.0;
        for (std::size_t j = 0; j < row.size(); ++j) {
            const double e = row[j] - (*reconstructed)[j];
            loss += e * e;
        }
        return loss / static_cast<double>(row.size());
    }
};

struct Var1Model {
    std::size_t dimensions = 0;
    std::vector<double> coefficients; // row-major output x (1 + dimensions)

    [[nodiscard]] std::expected<std::vector<double>, ClassicalError>
    predict(std::span<const double> previous) const {
        if (previous.size() != dimensions) return std::unexpected(ClassicalError::BadShape);
        std::vector<double> out(dimensions, 0.0);
        for (std::size_t i = 0; i < dimensions; ++i) {
            out[i] = coefficients[i * (dimensions + 1)];
            for (std::size_t j = 0; j < dimensions; ++j)
                out[i] += coefficients[i * (dimensions + 1) + 1 + j] * previous[j];
        }
        return out;
    }
};

[[nodiscard]] inline std::expected<Var1Model, ClassicalError>
fit_var1(std::span<const double> values, std::size_t rows, std::size_t dimensions,
         double ridge = 1e-8) {
    if (rows < 4 || dimensions == 0 || rows > values.size() / dimensions
        || !(ridge >= 0.0) || !std::isfinite(ridge)) return std::unexpected(ClassicalError::BadShape);
    const std::size_t p = dimensions + 1;
    std::vector<double> gram(p * p, 0.0);
    std::vector<double> rhs(dimensions * p, 0.0);
    for (std::size_t t = 1; t < rows; ++t) {
        std::vector<double> z(p, 1.0);
        for (std::size_t j = 0; j < dimensions; ++j) z[j + 1] = values[(t - 1) * dimensions + j];
        for (double v : z) if (!std::isfinite(v)) return std::unexpected(ClassicalError::NonFinite);
        for (std::size_t a = 0; a < p; ++a) for (std::size_t b = 0; b < p; ++b) gram[a * p + b] += z[a] * z[b];
        for (std::size_t i = 0; i < dimensions; ++i) {
            const double target = values[t * dimensions + i];
            if (!std::isfinite(target)) return std::unexpected(ClassicalError::NonFinite);
            for (std::size_t a = 0; a < p; ++a) rhs[i * p + a] += z[a] * target;
        }
    }
    for (std::size_t i = 0; i < p; ++i) gram[i * p + i] += ridge;
    Var1Model out; out.dimensions = dimensions; out.coefficients.resize(dimensions * p);
    for (std::size_t i = 0; i < dimensions; ++i) {
        std::vector<double> a = gram, b(rhs.begin() + static_cast<std::ptrdiff_t>(i * p),
                                        rhs.begin() + static_cast<std::ptrdiff_t>((i + 1) * p));
        if (!classical_detail::solve(a, b, p)) return std::unexpected(ClassicalError::Singular);
        std::copy(b.begin(), b.end(), out.coefficients.begin() + static_cast<std::ptrdiff_t>(i * p));
    }
    return out;
}

} // namespace altair
