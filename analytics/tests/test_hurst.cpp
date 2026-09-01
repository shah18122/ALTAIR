// P3-06c acceptance tests for analytics/hurst.hpp.
//
// A Hurst estimator is trivial to test into a false sense of security: feed it
// something obviously trending, watch H come back above 0.5, ship it. The
// checks that matter are the ones on data with a KNOWN answer, especially the
// null:
//
//   * independent noise must return 0.5, and the uncorrected estimator is run
//     alongside so its bias is measured rather than described;
//   * the same dataset fed as prices and as returns, because that single
//     mistake produces a confident 0.99 out of pure noise;
//   * AR(1) with known sign, in both directions;
//   * and the error bar, which is what decides whether any of it counts.
//
// No check description here may contain the substring FAIL.

#include <analytics/hurst.hpp>

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

class Gauss {
public:
    explicit Gauss(std::uint64_t seed) noexcept : s_(seed) {}
    double next() noexcept {
        if (have_) { have_ = false; return spare_; }
        double u1 = uniform();
        const double u2 = uniform();
        if (u1 < 1e-300) { u1 = 1e-300; }
        const double r = std::sqrt(-2.0 * std::log(u1));
        constexpr double kTwoPi = 6.28318530717958647692528676655901;
        spare_ = r * std::sin(kTwoPi * u2);
        have_ = true;
        return r * std::cos(kTwoPi * u2);
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

constexpr std::size_t kN = 8192;
double g_noise[kN];      // independent increments
double g_walk[kN];       // their running sum -- a price
double g_ar_pos[kN];     // AR(1), phi = +0.9  (persistent)
double g_ar_neg[kN];     // AR(1), phi = -0.9  (mean reverting)

void build_series()
{
    Gauss g{0xC0FFEEULL};
    double lvl = 0.0;
    double ap = 0.0;
    double an = 0.0;
    for (std::size_t i = 0; i < kN; ++i) {
        const double e = g.next();
        g_noise[i] = e;
        lvl += e;
        g_walk[i] = lvl;                    // a price series, in effect
        ap = 0.9 * ap + e;   g_ar_pos[i] = ap;
        an = -0.9 * an + e;  g_ar_neg[i] = an;
    }
}

/// The UNCORRECTED estimator: regress log(R/S) on log(m) and take the slope
/// as H directly, comparing implicitly against n^0.5. Present only so its
/// bias can be measured.
double uncorrected_hurst(const double* x, std::size_t n)
{
    double lx[24], ly[24];
    int k = 0;
    for (std::size_t m = 16; m <= n / 2 && k < 24; m *= 2) {
        const std::size_t blocks = n / m;
        double acc = 0.0;
        std::size_t used = 0;
        for (std::size_t b = 0; b < blocks; ++b) {
            double rs = 0.0;
            if (altair::detail::block_rs(x + b * m, m, rs)) { acc += rs; ++used; }
        }
        if (used == 0) { continue; }
        lx[k] = std::log(static_cast<double>(m));
        ly[k] = std::log(acc / static_cast<double>(used));
        ++k;
    }
    double sx = 0.0, sy = 0.0;
    for (int i = 0; i < k; ++i) { sx += lx[i]; sy += ly[i]; }
    const double mx = sx / k, my = sy / k;
    double sxy = 0.0, sxx = 0.0;
    for (int i = 0; i < k; ++i) {
        sxy += (lx[i] - mx) * (ly[i] - my);
        sxx += (lx[i] - mx) * (lx[i] - mx);
    }
    return sxy / sxx;
}

} // namespace

using namespace altair;

namespace {

// ── 1 ────────────────────────────────────────────────────────────────────
// The null. Independent noise IS a random walk in the R/S sense, and an
// estimator that cannot return 0.5 here will find structure in anything.
void independent_noise_returns_one_half()
{
    std::printf("\n1 independent_noise_returns_one_half\n");
    const auto e = hurst_rs(g_noise, kN);
    check(e.has_value(), "8192 independent increments estimate cleanly");
    if (!e) { return; }
    std::printf("    H = %.4f +/- %.4f   (%d scales, R2 %.4f, n %zu)\n",
                e->h, e->std_error, e->scales, e->r_squared, e->n);
    check(std::fabs(e->h - 0.5) < 0.05,
          "H is within 0.05 of 0.5 on independent data");
    check(e->r_squared > 0.99,
          "and the raw log-log scaling law fits well, so the slope means"
          " something");
    check(!hurst_departs_from_random_walk(*e),
          "and it is NOT reported as departing from a random walk");

    // The uncorrected estimator on the same data, at four lengths. Its bias
    // is upward and GROWS as the sample shrinks, which is the shape that
    // matters: short windows are where people actually run this.
    std::printf("    uncorrected vs corrected, same data:\n");
    int worse_everywhere = 0;
    double bias_short = 0.0;
    const std::size_t sizes[] = {512, 1024, 4096, 8192};
    for (int i = 0; i < 4; ++i) {
        const auto c = hurst_rs(g_noise, sizes[i]);
        const double raw = uncorrected_hurst(g_noise, sizes[i]);
        if (!c) { continue; }
        std::printf("      n = %5zu   corrected %.4f   uncorrected %.4f"
                    "   bias %+.4f\n", sizes[i], c->h, raw, raw - c->h);
        if (raw > c->h) { ++worse_everywhere; }
        if (i == 0) { bias_short = raw - c->h; }
    }
    check(worse_everywhere == 4,
          "the uncorrected estimator reads higher at every sample length");
    check(bias_short > 0.03,
          "and its upward bias at n=512 exceeds 0.03 -- comparable to the"
          " effects people claim to find, on data that has none");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// Trap 1, exhibited. One dataset, two ways of feeding it.
void a_price_series_reports_nonsense_and_returns_do_not()
{
    std::printf("\n2 a_price_series_reports_nonsense_and_returns_do_not\n");
    const auto levels = hurst_rs(g_walk, kN);
    const auto rets = hurst_rs(g_noise, kN);
    check(levels.has_value() && rets.has_value(), "both series estimate");
    if (!levels || !rets) { return; }
    std::printf("    fed as PRICES  H = %.4f +/- %.4f\n",
                levels->h, levels->std_error);
    std::printf("    fed as RETURNS H = %.4f +/- %.4f\n",
                rets->h, rets->std_error);
    check(levels->h > 0.9,
          "a price series reports H above 0.9 -- a restatement of what"
          " integration does, not evidence about the market");
    check(std::fabs(rets->h - 0.5) < 0.05,
          "and its own increments, the same data, report a random walk");
    check(levels->h - rets->h > 0.4,
          "the two differ by more than 0.4, so the mistake is not subtle in"
          " its consequences even though it is invisible in its output");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void persistence_and_mean_reversion_are_told_apart()
{
    std::printf("\n3 persistence_and_mean_reversion_are_told_apart\n");
    const auto pos = hurst_rs(g_ar_pos, kN);
    const auto neg = hurst_rs(g_ar_neg, kN);
    check(pos.has_value() && neg.has_value(), "both AR(1) series estimate");
    if (!pos || !neg) { return; }
    std::printf("    AR(1) phi=+0.9  H = %.4f +/- %.4f\n",
                pos->h, pos->std_error);
    std::printf("    AR(1) phi=-0.9  H = %.4f +/- %.4f\n",
                neg->h, neg->std_error);
    check(pos->h > 0.5, "positive autocorrelation reads as persistent");
    check(neg->h < 0.5, "negative autocorrelation reads as mean reverting");
    check(hurst_departs_from_random_walk(*pos),
          "and the persistent one is significant against the null");
    check(hurst_departs_from_random_walk(*neg),
          "as is the mean-reverting one");
}

// ── 4 ────────────────────────────────────────────────────────────────────
// The error bar. It has to grow as the sample shrinks, and it has to be big
// enough to stop a short-sample estimate being acted on.
void the_error_bar_grows_as_the_sample_shrinks()
{
    std::printf("\n4 the_error_bar_grows_as_the_sample_shrinks\n");
    const std::size_t sizes[] = {256, 1024, 4096, 8192};
    double se[4] = {};
    int significant = 0;
    for (int i = 0; i < 4; ++i) {
        const auto e = hurst_rs(g_noise, sizes[i]);
        if (!e) { check(false, "short sample estimates"); return; }
        se[i] = e->std_error;
        const bool sig = hurst_departs_from_random_walk(*e);
        if (sig) { ++significant; }
        std::printf("    n = %5zu   H = %.4f +/- %.4f   %s\n",
                    sizes[i], e->h, e->std_error,
                    sig ? "reported as SIGNIFICANT" : "consistent with a walk");
    }
    check(se[0] > se[3],
          "the standard error at n=256 exceeds the one at n=8192");
    check(significant == 0,
          "and independent data is never reported as significant at any"
          " length -- the estimator does not find structure in noise");
    std::printf("    -> at n=256 the error bar is %.3f, wider than most"
                " effects anyone\n       claims to find. That is the number"
                " that decides whether H is usable.\n", se[0]);
}

// ── 5 ────────────────────────────────────────────────────────────────────
void short_and_degenerate_input_is_refused()
{
    std::printf("\n5 short_and_degenerate_input_is_refused\n");
    check(!hurst_rs(g_noise, 127)
          && hurst_rs(g_noise, 127).error() == HurstError::TooShort,
          "127 observations are refused, not estimated");
    check(!hurst_rs(g_noise, 0), "an empty series is refused");
    check(hurst_rs(g_noise, kHurstMinSamples).has_value(),
          "and the documented minimum does estimate");

    static double flat[1024];
    for (double& v : flat) { v = 2'408'000.0; }
    const auto f = hurst_rs(flat, 1024);
    check(!f, "a constant series is refused, not given an exponent");
    check(f.error() == HurstError::Degenerate,
          "and the reason given is Degenerate, since every block divides by"
          " a zero standard deviation");

    static double bad[1024];
    for (std::size_t i = 0; i < 1024; ++i) { bad[i] = g_noise[i]; }
    bad[500] = std::nan("");
    const auto b = hurst_rs(bad, 1024);
    check(!b && b.error() == HurstError::NotFinite,
          "one NaN anywhere in the series refuses the whole estimate --"
          " a Hurst exponent over a hole is not a measurement");
}

// ── 6 ────────────────────────────────────────────────────────────────────
// The Anis-Lloyd expectation is the null this estimator regresses against.
// If it is wrong, everything above is wrong in a way no other check sees.
void the_anis_lloyd_expectation_is_right()
{
    std::printf("\n6 the_anis_lloyd_expectation_is_right\n");
    // Reference values computed independently in Python from
    // Gamma((n-1)/2)/(sqrt(pi)*Gamma(n/2)) * sum_{i=1}^{n-1} sqrt((n-i)/i).
    struct Ref { std::size_t n; double want; };
    const Ref refs[] = {{16, 4.03553068129229},
                        {64, 8.965567522183898},
                        {256, 18.943068380864545},
                        {1024, 38.96932719998295}};
    int matched = 0;
    for (const Ref& r : refs) {
        const double got = detail::anis_lloyd_expected_rs(r.n);
        if (std::fabs(got - r.want) / r.want < 1e-12) { ++matched; }
    }
    check(matched == 4, "E[R/S] matches independent reference values");

    // Anis and Lloyd switch to an asymptotic form above n = 340; this
    // implementation does not, because that form is systematically low and
    // introduces a STEP in the null exactly where blocks straddle it. There
    // must be no discontinuity across 340.
    const double a = detail::anis_lloyd_expected_rs(340);
    const double b = detail::anis_lloyd_expected_rs(341);
    const double asymptotic_341 = 21.978493523208144;   // what the split gives
    std::printf("    E[R/S]: n=340 %.9f  n=341 %.9f   (the abandoned"
                " asymptotic form gives %.9f)\n", a, b, asymptotic_341);
    check(b > a, "E[R/S] is continuous and increasing across n = 340");
    check((b - a) / a < 0.005,
          "with no step -- one exact formula is used at every n");
    check((b - asymptotic_341) / b > 1e-3,
          "and the asymptotic form it replaces really was low enough to"
          " matter, biasing the null downward and so H upward");

    // E[R/S] must grow, and must grow SLOWER than sqrt(n) at these lengths --
    // that gap is precisely the finite-sample bias being corrected for.
    bool growing = true;
    double prev = 0.0;
    for (std::size_t n : {16u, 32u, 64u, 128u, 256u, 512u, 1024u, 4096u}) {
        const double e = detail::anis_lloyd_expected_rs(n);
        if (e <= prev) { growing = false; }
        prev = e;
    }
    check(growing, "E[R/S] increases with n");

    const double e64 = detail::anis_lloyd_expected_rs(64);
    const double e4096 = detail::anis_lloyd_expected_rs(4096);
    const double implied = std::log(e4096 / e64)
                         / std::log(4096.0 / 64.0);
    std::printf("    implied exponent of E[R/S] from n=64 to 4096: %.4f\n",
                implied);
    check(implied > 0.5 && implied < 0.56,
          "the expected R/S of INDEPENDENT data scales above n^0.5 -- which"
          " is the entire bias, and why regressing against n^0.5 finds"
          " persistence in noise");
}

} // namespace

int main()
{
    std::printf("altair analytics hurst tests\n");
    build_series();
    independent_noise_returns_one_half();
    a_price_series_reports_nonsense_and_returns_do_not();
    persistence_and_mean_reversion_are_told_apart();
    the_error_bar_grows_as_the_sample_shrinks();
    short_and_degenerate_input_is_refused();
    the_anis_lloyd_expectation_is_right();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
