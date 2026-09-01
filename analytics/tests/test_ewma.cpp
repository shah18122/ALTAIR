// P3-06b acceptance tests for analytics/ewma.hpp.
//
// Two things carry this file:
//
//   * The step response of a time-decayed EWMA is ANALYTIC and independent of
//     the sampling pattern: after elapsed T the estimate is exactly
//     target + (seed - target) * exp(-T/tau), whatever the ticks did in
//     between. That is a strong invariant, and it is what a fixed-alpha EWMA
//     cannot satisfy. Test 2 runs the same path at four tick rates.
//
//   * The variance is checked against a KNOWN variance of a generated series,
//     which is the only check that catches the bias. Both the uncorrected
//     recursion and the post-update-mean variant are computed alongside, so
//     the corrections are measured rather than asserted.
//
// No check description here may contain the substring FAIL.

#include <analytics/ewma.hpp>

#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <chrono>

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

/// Box-Muller over a pinned LCG, so the series -- and therefore its known
/// variance -- is identical on every platform and every run.
class Gauss {
public:
    explicit Gauss(std::uint64_t seed) noexcept : s_(seed) {}
    double next(double sd) noexcept {
        if (have_) { have_ = false; return spare_ * sd; }
        double u1 = uniform();
        const double u2 = uniform();
        if (u1 < 1e-300) { u1 = 1e-300; }
        const double r = std::sqrt(-2.0 * std::log(u1));
        constexpr double kTwoPi = 6.28318530717958647692528676655901;
        spare_ = r * std::sin(kTwoPi * u2);
        have_ = true;
        return r * std::cos(kTwoPi * u2) * sd;
    }
private:
    double uniform() noexcept {
        s_ = s_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(s_ >> 11) * (1.0 / 9007199254740992.0);
    }
    std::uint64_t s_;
    double spare_ = 0.0;
    bool have_ = false;
};

} // namespace

using namespace altair;

namespace {

constexpr std::int64_t kMs = 1'000'000;          // ns in a millisecond
const Duration kTau = duration::millis(200);

// ── 1 ────────────────────────────────────────────────────────────────────
void the_step_response_is_the_analytic_one()
{
    std::printf("\n1 the_step_response_is_the_analytic_one\n");
    auto e = Ewma::create(kTau);
    check(e.has_value(), "an EWMA with a positive tau is constructible");
    if (!e) { return; }

    Timestamp t{0};
    check(*e->update(t, 100.0) == 100.0,
          "the first observation IS the estimate -- no zero-seeded ramp");
    check(e->initialised() && e->count() == 1, "and it counts");

    // Step to 200 and hold, sampling every millisecond.
    struct Probe { std::int64_t at_ms; double want; };
    const Probe probes[] = {{200, 163.21205588285576},
                            {500, 191.79150013761011},
                            {1000, 199.32620530009146}};
    std::size_t next = 0;
    for (std::int64_t ms = 1; ms <= 1000; ++ms) {
        t = Timestamp{ms * kMs};
        (void)e->update(t, 200.0);
        if (next < 3 && ms == probes[next].at_ms) {
            const double got = *e->value();
            std::printf("    t=%4lldms  %.11f  (analytic %.11f)\n",
                        static_cast<long long>(ms), got, probes[next].want);
            check(rel_near(got, probes[next].want, 1e-12),
                  "matches target + (seed-target)*exp(-T/tau)");
            ++next;
        }
    }
    check(next == 3, "all three probes were reached");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// The physics gate. The same price path, the same wall-clock second, four
// different tick rates. A time-decayed EWMA must return one answer; a
// fixed-alpha EWMA cannot, and the size of its spread is the point.
void the_same_path_at_four_tick_rates_gives_one_answer()
{
    std::printf("\n2 the_same_path_at_four_tick_rates_gives_one_answer\n");
    // alpha tuned so a FIXED-alpha EWMA is correct at exactly 1 ms ticks.
    const double kFixedAlpha = 1.0 - std::exp(-1.0 / 200.0);
    const double kAnalytic = 200.0 - 100.0 * std::exp(-1000.0 / 200.0);

    const std::int64_t rates_us[] = {100, 1000, 10'000, 50'000};
    double timed[4] = {};
    double fixed[4] = {};
    for (int i = 0; i < 4; ++i) {
        const std::int64_t step_ns = rates_us[i] * 1000;
        auto e = Ewma::create(kTau);
        double f = 100.0;
        Timestamp t{0};
        (void)e->update(t, 100.0);
        for (std::int64_t ns = step_ns; ns <= 1000 * kMs; ns += step_ns) {
            (void)e->update(Timestamp{ns}, 200.0);
            f += kFixedAlpha * (200.0 - f);
        }
        timed[i] = *e->value();
        fixed[i] = f;
        std::printf("    %6lld us ticks (%5lld in 1s):  time-decayed %.9f"
                    "   fixed-alpha %.6f\n",
                    static_cast<long long>(rates_us[i]),
                    static_cast<long long>(1000 * kMs / step_ns),
                    timed[i], fixed[i]);
    }

    bool all_same = true;
    for (int i = 0; i < 4; ++i) {
        if (!rel_near(timed[i], kAnalytic, 1e-12)) { all_same = false; }
    }
    check(all_same,
          "the time-decayed EWMA returns the analytic value at every rate");

    double lo = fixed[0], hi = fixed[0];
    for (int i = 1; i < 4; ++i) {
        if (fixed[i] < lo) { lo = fixed[i]; }
        if (fixed[i] > hi) { hi = fixed[i]; }
    }
    std::printf("    fixed-alpha spread across the same second: %.4f to %.4f"
                " -- %.1f price points\n", lo, hi, hi - lo);
    check(hi - lo > 10.0,
          "the fixed-alpha EWMA disagrees with itself by more than 10 points"
          " on identical data -- the hazard is real, not theoretical");
    check(rel_near(fixed[1], kAnalytic, 1e-9),
          "and it is correct only at the ONE rate its alpha was tuned for");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// The variance, against a known one. Three alphas, and the two wrong variants
// computed alongside so the corrections are measured.
void the_variance_recovers_a_known_variance()
{
    std::printf("\n3 the_variance_recovers_a_known_variance\n");
    constexpr double kSd = 3.0;
    constexpr double kTrue = 9.0;
    constexpr int kN = 400'000;
    constexpr int kBurn = 2'000;

    const double ratios[] = {0.1, 1.0, 2.0};
    int good = 0;
    for (double dt_over_tau : ratios) {
        const std::int64_t step = static_cast<std::int64_t>(
            dt_over_tau * static_cast<double>(kTau.raw()));
        const double alpha = 1.0 - std::exp(-dt_over_tau);

        auto ev = EwmaVariance::create(kTau);
        // The two wrong variants, run on the same stream.
        double m_un = 0.0, v_un = 0.0;      // uncorrected, pre-update mean
        double m_po = 0.0, v_po = 0.0;      // post-update mean
        Gauss g{0xbeef01ULL};
        double acc = 0.0, acc_un = 0.0, acc_po = 0.0;
        int cnt = 0;
        for (int i = 0; i < kN; ++i) {
            const double x = g.next(kSd);
            const Timestamp t{static_cast<std::int64_t>(i) * step};
            (void)ev->update(t, x);
            if (i == 0) { m_un = x; m_po = x; continue; }
            double d = x - m_un;
            m_un += alpha * d;
            v_un = (1.0 - alpha) * (v_un + alpha * d * d);
            d = x - m_po;
            m_po += alpha * d;
            const double dp = x - m_po;     // POST-update mean: the slip
            v_po = (1.0 - alpha) * (v_po + alpha * dp * dp);
            if (i > kBurn) {
                acc += *ev->variance();
                acc_un += v_un;
                acc_po += v_po;
                ++cnt;
            }
        }
        const double corrected = acc / cnt;
        const double uncorrected = acc_un / cnt;
        const double post = acc_po / cnt;
        std::printf("    alpha %.4f:  corrected %.4f (%+.1f%%)   uncorrected"
                    " %.4f (%+.1f%%)   post-mean %.4f (%+.1f%%)\n",
                    alpha, corrected, 100.0 * (corrected / kTrue - 1.0),
                    uncorrected, 100.0 * (uncorrected / kTrue - 1.0),
                    post, 100.0 * (post / kTrue - 1.0));
        if (std::fabs(corrected / kTrue - 1.0) < 0.02) { ++good; }
        // The wrong variants must actually be wrong, or the comparison is
        // not evidence of anything.
        if (dt_over_tau >= 1.0) {
            check(uncorrected < 0.7 * kTrue,
                  "the uncorrected recursion is badly low at this alpha");
            check(post < uncorrected,
                  "and the post-update-mean variant is lower still");
        }
    }
    check(good == 3,
          "the corrected variance is within 2% of the true one at every alpha");

    // Two observations must give exactly the two-point sample variance. This
    // single number pins the seeding and the correction together.
    auto ev = EwmaVariance::create(kTau);
    check(!ev->variance() && ev->variance().error() == EwmaError::Uninitialised,
          "an empty estimator has no variance");
    (void)ev->update(Timestamp{0}, 10.0);
    check(!ev->variance(), "one observation still has no variance -- not 0.0");
    (void)ev->update(Timestamp{kTau.raw()}, 16.0);
    std::printf("    two points 10 and 16: variance %.12f (want %.1f),"
                " effective n %.6f\n",
                *ev->variance(), 18.0, *ev->effective_n());
    check(rel_near(*ev->variance(), 18.0, 1e-12),
          "two points give exactly (x2-x1)^2/2, the two-point sample variance");
    // NOT 2. The two observations carry unequal weights (1-alpha and alpha),
    // and the effective sample size of unequal weights is strictly below
    // their count -- it reaches 2 only when both weigh one half.
    check(*ev->effective_n() > 1.0 && *ev->effective_n() < 2.0,
          "with unequal weights the effective sample size is between 1 and 2");

    // The two-point result above is not a coincidence of that one alpha. For
    // ANY weights w1 + w2 = 1 the corrected estimator gives
    //   w1*w2*(x1-x2)^2 / (1 - w1^2 - w2^2) = (x1-x2)^2 / 2,
    // because the denominator is exactly 2*w1*w2. Weight-independence is the
    // property worth pinning, so it is checked across four spacings.
    int exact = 0;
    for (std::int64_t frac : {1LL, 20LL, 200LL, 4000LL}) {
        auto t2 = EwmaVariance::create(kTau);
        (void)t2->update(Timestamp{0}, 10.0);
        (void)t2->update(Timestamp{frac * kMs}, 16.0);
        if (rel_near(*t2->variance(), 18.0, 1e-9)) { ++exact; }
    }
    check(exact == 4,
          "and it gives (x2-x1)^2/2 at every spacing, from 1 ms to 20 taus --"
          " the correction is weight-independent");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void time_going_backwards_is_refused_and_counted()
{
    std::printf("\n4 time_going_backwards_is_refused_and_counted\n");
    auto e = Ewma::create(kTau);
    (void)e->update(Timestamp{0}, 100.0);
    (void)e->update(Timestamp{100 * kMs}, 200.0);
    const double before = *e->value();

    const auto back = e->update(Timestamp{50 * kMs}, 900.0);
    check(!back && back.error() == EwmaError::TimeWentBackwards,
          "a tick from the past is refused");
    check(*e->value() == before,
          "and does not move the estimate -- a negative dt gives alpha > 1,"
          " which overshoots rather than averages");
    check(e->out_of_order() == 1, "the out-of-order tick is COUNTED");
    check(e->count() == 2, "and is not counted as an observation");
    check(e->last() == Timestamp{100 * kMs},
          "the last-accepted timestamp does not move backwards either");

    // It must still accept a later tick afterwards -- refusing one bad tick
    // must not wedge the estimator.
    const auto ok = e->update(Timestamp{200 * kMs}, 200.0);
    check(ok.has_value(), "a later tick is accepted after a refusal");

    auto ev = EwmaVariance::create(kTau);
    (void)ev->update(Timestamp{0}, 1.0);
    (void)ev->update(Timestamp{100 * kMs}, 2.0);
    check(!ev->update(Timestamp{1}, 3.0), "the variance estimator refuses too");
    check(ev->out_of_order() == 1, "and counts it");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void coincident_ticks_carry_zero_weight_and_are_counted()
{
    std::printf("\n5 coincident_ticks_carry_zero_weight_and_are_counted\n");
    auto e = Ewma::create(kTau);
    (void)e->update(Timestamp{0}, 100.0);
    (void)e->update(Timestamp{100 * kMs}, 200.0);
    const double before = *e->value();
    for (int i = 0; i < 10; ++i) {
        (void)e->update(Timestamp{100 * kMs}, 999.0);
    }
    check(*e->value() == before,
          "ten ticks at the same nanosecond do not move a TIME-weighted"
          " average, which is the correct answer and not a special case");
    check(e->coincident() == 10, "and all ten are counted");
    check(e->count() == 12, "they are accepted, not refused");

    // The next real tick must use the CURRENT price, not a stale one.
    (void)e->update(Timestamp{200 * kMs}, 200.0);
    check(*e->value() > before,
          "and the next spaced tick moves the estimate normally");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void degenerate_construction_and_input_are_refused()
{
    std::printf("\n6 degenerate_construction_and_input_are_refused\n");
    check(!Ewma::create(Duration{0})
          && Ewma::create(Duration{0}).error() == EwmaError::NonPositiveTau,
          "tau = 0 cannot construct an EWMA");
    check(!Ewma::create(Duration{-1}), "a negative tau cannot either");
    check(!EwmaVariance::create(Duration{0}),
          "nor can the variance estimator");

    auto e = Ewma::create(kTau);
    check(!e->value() && e->value().error() == EwmaError::Uninitialised,
          "an unfed EWMA reports Uninitialised, not 0.0");
    const double nan = std::nan("");
    check(!e->update(Timestamp{0}, nan)
          && e->update(Timestamp{0}, nan).error() == EwmaError::NotFinite,
          "a NaN observation is refused");
    check(e->rejected() == 2 && e->count() == 0,
          "refused observations are counted and do not initialise it");
    check(!e->initialised(),
          "a NaN does not sneak the estimator into an initialised state");
}

// ── 7 ────────────────────────────────────────────────────────────────────
void half_life_and_tau_convert_both_ways()
{
    std::printf("\n7 half_life_and_tau_convert_both_ways\n");
    const Duration hl = half_life_of(kTau);
    std::printf("    tau 200ms -> half-life %lld ns (%.4f ms)\n",
                static_cast<long long>(hl.raw()),
                static_cast<double>(hl.raw()) / 1e6);
    check(std::llabs(hl.raw() - 138'629'436LL) <= 1,
          "half-life of a 200 ms tau is 138.63 ms");
    const Duration back = tau_for_half_life(hl);
    check(std::llabs(back.raw() - kTau.raw()) <= 2,
          "and converting back returns the original tau");

    // The definition, checked against the estimator rather than the formula:
    // after one half-life the seed's influence must be exactly halved.
    auto e = Ewma::create(kTau);
    (void)e->update(Timestamp{0}, 0.0);
    (void)e->update(Timestamp{hl.raw()}, 100.0);
    std::printf("    after one half-life, a 0->100 step reads %.9f\n",
                *e->value());
    check(rel_near(*e->value(), 50.0, 1e-8),
          "after one half-life a 0-to-100 step is exactly half way");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void the_cost_is_known()
{
    std::printf("\n8 the_cost_is_known\n");
    constexpr int kReps = 1'000'000;
    auto e = Ewma::create(kTau);
    auto ev = EwmaVariance::create(kTau);
    double sink = 0.0;

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kReps; ++i) {
        const auto r = e->update(Timestamp{static_cast<std::int64_t>(i) * kMs},
                                 2'408'000.0 + static_cast<double>(i % 41));
        if (r) { sink += *r; }
    }
    auto t1 = std::chrono::steady_clock::now();
    const double mean_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count())
        / kReps;

    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kReps; ++i) {
        const auto r = ev->update(Timestamp{static_cast<std::int64_t>(i) * kMs},
                                  2'408'000.0 + static_cast<double>(i % 41));
        if (r) { sink += *r; }
    }
    t1 = std::chrono::steady_clock::now();
    const double var_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count())
        / kReps;

    std::printf("    Ewma::update %.1f ns   EwmaVariance::update %.1f ns"
                "   [sink %.3e]\n", mean_ns, var_ns, sink);
    check(sink != 0.0, "the benchmark loops were not optimised away");
    check(mean_ns < 100.0, "an EWMA update stays under 100 ns");
    check(var_ns < 150.0, "an EWMA variance update stays under 150 ns");
    std::printf("    (the exp() dominates; a fixed-alpha update avoids it,"
                " which is the\n     only thing it is actually better at)\n");
}

} // namespace

int main()
{
    std::printf("altair analytics ewma tests\n");
    the_step_response_is_the_analytic_one();
    the_same_path_at_four_tick_rates_gives_one_answer();
    the_variance_recovers_a_known_variance();
    time_going_backwards_is_refused_and_counted();
    coincident_ticks_carry_zero_weight_and_are_counted();
    degenerate_construction_and_input_are_refused();
    half_life_and_tau_convert_both_ways();
    the_cost_is_known();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
