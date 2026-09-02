// P8-10, P8-11 and P8-12 acceptance tests.
//
// Test 1 is the serving card: an in-place weight swap lets a concurrent reader
// straddle two models. The test builds the race and counts the torn reads.
//
// Test 2 is the aggregation card: two forecasts at different horizons are not
// two opinions about one thing, and the longer one dominates an unweighted
// mean by construction.
//
// Test 3: the interval a size is derived from carries the correlation among
// members AND the weights' own estimation error.
//
// No check description here may contain the substring FAIL.

#include <models/aggregator.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>

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

} // namespace

using namespace altair;

namespace {

constexpr std::size_t kF = 8;

ModelKey key_for(std::uint64_t fv)
{
    ModelKey k{};
    k.param_hash = 0xABCD;
    k.feature_version = fv;
    k.architecture_hash = 0x1234;
    k.train_from = 0;
    k.train_to = 1000;
    k.seed = 1;
    return k;
}

/// A linear model whose weights are all the same value, so a torn read is
/// immediately visible: a clean inference returns exactly value * sum(x), and
/// anything else means the reader saw two models.
struct Uniform {
    double w = 0.0;
    [[nodiscard]] double operator()(const double* x,
                                    std::size_t n) const noexcept {
        double acc = 0.0;
        for (std::size_t i = 0; i < n; ++i) { acc += w * x[i]; }
        return acc;
    }
};

// ── 1 ────────────────────────────────────────────────────────────────────
// THE serving card.
void an_in_place_swap_lets_a_reader_straddle_two_models()
{
    std::printf("\n1 an_in_place_swap_lets_a_reader_straddle_two_models\n");

    // THE UNSAFE VERSION: weights overwritten in place while a reader runs.
    static TearableModel<kF> shared;
    shared.overwrite(1.0);
    double x[kF];
    for (std::size_t i = 0; i < kF; ++i) { x[i] = 1.0; }

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> torn{0}, reads{0};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const double v = shared(x, kF);
            ++reads;
            // A clean read is 8*1 or 8*2. Anything between is a mixture of
            // both models -- a forecast from a model that never existed.
            if (!near(v, 8.0, 1e-12) && !near(v, 16.0, 1e-12)) {
                torn.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    // Run until the READER has done real work. A fixed writer count finishes
    // before the thread is scheduled at all, which measures nothing.
    for (int i = 0; i < 400000 && reads.load() < 20000; ++i) {
        shared.overwrite(i % 2 == 0 ? 2.0 : 1.0);
    }
    stop.store(true, std::memory_order_relaxed);
    reader.join();

    std::printf("    weights overwritten in place, %zu concurrent reads:"
                " %zu TORN\n",
                reads.load(), torn.load());
    check(reads.load() > 100, "the reader got a usable number of inferences");
    check(torn.load() > 0,
          "and some of them straddled the swap -- a forecast built from part"
          " of one model and part of another, which has never existed and"
          " cannot be reproduced from rule 10's tuple");
    std::printf("    -> it does not crash. The shapes are unchanged, the number"
                " is plausible, and the\n       decision record points at a"
                " model whose weights were only briefly what it\n       says"
                " they were.\n");

    // THE DOUBLE BUFFER: same swap rate, and a reader can never see a mixture.
    static ModelServer<Uniform, kF> server;
    check(server.stage(Uniform{1.0}, key_for(0x99), ModelStage::Live)
              .has_value(), "a model stages into the inactive slot");
    check(server.publish().error() == ServingError::NotWarmed,
          "and cannot be published before it is warmed -- the first inference"
          " after a swap is the slow one, and it would otherwise land on the"
          " first tick after a promotion, which is not a moment anyone chose");
    server.warm(64);
    const auto g1 = server.publish();
    check(g1.has_value() && *g1 == 1, "warmed, it publishes as generation 1");

    FeatureVector v{0x99, kF, Timestamp{7}};
    FeatureIndex idx[kF];
    for (std::size_t i = 0; i < kF; ++i) {
        idx[i] = static_cast<FeatureIndex>(i);
        (void)v.set(idx[i], 1.0);
    }

    std::atomic<bool> stop2{false};
    std::atomic<std::size_t> torn2{0}, reads2{0};
    std::thread reader2([&] {
        while (!stop2.load(std::memory_order_relaxed)) {
            const auto f = server.infer(v, idx);
            if (!f) { continue; }
            ++reads2;
            if (!near(f->value, 8.0, 1e-12) && !near(f->value, 16.0, 1e-12)) {
                torn2.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    std::size_t swaps = 0;
    for (int i = 0; i < 100000 && reads2.load() < 20000; ++i) {
        (void)server.stage(Uniform{i % 2 == 0 ? 2.0 : 1.0}, key_for(0x99),
                           ModelStage::Live);
        server.warm(2);
        (void)server.publish();
        ++swaps;
    }
    stop2.store(true, std::memory_order_relaxed);
    reader2.join();

    std::printf("    double-buffered with one atomic index, %zu concurrent"
                " reads across %zu swaps: %zu torn\n",
                reads2.load(), swaps, torn2.load());
    check(reads2.load() > 100, "the reader got a usable number of inferences");
    check(torn2.load() == 0,
          "and NOT ONE of them straddled a swap -- the writer fills the"
          " inactive slot and publishes with a single release store, and a"
          " reader takes one acquire load and holds that slot for the whole"
          " inference");

    // Serving still refuses the things it should.
    FeatureVector wrong{0x77, kF, Timestamp{7}};
    for (std::size_t i = 0; i < kF; ++i) { (void)wrong.set(idx[i], 1.0); }
    check(server.infer(wrong, idx).error()
          == ServingError::FeatureVersionMismatch,
          "a vector from a different feature registry is refused at serving"
          " time, which is the only place the mismatch is detectable");
    FeatureVector partial{0x99, kF, Timestamp{7}};
    for (std::size_t i = 0; i + 1 < kF; ++i) { (void)partial.set(idx[i], 1.0); }
    check(server.infer(partial, idx).error()
          == ServingError::IncompleteVector,
          "and a vector with an ABSENT feature is refused rather than having a"
          " zero substituted -- the substitute is a claim about the market"
          " disguised as a claim about missing data (P5-02)");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// THE aggregation card.
void forecasts_at_different_horizons_are_not_two_opinions()
{
    std::printf("\n2 forecasts_at_different_horizons_are_not_two_opinions\n");
    // Two models with the SAME underlying signal strength, trained at 10-bar
    // and 375-bar horizons. Under a random walk the return scales with the
    // square root of the horizon, so the long model's numbers are about six
    // times larger for no better reason.
    Member short_h{}, long_h{};
    short_h.forecast.value = 0.30;
    short_h.forecast.std_error = 0.10;
    short_h.forecast.feature_version = 0x55;
    short_h.horizon = 10;
    short_h.weight = 1.0;
    short_h.ready = true;

    long_h = short_h;
    long_h.horizon = 375;
    long_h.forecast.value = 0.30 * std::sqrt(375.0 / 10.0);

    Member mixed[2] = {short_h, long_h};
    const auto bad = aggregate(mixed, 2, 0.5, 0.0);
    check(!bad && bad.error() == AggregateError::HorizonMismatch,
          "combining a 10-bar and a 375-bar forecast is REFUSED -- they are"
          " estimates of DIFFERENT QUANTITIES and their mean estimates"
          " neither");

    // What the naive average would have been, and who dominates it.
    const double naive = 0.5 * (short_h.forecast.value + long_h.forecast.value);
    std::printf("    the same underlying signal at two horizons:\n"
                "      10-bar  forecast %.4f\n"
                "      375-bar forecast %.4f   (%.1fx, purely from sqrt(T))\n"
                "      unweighted mean  %.4f   -- the long model supplies"
                " %.0f%% of it\n",
                short_h.forecast.value, long_h.forecast.value,
                long_h.forecast.value / short_h.forecast.value, naive,
                100.0 * long_h.forecast.value
                    / (short_h.forecast.value + long_h.forecast.value));
    check(long_h.forecast.value > 5.0 * short_h.forecast.value,
          "the longer horizon's number is six times larger for identical"
          " signal strength, so an unweighted mean is dominated by it"
          " regardless of which member is any good");
    std::printf("    -> adding a short-horizon model to that ensemble barely"
                " moves the answer, which\n       looks like the short model"
                " having nothing to say and is actually the units.\n");

    // Rescaling is available and says what it is.
    const auto scaled = rescale_horizon(long_h.forecast.value, 375, 10);
    check(scaled.has_value()
          && near(*scaled, short_h.forecast.value, 1e-9),
          "rescaling the long forecast to the short horizon recovers the"
          " short one exactly, which is the check that the square-root rule is"
          " applied in the right direction");
    std::printf("    -> and it is EXPLICIT, because the square-root rule"
                " assumes independent increments\n       -- exactly the"
                " assumption a forecasting model exists to violate. Doing it"
                "\n       silently inside the aggregator would hide that.\n");

    Member same[2] = {short_h, short_h};
    same[1].forecast.value = 0.20;
    const auto ok = aggregate(same, 2, 0.5, 0.0);
    check(ok.has_value() && ok->horizon == 10,
          "matched horizons aggregate, and the result carries the horizon it"
          " belongs to");
    check(ok.has_value() && near(ok->value, 0.25, 1e-12),
          "with the value being the weighted mean");

    Member fv_clash[2] = {short_h, short_h};
    fv_clash[1].forecast.feature_version = 0x66;
    check(aggregate(fv_clash, 2, 0.5, 0.0).error()
          == AggregateError::FeatureVersionMismatch,
          "and members built from different feature registries are refused"
          " too, for the same reason P8-09 hashes the registry into the key");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void the_interval_carries_both_sources_of_uncertainty()
{
    std::printf("\n3 the_interval_carries_both_sources_of_uncertainty\n");
    Member m[5];
    for (auto& x : m) {
        x = Member{};
        x.forecast.value = 0.40;
        x.forecast.std_error = 0.10;
        x.forecast.feature_version = 0x55;
        x.horizon = 10;
        x.weight = 1.0;
        x.ready = true;
    }

    const auto independent = aggregate(m, 5, 0.0, 0.0);
    const auto correlated = aggregate(m, 5, 0.8, 0.0);
    const auto full = aggregate(m, 5, 0.8, 0.15);
    check(independent.has_value() && correlated.has_value()
          && full.has_value(), "all three aggregate");
    if (!independent || !correlated || !full) { return; }

    std::printf("    five members, each forecasting %.2f with a %.2f standard"
                " error:\n"
                "      assumed independent          se %.4f   n_eff %.2f\n"
                "      measured correlation 0.8     se %.4f   n_eff %.2f\n"
                "      plus 15%% weight-fit error    se %.4f\n",
                m[0].forecast.value, m[0].forecast.std_error,
                independent->std_error, independent->n_effective,
                correlated->std_error, correlated->n_effective,
                full->std_error);

    check(correlated->std_error > 2.0 * independent->std_error,
          "five models sharing their features, their training data and their"
          " labels are not five votes -- at a measured correlation of 0.8 the"
          " honest standard error is more than twice the naive one");
    check(correlated->n_effective < 1.6,
          "and their effective number is under two, not five -- the same"
          " arithmetic P6-04 applied to signals, applied here to models");
    check(full->std_error > correlated->std_error,
          "adding the WEIGHTS' own estimation error widens it further: the"
          " weights were fitted on a finite sample like everything else, and"
          " an interval carrying only the members' uncertainty is too tight by"
          " however much data they were fitted on");

    std::printf("      lower bound at k=1: independent %.4f, correlated %.4f,"
                " full %.4f\n",
                independent->lower_bound(1.0), correlated->lower_bound(1.0),
                full->lower_bound(1.0));
    check(full->lower_bound(1.0) < independent->lower_bound(1.0),
          "so the number a size is derived from shrinks as the honest"
          " uncertainty is added, which is the direction that keeps capital"
          " alive");
    check(full->direction() == 1, "the direction survives");

    // Net of cost, and there is no pre-cost accessor in paise.
    const auto net = net_edge_lower_bound(*full, 4000.0, 1.0, Notional{800});
    check(net.has_value(), "the net lower bound computes");
    if (net) {
        std::printf("    net edge at the lower bound: Rs %.2f (gross bound"
                    " %.4f x Rs 40.00/unit, less Rs 8.00 of cost)\n",
                    static_cast<double>(net->raw()) / 100.0,
                    full->lower_bound(1.0));
    }
    const auto expensive = net_edge_lower_bound(*full, 4000.0, 1.0,
                                                Notional{200'000});
    check(expensive.has_value() && expensive->raw() < 0,
          "and a large enough cost turns it negative before anything"
          " downstream sees it (rule 5)");

    // A member that is not ready is excluded, not counted as zero.
    Member partial[5];
    for (std::size_t i = 0; i < 5; ++i) {
        partial[i] = m[i];
        partial[i].ready = i < 2;
    }
    const auto two = aggregate(partial, 5, 0.8, 0.0);
    check(two.has_value() && two->members == 2,
          "two ready members of five are counted as two, not as five with"
          " three zeros -- a zero forecast is a real forecast meaning 'no"
          " move', and an absent one is not");

    Member none[5] = {};
    check(aggregate(none, 5, 0.8, 0.0).error() == AggregateError::NoMembers,
          "and with nothing ready there is no aggregate rather than an"
          " aggregate of zero");
    check(aggregate(m, 5, -1.0, 0.0).error()
          == AggregateError::NoCorrelation,
          "the correlation among members is required, because assuming zero is"
          " the bug and assuming anything else is a different guess");
}

} // namespace

int main()
{
    std::printf("altair serving, aggregation and confidence interval tests\n");
    an_in_place_swap_lets_a_reader_straddle_two_models();
    forecasts_at_different_horizons_are_not_two_opinions();
    the_interval_carries_both_sources_of_uncertainty();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
