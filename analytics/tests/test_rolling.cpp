// P3-06a acceptance tests for analytics/rolling.hpp.
//
// The checks that matter here are not "does the mean equal the mean". They
// are the ones where a wrong answer is a plausible answer:
//
//   * the textbook variance formula run side by side with Welford on real
//     paise magnitudes, so the failure is exhibited rather than described;
//   * the subtractive O(1) rolling update run side by side with exact
//     recomputation, showing both its drift AND its path dependence;
//   * short and constant series, where the tempting answer is 0.0.
//
// No check description here may contain the substring FAIL.

#include <analytics/rolling.hpp>

#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <chrono>
#include <utility>
#include <limits>

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

// A tiny deterministic PRNG, so the corpus is identical on every platform and
// a regression is reproducible. std::mt19937 would also do, but its stream is
// not what is being tested and pinning it here keeps the test self-contained.
class Lcg {
public:
    explicit Lcg(std::uint64_t seed) noexcept : s_(seed) {}
    /// Uniform integer in [-range, range].
    std::int64_t pm(std::int64_t range) noexcept {
        s_ = s_ * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint64_t v = (s_ >> 33);
        return static_cast<std::int64_t>(v % (2ULL * static_cast<std::uint64_t>(range) + 1ULL))
             - range;
    }
private:
    std::uint64_t s_;
};

// The textbook formula, present ONLY so the test can show it losing.
double naive_variance(const double* xs, std::size_t n)
{
    double s = 0.0;
    double s2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) { s += xs[i]; s2 += xs[i] * xs[i]; }
    const double m = s / static_cast<double>(n);
    return s2 / static_cast<double>(n) - m * m;
}

// Neumaier compensated summation. Recovers the rounding lost at each step and
// adds it back at the end, so the result is correct to within an ulp of the
// true sum regardless of how many terms were added.
class Compensated {
public:
    void add(double v) noexcept {
        const double t = s_ + v;
        if (std::fabs(s_) >= std::fabs(v)) { c_ += (s_ - t) + v; }
        else                               { c_ += (v - t) + s_; }
        s_ = t;
    }
    [[nodiscard]] double value() const noexcept { return s_ + c_; }
private:
    double s_ = 0.0;
    double c_ = 0.0;
};

// The referee for every variance in this file.
//
// NOT `long double`. On MSVC `long double` IS `double` -- 64 bits, no extra
// precision at all -- which is already carried debt in prompts/LEDGER.md
// against P0-01. A reference built on it would silently be the very thing it
// is meant to check, and an earlier draft of this file made exactly that
// mistake: the "high precision" reference agreed with plain summation to the
// bit, so the plain sum scored a perfect zero and Kahan appeared to be worse.
//
// Two passes AND compensation, which is neither of the algorithms under test:
// the two-pass structure removes the conditioning error that breaks the naive
// formula, and the compensation removes the accumulation error that a plain
// sum carries. Independent of both candidates, so it can referee between them.
double reference_variance(const double* xs, std::size_t n)
{
    Compensated s;
    for (std::size_t i = 0; i < n; ++i) { s.add(xs[i]); }
    const double m = s.value() / static_cast<double>(n);
    Compensated a;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = xs[i] - m;
        a.add(d * d);
    }
    return a.value() / static_cast<double>(n);
}

} // namespace

using namespace altair;

namespace {

// ── 1 ────────────────────────────────────────────────────────────────────
void matches_independent_reference_moments()
{
    std::printf("\n1 matches_independent_reference_moments\n");
    const double xs[] = {12.0, 7.5, 19.25, 3.0, 22.75, 8.5,
                         15.0, 4.25, 30.5, 6.0, 11.75, 25.0};
    constexpr std::size_t kN = sizeof(xs) / sizeof(xs[0]);

    RunningMoments r;
    RollingWindow<kN> w;
    for (double x : xs) { r.add(x); w.push(x); }

    check(r.count() == kN, "every observation was counted");
    check(w.full() && w.size() == kN, "the window filled exactly");

    // References computed in Python from the same twelve values.
    check(rel_near(*r.mean(), 13.791666666666666, 1e-15), "mean");
    check(rel_near(*r.variance(), 71.6232638888889, 1e-14),
          "population variance divides by n");
    check(rel_near(*r.sample_variance(), 78.1344696969697, 1e-14),
          "sample variance divides by n-1");
    check(rel_near(*r.stddev(), 8.839370435555335, 1e-14), "sample stddev");
    check(rel_near(*r.skewness(), 0.5424638322797647, 1e-12), "skewness");
    check(rel_near(*r.excess_kurtosis(), -0.9050320476941187, 1e-12),
          "EXCESS kurtosis is 0 for a Gaussian, not 3");
    check(rel_near(*r.zscore(20.0), 0.7023501705914528, 1e-13), "z-score");

    // The window and the running accumulator must agree exactly in intent --
    // two independent implementations of the same statistics.
    check(rel_near(*w.mean(), *r.mean(), 1e-14),
          "window mean agrees with the streaming mean");
    check(rel_near(*w.variance(), *r.sample_variance(), 1e-13),
          "window variance agrees with the streaming sample variance");
    check(rel_near(*w.skewness(), *r.skewness(), 1e-12),
          "window skewness agrees with the streaming skewness");
    check(rel_near(*w.excess_kurtosis(), *r.excess_kurtosis(), 1e-12),
          "window kurtosis agrees with the streaming kurtosis");
    check(*w.min() == 3.0 && *w.max() == 30.5, "window min and max");
    check(*w.last() == 25.0 && *w.oldest() == 12.0,
          "the ring reports its ends the right way round");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// The reason this file exists. Run the textbook formula next to Welford on
// magnitudes the ledger actually holds, and require the textbook one to be
// visibly wrong -- otherwise the test is not exercising the hazard.
void welford_survives_where_the_textbook_formula_dies()
{
    std::printf("\n2 welford_survives_where_the_textbook_formula_dies\n");
    struct Case {
        const char* what;
        double base;
        std::int64_t jitter;
        std::size_t n;
        double min_naive_rel_err;   // the naive formula must be AT LEAST this bad
    };
    const Case cases[] = {
        {"Rs     24,080 +/- 5 paise", 2'408'000.0,       5, 50'000, 0.02},
        {"Rs     24,080 +/- 1 paise", 2'408'000.0,       1, 50'000, 0.50},
        {"Rs    240,800 +/- 1 paise", 24'080'000.0,      1, 50'000, 1.00},
        {"Rs  1,00,00,000 +/- 5 p  ", 100'000'000'000.0, 5, 20'000, 100.0},
    };

    static double buf[50'000];
    int welford_ok = 0;
    int naive_bad = 0;
    double worst_ratio = 0.0;
    for (const Case& c : cases) {
        Lcg rng{0x5eed1234ULL};
        RunningMoments r;
        for (std::size_t i = 0; i < c.n; ++i) {
            buf[i] = c.base + static_cast<double>(rng.pm(c.jitter));
            r.add(buf[i]);
        }
        const double ref = reference_variance(buf, c.n);
        const double wel = *r.variance();
        const double nai = naive_variance(buf, c.n);
        const double we = std::fabs(wel - ref) / ref;
        const double ne = std::fabs(nai - ref) / ref;
        std::printf("    %s  kappa %.1e  welford %.1e  naive %.1e%s\n",
                    c.what, c.base / std::sqrt(ref), we, ne,
                    nai < 0.0 ? "   NEGATIVE VARIANCE" : "");
        if (we < 1e-4) { ++welford_ok; }
        if (ne >= c.min_naive_rel_err) { ++naive_bad; }
        if (we > 0.0 && ne / we > worst_ratio) { worst_ratio = ne / we; }
    }
    check(welford_ok == 4, "Welford is within 1e-4 relative in all four cases");
    check(naive_bad == 4,
          "the textbook formula is visibly wrong in all four -- the hazard is real");
    check(worst_ratio > 1000.0,
          "and Welford beats it by at least three orders of magnitude");
    std::printf("    Welford is up to %.1ex more accurate here. Neither is"
                " exact: the\n    condition number of a variance is"
                " ~base/stddev, and error grows as\n    kappa*eps for Welford"
                " against kappa^2*eps for the textbook form.\n", worst_ratio);

    // The specific one that does not announce itself.
    Lcg rng{0x5eed1234ULL};
    for (std::size_t i = 0; i < 50'000; ++i) {
        buf[i] = 24'080'000.0 + static_cast<double>(rng.pm(1));
    }
    const double ref = reference_variance(buf, 50'000);
    const double nai = naive_variance(buf, 50'000);
    check(nai > 0.0 && nai > 4.0 * ref,
          "on a quiet high-priced counter the naive answer is positive, finite,"
          " and several times too large");
    std::printf("    -> naive stddev %.4f vs true %.4f paise: a vol estimate"
                " %.1fx too high\n",
                std::sqrt(nai), std::sqrt(ref), std::sqrt(nai / ref));
}

// ── 3 ────────────────────────────────────────────────────────────────────
void short_series_are_refused_not_zeroed()
{
    std::printf("\n3 short_series_are_refused_not_zeroed\n");
    RunningMoments r;
    check(!r.mean() && r.mean().error() == StatError::NotEnoughData,
          "an empty accumulator has no mean");
    check(!r.variance(), "an empty accumulator has no variance");
    r.add(5.0);
    check(r.mean() && *r.mean() == 5.0, "one point has a mean");
    check(!r.variance() && r.variance().error() == StatError::NotEnoughData,
          "one point has no variance -- not 0.0");
    r.add(9.0);
    check(r.variance().has_value(), "two points have a variance");
    check(!r.skewness() && r.skewness().error() == StatError::NotEnoughData,
          "two points have no skewness");
    r.add(2.0);
    check(r.skewness().has_value(), "three points have a skewness");
    check(!r.excess_kurtosis(), "three points have no kurtosis");
    r.add(7.0);
    check(r.excess_kurtosis().has_value(), "four points have a kurtosis");

    RollingWindow<8> w;
    check(!w.mean() && !w.last() && !w.oldest(),
          "an empty window refuses every accessor");
    check(w.size() == 0 && !w.full(), "an empty window knows it is empty");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void a_constant_series_has_no_shape()
{
    std::printf("\n4 a_constant_series_has_no_shape\n");
    RunningMoments r;
    for (int i = 0; i < 100; ++i) { r.add(2'408'000.0); }
    check(r.variance() && *r.variance() == 0.0,
          "a constant series has exactly zero variance");
    check(!r.skewness() && r.skewness().error() == StatError::ZeroSpread,
          "skewness of a constant series is refused, not returned as 0.0");
    check(!r.excess_kurtosis()
          && r.excess_kurtosis().error() == StatError::ZeroSpread,
          "kurtosis of a constant series is refused");
    check(!r.zscore(2'408'001.0)
          && r.zscore(2'408'001.0).error() == StatError::ZeroSpread,
          "a z-score against zero spread is refused, not infinite");

    RollingWindow<16> w;
    for (int i = 0; i < 16; ++i) { w.push(2'408'000.0); }
    check(w.variance() && *w.variance() == 0.0, "same for a window");
    check(!w.skewness() && w.skewness().error() == StatError::ZeroSpread,
          "and its skewness is refused too");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void the_window_holds_exactly_the_last_n()
{
    std::printf("\n5 the_window_holds_exactly_the_last_n\n");
    constexpr std::size_t W = 32;
    RollingWindow<W> w;
    for (int i = 1; i <= 100; ++i) { w.push(static_cast<double>(i)); }
    check(w.size() == W && w.full(), "the window is full and capped at N");
    check(w.total() == 100, "it remembers how many it has ever seen");
    check(*w.oldest() == 69.0 && *w.last() == 100.0,
          "after 100 pushes a 32-window holds 69..100");

    bool ordered = true;
    for (std::size_t i = 0; i < W; ++i) {
        if (w.at(i) != static_cast<double>(69 + i)) { ordered = false; }
    }
    check(ordered, "at(i) walks the ring oldest-first across the wrap");

    // The window's answer must equal a from-scratch computation over exactly
    // those values -- computed here without touching the class under test.
    double direct[W];
    for (std::size_t i = 0; i < W; ++i) {
        direct[i] = static_cast<double>(69 + i);
    }
    const double ref = reference_variance(direct, W) * static_cast<double>(W)
                     / static_cast<double>(W - 1);
    check(rel_near(*w.variance(), ref, 1e-13),
          "window variance equals an independent computation over its contents");
}

// ── 6 ────────────────────────────────────────────────────────────────────
// The design decision behind the O(N) query, demonstrated rather than
// asserted. The subtractive O(1) update is run alongside exact recomputation.
void the_subtractive_update_drifts_and_depends_on_history()
{
    std::printf("\n6 the_subtractive_update_drifts_and_depends_on_history\n");
    constexpr std::size_t W = 64;

    // A subtractive rolling Welford, written here and nowhere else.
    struct Subtractive {
        double ring[W] = {};
        std::size_t head = 0, size = 0;
        double n = 0.0, mean = 0.0, m2 = 0.0;
        void push(double x) {
            n += 1.0;
            double d = x - mean;
            mean += d / n;
            m2 += d * (x - mean);
            if (size == W) {
                const double o = ring[head];
                d = o - mean;
                mean -= d / (n - 1.0);
                n -= 1.0;
                m2 -= d * (o - mean);
            } else {
                ++size;
            }
            ring[head] = x;
            head = (head + 1 == W) ? 0 : head + 1;
        }
        double var() const { return m2 / (n - 1.0); }
    };

    // A volatile stretch, then a dead-quiet one: the shape of the half hour
    // after a news spike settles.
    auto run = [](std::uint64_t seed, std::size_t noisy) {
        Lcg rng{seed};
        Subtractive sub;
        RollingWindow<W> exact;
        for (std::size_t i = 0; i < noisy; ++i) {
            const double x = 2'408'000.0 + static_cast<double>(rng.pm(2'000'000));
            sub.push(x); exact.push(x);
        }
        // The SAME tail values regardless of what came before.
        Lcg tail{0xabcdefULL};
        for (std::size_t i = 0; i < 2000; ++i) {
            const double x = 2'408'000.0 + static_cast<double>(tail.pm(1));
            sub.push(x); exact.push(x);
        }
        return std::pair<double, double>{sub.var(), *exact.variance()};
    };

    const auto a = run(0x1111ULL, 2000);
    const auto b = run(0x2222ULL, 5000);
    std::printf("    history A: subtractive %.12f   exact %.12f   rel %.3e\n",
                a.first, a.second, std::fabs(a.first - a.second) / a.second);
    std::printf("    history B: subtractive %.12f   exact %.12f   rel %.3e\n",
                b.first, b.second, std::fabs(b.first - b.second) / b.second);

    check(rel_near(a.second, b.second, 1e-15),
          "the exact window gives the SAME answer for identical contents");
    check(a.first != b.first,
          "the subtractive window gives DIFFERENT answers for identical"
          " contents -- it is path dependent, so not reproducible (rule 10)");
    const double worst = std::fabs(a.first - a.second) / a.second
                       > std::fabs(b.first - b.second) / b.second
                       ? std::fabs(a.first - a.second) / a.second
                       : std::fabs(b.first - b.second) / b.second;
    check(worst > 1e-6,
          "and its drift after a volatility regime change is not negligible");
}

// ── 7 ────────────────────────────────────────────────────────────────────
// CLAUDE.md requires Kahan on session-long accumulators. Whether it actually
// buys anything is a measurement, so it is measured: the compensated
// accumulator is compared against a long-double reference over a full session
// of ticks. The number is REPORTED, and only a weak bound is asserted --
// claiming an improvement the hardware does not deliver would be worse than
// not compensating.
void the_algorithm_matters_more_than_the_compensation()
{
    std::printf("\n7 the_algorithm_matters_more_than_the_compensation\n");
    constexpr std::size_t kTicks = 2'000'000;   // a busy session
    Lcg rng{0xfeed99ULL};

    // Three variance estimators over the same two million ticks:
    //   A. Welford            -- what this header ships
    //   B. naive sums, Kahan-compensated on BOTH accumulators
    //   C. naive sums, plain
    // B exists to show Kahan doing exactly what it is advertised to do, and
    // still losing badly. Compensation fixes ACCUMULATION error. The naive
    // variance is broken by CONDITIONING, which no compensator addresses.
    RunningMoments r;
    Compensated ks, ks2, ref_sum;
    double ps = 0.0, ps2 = 0.0;
    static double all[kTicks];
    // NOTE the /7.0. With whole paise the sum of two million quotes is about
    // 4.8e12, comfortably inside the 2^53 where double addition of integers
    // is EXACT -- so Kahan and plain agree bit for bit and the comparison
    // below would demonstrate nothing. (That exactness is worth knowing on its
    // own: a paise ledger sums without rounding up to Rs 90,000 crore.) The
    // seventh is there to force real rounding, as a mid-price, a VWAP or a
    // log-return would.
    for (std::size_t i = 0; i < kTicks; ++i) {
        const double x = 2'408'000.0 + static_cast<double>(rng.pm(40)) / 7.0;
        all[i] = x;
        r.add(x);
        ks.add(x); ks2.add(x * x);
        ps += x;   ps2 += x * x;
        ref_sum.add(x);
    }
    const double n = static_cast<double>(kTicks);
    const double ref_mean = ref_sum.value() / n;
    const double ref_var = reference_variance(all, kTicks);

    const double km = ks.value() / n;
    const double pm = ps / n;
    const double var_welford = *r.variance();
    const double var_kahan = ks2.value() / n - km * km;
    const double var_plain = ps2 / n - pm * pm;

    const double e_w = std::fabs(var_welford - ref_var) / ref_var;
    const double e_k = std::fabs(var_kahan - ref_var) / ref_var;
    const double e_p = std::fabs(var_plain - ref_var) / ref_var;

    std::printf("    %zu ticks, reference mean %.10f  variance %.10f\n",
                kTicks, ref_mean, ref_var);
    std::printf("    variance rel error: Welford %.3e   naive+Kahan %.3e"
                "   naive %.3e\n", e_w, e_k, e_p);
    (void)km; (void)pm;

    // Kahan must be shown to work on the thing it IS for, or the comparison
    // above is not evidence of anything. Session turnover in paise, refereed
    // by an exact int64 -- no floating-point referee, so nothing is circular.
    //
    // Both regimes are run, because the honest finding is a THRESHOLD rather
    // than a blanket rule. Doubles represent integers exactly up to
    // 2^53 = 9,007,199,254,740,992 paise, which is Rs 90.07 lakh crore. So:
    //
    //   * NSE cash turnover, about Rs 1 lakh crore a day = 1e14 paise, is 90x
    //     BELOW that. A daily paise accumulator needs no compensation at all,
    //     and adding it would be cargo cult.
    //   * NSE F&O NOTIONAL turnover, about Rs 400 lakh crore a day = 4e16
    //     paise, is 4x ABOVE it. That accumulator does lose paise, every day.
    //
    // Which is worth knowing precisely, because P3-09 carries debt about
    // session-accumulated turnover and this says exactly which accumulator it
    // applies to.
    {
        auto run = [](std::int64_t trade_paise, std::int64_t trades,
                      const char* label) {
            std::int64_t exact = 0;
            double plain = 0.0;
            Compensated comp;
            for (std::int64_t i = 0; i < trades; ++i) {
                const std::int64_t v = trade_paise + (i % 97);  // never uniform
                exact += v;
                plain += static_cast<double>(v);
                comp.add(static_cast<double>(v));
            }
            const double ref = static_cast<double>(exact);
            std::printf("    %s: %lld paise = Rs %.1f lakh crore\n",
                        label, static_cast<long long>(exact), ref / 1e14);
            std::printf("      plain off by %.0f paise, compensated off by"
                        " %.0f paise\n",
                        std::fabs(plain - ref), std::fabs(comp.value() - ref));
            return std::pair<double, double>{std::fabs(plain - ref),
                                             std::fabs(comp.value() - ref)};
        };
        // Rs 50 lakh cash trades to about Rs 1 lakh crore.
        const auto cash = run(500'000'000LL, 200'000LL, "cash turnover  ");
        // Rs 5 crore notional lots to about Rs 400 lakh crore.
        const auto fno  = run(5'000'000'000LL, 8'000'000LL, "F&O notional   ");

        check(cash.first == 0.0 && cash.second == 0.0,
              "below 2^53 paise a plain double sum is EXACT -- a daily cash"
              " turnover needs no compensation");
        check(fno.first > 0.0,
              "past 2^53 paise a plain double sum does lose paise -- the F&O"
              " notional accumulator is genuinely exposed");
        check(fno.second < fno.first,
              "Kahan strictly improves a raw running sum, which is what it"
              " is for");
    }

    check(e_w < e_k * 1e-3,
          "and Welford still beats the compensated naive form by 1000x or"
          " more -- the algorithm matters more than the compensation");
    check(std::fabs(*r.mean() - ref_mean) < 1e-6,
          "the session-long mean stays within a millionth of a paise");
    check(r.count() == kTicks, "every tick was counted");
    std::printf("    -> Kahan belongs on a RAW SUM past 2^53 paise: the F&O"
                " notional\n       accumulator, and the conservation check."
                " NOT inside Welford, and NOT\n       on a daily cash"
                " turnover, which is already exact without it.\n");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void non_finite_input_is_counted_and_the_cost_is_known()
{
    std::printf("\n8 non_finite_input_is_counted_and_the_cost_is_known\n");
    RunningMoments r;
    RollingWindow<16> w;
    for (int i = 0; i < 10; ++i) { r.add(1.0 + i); w.push(1.0 + i); }
    const double nan = std::nan("");
    const double inf = std::numeric_limits<double>::infinity();
    r.add(nan); r.add(inf); w.push(nan); w.push(inf);
    check(r.count() == 10 && r.rejected() == 2,
          "a NaN or infinity is rejected and COUNTED, never absorbed");
    check(w.size() == 10 && w.rejected() == 2, "same for the window");
    check(std::isfinite(*r.mean()) && std::isfinite(*r.variance()),
          "the accumulator is still finite afterwards");

    // Cost. The O(N) query is the deliberate trade in this header, so its
    // price is on the record for whoever builds the feature registry.
    constexpr std::size_t W = 256;
    constexpr int kReps = 200'000;
    RollingWindow<W> bench;
    Lcg rng{7};
    for (std::size_t i = 0; i < W; ++i) {
        bench.push(2'408'000.0 + static_cast<double>(rng.pm(40)));
    }
    double sink = 0.0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kReps; ++i) {
        bench.push(2'408'000.0 + static_cast<double>(rng.pm(40)));
        sink += *bench.variance();
    }
    auto t1 = std::chrono::steady_clock::now();
    const double q_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count())
        / kReps;

    RunningMoments rb;
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kReps; ++i) {
        rb.add(2'408'000.0 + static_cast<double>(rng.pm(40)));
    }
    // Consume the accumulator. Without this the optimiser deletes the whole
    // loop -- the first run of this benchmark reported 0.0 ns, which is the
    // measurement saying "I did not run", not "it is free".
    sink += *rb.mean() + *rb.variance();
    t1 = std::chrono::steady_clock::now();
    const double a_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count())
        / kReps;

    std::printf("    push + variance over a %zu-window: %.0f ns\n", W, q_ns);
    std::printf("    RunningMoments::add (O(1), 4 moments): %.1f ns\n", a_ns);
    std::printf("    [sink %.3e]\n", sink);
    check(q_ns < 5000.0, "an O(N) window query at N=256 stays under 5 us");
    check(a_ns < 200.0, "an O(1) streaming update stays under 200 ns");
}

} // namespace

int main()
{
    std::printf("altair analytics rolling tests\n");
    matches_independent_reference_moments();
    welford_survives_where_the_textbook_formula_dies();
    short_series_are_refused_not_zeroed();
    a_constant_series_has_no_shape();
    the_window_holds_exactly_the_last_n();
    the_subtractive_update_drifts_and_depends_on_history();
    the_algorithm_matters_more_than_the_compensation();
    non_finite_input_is_counted_and_the_cost_is_known();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
