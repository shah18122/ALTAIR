// P6-06 acceptance tests for backtest/validation.hpp.
//
// Test 1 counts the contamination: with a 20-bar forward label, how many
// training samples a naive 5-fold split keeps that share their future with the
// test block. No model, no metric, no interpretation -- just the count.
//
// Test 2 measures what that is worth, using a 1-nearest-neighbour predictor on
// a series with NO relationship in it at all. The naive split reports skill.
//
// Test 3 is the embargo specifically: purging alone leaves a path open through
// the FEATURES, and the test shows which samples it misses.
//
// No check description here may contain the substring FAIL.

#include <backtest/validation.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

struct Lcg {
    std::uint64_t s;
    double uniform()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & ((1ULL << 53) - 1))
               / static_cast<double>(1ULL << 53);
    }
    double normal()
    {
        const double u1 = uniform();
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1 + 1e-300))
               * std::cos(6.283185307179586 * u2);
    }
};

} // namespace

using namespace altair;

namespace {

constexpr std::size_t kN = 10'000;
constexpr std::size_t kHorizon = 20;
constexpr std::size_t kLookback = 20;

constexpr std::size_t kDim = 8;

LabelWindow windows[kN];
double feature[kN][kDim];
double label[kN];

/// Pure noise, with an EIGHT-DIMENSIONAL feature.
///
/// Every component looks STRICTLY BACKWARD over its own lookback and the label
/// looks STRICTLY FORWARD over kHorizon, so feature and label share no bar and
/// the true relationship between them is exactly zero. Any skill a validation
/// scheme reports here is manufactured by the scheme.
///
/// The dimensionality is not decoration. In ONE dimension the nearest
/// neighbour among 5,000 candidates is a VALUE neighbour at distance ~0.001,
/// nowhere near in time, and no leak appears -- an earlier draft of this test
/// measured exactly that and found nothing, correctly. In eight dimensions a
/// random sample sits at distance ~1.6 while the bar next door sits at ~0.9,
/// so nearest-in-feature becomes nearest-in-TIME. That is the regime real
/// feature vectors live in, and it is when a memoriser starts reading labels
/// off its neighbours.
void make_data(std::uint64_t seed)
{
    static const std::size_t lookbacks[kDim] = {5, 8, 12, 18, 26, 38, 55, 80};
    static double noise[kN + kHorizon + 8];
    Lcg g{seed};
    for (std::size_t i = 0; i < kN + kHorizon + 8; ++i) {
        noise[i] = g.normal();
    }
    for (std::size_t i = 0; i < kN; ++i) {
        for (std::size_t d = 0; d < kDim; ++d) {
            const std::size_t w = lookbacks[d];
            double f = 0.0;
            for (std::size_t k = 0; k < w; ++k) {
                const std::size_t idx = i >= k + 1 ? i - k - 1 : 0;
                f += noise[idx];
            }
            feature[i][d] = f / std::sqrt(static_cast<double>(w));
        }
        double l = 0.0;
        for (std::size_t k = 0; k < kHorizon; ++k) { l += noise[i + k]; }
        label[i] = l / std::sqrt(static_cast<double>(kHorizon));
    }
    forward_horizon_windows(windows, kN, kHorizon);
}

/// Squared euclidean distance between two feature vectors.
[[nodiscard]] double dist2(std::size_t a, std::size_t b) noexcept
{
    double acc = 0.0;
    for (std::size_t d = 0; d < kDim; ++d) {
        const double e = feature[a][d] - feature[b][d];
        acc += e * e;
    }
    return acc;
}

// ── 1 ────────────────────────────────────────────────────────────────────
// THE card, counted.
void a_naive_split_trains_on_the_answer()
{
    std::printf("\n1 a_naive_split_trains_on_the_answer\n");
    make_data(0xCA1BE);

    PurgedKFoldSpec s{};
    s.folds = 5;
    s.embargo = kLookback;

    std::size_t total_bad = 0, total_train = 0;
    for (std::size_t f = 0; f < s.folds; ++f) {
        const auto bad = contaminated_count(kN, s, f, windows);
        const auto blk = kfold_test_block(kN, s, f);
        check(bad.has_value() && blk.has_value(), "each fold partitions");
        if (!bad || !blk) { return; }
        total_bad += *bad;
        total_train += kN - blk->size();
    }
    std::printf("    %zu samples, %zu-bar forward label, %zu-bar feature"
                " lookback, 5 folds:\n"
                "      training samples a naive split keeps   %6zu\n"
                "      of those, CONTAMINATED                 %6zu  (%.2f%%)\n"
                "      purging and embargo drop them, leaving %6zu\n",
                kN, kHorizon, kLookback, total_train, total_bad,
                100.0 * static_cast<double>(total_bad)
                    / static_cast<double>(total_train),
                total_train - total_bad);

    check(total_bad > 0,
          "a naive split keeps training samples whose label window overlaps"
          " the test block's -- they share the future the model is about to be"
          " graded on");
    std::printf("    -> nothing is copied, no index is off by one, and no"
                " amount of staring at the\n       split code reveals it. The"
                " leak lives in the LABEL DEFINITION, which is why\n       the"
                " harness has to be given each sample's label window to purge"
                " at all.\n");

    check(total_bad < total_train / 50,
          "and it is under 2% -- which is the honest finding about CONTIGUOUS"
          " folds: a block touches the training set only at its two edges, so"
          " purging there removes a boundary layer and little else");

    // THE SPLIT CLAUDE.md ACTUALLY BANS: shuffled K-fold, which interleaves
    // test and training samples throughout rather than blocking them.
    static std::size_t test_idx[kN / 5 + 8];
    static bool is_test[kN];
    Lcg g{0x5F0FF1EE};
    for (auto& b : is_test) { b = false; }
    std::size_t k = 0;
    while (k < kN / 5) {
        const std::size_t j = static_cast<std::size_t>(g.uniform()
                                                       * static_cast<double>(kN));
        if (j < kN && !is_test[j]) { is_test[j] = true; test_idx[k++] = j; }
    }
    // Contamination by the fixed-horizon rule: sample j overlaps test i when
    // |i - j| < horizon, so mark that band around every test index.
    static bool dirty[kN];
    for (auto& b : dirty) { b = false; }
    for (std::size_t i = 0; i < k; ++i) {
        const std::size_t t = test_idx[i];
        const std::size_t lo = t >= kHorizon - 1 ? t - (kHorizon - 1) : 0;
        const std::size_t hi = t + kHorizon - 1 < kN ? t + kHorizon - 1 : kN - 1;
        for (std::size_t j = lo; j <= hi; ++j) { dirty[j] = true; }
    }
    std::size_t shuffled_train = 0, shuffled_bad = 0;
    for (std::size_t j = 0; j < kN; ++j) {
        if (is_test[j]) { continue; }
        ++shuffled_train;
        if (dirty[j]) { ++shuffled_bad; }
    }
    std::printf("    the SAME data under a SHUFFLED 5-fold, one fold:\n"
                "      training samples kept                  %6zu\n"
                "      of those, CONTAMINATED                 %6zu  (%.1f%%)\n",
                shuffled_train, shuffled_bad,
                100.0 * static_cast<double>(shuffled_bad)
                    / static_cast<double>(shuffled_train));
    check(shuffled_bad > shuffled_train * 9 / 10,
          "shuffling contaminates over 90% of the training set, because"
          " scattering the test samples puts nearly every training sample"
          " within a label horizon of one of them");
    check(shuffled_bad > 100 * total_bad / 5,
          "which is two orders of magnitude worse than the contiguous split --"
          " these are not the same mistake by different amounts, they are"
          " different mistakes, and shuffled K-fold is the one CLAUDE.md bans");
    std::printf("    -> and purging a shuffled split honestly leaves %zu usable"
                " training samples\n       out of %zu. That is the proof the"
                " split was never valid: there is no version\n       of it that"
                " both removes the leak and keeps enough data to fit"
                " anything.\n",
                shuffled_train - shuffled_bad, shuffled_train);

    // The header's general routine agrees with the fast mask, on a sample.
    std::size_t agree = 0, checked = 0;
    for (std::size_t j = 0; j < kN; j += 997) {
        if (is_test[j]) { continue; }
        const bool a = survives_purge_set(j, test_idx, k, windows, kN, 0);
        if (a == !dirty[j]) { ++agree; }
        ++checked;
    }
    check(checked > 5 && agree == checked,
          "and survives_purge_set agrees with the fixed-horizon band on every"
          " sampled index, so the fast count above is the same rule the header"
          " applies");

    // A harness with no label windows cannot purge, by construction.
    check(!LabelWindow{0, 1}.overlaps(LabelWindow{1, 2}),
          "with a point label -- horizon of one bar -- neighbouring windows do"
          " not overlap and purging has nothing to remove, which is the one"
          " case shuffled K-fold is safe in and is not the case anyone trades");
}

// ── 2 ───────────────────────────────────────────────────────────────────────
void the_leak_is_worth_measurable_skill_on_pure_noise()
{
    std::printf("\n2 the_leak_is_worth_measurable_skill_on_pure_noise\n");
    make_data(0x5EED11);

    // The comparison is between the two schemes a person actually chooses:
    // a SHUFFLED K-fold, and a purged CONTIGUOUS one. (Comparing shuffled
    // against purged-shuffled is not available, and test 1 says why: purging a
    // shuffled split honestly leaves zero training samples.)
    //
    // The predictor is 1-nearest-neighbour in feature space -- the canonical
    // memoriser, and the honest way to expose leakage. A linear model averages
    // the leak away, which is why a contaminated backtest passes a linear
    // sanity check and then dies in production on a tree.
    static bool is_test[kN];
    static std::size_t test_idx[kN / 5 + 8];

    auto correlation = [&](bool shuffled, std::size_t fold) {
        Block block{};
        std::size_t k = 0;
        if (shuffled) {
            Lcg g{0xA5A5 + fold};
            for (auto& b : is_test) { b = false; }
            while (k < kN / 5) {
                const std::size_t j = static_cast<std::size_t>(
                    g.uniform() * static_cast<double>(kN));
                if (j < kN && !is_test[j]) { is_test[j] = true; test_idx[k++] = j; }
            }
        } else {
            PurgedKFoldSpec s{};
            s.folds = 5;
            s.embargo = kLookback;
            const auto b = kfold_test_block(kN, s, fold);
            if (!b) { return 0.0; }
            block = *b;
        }

        auto usable = [&](std::size_t j) {
            if (shuffled) {
                // The naive rule: exclude the test samples, and nothing else.
                return !is_test[j];
            }
            PurgedKFoldSpec s{};
            s.folds = 5;
            s.embargo = kLookback;
            return survives_purge(j, block, windows, kN, s.embargo);
        };

        // One pass, Welford-free: accumulate sums and form the correlation at
        // the end. n is small enough that the shortcut is exact here.
        double sx = 0.0, sy = 0.0, sxx = 0.0, syy = 0.0, sxy = 0.0;
        std::size_t m = 0;
        const std::size_t lo = shuffled ? 0 : block.start;
        const std::size_t hi = shuffled ? kN : block.end;
        for (std::size_t i = lo; i < hi; i += 2) {
            if (shuffled && !is_test[i]) { continue; }
            double best = 1e300, pred = 0.0;
            bool found = false;
            for (std::size_t j = 0; j < kN; j += 2) {
                if (!usable(j)) { continue; }
                const double d = dist2(j, i);
                if (d < best) { best = d; pred = label[j]; found = true; }
            }
            if (!found) { continue; }
            sx += pred; sy += label[i];
            sxx += pred * pred; syy += label[i] * label[i];
            sxy += pred * label[i];
            ++m;
        }
        if (m < 10) { return 0.0; }
        const double n = static_cast<double>(m);
        const double cov = sxy / n - (sx / n) * (sy / n);
        const double vx = sxx / n - (sx / n) * (sx / n);
        const double vy = syy / n - (sy / n) * (sy / n);
        return (vx > 0.0 && vy > 0.0) ? cov / std::sqrt(vx * vy) : 0.0;
    };

    double shuffled_sum = 0.0, purged_sum = 0.0;
    for (std::size_t f = 0; f < 5; ++f) {
        shuffled_sum += correlation(true, f);
        purged_sum += correlation(false, f);
    }
    const double shuffled = shuffled_sum / 5.0;
    const double purged = purged_sum / 5.0;
    std::printf("    1-NN out-of-sample correlation, averaged over 5 folds, on"
                " data whose feature\n    and label share NO BAR -- the true"
                " relationship is exactly zero:\n"
                "      SHUFFLED K-fold (test samples excluded only)  %+.4f\n"
                "      purged contiguous fold                        %+.4f\n",
                shuffled, purged);

    check(shuffled > 0.05,
          "the shuffled split reports substantial POSITIVE out-of-sample"
          " correlation on a feature and a label that share no bar at all --"
          " skill that does not exist, from a scheme that looks rigorous");
    check(std::fabs(purged) < shuffled / 2.0,
          "while the purged contiguous fold reports far less, toward the zero"
          " it should have been");
    std::printf("    -> the mechanism is simple: 1-NN's nearest neighbour in"
                " feature space is usually\n       a TEMPORAL neighbour,"
                " because the feature is a rolling window and moves\n"
                "       slowly. A temporal neighbour's label overlaps the test"
                " label by up to %zu of\n       its %zu bars. The model is not"
                " predicting; it is reading the answer off a\n       sample"
                " that shares it.\n",
                kHorizon - 1, kHorizon);
}

// ── 3 ────────────────────────────────────────────────────────────────────
void the_embargo_closes_a_path_that_purging_leaves_open()
{
    std::printf("\n3 the_embargo_closes_a_path_that_purging_leaves_open\n");
    make_data(0xE11BA);
    const Block test{4000, 6000};

    // Samples just after the test block. Their LABELS do not overlap it --
    // sample 6020's label covers [6020, 6040) and the test block's labels end
    // at 6019. But their FEATURES look back 20 bars, into the block.
    std::size_t purge_only = 0, with_embargo = 0;
    for (std::size_t j = test.end; j < test.end + kLookback; ++j) {
        if (survives_purge(j, test, windows, kN, 0)) { ++purge_only; }
        if (survives_purge(j, test, windows, kN, kLookback)) { ++with_embargo; }
    }
    std::printf("    the %zu samples immediately after a test block ending at"
                " %zu:\n"
                "      kept by PURGING alone      %2zu\n"
                "      kept WITH the embargo      %2zu\n",
                kLookback, test.end, purge_only, with_embargo);
    check(purge_only > 0,
          "purging alone keeps samples that start right after the test block");
    check(with_embargo == 0,
          "and the embargo removes every one of them -- their labels are"
          " clean, but their FEATURES are computed from bars inside the test"
          " block, so training on them is training on the test set through a"
          " door purging does not watch");

    // The embargo has no default, and saying so is the point.
    PurgedKFoldSpec none{};
    none.folds = 5;
    check(kfold_test_block(kN, none, 0).error() == ValidationError::NoEmbargo,
          "a spec with no embargo is REFUSED: the right length is the longest"
          " feature lookback in the set, which this file does not know and"
          " P5-01's warmup() does");
    none.embargo_deliberately_zero = true;
    check(kfold_test_block(kN, none, 0).has_value(),
          "a caller who has genuinely decided zero is right can say so, which"
          " is a different act from not having thought about it");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void walk_forward_never_trains_on_the_future()
{
    std::printf("\n4 walk_forward_never_trains_on_the_future\n");
    WalkForwardSpec s{};
    s.initial_train = 2000;
    s.test_len = 500;
    s.step = 500;
    s.gap = kHorizon;
    s.expanding = true;

    const auto k = walk_forward_count(kN, s);
    check(k.has_value() && *k > 10, "the schedule produces a usable fold count");
    if (!k) { return; }

    bool ordered = true, gapped = true;
    std::size_t min_gap = 1'000'000;
    for (std::size_t i = 0; i < *k; ++i) {
        const auto f = walk_forward_fold(kN, s, i);
        if (!f) { ordered = false; break; }
        if (f->train.end > f->test.start) { ordered = false; }
        const std::size_t g = f->test.start - f->train.end;
        if (g < s.gap) { gapped = false; }
        if (g < min_gap) { min_gap = g; }
    }
    std::printf("    %zu folds, expanding window, %zu-bar gap:\n"
                "      every training block ends before its test block starts:"
                " %s\n"
                "      smallest observed gap: %zu bars (required %zu)\n",
                *k, s.gap, ordered ? "yes" : "no", min_gap, s.gap);
    check(ordered,
          "no fold's training block reaches past the start of its test block");
    check(gapped,
          "and every one is separated by at least the gap -- with a 20-bar"
          " label the last 20 training labels reach into the test block, so a"
          " gap of at least the horizon is what makes the split mean anything");

    // Rolling vs expanding are different questions, so neither is a default.
    WalkForwardSpec roll = s;
    roll.expanding = false;
    const auto e = walk_forward_fold(kN, s, 5);
    const auto r = walk_forward_fold(kN, roll, 5);
    check(e.has_value() && r.has_value(), "both window styles enumerate");
    if (e && r) {
        check(e->train.start == 0 && r->train.start > 0,
              "expanding keeps every earlier bar and rolling drops the oldest"
              " -- expanding assumes the relationship is stable, rolling"
              " assumes it decays, and those are different claims about the"
              " market rather than a tuning knob");
        check(e->test.start == r->test.start && e->test.end == r->test.end,
              "while the test blocks are identical, so the two are comparable");
    }

    check(walk_forward_count(100, s).error()
          == ValidationError::TooFewSamples,
          "and too little history is refused rather than producing one"
          " degenerate fold that would be reported as a walk-forward result");
}

} // namespace

int main()
{
    std::printf("altair walk-forward and purged CV tests\n");
    a_naive_split_trains_on_the_answer();
    the_leak_is_worth_measurable_skill_on_pure_noise();
    the_embargo_closes_a_path_that_purging_leaves_open();
    walk_forward_never_trains_on_the_future();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
