// models/markov.hpp -- a discrete Markov regime chain, and the test that asks
// whether it is a chain at all.
//
// P8-13. Added to the plan at Smit's request; it earns its place because it is
// the ONE model in this tree that the available data can actually train. The
// neural tier needs intraday ticks that do not exist. A first-order Markov
// chain over daily return states needs a few thousand daily bars, and there
// are 8,756 of them.
//
// THE STATE BOUNDARIES ARE THE LOOK-AHEAD, AND THEY ARE INVISIBLE.
//
// This is the card. Binning returns into states -- "strong down / down / flat
// / up / strong up" -- needs cut points, and the obvious way to get them is
// the quantiles of the return series. Of the WHOLE return series.
//
// That is look-ahead, and it hides in a line that reads like data preparation
// rather than like modelling. The boundary that says "this day was a strong
// down day" was computed from days that had not happened yet. Every state
// label in the training set is contaminated, the transition matrix is fitted
// on contaminated labels, and nothing in the fit reports a problem.
//
// So `fit_expanding` recomputes the cut points from the PAST ONLY at each
// step, and `fit_full_sample` exists to be measured against it. On the real
// daily NIFTY series the two disagree about the state of a measurable
// fraction of days -- and the disagreement is concentrated at the boundaries,
// which is where every regime call is made.
//
// A TRANSITION PROBABILITY FROM SIX OBSERVATIONS IS NOT A PROBABILITY.
//
// A five-state chain has twenty-five cells. Fit it on a thousand days and the
// corner cells -- strong-down to strong-up -- collect a handful of
// observations each, and the fit dutifully reports 0.167 for a cell with one
// observation in six. `TransitionMatrix` therefore carries the COUNT beside
// every probability, and `min_count()` reports the thinnest cell, so a number
// nobody should size on is visible as one.
//
// AND THE REAL QUESTION IS WHETHER IT IS MARKOV AT ALL.
//
// A fitted transition matrix always looks like something. The question that
// decides whether it is worth anything is whether the next state depends on
// the current one -- or whether the rows are all just the unconditional
// distribution with sampling noise on top.
//
// `independence_chi_square` answers that, and the honest expectation for daily
// equity returns is that it mostly does not reject. A model that cannot beat
// "tomorrow is drawn from the unconditional distribution" is not a model, and
// this file is built so that answer is reported rather than buried.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

namespace altair {

/// How many regime states the chain has. Odd, so there is a middle.
inline constexpr std::size_t kMaxStates = 9;

enum class MarkovError : std::uint8_t {
    /// Fewer observations than the fit needs.
    TooFewSamples,
    /// A state count that is not usable: 0, 1, or beyond kMaxStates.
    BadStateCount,
    /// The cut points were not supplied and could not be derived.
    NoBoundaries,
    /// Every observation landed in one state, so there is nothing to model.
    Degenerate
};

/// The cut points that turn a return into a state.
///
/// `edges` has `states - 1` entries, ascending. A return below edges[0] is
/// state 0; at or above edges[n-1] is state n.
struct StateBoundaries {
    double edges[kMaxStates - 1] = {};
    std::size_t states = 0;

    [[nodiscard]] std::size_t classify(double r) const noexcept {
        std::size_t s = 0;
        while (s + 1 < states && r >= edges[s]) {
            ++s;
        }
        return s;
    }
};

/// Quantile cut points from a sample.
///
/// PRECONDITION: `values` may be reordered. The caller passes a copy when it
/// still needs the original, which is deliberate -- sorting in place is the
/// cheap path and hiding a copy inside would make an O(n log n) call look
/// free at every step of an expanding fit.
[[nodiscard]] inline std::expected<StateBoundaries, MarkovError>
quantile_boundaries(std::vector<double>& values, std::size_t states) {
    if (states < 2 || states > kMaxStates) {
        return std::unexpected(MarkovError::BadStateCount);
    }
    if (values.size() < states * 2) {
        return std::unexpected(MarkovError::TooFewSamples);
    }
    std::sort(values.begin(), values.end());
    StateBoundaries b{};
    b.states = states;
    for (std::size_t i = 1; i < states; ++i) {
        const double q = static_cast<double>(i) / static_cast<double>(states);
        const auto idx = static_cast<std::size_t>(
            q * static_cast<double>(values.size() - 1));
        b.edges[i - 1] = values[idx];
    }
    // A degenerate series -- a long run of identical values -- produces equal
    // edges and therefore states nothing can land in. Refused rather than
    // returning a chain whose rows are all zero.
    for (std::size_t i = 1; i + 1 < states; ++i) {
        if (!(b.edges[i] > b.edges[i - 1])) {
            return std::unexpected(MarkovError::Degenerate);
        }
    }
    return b;
}

/// A fitted chain. Probabilities and the counts they came from, together.
struct TransitionMatrix {
    std::size_t states = 0;
    /// p[from][to]. Rows sum to 1, except a row with no observations, which
    /// is all zero -- and is distinguishable from a uniform row precisely
    /// because the counts are here too.
    double p[kMaxStates][kMaxStates] = {};
    std::uint32_t n[kMaxStates][kMaxStates] = {};
    std::uint32_t row_total[kMaxStates] = {};
    std::size_t transitions = 0;

    /// The thinnest cell that has any observations at all. A chain whose
    /// smallest populated cell holds six transitions is not a chain anyone
    /// should size a position on.
    [[nodiscard]] std::uint32_t min_count() const noexcept {
        std::uint32_t m = 0;
        bool seen = false;
        for (std::size_t i = 0; i < states; ++i) {
            for (std::size_t j = 0; j < states; ++j) {
                if (n[i][j] == 0) {
                    continue;
                }
                if (!seen || n[i][j] < m) {
                    m = n[i][j];
                    seen = true;
                }
            }
        }
        return seen ? m : 0;
    }

    /// Cells with no observations at all. Each one is a transition the chain
    /// says is impossible on the strength of never having seen it.
    [[nodiscard]] std::size_t empty_cells() const noexcept {
        std::size_t e = 0;
        for (std::size_t i = 0; i < states; ++i) {
            for (std::size_t j = 0; j < states; ++j) {
                if (n[i][j] == 0) ++e;
            }
        }
        return e;
    }

    /// The unconditional distribution of the NEXT state, ignoring the current
    /// one. What the chain has to beat to be worth anything.
    [[nodiscard]] std::vector<double> unconditional() const {
        std::vector<double> u(states, 0.0);
        double total = 0.0;
        for (std::size_t i = 0; i < states; ++i) {
            for (std::size_t j = 0; j < states; ++j) {
                u[j] += n[i][j];
                total += n[i][j];
            }
        }
        if (total > 0.0) {
            for (double& x : u) {
                x /= total;
            }
        }
        return u;
    }
};

/// Count transitions between already-classified states.
[[nodiscard]] inline std::expected<TransitionMatrix, MarkovError>
fit_from_states(const std::vector<std::size_t>& states_seq,
                std::size_t states) {
    if (states < 2 || states > kMaxStates) {
        return std::unexpected(MarkovError::BadStateCount);
    }
    if (states_seq.size() < 2) {
        return std::unexpected(MarkovError::TooFewSamples);
    }
    TransitionMatrix m{};
    m.states = states;
    for (std::size_t k = 0; k + 1 < states_seq.size(); ++k) {
        const std::size_t a = states_seq[k];
        const std::size_t b = states_seq[k + 1];
        if (a >= states || b >= states) {
            continue;
        }
        ++m.n[a][b];
        ++m.row_total[a];
        ++m.transitions;
    }
    for (std::size_t i = 0; i < states; ++i) {
        if (m.row_total[i] == 0) {
            continue;   // left as all-zero, NOT as uniform
        }
        for (std::size_t j = 0; j < states; ++j) {
            m.p[i][j] = static_cast<double>(m.n[i][j])
                      / static_cast<double>(m.row_total[i]);
        }
    }
    return m;
}

/// Fit using boundaries from the WHOLE sample.
///
/// Present so the look-ahead can be measured, and used nowhere else. The cut
/// points come from returns that had not happened at the moment each state was
/// assigned.
[[nodiscard]] inline std::expected<TransitionMatrix, MarkovError>
fit_full_sample(const std::vector<double>& returns, std::size_t states) {
    std::vector<double> copy = returns;
    const auto b = quantile_boundaries(copy, states);
    if (!b) {
        return std::unexpected(b.error());
    }
    std::vector<std::size_t> seq;
    seq.reserve(returns.size());
    for (double r : returns) {
        seq.push_back(b->classify(r));
    }
    return fit_from_states(seq, states);
}

/// The state sequence an EXPANDING fit produces.
///
/// At each step the cut points come from returns strictly BEFORE that step.
/// `warmup` is how many observations must accumulate before any state is
/// assigned; below it there is no answer, and the sequence starts later rather
/// than being filled with a guess.
///
/// THE OBVIOUS IMPLEMENTATION IS QUADRATIC AND IT SHOWS UP AS A FROZEN WINDOW.
///
/// The first version copied the whole past and sorted it at every step:
/// O(n^2 log n), which on 8,255 steps over 8,755 returns is about 9x10^8
/// operations with an allocation per step. Roughly two to three seconds. That
/// is tolerable in a test and is not tolerable on a UI thread, where it
/// arrived as a window that did not paint when a row was selected.
///
/// This keeps ONE sorted vector and inserts each new return into position, so
/// the cost is a memmove per step rather than a sort: O(n^2) moves total, and
/// no per-step allocation. The quantile lookup is then a direct index.
///
/// MEASURED: the desktop fits test went from 7.3 s to 0.08 s. That is 91x, and
/// more than the ~26x the move count alone predicts -- the allocation per step
/// was costing more than the sort was.
///
/// The RESULT IS IDENTICAL -- same boundaries, same labels, same chi-square,
/// same look-ahead percentage. The tests are what say so.
[[nodiscard]] inline std::expected<std::vector<std::size_t>, MarkovError>
expanding_states(const std::vector<double>& returns, std::size_t states,
                 std::size_t warmup) {
    if (states < 2 || states > kMaxStates) {
        return std::unexpected(MarkovError::BadStateCount);
    }
    if (returns.size() <= warmup || warmup < states * 2) {
        return std::unexpected(MarkovError::TooFewSamples);
    }
    std::vector<std::size_t> seq;
    seq.reserve(returns.size() - warmup);

    // The past, kept sorted. Reserved once: a reallocation mid-loop would put
    // the per-step allocation straight back.
    std::vector<double> sorted;
    sorted.reserve(returns.size());
    for (std::size_t i = 0; i < warmup; ++i) {
        sorted.push_back(returns[i]);
    }
    std::sort(sorted.begin(), sorted.end());

    StateBoundaries b{};
    b.states = states;

    for (std::size_t i = warmup; i < returns.size(); ++i) {
        // Boundaries from the past ONLY -- `sorted` does not yet contain
        // returns[i], and that ordering is the whole point of the function.
        bool degenerate = false;
        for (std::size_t k = 1; k < states; ++k) {
            const double q =
                static_cast<double>(k) / static_cast<double>(states);
            const auto idx = static_cast<std::size_t>(
                q * static_cast<double>(sorted.size() - 1));
            b.edges[k - 1] = sorted[idx];
        }
        for (std::size_t k = 1; k + 1 < states; ++k) {
            if (!(b.edges[k] > b.edges[k - 1])) {
                degenerate = true;
                break;
            }
        }
        if (degenerate) {
            return std::unexpected(MarkovError::Degenerate);
        }

        seq.push_back(b.classify(returns[i]));

        // Now admit it to the past, in order.
        const auto at =
            std::lower_bound(sorted.begin(), sorted.end(), returns[i]);
        sorted.insert(at, returns[i]);
    }
    return seq;
}

[[nodiscard]] inline std::expected<TransitionMatrix, MarkovError>
fit_expanding(const std::vector<double>& returns, std::size_t states,
              std::size_t warmup) {
    const auto seq = expanding_states(returns, states, warmup);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    return fit_from_states(*seq, states);
}

// ---------------------------------------------------------------------------
// Is it a chain at all?
// ---------------------------------------------------------------------------

struct IndependenceTest {
    double chi_square = 0.0;
    std::size_t degrees_of_freedom = 0;
    /// Cells whose EXPECTED count is below five, the usual rule of thumb for
    /// when a chi-square statistic stops meaning what it claims.
    ///
    /// THEY ARE COUNTED AND THEY ARE ALSO INCLUDED IN THE STATISTIC. That is
    /// the standard treatment and it is the right one -- dropping cells
    /// changes the degrees of freedom and quietly makes the test something
    /// else -- but an earlier version of this comment said "counted and
    /// reported rather than quietly included", which reads as though they
    /// were excluded. They are not.
    ///
    /// So this number is a CAVEAT ON `rejects_independence`, not a diagnostic
    /// beside it: a rejection carrying many thin cells is a rejection the
    /// approximation does not support.
    std::size_t thin_cells = 0;
    /// Critical value at 5% for `degrees_of_freedom`, or 0 when the table
    /// does not cover it.
    double critical_5pct = 0.0;
    /// True when the chain differs from the unconditional distribution by
    /// more than sampling noise.
    bool rejects_independence = false;
};

/// Chi-square critical values at 5%, indexed by degrees of freedom.
///
/// P33-03: THE TABLE STOPPED AT df 63 AND A 9-STATE CHAIN NEEDS 64.
///
/// A k-state chain tests independence with (k-1)^2 degrees of freedom, and
/// kMaxStates is 9 -- so the largest chain this file will build lands exactly
/// one past the end. `chi2_critical_5pct` returned 0.0, `rejects_independence`
/// requires a positive critical value, and the result was that a nine-state
/// chain could NEVER reject independence. Silently: the statistic was computed
/// correctly, the struct reported it, and the one boolean anybody reads said
/// "the chain is no better than the unconditional distribution" regardless of
/// what the data said.
///
/// That is the worse direction of the two. A test that always says "no
/// structure" on a model whose whole purpose is to find structure looks
/// exactly like an honest negative result, and this tree is full of honest
/// negative results.
///
/// Extended to df 87, and every entry VERIFIED against a computed inverse
/// regularised incomplete gamma rather than copied from a printed table --
/// P23-08's `ks_critical` was wrong by a factor of 27 because a critical value
/// was taken on trust.
///
/// A lookup past the end still returns 0.0 and is still reported as
/// unavailable rather than extrapolated.
[[nodiscard]] inline double chi2_critical_5pct(std::size_t df) noexcept {
    static constexpr double kTable[] = {
        0.0,    3.841,  5.991,  7.815,  9.488, 11.070, 12.592, 14.067,
        15.507, 16.919, 18.307, 19.675, 21.026, 22.362, 23.685, 24.996,
        26.296, 27.587, 28.869, 30.144, 31.410, 32.671, 33.924, 35.172,
        36.415, 37.652, 38.885, 40.113, 41.337, 42.557, 43.773, 44.985,
        46.194, 47.400, 48.602, 49.802, 50.998, 52.192, 53.384, 54.572,
        55.758, 56.942, 58.124, 59.304, 60.481, 61.656, 62.830, 64.001,
        65.171, 66.339, 67.505, 68.669, 69.832, 70.993, 72.153, 73.311,
        74.468, 75.624, 76.778, 77.931, 79.082, 80.232, 81.381, 82.529,
        // df 64 is a NINE-state chain -- the largest kMaxStates allows.
        83.675, 84.821, 85.965, 87.108, 88.250, 89.391, 90.531, 91.670,
        92.808, 93.945, 95.081, 96.217, 97.351, 98.484, 99.617, 100.749,
        101.879, 103.010, 104.139, 105.267, 106.395, 107.522, 108.648, 109.773
    };
    constexpr std::size_t kN = sizeof(kTable) / sizeof(kTable[0]);
    return df < kN ? kTable[df] : 0.0;
}

/// Test the fitted chain against "the next state is drawn from the
/// unconditional distribution".
///
/// THE QUESTION THAT DECIDES WHETHER THE MODEL IS WORTH ANYTHING. A fitted
/// transition matrix always looks like structure; this asks whether the rows
/// actually differ from one another by more than sampling noise.
[[nodiscard]] inline IndependenceTest
independence_chi_square(const TransitionMatrix& m) {
    IndependenceTest t{};
    const std::vector<double> u = m.unconditional();
    double total = 0.0;
    for (std::size_t i = 0; i < m.states; ++i) {
        total += m.row_total[i];
    }
    if (!(total > 0.0)) {
        return t;
    }
    for (std::size_t i = 0; i < m.states; ++i) {
        for (std::size_t j = 0; j < m.states; ++j) {
            const double expected = m.row_total[i] * u[j];
            if (!(expected > 0.0)) {
                continue;
            }
            if (expected < 5.0) {
                ++t.thin_cells;
            }
            const double diff = m.n[i][j] - expected;
            t.chi_square += diff * diff / expected;
        }
    }
    const std::size_t k = m.states;
    t.degrees_of_freedom = (k - 1) * (k - 1);
    t.critical_5pct = chi2_critical_5pct(t.degrees_of_freedom);
    t.rejects_independence =
        t.critical_5pct > 0.0 && t.chi_square > t.critical_5pct;
    return t;
}

/// Log returns from a price series, in the order given.
///
/// Log rather than simple, because they add across periods -- which is what
/// makes a regime defined on them the same regime at any horizon.
[[nodiscard]] inline std::vector<double>
log_returns(const std::vector<double>& prices) {
    std::vector<double> r;
    if (prices.size() < 2) {
        return r;
    }
    r.reserve(prices.size() - 1);
    for (std::size_t i = 1; i < prices.size(); ++i) {
        if (prices[i - 1] > 0.0 && prices[i] > 0.0) {
            r.push_back(std::log(prices[i] / prices[i - 1]));
        }
    }
    return r;
}

/// Stationary distribution by power iteration. Converges for any chain whose
/// rows are populated; returns the last iterate otherwise, which a caller can
/// spot because it will not sum to one.
[[nodiscard]] inline std::vector<double>
stationary(const TransitionMatrix& m, int iterations = 500) {
    std::vector<double> v(m.states, 1.0 / static_cast<double>(m.states));
    std::vector<double> next(m.states, 0.0);
    for (int it = 0; it < iterations; ++it) {
        std::fill(next.begin(), next.end(), 0.0);
        for (std::size_t i = 0; i < m.states; ++i) {
            for (std::size_t j = 0; j < m.states; ++j) {
                next[j] += v[i] * m.p[i][j];
            }
        }
        v = next;
    }
    return v;
}

} // namespace altair
