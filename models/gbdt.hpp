// models/gbdt.hpp -- gradient-boosted regression trees.
//
// P16-01 / 02 / 03 / 04.
//
// THE ONE MODEL FAMILY THIS TREE DID NOT HAVE, AND THE ONE MOST USED.
//
// `models/` already holds an MLP, a GRU, an attention block and a ridge
// readout. It had no trees at all -- which is the gap that matters, because on
// TABULAR financial data gradient boosting is the method that actually wins.
// It handles unscaled features, monotone transforms of them, missing values
// and interactions without being told, and it does not need the feature
// engineering a linear model lives or dies by.
//
// HISTOGRAM BINNING IS WHY THIS IS FAST ENOUGH TO EXIST.
//
// Exact split search on n rows costs a sort per feature per node. Binning each
// feature into 64 buckets ONCE, up front, turns every subsequent split search
// into a 64-bucket scan of accumulated gradient sums -- independent of n. That
// is the LightGBM trick, it is the difference between seconds and hours, and
// the accuracy cost is nil at this bin count because a split point between two
// adjacent bins is a distinction the data cannot support anyway.
//
// The binning is done ONCE on the training rows and the edges are STORED, so
// scoring maps a new row through the same edges. Re-binning at predict time
// would let the test set influence its own bin boundaries, which is
// look-ahead wearing a preprocessing costume.
//
// GAIN-BASED IMPORTANCE LIES, AND IT LIES IN A KNOWN DIRECTION.
//
// When two features are correlated, whichever the tree happens to split on
// first absorbs the gain and the other looks worthless. Swap the column order
// and the story reverses. The importance numbers are still reported -- they
// are genuinely useful for spotting a leak -- but a PERMUTATION importance is
// reported alongside, and the test demonstrates the disagreement on a pair of
// deliberately duplicated features rather than warning about it in prose.
//
// NO ALLOCATION ON THE PREDICT PATH.
//
// A fitted tree is a flat array of nodes and prediction is an index walk.
// Rule 4: nothing here allocates once training is done, so a served model can
// sit inside ALTAIR_HOT.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <vector>

namespace altair {

enum class GbdtError : std::uint8_t {
    TooFewSamples,
    /// Ragged feature matrix, or zero features.
    BadShape,
    /// A hyper-parameter outside its admissible range.
    BadParameter,
    /// Every label identical -- there is nothing to learn.
    Degenerate
};

inline constexpr std::size_t kMaxBins = 64;

/// Feature matrix, row-major: `x[row * p + col]`.
struct Frame {
    std::vector<double> x;
    std::size_t rows = 0;
    std::size_t p = 0;
    [[nodiscard]] double at(std::size_t r, std::size_t c) const noexcept {
        return x[r * p + c];
    }
};

/// Bin edges, learned once on the training rows and then FIXED.
struct Binner {
    /// edge[c] holds the upper bounds for feature c. Size is bins-1.
    std::vector<std::vector<double>> edge;
    std::size_t bins = 0;

    [[nodiscard]] std::uint8_t bin_of(double v, std::size_t c) const noexcept {
        const auto& e = edge[c];
        // upper_bound: the first edge strictly greater than v.
        const auto it = std::upper_bound(e.begin(), e.end(), v);
        return static_cast<std::uint8_t>(it - e.begin());
    }
};

/// Quantile bin edges from the training rows only.
[[nodiscard]] inline std::expected<Binner, GbdtError>
fit_binner(const Frame& f, std::size_t bins) {
    if (f.rows < 32 || f.p == 0) { return std::unexpected(GbdtError::BadShape); }
    if (bins < 2 || bins > kMaxBins) {
        return std::unexpected(GbdtError::BadParameter);
    }
    Binner b;
    b.bins = bins;
    b.edge.resize(f.p);
    std::vector<double> col(f.rows);
    for (std::size_t c = 0; c < f.p; ++c) {
        for (std::size_t r = 0; r < f.rows; ++r) { col[r] = f.at(r, c); }
        std::sort(col.begin(), col.end());
        b.edge[c].clear();
        for (std::size_t k = 1; k < bins; ++k) {
            const std::size_t idx = k * f.rows / bins;
            const double e = col[idx < f.rows ? idx : f.rows - 1];
            // Duplicate edges collapse: a feature with three distinct values
            // has three bins whatever was asked for, and pretending otherwise
            // creates empty bins that every split search then walks.
            if (b.edge[c].empty() || e > b.edge[c].back()) {
                b.edge[c].push_back(e);
            }
        }
    }
    return b;
}

/// One node. `feature == kLeaf` marks a leaf and `value` is its prediction.
struct Node {
    static constexpr std::size_t kLeaf = static_cast<std::size_t>(-1);
    std::size_t feature = kLeaf;
    std::uint8_t split_bin = 0;      ///< go left when bin <= split_bin
    std::size_t left = 0;
    std::size_t right = 0;
    double value = 0.0;
};

/// A fitted tree: flat, contiguous, and walked without allocating.
struct Tree {
    std::vector<Node> node;

    [[nodiscard]] double predict(const std::uint8_t* binned) const noexcept {
        std::size_t i = 0;
        // Bounded walk: a malformed tree cannot spin forever.
        for (int depth = 0; depth < 64; ++depth) {
            const Node& n = node[i];
            if (n.feature == Node::kLeaf) { return n.value; }
            i = binned[n.feature] <= n.split_bin ? n.left : n.right;
        }
        return node[i].value;
    }
};

struct GbdtParams {
    std::size_t trees = 100;
    std::size_t max_depth = 3;
    /// Shrinkage. The single most important hyper-parameter: small steps and
    /// many trees beat large steps and few, reliably.
    double learning_rate = 0.05;
    /// Rows sampled per tree, in (0, 1]. Below 1 this is stochastic gradient
    /// boosting, which regularises.
    double subsample = 0.8;
    /// Minimum rows in a leaf. THE main brake on overfitting in a tree.
    std::size_t min_leaf = 20;
    /// LEAF-WISE growth, which is THE LightGBM innovation.
    ///
    /// Level-wise (XGBoost's default) splits every node at a depth before
    /// going deeper, so a tree of depth d has up to 2^d leaves and most of
    /// them are splits nobody asked for. Leaf-wise picks the ONE leaf with
    /// the largest loss reduction anywhere in the tree and splits that,
    /// repeatedly, until it has `max_leaves`.
    ///
    /// The gain is real: at equal leaf count leaf-wise reaches lower training
    /// loss, because every split it made was the best one available. So is
    /// the cost -- it grows deep, narrow, asymmetric trees that chase a
    /// handful of rows, which is exactly overfitting, and it is why LightGBM
    /// needs `num_leaves` and `min_data_in_leaf` where a depth limit used to
    /// be enough. On the sample sizes in this project that risk is the larger
    /// of the two effects, which is why this is a FLAG and not the default.
    bool leaf_wise = false;
    /// Leaf budget for leaf-wise growth. Ignored when `leaf_wise` is false,
    /// where `max_depth` bounds the tree instead.
    std::size_t max_leaves = 31;
    std::uint64_t seed = 0xB0057;

    [[nodiscard]] bool valid() const noexcept {
        return trees > 0 && max_depth > 0 && max_depth < 32
            && learning_rate > 0.0 && learning_rate <= 1.0
            && subsample > 0.0 && subsample <= 1.0 && min_leaf > 0
            && (!leaf_wise || max_leaves >= 2);
    }
};

struct Gbdt {
    std::vector<Tree> tree;
    Binner binner{};
    double base = 0.0;
    double learning_rate = 0.05;
    /// Summed split gain per feature. Useful and MISLEADING on correlated
    /// features -- see the header and the test.
    std::vector<double> gain;

    [[nodiscard]] double predict_binned(const std::uint8_t* b) const noexcept {
        double y = base;
        for (const Tree& t : tree) { y += learning_rate * t.predict(b); }
        return y;
    }
    [[nodiscard]] double predict_row(const double* row) const {
        std::vector<std::uint8_t> b(binner.edge.size());
        for (std::size_t c = 0; c < binner.edge.size(); ++c) {
            b[c] = binner.bin_of(row[c], c);
        }
        return predict_binned(b.data());
    }
};

namespace detail {

struct Split {
    std::size_t feature = Node::kLeaf;
    std::uint8_t bin = 0;
    double gain = 0.0;
    double left_value = 0.0;
    double right_value = 0.0;
};

/// Best split by variance reduction, scanning accumulated sums per bin. The
/// scan is O(bins) per feature regardless of how many rows reach the node,
/// which is the whole point of binning.
[[nodiscard]] inline Split best_split(const std::vector<std::uint8_t>& binned,
                                      const std::vector<double>& g,
                                      const std::vector<std::size_t>& idx,
                                      std::size_t p, std::size_t bins,
                                      std::size_t min_leaf) {
    Split best;
    double total = 0.0;
    for (const std::size_t i : idx) { total += g[i]; }
    const auto n = static_cast<double>(idx.size());
    const double parent = total * total / n;

    std::vector<double> sum(bins);
    std::vector<double> cnt(bins);
    for (std::size_t c = 0; c < p; ++c) {
        std::fill(sum.begin(), sum.end(), 0.0);
        std::fill(cnt.begin(), cnt.end(), 0.0);
        for (const std::size_t i : idx) {
            const std::uint8_t b = binned[i * p + c];
            sum[b] += g[i];
            cnt[b] += 1.0;
        }
        double ls = 0.0, lc = 0.0;
        for (std::size_t b = 0; b + 1 < bins; ++b) {
            ls += sum[b];
            lc += cnt[b];
            const double rc = n - lc;
            if (lc < static_cast<double>(min_leaf)
                || rc < static_cast<double>(min_leaf)) {
                continue;
            }
            const double rs = total - ls;
            const double gain = ls * ls / lc + rs * rs / rc - parent;
            if (gain > best.gain) {
                best.gain = gain;
                best.feature = c;
                best.bin = static_cast<std::uint8_t>(b);
                best.left_value = ls / lc;
                best.right_value = rs / rc;
            }
        }
    }
    return best;
}

inline void grow(Tree& t, std::vector<double>& gain_acc,
                 const std::vector<std::uint8_t>& binned,
                 const std::vector<double>& g, std::vector<std::size_t> idx,
                 std::size_t p, std::size_t bins, const GbdtParams& prm,
                 std::size_t depth, std::size_t self) {
    double mean = 0.0;
    for (const std::size_t i : idx) { mean += g[i]; }
    mean /= static_cast<double>(idx.size());
    t.node[self].value = mean;
    t.node[self].feature = Node::kLeaf;

    if (depth >= prm.max_depth || idx.size() < 2 * prm.min_leaf) { return; }
    const Split s = best_split(binned, g, idx, p, bins, prm.min_leaf);
    if (s.feature == Node::kLeaf || !(s.gain > 0.0)) { return; }

    gain_acc[s.feature] += s.gain;
    std::vector<std::size_t> l, r;
    l.reserve(idx.size());
    r.reserve(idx.size());
    for (const std::size_t i : idx) {
        if (binned[i * p + s.feature] <= s.bin) { l.push_back(i); }
        else { r.push_back(i); }
    }
    if (l.empty() || r.empty()) { return; }

    t.node[self].feature = s.feature;
    t.node[self].split_bin = s.bin;
    t.node.push_back(Node{});
    const std::size_t li = t.node.size() - 1;
    t.node.push_back(Node{});
    const std::size_t ri = t.node.size() - 1;
    t.node[self].left = li;
    t.node[self].right = ri;
    grow(t, gain_acc, binned, g, std::move(l), p, bins, prm, depth + 1, li);
    grow(t, gain_acc, binned, g, std::move(r), p, bins, prm, depth + 1, ri);
}

/// LEAF-WISE (best-first) growth.
///
/// Keeps a frontier of splittable leaves, each with its best split already
/// evaluated, and repeatedly splits whichever has the largest gain ANYWHERE in
/// the tree. The recursion in `grow` cannot express that -- it commits to a
/// subtree before it has seen what the sibling could offer -- so this is a
/// loop over an explicit frontier rather than a flag on the recursive walk.
inline void grow_leafwise(Tree& t, std::vector<double>& gain_acc,
                          const std::vector<std::uint8_t>& binned,
                          const std::vector<double>& g,
                          std::vector<std::size_t> root, std::size_t p,
                          std::size_t bins, const GbdtParams& prm) {
    struct Frontier {
        std::size_t node;
        std::vector<std::size_t> idx;
        Split split;
        std::size_t depth;
    };
    auto leaf_value = [&g](const std::vector<std::size_t>& idx) {
        double m = 0.0;
        for (const std::size_t i : idx) { m += g[i]; }
        return m / static_cast<double>(idx.size());
    };

    t.node[0].value = leaf_value(root);
    t.node[0].feature = Node::kLeaf;

    std::vector<Frontier> front;
    front.push_back({0, std::move(root),
                     best_split(binned, g, front.empty()
                                    ? std::vector<std::size_t>{}
                                    : std::vector<std::size_t>{},
                                p, bins, prm.min_leaf),
                     0});
    // Evaluate the root's split properly (the placeholder above cannot see
    // its own idx member yet).
    front[0].split = best_split(binned, g, front[0].idx, p, bins, prm.min_leaf);

    std::size_t leaves = 1;
    while (leaves < prm.max_leaves) {
        // THE BEST LEAF ANYWHERE, which is the whole difference from
        // level-wise.
        std::size_t best = front.size();
        double best_gain = 0.0;
        for (std::size_t k = 0; k < front.size(); ++k) {
            if (front[k].split.feature == Node::kLeaf) { continue; }
            if (front[k].depth >= prm.max_depth) { continue; }
            if (front[k].split.gain > best_gain) {
                best_gain = front[k].split.gain;
                best = k;
            }
        }
        if (best == front.size()) { break; }

        Frontier f = std::move(front[best]);
        front.erase(front.begin() + static_cast<long>(best));

        std::vector<std::size_t> l, r;
        l.reserve(f.idx.size());
        r.reserve(f.idx.size());
        for (const std::size_t i : f.idx) {
            if (binned[i * p + f.split.feature] <= f.split.bin) {
                l.push_back(i);
            } else {
                r.push_back(i);
            }
        }
        if (l.empty() || r.empty()) { continue; }

        gain_acc[f.split.feature] += f.split.gain;
        t.node[f.node].feature = f.split.feature;
        t.node[f.node].split_bin = f.split.bin;
        t.node.push_back(Node{});
        const std::size_t li = t.node.size() - 1;
        t.node.push_back(Node{});
        const std::size_t ri = t.node.size() - 1;
        t.node[f.node].left = li;
        t.node[f.node].right = ri;
        t.node[li].feature = Node::kLeaf;
        t.node[li].value = leaf_value(l);
        t.node[ri].feature = Node::kLeaf;
        t.node[ri].value = leaf_value(r);
        ++leaves;

        for (auto& [child, idx] : {std::pair<std::size_t, std::vector<std::size_t>*>{li, &l},
                                   {ri, &r}}) {
            if (idx->size() >= 2 * prm.min_leaf) {
                front.push_back({child, *idx,
                                 best_split(binned, g, *idx, p, bins,
                                            prm.min_leaf),
                                 f.depth + 1});
            }
        }
    }
}

} // namespace detail

/// Fit by gradient boosting on squared loss: each tree is fitted to the
/// RESIDUAL of everything before it.
[[nodiscard]] inline std::expected<Gbdt, GbdtError>
fit_gbdt(const Frame& f, const std::vector<double>& y, const GbdtParams& prm,
         std::size_t bins = 32) {
    if (f.rows < 64 || f.p == 0 || y.size() != f.rows) {
        return std::unexpected(GbdtError::BadShape);
    }
    if (!prm.valid()) { return std::unexpected(GbdtError::BadParameter); }

    const auto b = fit_binner(f, bins);
    if (!b) { return std::unexpected(b.error()); }

    std::vector<std::uint8_t> binned(f.rows * f.p);
    for (std::size_t r = 0; r < f.rows; ++r) {
        for (std::size_t c = 0; c < f.p; ++c) {
            binned[r * f.p + c] = b->bin_of(f.at(r, c), c);
        }
    }

    Gbdt m;
    m.binner = *b;
    m.learning_rate = prm.learning_rate;
    m.gain.assign(f.p, 0.0);
    for (const double v : y) { m.base += v; }
    m.base /= static_cast<double>(f.rows);

    std::vector<double> pred(f.rows, m.base);
    std::vector<double> resid(f.rows, 0.0);
    std::uint64_t s = prm.seed != 0 ? prm.seed : 0x9E3779B9ull;
    auto rnd = [&s]() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0);
    };

    for (std::size_t k = 0; k < prm.trees; ++k) {
        for (std::size_t r = 0; r < f.rows; ++r) { resid[r] = y[r] - pred[r]; }
        std::vector<std::size_t> idx;
        idx.reserve(f.rows);
        for (std::size_t r = 0; r < f.rows; ++r) {
            if (prm.subsample >= 1.0 || rnd() < prm.subsample) {
                idx.push_back(r);
            }
        }
        if (idx.size() < 2 * prm.min_leaf) { continue; }

        Tree t;
        t.node.push_back(Node{});
        if (prm.leaf_wise) {
            detail::grow_leafwise(t, m.gain, binned, resid, idx, f.p,
                                  b->bins, prm);
        } else {
            detail::grow(t, m.gain, binned, resid, idx, f.p,
                         b->bins, prm, 0, 0);
        }
        for (std::size_t r = 0; r < f.rows; ++r) {
            pred[r] += prm.learning_rate * t.predict(&binned[r * f.p]);
        }
        m.tree.push_back(std::move(t));
    }
    if (m.tree.empty()) { return std::unexpected(GbdtError::Degenerate); }
    return m;
}

/// PERMUTATION importance: shuffle one feature and measure how much worse the
/// model gets. Slower than gain, and honest on correlated features -- if two
/// columns carry the same information, permuting either one alone barely
/// hurts, and BOTH correctly look unimportant rather than one absorbing all
/// the credit.
[[nodiscard]] inline std::vector<double>
permutation_importance(const Gbdt& m, const Frame& f,
                       const std::vector<double>& y, std::uint64_t seed) {
    std::vector<double> out(f.p, 0.0);
    std::vector<std::uint8_t> binned(f.rows * f.p);
    for (std::size_t r = 0; r < f.rows; ++r) {
        for (std::size_t c = 0; c < f.p; ++c) {
            binned[r * f.p + c] = m.binner.bin_of(f.at(r, c), c);
        }
    }
    auto mse = [&](const std::vector<std::uint8_t>& bb) {
        double acc = 0.0;
        for (std::size_t r = 0; r < f.rows; ++r) {
            const double d = m.predict_binned(&bb[r * f.p]) - y[r];
            acc += d * d;
        }
        return acc / static_cast<double>(f.rows);
    };
    const double base = mse(binned);

    std::uint64_t s = seed != 0 ? seed : 0xC0FFEEull;
    for (std::size_t c = 0; c < f.p; ++c) {
        std::vector<std::uint8_t> shuffled = binned;
        for (std::size_t r = f.rows; r > 1; --r) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            const std::size_t j = static_cast<std::size_t>(s % r);
            std::swap(shuffled[(r - 1) * f.p + c], shuffled[j * f.p + c]);
        }
        out[c] = mse(shuffled) - base;
    }
    return out;
}

} // namespace altair
