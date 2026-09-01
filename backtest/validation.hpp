// backtest/validation.hpp -- walk-forward, purged K-fold, and the embargo.
//
// P6-06.
//
// RANDOM K-FOLD IS BANNED IN THIS TREE (CLAUDE.md), AND THIS FILE IS WHY.
//
// The reason usually given is "markets are not ergodic", which is true and is
// the reason walk-forward exists. But the sharper reason -- the one that turns
// a plausible backtest into a fictional one -- is that FINANCIAL LABELS
// OVERLAP.
//
// A label is normally a forward return: the label at bar t is what happens
// over [t, t+h). So the labels at t and t+1 share h-1 bars of the same future.
// Put t in the training set and t+1 in the test set and the model has been
// trained on most of the answer to a question it is about to be graded on.
// Nothing is copied, no index is off by one, and no amount of staring at the
// split code reveals it -- the leak is in the label definition, not the split.
//
// Random K-fold guarantees this happens, because it scatters temporal
// neighbours across folds by design. The fix is two steps, and both are needed:
//
//   PURGE. Drop every training sample whose label window overlaps the test
//   set's label windows. That removes the direct sharing.
//
//   EMBARGO. Additionally drop training samples that start shortly AFTER the
//   test set ends. Their labels do not overlap, but features are built from
//   rolling windows, so a sample just after the test block computes its
//   features partly from bars inside it. Purging alone leaves that path open,
//   and it is the one people forget.
//
// WALK-FORWARD IS THE DEFAULT AND THE ONLY ONE THAT ANSWERS THE QUESTION.
//
// Even a perfectly purged K-fold trains on data from after the test period,
// which answers "could this pattern have been found?" rather than "could it
// have been traded?". Only walk-forward answers the second, and the second is
// the one capital is deployed on. Purged K-fold is here for model selection
// and diagnosis, not for the number that goes in a report.
//
// HOW MUCH IT MATTERS DEPENDS ENTIRELY ON WHETHER THE FOLDS ARE CONTIGUOUS.
//
// Measured on 10,000 samples with a 20-bar forward label and a 20-bar feature
// lookback:
//
//   contiguous 5-fold    156 of 40,000 training samples contaminated (0.39%)
//   SHUFFLED 5-fold      8,000 of 8,000 contaminated (100%)
//
// A contiguous block touches the training set only at its two edges, so
// purging removes a boundary layer and little else. Shuffling interleaves the
// two throughout, and every training sample lands within a label horizon of
// some test sample. These are not the same mistake by different amounts.
//
// And purging a shuffled split honestly leaves ZERO usable training samples
// out of 8,000. That is the proof the split was never valid: there is no
// version of it that both removes the leak and keeps enough data to fit
// anything.
//
// WHAT THE LEAK IS WORTH, MEASURED.
//
// A 1-nearest-neighbour predictor on data whose feature and label share no bar
// -- the true relationship is exactly zero:
//
//   SHUFFLED K-fold        +0.1656 out-of-sample correlation
//   purged contiguous      +0.0041
//
// Skill that does not exist, from a scheme that looks rigorous. The mechanism:
// 1-NN's nearest neighbour in feature space is usually a TEMPORAL neighbour,
// because features are rolling windows and move slowly; a temporal neighbour's
// label overlaps the test label by up to 19 of its 20 bars. The model is not
// predicting, it is reading the answer off a sample that shares it.
//
// The predictor is 1-NN deliberately. A linear model averages the leak away,
// which is why a contaminated backtest passes a linear sanity check and dies
// in production on a tree. And the feature is EIGHT-DIMENSIONAL for the same
// reason: in one dimension the nearest neighbour among 5,000 candidates is a
// value neighbour nowhere near in time and no leak appears at all -- an early
// draft of the test measured exactly that and correctly found nothing.

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class ValidationError : std::uint8_t {
    /// Fewer samples than the split needs.
    TooFewSamples,
    /// A fold count or window length of zero.
    BadPartition,
    /// The embargo was not specified. There is no default.
    NoEmbargo,
    /// A label window was inverted or empty.
    BadLabelWindow,
    /// The requested fold does not exist.
    NoSuchFold
};

/// One sample's LABEL WINDOW, in bar indices, half-open [start, end).
///
/// Every sample carries the span of future its label depends on. This is the
/// object purging works on, and a harness that does not have it cannot purge:
/// you cannot remove overlapping labels if you do not know what each label
/// covers.
struct LabelWindow {
    std::size_t start = 0;
    std::size_t end = 0;        // exclusive
    [[nodiscard]] bool valid() const noexcept { return end > start; }
    [[nodiscard]] bool overlaps(const LabelWindow& o) const noexcept {
        return start < o.end && o.start < end;
    }
};

/// A contiguous block of sample indices, half-open.
struct Block {
    std::size_t start = 0;
    std::size_t end = 0;
    [[nodiscard]] std::size_t size() const noexcept {
        return end > start ? end - start : 0;
    }
    [[nodiscard]] bool contains(std::size_t i) const noexcept {
        return i >= start && i < end;
    }
};

// ---------------------------------------------------------------------------
// Walk-forward
// ---------------------------------------------------------------------------

/// A walk-forward schedule: train on the past, test on what comes next.
///
/// `expanding` keeps every earlier bar in the training set; otherwise the
/// window rolls and old data falls out. Both are defensible and they answer
/// different questions -- expanding assumes the relationship is stable, rolling
/// assumes it decays -- so there is no default.
struct WalkForwardSpec {
    std::size_t initial_train = 0;
    std::size_t test_len = 0;
    /// How far the window advances between folds. Equal to `test_len` gives
    /// non-overlapping test blocks, which is what a report wants.
    std::size_t step = 0;
    /// Bars between the end of training and the start of testing. NOT
    /// optional: with an h-bar label, the last h training labels reach into
    /// the test block, so a gap of at least h is required for the split to
    /// mean anything.
    std::size_t gap = 0;
    bool expanding = true;
};

struct WalkForwardFold {
    Block train{};
    Block test{};
    std::size_t index = 0;
};

/// Enumerate walk-forward folds over `n` samples.
///
/// Every fold's test block starts strictly after its training block ends, plus
/// the gap. That ordering is the whole guarantee, and it is checked rather
/// than assumed.
[[nodiscard]] inline std::expected<std::size_t, ValidationError>
walk_forward_count(std::size_t n, const WalkForwardSpec& s) noexcept {
    if (s.initial_train == 0 || s.test_len == 0 || s.step == 0) {
        return std::unexpected(ValidationError::BadPartition);
    }
    const std::size_t first_test = s.initial_train + s.gap;
    if (n <= first_test + s.test_len) {
        return std::unexpected(ValidationError::TooFewSamples);
    }
    std::size_t k = 0;
    for (std::size_t start = first_test; start + s.test_len <= n;
         start += s.step) {
        ++k;
    }
    return k;
}

[[nodiscard]] inline std::expected<WalkForwardFold, ValidationError>
walk_forward_fold(std::size_t n, const WalkForwardSpec& s,
                  std::size_t i) noexcept {
    const auto k = walk_forward_count(n, s);
    if (!k) { return std::unexpected(k.error()); }
    if (i >= *k) { return std::unexpected(ValidationError::NoSuchFold); }

    WalkForwardFold f{};
    f.index = i;
    f.test.start = s.initial_train + s.gap + i * s.step;
    f.test.end = f.test.start + s.test_len;
    // Training ends `gap` bars before testing begins, always.
    f.train.end = f.test.start - s.gap;
    f.train.start = s.expanding ? 0
                                : (f.train.end > s.initial_train
                                       ? f.train.end - s.initial_train : 0);
    return f;
}

// ---------------------------------------------------------------------------
// Purged K-fold with embargo
// ---------------------------------------------------------------------------

struct PurgedKFoldSpec {
    std::size_t folds = 0;
    /// Bars of embargo AFTER each test block. No default: it must cover the
    /// longest feature lookback in the set, and this file does not know what
    /// that is. P5-01's `warmup()` does.
    std::size_t embargo = 0;
    /// Set true only if the caller has genuinely decided a zero embargo is
    /// right -- which it is when no feature has any lookback at all, and
    /// essentially never otherwise.
    bool embargo_deliberately_zero = false;
};

/// Whether training sample `j` survives purging and embargo for a given test
/// block.
///
/// Three ways to be excluded:
///   - it is IN the test block;
///   - its label window overlaps any test sample's label window (PURGE);
///   - it begins inside the embargo period after the test block (EMBARGO).
[[nodiscard]] inline bool
survives_purge(std::size_t j, const Block& test, const LabelWindow* windows,
               std::size_t n, std::size_t embargo) noexcept {
    if (test.contains(j)) { return false; }
    if (j >= n) { return false; }

    // PURGE. The test block's label windows span from the earliest start to
    // the latest end among its samples; a training label overlapping that span
    // shares future with the test set.
    LabelWindow span{windows[test.start].start, windows[test.start].end};
    for (std::size_t i = test.start + 1; i < test.end && i < n; ++i) {
        if (windows[i].start < span.start) { span.start = windows[i].start; }
        if (windows[i].end > span.end) { span.end = windows[i].end; }
    }
    if (windows[j].overlaps(span)) { return false; }

    // EMBARGO. A sample beginning just after the test block computes its
    // FEATURES from bars inside it, even though its label does not overlap.
    // Purging alone leaves this path open.
    if (embargo > 0 && j >= test.end && j < test.end + embargo) {
        return false;
    }
    return true;
}

/// The same question for a SCATTERED test set -- the shuffled K-fold that
/// CLAUDE.md bans.
///
/// A contiguous test block only touches the training set at its two edges, so
/// purging there removes a boundary layer. A shuffled split interleaves test
/// and training samples throughout, so nearly every training sample sits
/// within a label horizon of some test sample. The two are not the same
/// mistake by different amounts; they are different mistakes, and the test
/// measures both.
[[nodiscard]] inline bool
survives_purge_set(std::size_t j, const std::size_t* test_idx, std::size_t k,
                   const LabelWindow* windows, std::size_t n,
                   std::size_t embargo) noexcept {
    if (j >= n) { return false; }
    for (std::size_t i = 0; i < k; ++i) {
        const std::size_t t = test_idx[i];
        if (t == j) { return false; }               // it IS a test sample
        if (t >= n) { continue; }
        if (windows[j].overlaps(windows[t])) { return false; }   // PURGE
        if (embargo > 0 && j > t && j - t <= embargo) {
            return false;                                        // EMBARGO
        }
    }
    return true;
}

/// The `i`th test block of a k-fold partition, in time order.
[[nodiscard]] inline std::expected<Block, ValidationError>
kfold_test_block(std::size_t n, const PurgedKFoldSpec& s,
                 std::size_t i) noexcept {
    if (s.folds == 0) { return std::unexpected(ValidationError::BadPartition); }
    if (s.embargo == 0 && !s.embargo_deliberately_zero) {
        return std::unexpected(ValidationError::NoEmbargo);
    }
    if (n < s.folds) { return std::unexpected(ValidationError::TooFewSamples); }
    if (i >= s.folds) { return std::unexpected(ValidationError::NoSuchFold); }
    const std::size_t base = n / s.folds;
    const std::size_t rem = n % s.folds;
    Block b{};
    b.start = i * base + (i < rem ? i : rem);
    b.end = b.start + base + (i < rem ? 1 : 0);
    return b;
}

/// How many training samples a NAIVE split would have kept that purging drops.
///
/// The direct count of contamination -- no model, no metric, no
/// interpretation. Exposed because it is the part of the argument that does
/// not depend on anybody's choice of estimator.
[[nodiscard]] inline std::expected<std::size_t, ValidationError>
contaminated_count(std::size_t n, const PurgedKFoldSpec& s, std::size_t fold,
                   const LabelWindow* windows) noexcept {
    const auto test = kfold_test_block(n, s, fold);
    if (!test) { return std::unexpected(test.error()); }
    std::size_t bad = 0;
    for (std::size_t j = 0; j < n; ++j) {
        if (test->contains(j)) { continue; }
        if (!survives_purge(j, *test, windows, n, s.embargo)) { ++bad; }
    }
    return bad;
}

/// Build the label windows for a fixed forward horizon.
///
/// The common case, and provided so a caller does not hand-roll it and get the
/// half-open convention wrong -- an off-by-one here silently changes how much
/// purging happens.
inline void forward_horizon_windows(LabelWindow* out, std::size_t n,
                                    std::size_t horizon) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        out[i].start = i;
        out[i].end = i + horizon;
    }
}

} // namespace altair
