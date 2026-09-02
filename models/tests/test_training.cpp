// P8-03 and P8-09 acceptance tests.
//
// Test 1 is the card: early stopping selects the minimum over E noisy epochs,
// so the validation loss it reports is biased downward even when the model
// learned nothing. Measured on pure noise, where the true expected loss is
// known exactly.
//
// Test 2: the best checkpoint is not the last one.
//
// Test 3: a weights-only model hash matches two models that multiply different
// columns by identical numbers.
//
// No check description here may contain the substring FAIL.

#include <models/registry.hpp>

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

constexpr std::size_t kRows = 1200;
constexpr std::size_t kCols = 3;
double xs[kRows * kCols], ys[kRows], ws[kRows], preds[kRows];
std::size_t bars[kRows];
LabelWindow wins[kRows];

/// Features and labels that are INDEPENDENT. The best any model can do is
/// predict the mean, so the true expected MSE is the variance of y -- known
/// exactly, which is what makes the bias below measurable rather than
/// arguable.
Dataset make_noise(std::uint64_t seed)
{
    Lcg g{seed};
    for (std::size_t i = 0; i < kRows; ++i) {
        for (std::size_t j = 0; j < kCols; ++j) {
            xs[i * kCols + j] = g.normal();
        }
        ys[i] = g.normal();             // independent of every x
        ws[i] = 1.0;
        bars[i] = i;
        wins[i] = LabelWindow{i, i + 1};
    }
    Dataset d{};
    d.x = Matrix{xs, kRows, kCols};
    d.y = ys;
    d.weight = ws;
    d.bar = bars;
    d.window = wins;
    d.rows = kRows;
    return d;
}

/// A model that does not learn: it emits a small seeded perturbation each
/// epoch. Its validation loss therefore wanders around the true value with no
/// downward trend, which isolates the SELECTION effect from any real learning.
struct NoisyModel {
    double p[kCols + 1] = {};
    Lcg g{1};
    std::size_t epoch = 0;

    void reset(std::uint64_t seed) noexcept {
        g = Lcg{seed != 0 ? seed : 1};
        for (double& v : p) { v = 0.0; }
        epoch = 0;
    }
    double train_epoch(const Dataset&, const Block&, double) noexcept {
        // Wander. No gradient, no learning -- the point is that the
        // validation curve is noise and early stopping still "improves" it.
        for (double& v : p) { v = g.normal() * 0.05; }
        ++epoch;
        return 1.0;
    }
    void predict(const Dataset& d, const Block& b, double* out) noexcept {
        for (std::size_t i = b.start; i < b.end; ++i) {
            double acc = p[kCols];
            for (std::size_t j = 0; j < kCols; ++j) {
                acc += p[j] * d.x.at(i, j);
            }
            out[i] = acc;
        }
    }
    [[nodiscard]] std::size_t param_count() const noexcept { return kCols + 1; }
    [[nodiscard]] const double* params() const noexcept { return p; }
    void load_params(const double* src) noexcept {
        for (std::size_t i = 0; i < kCols + 1; ++i) { p[i] = src[i]; }
    }
};

Splits three_way()
{
    Splits s{};
    s.train = Block{0, 700};
    s.validation = Block{700, 950};
    s.test = Block{950, kRows};
    return s;
}

TrainConfig cfg(std::size_t epochs, std::size_t patience)
{
    TrainConfig c{};
    c.lr0 = 0.01;
    c.lr_min = 0.0001;
    c.max_epochs = epochs;
    c.patience = patience;
    c.schedule = LrSchedule::Cosine;
    c.seed = 20260902;
    return c;
}

// ── 1 ────────────────────────────────────────────────────────────────────
// THE card.
void early_stopping_makes_its_own_validation_score_optimistic()
{
    std::printf("\n1 early_stopping_makes_its_own_validation_score"
                "_optimistic\n");
    const Dataset d = make_noise(0xE5709);
    const Splits s = three_way();

    // The TRUE expected loss: features and labels are independent, so the best
    // possible prediction is the mean and the expected MSE is the variance.
    double m = 0.0;
    for (std::size_t i = s.validation.start; i < s.validation.end; ++i) {
        m += d.y[i];
    }
    m /= static_cast<double>(s.validation.size());
    double var = 0.0;
    for (std::size_t i = s.validation.start; i < s.validation.end; ++i) {
        var += (d.y[i] - m) * (d.y[i] - m);
    }
    var /= static_cast<double>(s.validation.size());

    std::printf("    features and labels are INDEPENDENT, so the best possible"
                " validation MSE is\n    the variance of y: %.4f\n\n"
                "      epochs   reported best validation MSE   below the truth"
                " by\n", var);
    double bias_10 = 0.0, bias_200 = 0.0;
    for (const std::size_t E : {std::size_t{10}, std::size_t{50},
                                std::size_t{200}}) {
        NoisyModel nm;
        Trainer t;
        static double best[kCols + 1];
        const auto r = t.run(nm, d, s, cfg(E, 0), preds, best);
        check(r.has_value(), "the run completes");
        if (!r) { continue; }
        const double below = var - r->best_validation_loss;
        std::printf("      %5zu    %26.4f   %14.4f\n",
                    E, r->best_validation_loss, below);
        if (E == 10) { bias_10 = below; }
        if (E == 200) { bias_200 = below; }
    }

    check(bias_10 > 0.0 && bias_200 > 0.0,
          "the reported validation loss is BELOW the true expected loss at"
          " every epoch budget -- on a model that learns nothing, from data"
          " with no relationship in it");
    check(bias_200 > bias_10,
          "and the gap GROWS with the number of epochs, because keeping the"
          " best of E is a selection over E candidates and the minimum of E"
          " noisy draws falls further below their mean as E rises");
    std::printf("    -> the same extreme-value arithmetic as P5-08's scanner"
                " and P7-03's deflated\n       Sharpe, now applied to epochs."
                " Early stopping is right and almost everybody\n       does it;"
                " what it also does is turn the validation set into training"
                " data.\n");

    // The fix: a third split, touched once.
    NoisyModel nm;
    Trainer t;
    static double best[kCols + 1];
    check(t.run(nm, d, s, cfg(50, 0), preds, best).has_value(),
          "a run completes on the three-way split");
    const auto first = t.consume_test(nm, d, s, preds);
    check(first.has_value(), "and the test split can be scored");
    const auto second = t.consume_test(nm, d, s, preds);
    check(!second && second.error() == TrainError::TestAlreadyUsed,
          "but only ONCE -- reading it twice is how it becomes a selection"
          " criterion, exactly as the validation set already did");
    if (first) {
        // The TEST split's OWN variance -- not the validation split's. They
        // are different data, and comparing across them measures the gap
        // between two samples rather than the bias under test. (An earlier
        // draft printed the validation variance here and called the result
        // "unbiased", which the numbers did not support.)
        double tm = 0.0;
        for (std::size_t i = s.test.start; i < s.test.end; ++i) { tm += d.y[i]; }
        tm /= static_cast<double>(s.test.size());
        double tvar = 0.0;
        for (std::size_t i = s.test.start; i < s.test.end; ++i) {
            tvar += (d.y[i] - tm) * (d.y[i] - tm);
        }
        tvar /= static_cast<double>(s.test.size());
        std::printf("    the third split, touched once: %.4f against ITS OWN"
                    " true value %.4f (gap %+.4f)\n"
                    "      -- and the gap is in whichever direction the sample"
                    " fell, because nothing\n         was selected on it.\n",
                    *first, tvar, *first - tvar);
    }

    Trainer fresh;
    NoisyModel nm2;
    check(fresh.consume_test(nm2, d, s, preds).error()
          == TrainError::NotTrained,
          "and the test split cannot be read before training, which would make"
          " it a model-selection set from the start");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void the_best_checkpoint_is_not_the_last_one()
{
    std::printf("\n2 the_best_checkpoint_is_not_the_last_one\n");
    const Dataset d = make_noise(0xC4EC4);
    const Splits s = three_way();
    NoisyModel nm;
    Trainer t;
    static double best[kCols + 1];
    const auto r = t.run(nm, d, s, cfg(60, 0), preds, best);
    check(r.has_value(), "the run completes");
    if (!r) { return; }

    std::printf("    %zu epochs run, best validation at epoch %zu\n",
                r->epochs_run, r->best_epoch);
    check(r->epochs_run == 60, "all sixty epochs ran (no patience set)");
    check(r->best_epoch < r->epochs_run - 1,
          "and the best epoch is NOT the last -- a loop that does not save the"
          " best ships whatever the final epoch happened to produce, which is"
          " past the optimum by construction whenever the curve is not"
          " monotone");

    // The restored parameters are the best ones, and the hash proves it.
    check(r->param_hash == hash_params(best, nm.param_count()),
          "the recorded parameter hash is of the RESTORED best weights, not of"
          " the final epoch's -- so what is registered is what is served");
    check(r->candidates == 60,
          "and the candidate count travels with the result, because it is the"
          " N the selection bias depends on");

    std::printf("    selection bias at 60 candidates with a 0.05 spread:"
                " %.4f\n", selection_bias(60, 0.05));
    check(selection_bias(60, 0.05) > selection_bias(6, 0.05),
          "and the bias estimator rises with the candidate count, as the"
          " mechanism requires");
    check(selection_bias(1, 0.05) == 0.0,
          "with a single candidate there is no selection and no bias");

    // Early stopping fires and is reported.
    NoisyModel nm3;
    Trainer t3;
    const auto e = t3.run(nm3, d, s, cfg(500, 5), preds, best);
    check(e.has_value() && e->early_stopped && e->epochs_run < 500,
          "with patience set the run stops early and says so");

    // The schedule is deterministic and part of the model.
    const auto lr0 = learning_rate(cfg(100, 0), 0);
    const auto lr99 = learning_rate(cfg(100, 0), 99);
    check(lr0.has_value() && lr99.has_value() && *lr0 > *lr99,
          "the cosine schedule anneals downward");
    TrainConfig unset = cfg(10, 0);
    unset.schedule = LrSchedule::Unspecified;
    check(learning_rate(unset, 0).error() == TrainError::BadParameter,
          "and a run with no schedule chosen is refused -- constant, step and"
          " cosine produce different models from identical data, so the"
          " schedule is part of the model rather than a tuning detail");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void a_weights_only_hash_is_not_a_model_hash()
{
    std::printf("\n3 a_weights_only_hash_is_not_a_model_hash\n");
    ModelRegistry reg;
    TrainingResult tr{};
    tr.best_epoch = 12;
    tr.candidates = 60;

    ModelKey a{};
    a.param_hash = 0xAAAA;
    a.feature_version = 0x1111;         // registry A
    a.architecture_hash = 0xBBBB;
    a.train_from = 0;
    a.train_to = 1000;
    a.seed = 7;

    // IDENTICAL WEIGHTS, different feature registry. P5-01 hashes feature
    // ORDER, so this is a registry whose columns are in a different sequence.
    ModelKey b = a;
    b.feature_version = 0x2222;

    check(a.param_hash == b.param_hash,
          "two models with byte-identical weights");
    check(!(a == b),
          "are NOT the same model when their feature registries differ --"
          " reorder two features and every weight multiplies the wrong column,"
          " with no runtime symptom whatsoever");
    std::printf("    identical weights, different feature_version:"
                " digests %016llx vs %016llx\n",
                static_cast<unsigned long long>(a.digest()),
                static_cast<unsigned long long>(b.digest()));

    const auto v1 = reg.register_model("tenmin_forecast", a, tr);
    const auto v2 = reg.register_model("tenmin_forecast", b, tr);
    check(v1.has_value() && v2.has_value() && *v1 == 1 && *v2 == 2,
          "so they register as two versions, not one");
    const auto v1again = reg.register_model("tenmin_forecast", a, tr);
    check(v1again.has_value() && *v1again == 1,
          "while re-registering an IDENTICAL key is a no-op returning the"
          " existing version -- retraining that lands in the same place is not"
          " a new model");
    check(reg.versions_of("tenmin_forecast") == 2, "two versions on record");

    ModelKey incomplete{};
    incomplete.param_hash = 0xAAAA;
    check(reg.register_model("x", incomplete, tr).error()
          == ModelRegistryError::IncompleteKey,
          "a key with weights but no feature version is refused outright");

    // Serving checks the vector against the key.
    FeatureVector good{0x1111, 4, Timestamp{0}};
    FeatureVector bad{0x2222, 4, Timestamp{0}};
    check(reg.check_vector(a, good).has_value(),
          "a vector from the registry the model was trained against is"
          " accepted");
    check(reg.check_vector(a, bad).error()
          == ModelRegistryError::FeatureVersionMismatch,
          "and one from a different registry is REFUSED at serving time, which"
          " is the only place the mismatch is detectable at all");

    // The pipeline is enumerated: nothing goes straight to Live.
    check(reg.promote(a, ModelStage::Live).error()
          == ModelRegistryError::NotServable,
          "a freshly registered model cannot go straight to Live");
    check(reg.promote(a, ModelStage::Shadow).has_value()
          && reg.promote(a, ModelStage::Canary).has_value()
          && reg.promote(a, ModelStage::Live).has_value(),
          "it goes Registered -> Shadow -> Canary -> Live, which is what stops"
          " quarterly retraining becoming a slow-motion self-inflicted loss");
    check(reg.live("tenmin_forecast") != nullptr
          && reg.live("tenmin_forecast")->key == a,
          "and the live version is findable by name");

    // The test loss is recorded once, matching P8-03.
    check(reg.record_test(a, 0.98).has_value(), "the test loss records");
    check(reg.record_test(a, 0.42).error() == ModelRegistryError::Immutable,
          "and cannot be overwritten with a better one -- immutability is what"
          " makes last Tuesday's decision still explicable after this"
          " Tuesday's retrain");

    ModelRecord blank{};
    check(blank.stage == ModelStage::Unknown && !blank.key.complete(),
          "a zeroed record is Unknown with an incomplete key, so it cannot"
          " pass for a live model");
}

} // namespace

int main()
{
    std::printf("altair training harness and model registry tests\n");
    early_stopping_makes_its_own_validation_score_optimistic();
    the_best_checkpoint_is_not_the_last_one();
    a_weights_only_hash_is_not_a_model_hash();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
