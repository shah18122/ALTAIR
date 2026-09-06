// models/regime_rl.hpp -- regime clustering, a Q-learner, and the ten-line
// heuristic that is expected to beat it.
//
// P18-01 / 02 / 03.
//
// THE CONTROL IS THE CARD.
//
// QUANTLAB trained a PPO agent and REJECTED it: median +0.260 against a
// ten-line heuristic's +0.524. That is not a failure of tuning, it is the
// normal outcome, and it is the reason P18-03 exists as its own card rather
// than as a paragraph in P18-02. An RL result reported without the heuristic
// it must beat is not a result.
//
// So `heuristic_policy` is written first, in this file, deliberately trivial,
// and every agent here is scored against it on identical episodes with
// identical seeds. If the learner wins, that is worth knowing. If it loses --
// which is what the prior says -- the card records a closed question rather
// than an open project.
//
// WHAT THE AGENT IS ALLOWED TO DECIDE.
//
// EXECUTION TIMING ONLY. Never direction, never size. That constraint is
// inherited from QUANTLAB's L3 design and it is not negotiable here for the
// same reason: a learner that can choose direction will find a direction in
// noise, and P8-15 and P8-16 already established there is no direction to find
// at these horizons. Timing is a genuinely different problem -- the quantity
// is fixed and only the schedule is free -- and it is the one place a policy
// can help without inventing alpha.
//
// Nothing in this file places an order. It produces a RECOMMENDATION; `oms/`
// is the only thing that can act on one, which is why this lives in models/
// and not there.
//
// REGIMES ARE RANKED BY A PRE-REGISTERED, RETURN-BLIND RULE.
//
// QUANTLAB's Phase 8 ranked seventeen regime configurations by a rule fixed
// BEFORE any returns were looked at, and that ordering is the only reason its
// winner means anything. Choosing a regime model by which one makes the
// backtest look best is choosing the model that overfits hardest. So
// `cluster_quality` here scores separation and stability -- both computable
// without a single forward return -- and the test fixes the rule before
// running it.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <vector>

namespace altair {

enum class RegimeMlError : std::uint8_t {
    TooFewSamples,
    BadParameter,
    /// Every observation identical -- no clustering is possible.
    Degenerate
};

// ---------------------------------------------------------------------------
// P18-01 — clustering
// ---------------------------------------------------------------------------

struct Clustering {
    /// Cluster index per observation.
    std::vector<std::size_t> label;
    /// Centre of each cluster, in feature space.
    std::vector<std::vector<double>> centre;
    /// Within-cluster sum of squares. Lower is tighter.
    double wcss = 0.0;
};

/// k-means with k-means++ seeding, on a fixed seed.
///
/// Deterministic from the seed, which rule 10 needs: a regime label that
/// changes between runs cannot appear in a decision record. Lloyd's algorithm
/// converges to a local optimum, so the seeding matters and ++ is used rather
/// than uniform -- uniform seeding on financial features routinely produces an
/// empty cluster and a k that silently became k-1.
[[nodiscard]] inline std::expected<Clustering, RegimeMlError>
kmeans(const std::vector<std::vector<double>>& x, std::size_t k,
       std::uint64_t seed, std::size_t iters = 100) {
    if (x.size() < k * 5 || k < 2) {
        return std::unexpected(RegimeMlError::TooFewSamples);
    }
    const std::size_t d = x[0].size();
    for (const auto& r : x) {
        if (r.size() != d) { return std::unexpected(RegimeMlError::BadParameter); }
    }
    std::uint64_t s = seed != 0 ? seed : 0x9E3779B9ull;
    auto rnd = [&s]() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0);
    };
    auto dist2 = [d](const std::vector<double>& a, const std::vector<double>& b) {
        double t = 0.0;
        for (std::size_t i = 0; i < d; ++i) {
            const double e = a[i] - b[i];
            t += e * e;
        }
        return t;
    };

    Clustering c;
    c.centre.push_back(x[static_cast<std::size_t>(rnd() * static_cast<double>(x.size()))
                         % x.size()]);
    while (c.centre.size() < k) {
        // k-means++: sample the next centre with probability proportional to
        // squared distance from the nearest existing one.
        std::vector<double> w(x.size(), 0.0);
        double tot = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            double best = 1e300;
            for (const auto& ce : c.centre) {
                best = std::min(best, dist2(x[i], ce));
            }
            w[i] = best;
            tot += best;
        }
        if (!(tot > 0.0)) { return std::unexpected(RegimeMlError::Degenerate); }
        double pick = rnd() * tot, acc = 0.0;
        std::size_t chosen = x.size() - 1;
        for (std::size_t i = 0; i < x.size(); ++i) {
            acc += w[i];
            if (acc >= pick) { chosen = i; break; }
        }
        c.centre.push_back(x[chosen]);
    }

    c.label.assign(x.size(), 0);
    for (std::size_t it = 0; it < iters; ++it) {
        bool moved = false;
        for (std::size_t i = 0; i < x.size(); ++i) {
            std::size_t best = 0;
            double bd = 1e300;
            for (std::size_t j = 0; j < k; ++j) {
                const double dd = dist2(x[i], c.centre[j]);
                if (dd < bd) { bd = dd; best = j; }
            }
            if (c.label[i] != best) { c.label[i] = best; moved = true; }
        }
        std::vector<std::vector<double>> sum(k, std::vector<double>(d, 0.0));
        std::vector<std::size_t> cnt(k, 0);
        for (std::size_t i = 0; i < x.size(); ++i) {
            for (std::size_t j = 0; j < d; ++j) { sum[c.label[i]][j] += x[i][j]; }
            ++cnt[c.label[i]];
        }
        for (std::size_t j = 0; j < k; ++j) {
            // AN EMPTY CLUSTER IS NOT SILENTLY DROPPED. It keeps its previous
            // centre, so k stays k -- the alternative is a run that reports
            // three regimes and found two.
            if (cnt[j] == 0) { continue; }
            for (std::size_t m = 0; m < d; ++m) {
                c.centre[j][m] = sum[j][m] / static_cast<double>(cnt[j]);
            }
        }
        if (!moved) { break; }
    }

    c.wcss = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        c.wcss += dist2(x[i], c.centre[c.label[i]]);
    }
    return c;
}

/// RETURN-BLIND quality score. Separation over spread, plus persistence.
///
/// Nothing here touches a forward return, which is the whole point: a regime
/// model chosen by which one flatters the backtest is the one that overfits
/// hardest. `persistence` is the fraction of adjacent observations sharing a
/// label -- a regime that flips every bar is a relabelling of noise, and no
/// separation score alone would catch it.
struct ClusterQuality {
    double separation = 0.0;    ///< between-centre distance / within spread
    double persistence = 0.0;   ///< fraction of adjacent pairs with one label
    [[nodiscard]] double score() const noexcept {
        return separation * persistence;
    }
};

[[nodiscard]] inline ClusterQuality
cluster_quality(const std::vector<std::vector<double>>& x,
                const Clustering& c) {
    ClusterQuality q;
    const std::size_t k = c.centre.size();
    if (k < 2 || x.empty()) { return q; }
    const std::size_t d = x[0].size();

    double between = 0.0;
    std::size_t pairs = 0;
    for (std::size_t i = 0; i < k; ++i) {
        for (std::size_t j = i + 1; j < k; ++j) {
            double t = 0.0;
            for (std::size_t m = 0; m < d; ++m) {
                const double e = c.centre[i][m] - c.centre[j][m];
                t += e * e;
            }
            between += std::sqrt(t);
            ++pairs;
        }
    }
    between /= static_cast<double>(pairs);
    const double within =
        std::sqrt(c.wcss / static_cast<double>(x.size()));
    q.separation = within > 0.0 ? between / within : 0.0;

    std::size_t same = 0;
    for (std::size_t i = 1; i < c.label.size(); ++i) {
        if (c.label[i] == c.label[i - 1]) { ++same; }
    }
    q.persistence = c.label.size() > 1
        ? static_cast<double>(same) / static_cast<double>(c.label.size() - 1)
        : 0.0;
    return q;
}

// ---------------------------------------------------------------------------
// P18-02 / P18-03 — the execution-timing problem, an agent, and its control
// ---------------------------------------------------------------------------

/// One episode: a price path and a quantity to work through it.
///
/// The agent sees a discretised state (how much time is left, how the price
/// has moved) and chooses how aggressive to be. Reward is NEGATIVE cost, so
/// higher is better throughout and no sign flips anywhere.
struct Episode {
    std::vector<double> price;      ///< paise
    std::size_t slices = 0;
};

/// THE CONTROL. Ten lines, no training, no state.
///
/// Work the remaining quantity evenly, but lean in when the price is below the
/// episode's running average -- buy more when it is cheap. That is the entire
/// policy, it has one parameter, and it is what a competent human does without
/// thinking about it. Any learner has to beat this before it is worth a
/// dependency, let alone a retraining schedule.
[[nodiscard]] inline double heuristic_action(double price, double running_avg,
                                             double lean = 0.5) noexcept {
    if (!(running_avg > 0.0)) { return 1.0; }
    const double rel = (running_avg - price) / running_avg;
    const double a = 1.0 + lean * rel * 100.0;
    return a < 0.2 ? 0.2 : (a > 3.0 ? 3.0 : a);
}

/// Tabular Q-learning over a small discrete state-action space.
///
/// Tabular rather than deep, deliberately. The state here is two-dimensional
/// and coarse; a neural policy would add a function approximator, a second set
/// of hyper-parameters and a convergence question, to solve a problem a table
/// covers exactly. QUANTLAB's PPO lost to a heuristic -- adding capacity was
/// not what was missing.
class QLearner {
public:
    static constexpr std::size_t kTimeBuckets = 5;
    static constexpr std::size_t kPriceBuckets = 5;
    static constexpr std::size_t kActions = 5;

    /// Action a maps to an aggression multiplier in [0.2, 3.0].
    [[nodiscard]] static double aggression(std::size_t a) noexcept {
        static constexpr double m[kActions] = {0.2, 0.6, 1.0, 1.8, 3.0};
        return m[a < kActions ? a : kActions - 1];
    }

    [[nodiscard]] static std::size_t state_of(double frac_time_left,
                                              double rel_price) noexcept {
        auto b = [](double v, std::size_t n) {
            const auto i = static_cast<std::size_t>(v * static_cast<double>(n));
            return i < n ? i : n - 1;
        };
        const std::size_t t = b(frac_time_left < 0.0 ? 0.0
                                : (frac_time_left > 1.0 ? 1.0 : frac_time_left),
                                kTimeBuckets);
        // rel_price is centred on 0 and clipped to +/- 2%.
        const double p = (rel_price + 0.02) / 0.04;
        const std::size_t r = b(p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p),
                                kPriceBuckets);
        return t * kPriceBuckets + r;
    }

    QLearner() { q_.assign(kTimeBuckets * kPriceBuckets * kActions, 0.0); }

    [[nodiscard]] std::size_t greedy(std::size_t s) const noexcept {
        std::size_t best = 0;
        double bv = -1e300;
        for (std::size_t a = 0; a < kActions; ++a) {
            const double v = q_[s * kActions + a];
            if (v > bv) { bv = v; best = a; }
        }
        return best;
    }

    void learn(std::size_t s, std::size_t a, double reward, std::size_t s2,
               double alpha, double gamma) noexcept {
        double best = -1e300;
        for (std::size_t j = 0; j < kActions; ++j) {
            best = std::max(best, q_[s2 * kActions + j]);
        }
        double& cell = q_[s * kActions + a];
        cell += alpha * (reward + gamma * best - cell);
    }

private:
    std::vector<double> q_;
};

} // namespace altair
