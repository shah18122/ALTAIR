// P5-02 acceptance tests for features/vector.hpp and features/kinematics.hpp.
//
// Test 1 is the card: a feature that is not ready is ABSENT, not zero. Zero is
// an ordinary value for almost everything a builder produces, so a vector that
// fills unready slots with 0.0 hands a model a plausible number for a feature
// that has none.
//
// Test 3 is the second one that matters: Hurst is fed RETURNS here, once,
// where it can be seen -- P3-06c reports 0.96 on prices and 0.50 on the same
// data's increments.
//
// No check description here may contain the substring FAIL.

#include <features/kinematics.hpp>

#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cmath>

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

bool rel_near(double a, double b, double tol)
{
    const double d = std::fabs(a - b);
    const double m = std::fabs(b) > 1.0 ? std::fabs(b) : 1.0;
    return d / m <= tol;
}

class Lcg {
public:
    explicit Lcg(std::uint64_t s) noexcept : s_(s) {}
    double sym() noexcept {
        s_ = s_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return 2.0 * (static_cast<double>(s_ >> 11)
                      * (1.0 / 9007199254740992.0)) - 1.0;
    }
private:
    std::uint64_t s_;
};

} // namespace

using namespace altair;

namespace {

constexpr std::size_t kN = 1024;
constexpr std::int64_t kMs = 1'000'000;
Timestamp at(std::int64_t ms) { return Timestamp{ms * kMs}; }

// ── 1 ────────────────────────────────────────────────────────────────────
// THE card.
void an_unready_feature_is_absent_not_zero()
{
    std::printf("\n1 an_unready_feature_is_absent_not_zero\n");
    FeatureVector v{0xABCD, 4, at(0)};
    check(v.present_count() == 0 && !v.complete(),
          "a fresh vector has nothing in it and knows it is incomplete");

    const auto miss = v.value(2);
    check(!miss && miss.error() == VectorError::Absent,
          "reading an unset feature is an ERROR, not a zero -- a caller"
          " cannot read a feature without being handed the question of"
          " whether it is there");

    // Zero is a perfectly good VALUE, and must be distinguishable from absent.
    check(v.set(2, 0.0).has_value(), "zero can be stored");
    check(v.has(2) && v.value(2).value() == 0.0,
          "and reads back as a present zero -- a z-score of zero means at the"
          " mean, and it must not be confused with having no z-score");
    check(v.present_count() == 1 && !v.complete(),
          "with the rest still absent");

    // A NaN is not the answer either.
    const auto nan = v.set(3, std::nan(""));
    check(!nan && nan.error() == VectorError::NotFinite,
          "a NaN is REFUSED rather than stored -- it propagates, it is not"
          " comparable, and half the code that meets one turns it back into"
          " zero");
    check(!v.has(3), "so the slot stays absent, which is the honest answer");

    (void)v.set(0, 1.0); (void)v.set(1, -2.5); (void)v.set(3, 9.0);
    check(v.complete() && v.present_count() == 4,
          "once every slot is filled the vector reports complete");

    // Rule 10: the vector carries the registry that shaped it.
    check(v.feature_version() == 0xABCD, "it carries its feature_version");
    check(v.require_version(0xABCD).has_value(), "which a model can require");
    const auto wrong = v.require_version(0x1234);
    check(!wrong && wrong.error() == VectorError::VersionMismatch,
          "and a vector from a DIFFERENT registry is refused, rather than a"
          " model trusting that the wiring is right");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void warmup_is_per_feature_not_per_vector()
{
    std::printf("\n2 warmup_is_per_feature_not_per_vector\n");
    PriceHistory<kN> h;
    // A steady drift of 500 paise/second, sampled every 10 ms.
    for (std::int64_t ms = 0; ms <= 400; ms += 10) {
        (void)h.push(at(ms), 2'408'000.0 + 500.0 * (ms / 1000.0));
    }
    std::printf("    history: %zu points spanning %.2f s\n",
                h.size(), static_cast<double>(h.span().raw()) / 1e9);

    // A 200 ms feature is ready; a 5-second one is not.
    const auto fast = kinematics_at(h, at(400), duration::millis(200));
    const auto slow = kinematics_at(h, at(400), duration::seconds(5));
    check(fast.has_value(), "a 200 ms scale has enough history");
    check(!slow && slow.error() == BuilderError::WarmingUp,
          "a 5-second scale does not, and says WarmingUp");
    check(rel_near(fast->velocity, 500.0, 1e-6),
          "and the ready one recovers the drift exactly");

    // Both written into one vector: the fast slot fills, the slow one does not.
    FeatureVector v{1, 4, at(400)};
    KinematicSlots fs{}; fs.velocity = 0;
    KinematicSlots ss{}; ss.velocity = 1;
    (void)build_kinematics(h, at(400), duration::millis(200), fs, v);
    (void)build_kinematics(h, at(400), duration::seconds(5), ss, v);
    check(v.has(0) && !v.has(1),
          "in ONE vector the fast feature is present and the slow one absent"
          " -- gating the whole vector on the slowest would throw away the"
          " first five minutes of every session for every fast model");
    check(!v.complete(), "so the vector reports itself incomplete");
    std::printf("    -> 5-second volatility computed from 0.4 s of data is not"
                " a small version\n       of that feature; it is a different"
                " and much noisier one.\n");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// Hurst is fed increments, once, where it can be seen.
void hurst_is_fed_returns_not_prices()
{
    std::printf("\n3 hurst_is_fed_returns_not_prices\n");
    PriceHistory<kN> h;
    Lcg rng{0x48520001ULL};
    double px = 2'408'000.0;
    for (std::int64_t i = 0; i < 900; ++i) {
        px += 40.0 * rng.sym();               // a random walk in PRICE
        (void)h.push(at(i * 10), px);
    }

    FeatureVector v{1, 2, at(9000)};
    DistributionSlots s{};
    s.hurst = 0;
    s.hurst_stderr = 1;
    const auto n = build_hurst(h, s, v);
    check(n.has_value(), "the Hurst builder runs");
    if (!n) { return; }
    const double H = v.value(0).value();
    const double se = v.value(1).value();
    std::printf("    a random walk in PRICE: H = %.4f +/- %.4f\n", H, se);
    check(std::fabs(H - 0.5) < 0.12,
          "the builder returns H near 0.5 -- because it differences the window"
          " first. Fed the prices directly, P3-06c returns 0.96 on this same"
          " data, and that is a restatement of what integration does");
    check(v.has(1) && se > 0.0,
          "and the STANDARD ERROR is carried as a feature of its own, so a"
          " model sees how little the estimate is worth (ROADMAP section 3)");
    // A 2-sigma test on a TRUE null flags about 5% of samples by
    // construction -- that is what a 2-sigma threshold means, and this
    // particular walk lands at 2.05 sigma. Asserting that one sample is never
    // flagged would be asserting away the false-positive rate. So the
    // calibration is what gets checked: many independent walks, and a flag
    // rate that stays low rather than zero.
    int flagged = 0;
    constexpr int kTrials = 40;
    for (int t = 0; t < kTrials; ++t) {
        PriceHistory<kN> g;
        Lcg r{0x9E3779B9ULL * static_cast<std::uint64_t>(t + 1)};
        double p = 2'408'000.0;
        for (std::int64_t i = 0; i < 900; ++i) {
            p += 40.0 * r.sym();
            (void)g.push(at(i * 10), p);
        }
        FeatureVector fv{1, 2, at(9000)};
        if (!build_hurst(g, s, fv)) { continue; }
        const HurstEstimate e2{fv.value(0).value(), fv.value(1).value(),
                               0.99, 8, 900};
        if (hurst_departs_from_random_walk(e2)) { ++flagged; }
    }
    std::printf("    %d of %d independent random walks flagged at 2 sigma"
                " (%.0f%%)\n", flagged, kTrials,
                100.0 * flagged / kTrials);
    check(flagged < kTrials / 4,
          "across 40 independent random walks the flag rate stays low -- a"
          " 2-sigma test on a true null fires sometimes, and an estimator"
          " that NEVER fired would have an error bar that was too wide to"
          " be useful");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void a_refused_estimator_leaves_its_slot_absent()
{
    std::printf("\n4 a_refused_estimator_leaves_its_slot_absent\n");
    PriceHistory<kN> h;
    // A perfectly CONSTANT price. Variance is zero; skew and kurtosis do not
    // exist, and P3-06 refuses them rather than returning zero.
    for (std::int64_t i = 0; i < 200; ++i) {
        (void)h.push(at(i * 10), 2'408'000.0);
    }
    FeatureVector v{1, 6, at(2000)};
    DistributionSlots s{};
    s.mean = 0; s.stddev = 1; s.skew = 2; s.kurtosis = 3; s.zscore = 4;
    const auto n = build_distribution(h, s, v);
    check(n.has_value(), "the builder runs on a constant series");
    std::printf("    constant price: mean %s, stddev %s, skew %s,"
                " kurtosis %s, z %s\n",
                v.has(0) ? "present" : "ABSENT", v.has(1) ? "present" : "ABSENT",
                v.has(2) ? "present" : "ABSENT", v.has(3) ? "present" : "ABSENT",
                v.has(4) ? "present" : "ABSENT");
    check(v.has(0) && v.has(1), "mean and standard deviation exist");
    check(v.value(1).value() == 0.0, "and the deviation is a present zero");
    check(!v.has(2) && !v.has(3),
          "but skew and kurtosis are ABSENT -- a constant series has no shape,"
          " and P3-06's refusal is carried through rather than smoothed into"
          " a zero that a model would read as symmetric");
    check(!v.has(4),
          "and the z-score is absent too, rather than being infinite or zero");

    // With real variation they all appear.
    PriceHistory<kN> g;
    Lcg rng{7};
    for (std::int64_t i = 0; i < 200; ++i) {
        (void)g.push(at(i * 10), 2'408'000.0 + 300.0 * rng.sym());
    }
    FeatureVector w{1, 6, at(2000)};
    (void)build_distribution(g, s, w);
    check(w.has(2) && w.has(3) && w.has(4),
          "while a series with real variation fills all five, so the absences"
          " above are about the data and not about the builder");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void out_of_order_ticks_are_refused_and_counted()
{
    std::printf("\n5 out_of_order_ticks_are_refused_and_counted\n");
    PriceHistory<kN> h;
    (void)h.push(at(100), 2'408'000.0);
    (void)h.push(at(200), 2'408'100.0);
    const auto back = h.push(at(150), 2'407'000.0);
    check(!back, "a tick from the past is refused");
    check(h.out_of_order() == 1 && h.size() == 2,
          "counted, and not folded in -- the derivative builders fit against"
          " timestamps, and an out-of-order point would bend the fit toward a"
          " past that already happened");
    const auto same = h.push(at(200), 2'408'200.0);
    check(!same && h.out_of_order() == 2,
          "and a tick at exactly the last timestamp is refused too");

    const auto nan = h.push(at(300), std::nan(""));
    check(!nan && h.rejected() == 1 && h.size() == 2,
          "a non-finite price is rejected and counted separately from an"
          " out-of-order one -- they are different faults");

    check(h.push(at(300), 2'408'200.0).has_value() && h.size() == 3,
          "and a proper later tick is still accepted afterwards");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void a_skipped_slot_is_never_written()
{
    std::printf("\n6 a_skipped_slot_is_never_written\n");
    PriceHistory<kN> h;
    for (std::int64_t ms = 0; ms <= 600; ms += 10) {
        (void)h.push(at(ms), 2'408'000.0 + 500.0 * (ms / 1000.0));
    }
    FeatureVector v{1, 4, at(600)};
    (void)v.set(1, 12345.0);              // something else owns slot 1

    KinematicSlots s{};
    s.velocity = 0;                       // only velocity is registered
    // acceleration, jerk and trend_deviation stay kSkip
    const auto n = build_kinematics(h, at(600), duration::millis(400), s, v);
    check(n.has_value() && *n == 1, "exactly one slot was written");
    check(v.has(0), "velocity landed");
    check(v.value(1).value() == 12345.0,
          "and slot 1 is UNTOUCHED -- a builder that invented a slot would"
          " write over whatever the registry actually put there");
    check(!v.has(2) && !v.has(3), "the unregistered slots stay absent");
}

} // namespace

int main()
{
    std::printf("altair features kinematics tests\n");
    an_unready_feature_is_absent_not_zero();
    warmup_is_per_feature_not_per_vector();
    hurst_is_fed_returns_not_prices();
    a_refused_estimator_leaves_its_slot_absent();
    out_of_order_ticks_are_refused_and_counted();
    a_skipped_slot_is_never_written();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
