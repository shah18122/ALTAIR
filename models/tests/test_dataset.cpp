// P8-02 acceptance tests for models/dataset.hpp and models/tensor.hpp.
//
// Test 1 is the card: normalisation fitted on the whole dataset leaks the test
// set into every training row. Measured on data with a deliberate level shift
// between train and test, which is what a real regime change looks like.
//
// Test 2: a tensor has no way to say "absent", so the builder has to decide,
// and both options change the data.
//
// Test 3: shapes are checked; softmax subtracts the max.
//
// No check description here may contain the substring FAIL.

#include <models/dataset.hpp>

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

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

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

constexpr std::size_t kRows = 2000;
constexpr std::size_t kCols = 4;
constexpr std::size_t kTrain = 1400;

double xs_all[kRows * kCols];
double xs_train[kRows * kCols];

/// A feature whose LEVEL SHIFTS between the training period and the test
/// period -- which is what a regime change is, and is the ordinary case rather
/// than an adversarial one.
void make_shifted(std::uint64_t seed)
{
    Lcg g{seed};
    for (std::size_t i = 0; i < kRows; ++i) {
        const double shift = i < kTrain ? 0.0 : 3.0;
        for (std::size_t j = 0; j < kCols; ++j) {
            const double v = g.normal() + shift;
            xs_all[i * kCols + j] = v;
            xs_train[i * kCols + j] = v;
        }
    }
}

// ── 1 ────────────────────────────────────────────────────────────────────
// THE card.
void fitting_the_scaler_on_everything_leaks_the_test_set()
{
    std::printf("\n1 fitting_the_scaler_on_everything_leaks_the_test_set\n");
    make_shifted(0x5CA1E5);

    Matrix all{xs_all, kRows, kCols};
    Matrix tr{xs_train, kRows, kCols};

    // THE LEAK: fit across every row, then split. Nothing is copied and the
    // split below is still perfectly clean.
    Scaler leaky;
    check(leaky.fit(all, 0, kRows).has_value(), "the leaky scaler fits");
    // THE CORRECT WAY: fit on the training rows only.
    Scaler honest;
    check(honest.fit(tr, 0, kTrain).has_value(),
          "the honest scaler fits on the training rows only");

    std::printf("    a feature whose level shifts by +3.0 at the train/test"
                " boundary (row %zu of %zu):\n"
                "      fitted on EVERYTHING   mean %+.4f  sd %.4f\n"
                "      fitted on TRAIN only   mean %+.4f  sd %.4f\n",
                kTrain, kRows, leaky.mean(0), leaky.sd(0),
                honest.mean(0), honest.sd(0));
    check(std::fabs(leaky.mean(0) - honest.mean(0)) > 0.5,
          "the two scalers disagree materially about the mean, because the"
          " leaky one has seen the shift and the honest one has not");
    check(leaky.sd(0) > honest.sd(0),
          "and the leaky standard deviation is inflated by a shift that has"
          " not happened yet from the training rows' point of view");

    // What that does to the TRAINING rows -- the ones the model actually fits.
    check(leaky.transform(all, 0, kTrain, false).has_value(),
          "the leaky scaler standardises the training rows");
    check(honest.transform(tr, 0, kTrain, false).has_value(),
          "and so does the honest one");
    double max_gap = 0.0, sum_gap = 0.0;
    for (std::size_t i = 0; i < kTrain; ++i) {
        for (std::size_t j = 0; j < kCols; ++j) {
            const double g = std::fabs(all.at(i, j) - tr.at(i, j));
            if (g > max_gap) { max_gap = g; }
            sum_gap += g;
        }
    }
    std::printf("      every TRAINING row now differs between the two:"
                " mean |gap| %.4f, worst %.4f\n",
                sum_gap / static_cast<double>(kTrain * kCols), max_gap);
    check(max_gap > 0.5,
          "EVERY training row is expressed in different units under the two"
          " scalers -- the training data itself has been changed by rows the"
          " model is not supposed to have seen");
    std::printf("    -> nothing was copied. The fold boundary is identical in"
                " both cases and any\n       review reading the split code"
                " finds it correct. The leak is in the\n       PREPROCESSING,"
                " which usually lives in a different file written at a"
                " different\n       time, and that is why it survives every"
                " other precaution.\n");

    // The ordering is enforced, not documented.
    Scaler fresh;
    Matrix m{xs_all, kRows, kCols};
    check(fresh.transform(m, 0, 10, false).error() == DatasetError::NotFitted,
          "a scaler cannot transform before it has been fitted");
    check(fresh.fit(m, 0, kTrain).has_value(), "it fits once");
    check(fresh.fit(m, 0, kRows).error() == DatasetError::AlreadyFitted,
          "and cannot be refitted -- refitting on a wider range after seeing a"
          " disappointing result is the leak arriving by a second route");
    check(fresh.transform(m, kTrain, kRows, false).error()
          == DatasetError::OutOfFittedRange,
          "transforming rows OUTSIDE the fitted range is refused unless the"
          " caller says it is applying a training-fitted scaler to holdout"
          " data");
    check(fresh.transform(m, kTrain, kRows, true).has_value(),
          "which is the one correct use, and saying so is what makes the"
          " incorrect use unreachable by accident");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void a_tensor_has_no_way_to_say_absent()
{
    std::printf("\n2 a_tensor_has_no_way_to_say_absent\n");
    constexpr std::size_t kN = 600;
    static FeatureVector vecs[kN];
    static Label labs[kN];
    static double uniq[kN];
    static double xs[kN * 3], ms[kN * 3], ys[kN], ws[kN];
    static std::size_t bars[kN];
    static LabelWindow wins[kN];

    Lcg g{0xAB5E17};
    for (std::size_t i = 0; i < kN; ++i) {
        vecs[i] = FeatureVector{7, 3, Timestamp{static_cast<std::int64_t>(i)}};
        // The first 60 rows are WARMING UP -- feature 2 is absent, which is
        // exactly how a real series starts and is not random.
        (void)vecs[i].set(0, g.normal());
        (void)vecs[i].set(1, g.normal());
        if (i >= 60) { (void)vecs[i].set(2, g.normal()); }
        labs[i].sign = (i % 2 == 0) ? 1 : -1;
        labs[i].touched = BarrierTouched::Target;
        labs[i].window = LabelWindow{i, i + 5};
        uniq[i] = 0.4;
    }
    const FeatureIndex idx[3] = {0, 1, 2};

    const auto dropped = build_dataset(vecs, labs, uniq, kN, idx, 3,
                                       MissingPolicy::DropRow, xs, nullptr,
                                       ys, ws, bars, wins);
    check(dropped.has_value(), "DropRow assembles");
    const auto masked = build_dataset(vecs, labs, uniq, kN, idx, 3,
                                      MissingPolicy::MaskAndImpute, xs, ms,
                                      ys, ws, bars, wins);
    check(masked.has_value(), "MaskAndImpute assembles");
    if (!dropped || !masked) { return; }

    std::printf("    %zu rows, feature 2 absent for the first 60 (warmup):\n"
                "      DropRow          %zu rows kept, %zu dropped for absence\n"
                "      MaskAndImpute    %zu rows kept, mask marks the gaps\n",
                kN, dropped->rows, dropped->dropped_absent, masked->rows);
    check(dropped->rows == kN - 60 && dropped->dropped_absent == 60,
          "DropRow removes exactly the warmup rows");
    check(masked->rows == kN,
          "MaskAndImpute keeps every row");
    std::printf("    -> and the dropped rows are NOT a random sample. Warmup,"
                " gaps and illiquid\n       symbols go first, which are"
                " exactly the hard cases. Neither option is\n       free, there"
                " is no third where a zero is harmless, and so MissingPolicy"
                " has\n       no default and zero-fill is not offered at"
                " all.\n");

    FeatureVector blank[2] = {};
    Label bl[2] = {};
    check(build_dataset(blank, bl, nullptr, 2, idx, 3,
                        MissingPolicy::Unspecified, xs, ms, ys, ws, bars, wins)
              .error() == DatasetError::NoMissingPolicy,
          "an unspecified policy is refused outright");

    // The imputation value is a TRAINING statistic and the ordering enforces it.
    Scaler s;
    check(s.fit(masked->x, 0, 400).has_value(), "a scaler fits on 400 rows");
    Dataset d = *masked;
    const auto filled = impute_with_fitted_mean(d, s);
    check(filled.has_value() && *filled == 60,
          "and imputation fills exactly the 60 masked entries with the FITTED"
          " mean -- a training-fold statistic. Computing that mean over every"
          " row, which is what an impute-then-split pipeline does, is the same"
          " leak as test 1 in a different coat");
    Scaler unfitted;
    check(impute_with_fitted_mean(d, unfitted).error()
          == DatasetError::NotFitted,
          "so imputing before fitting is refused");

    // Weights are the uniqueness, not ones.
    std::printf("    %zu rows carry %.1f effective observations (%.2f each)\n",
                d.rows, effective_rows(d), effective_rows(d)
                                             / static_cast<double>(d.rows));
    check(near(effective_rows(d), 0.4 * static_cast<double>(d.rows), 1e-9),
          "sample weights are P8-01's uniqueness, not ones -- rows whose"
          " labels overlap are not separate observations, and the dataset"
          " carries that through rather than making the trainer rediscover it");
    check(d.bar != nullptr && d.bar[0] == 0 && d.bar[10] == 10,
          "and every row keeps an index back to its bar, because a shuffled"
          " dataset with no route back to time cannot be purged, walked"
          " forward or audited");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void shapes_are_checked_and_softmax_subtracts_the_max()
{
    std::printf("\n3 shapes_are_checked_and_softmax_subtracts_the_max\n");
    double a[6] = {1, 2, 3, 4, 5, 6};       // 2x3
    double b[6] = {1, 0, 0, 1, 1, 1};       // 3x2
    double o[4] = {};
    Matrix A{a, 2, 3}, B{b, 3, 2}, O{o, 2, 2};
    check(matmul(A, B, O).has_value(), "a 2x3 times a 3x2 multiplies");
    check(near(O.at(0, 0), 1 * 1 + 2 * 0 + 3 * 1, 1e-12)
          && near(O.at(1, 1), 4 * 0 + 5 * 1 + 6 * 1, 1e-12),
          "and the result is right");

    double bad[6] = {};
    Matrix Bad{bad, 2, 3};
    check(matmul(A, Bad, O).error() == TensorError::ShapeMismatch,
          "a 2x3 times a 2x3 is REFUSED -- a shape mismatch in a matmul is the"
          " commonest bug in model code and it is silent whenever the"
          " dimensions happen to line up for the wrong reason");

    // Softmax on logits that would overflow.
    double big[3] = {800.0, 801.0, 799.0};
    softmax_row(big, 3);
    std::printf("    softmax of {800, 801, 799}: %.4f %.4f %.4f  (sum %.6f)\n",
                big[0], big[1], big[2], big[0] + big[1] + big[2]);
    check(std::isfinite(big[0]) && std::isfinite(big[1])
          && std::isfinite(big[2]),
          "softmax of logits near 800 is finite -- exp(800) is infinity and"
          " infinity/infinity is NaN, so subtracting the max first is not an"
          " optimisation, it is the whole function");
    check(near(big[0] + big[1] + big[2], 1.0, 1e-12),
          "and the row sums to one");
    check(big[1] > big[0] && big[0] > big[2],
          "with the ordering preserved");

    // Sigmoid in the tail.
    check(std::isfinite(sigmoid(-800.0)) && sigmoid(-800.0) >= 0.0,
          "the logistic is finite at -800, where 1/(1+exp(-x)) overflows --"
          " the same reasoning as analytics/greeks.hpp using erfc rather than"
          " erf");
    check(near(sigmoid(0.0), 0.5, 1e-15), "and correct at zero");

    // Initialisation is seeded and reproducible (rule 10).
    double w1[64] = {}, w2[64] = {};
    Matrix W1{w1, 8, 8}, W2{w2, 8, 8};
    Init i1{42}, i2{42}, i3{43};
    i1.xavier(W1, 8, 8);
    i2.xavier(W2, 8, 8);
    bool same = true;
    for (std::size_t i = 0; i < 64; ++i) { if (w1[i] != w2[i]) { same = false; } }
    check(same,
          "the same seed gives bit-identical initial weights, so a model can"
          " be retrained to the same place -- rule 10 wants every decision"
          " reproducible and that starts at the initialisation");
    i3.xavier(W2, 8, 8);
    bool differs = false;
    for (std::size_t i = 0; i < 64; ++i) { if (w1[i] != w2[i]) { differs = true; } }
    check(differs, "and a different seed gives different weights");
}

void targets_scale_down_and_restore_original_units()
{
    std::printf("\n4 targets_scale_down_and_restore_original_units\n");
    const double prices[]{10'000.0, 10'010.0, 9'990.0, 10'020.0,
                          40'000.0, 40'100.0};
    TargetScaler scaler;
    check(scaler.fit(prices, 0, 4).has_value(),
          "target scale is fitted on the training fold only");
    const auto scaled = scaler.transform(prices[4]);
    const auto restored = scaled ? scaler.inverse(*scaled)
                                 : std::expected<double, DatasetError>{
                                       std::unexpected(DatasetError::NotFitted)};
    check(scaled && restored && std::fabs(*scaled) < prices[4]
          && near(*restored, prices[4], 1e-9),
          "large price targets train at reduced magnitude and invert to exact original units");
    check(scaler.fitted_to() == 4 && scaler.mean() < 20'000.0,
          "holdout price levels do not leak into target scaling statistics");
}

} // namespace

int main()
{
    std::printf("altair dataset assembly tests\n");
    fitting_the_scaler_on_everything_leaks_the_test_set();
    a_tensor_has_no_way_to_say_absent();
    shapes_are_checked_and_softmax_subtracts_the_max();
    targets_scale_down_and_restore_original_units();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
