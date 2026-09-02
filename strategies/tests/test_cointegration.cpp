// P10-04 and P10-05 acceptance tests.
//
// Test 1 is the card: two independent random walks are highly CORRELATED and
// not cointegrated; a genuinely tied pair is firmly cointegrated and only
// weakly correlated day to day. A correlation screen selects the first kind.
//
// Test 2: a cointegration test says the spread is stationary and says nothing
// about how fast.
//
// Test 3: the Engle-Granger critical values are not the Dickey-Fuller ones.
//
// Test 4: a break in the RELATIONSHIP is not a move in the spread.
//
// No check description here may contain the substring FAIL.

#include <strategies/cointegration.hpp>

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

constexpr std::size_t kN = 500;
double ya[kN], xa[kN], yb[kN], xb[kN];

/// Two stocks driven by a shared MARKET factor plus independent walks of their
/// own. Highly correlated in levels, and NOT cointegrated: each carries its own
/// unit root, so the spread is itself a random walk and never comes back.
///
/// A shared factor rather than two bare independent walks, because the
/// correlation of two independent walks is a random variable -- P5-04 measured
/// a mean |0.492| across forty trials, which means any single draw can land
/// anywhere. A common market factor is both the realistic reason two unrelated
/// stocks look correlated and the only way to make the fixture deterministic.
void make_spurious(std::uint64_t seed)
{
    Lcg g{seed};
    double market = 0.0, a = 100.0, b = 100.0;
    for (std::size_t i = 0; i < kN; ++i) {
        market += g.normal() * 1.2;             // the common factor
        a += g.normal() * 0.5;                  // and each stock's own walk
        b += g.normal() * 0.5;
        ya[i] = a + 1.0 * market;
        xa[i] = b + 1.0 * market;
    }
}

/// A genuinely cointegrated pair: y = 2*x + a STATIONARY error. The daily
/// moves are dominated by independent noise, so the correlation of the LEVELS
/// is high but the correlation of the moves is not -- and the spread comes
/// back.
void make_cointegrated(std::uint64_t seed)
{
    Lcg g{seed};
    double x = 50.0, e = 0.0;
    for (std::size_t i = 0; i < kN; ++i) {
        x += g.normal() * 0.9;
        e = 0.90 * e + g.normal() * 1.2;    // OU, half-life ~6.6
        xb[i] = x;
        yb[i] = 2.0 * x + e;
    }
}

// ── 1 ────────────────────────────────────────────────────────────────────
// THE card.
void correlation_is_not_cointegration()
{
    std::printf("\n1 correlation_is_not_cointegration\n");
    make_spurious(0x5B0212);
    make_cointegrated(0xC01A17);

    const auto sp = assess_pair(ya, xa, kN, 1);
    const auto co = assess_pair(yb, xb, kN, 1);
    check(sp.has_value() && co.has_value(), "both pairs assess");
    if (!sp || !co) { return; }

    std::printf("    %zu observations each:\n"
                "                          level corr   EG t-stat   crit(5%%)"
                "   cointegrated\n"
                "      two random walks      %+.3f      %+8.3f    %+8.3f"
                "        %s\n"
                "      a genuinely tied pair %+.3f      %+8.3f    %+8.3f"
                "        %s\n",
                kN,
                sp->eg.level_correlation, sp->eg.residual_test.t_stat,
                sp->eg.critical_5pct, sp->eg.cointegrated ? "YES" : "no",
                co->eg.level_correlation, co->eg.residual_test.t_stat,
                co->eg.critical_5pct, co->eg.cointegrated ? "YES" : "no");

    check(std::fabs(sp->eg.level_correlation) > 0.7,
          "the two INDEPENDENT random walks are strongly correlated in levels"
          " -- which is the spurious regression P5-04 already measured, now"
          " arriving as a pair-selection criterion");
    check(!sp->eg.cointegrated,
          "and they are NOT cointegrated: their spread wanders off and never"
          " comes back, so a pairs trade on them is a directional bet with a"
          " market-neutral label");
    check(co->eg.cointegrated,
          "while the genuinely tied pair IS cointegrated");
    std::printf("    -> a correlation screen selects the first kind, which is"
                " the kind that blows up.\n       The two properties are"
                " different and neither implies the other.\n");

    // Johansen agrees, and it is the symmetric test.
    std::printf("      Johansen trace: random walks %.2f, tied pair %.2f"
                "  (5%% critical %.2f)\n",
                sp->joh.trace_stat, co->joh.trace_stat, co->joh.critical_5pct);
    check(co->joh.cointegrated && !sp->joh.cointegrated,
          "Johansen reaches the same verdict on both, from the symmetric"
          " formulation -- Engle-Granger regressing y on x and x on y can"
          " disagree, so a pair the two directions differ about is one to look"
          " at rather than a coin to flip");
    check(sp->tests_agree() && co->tests_agree(),
          "and both pairs have the two tests agreeing, which is what"
          " `tradeable` requires");

    // The hedge ratio is recovered.
    std::printf("      recovered hedge ratio on the tied pair: %.4f"
                " (constructed as 2.0)\n", co->eg.hedge.beta);
    check(near(co->eg.hedge.beta, 2.0, 0.1),
          "and the cointegrating vector is recovered close to the 2.0 it was"
          " built with");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void a_stationary_spread_says_nothing_about_how_fast()
{
    std::printf("\n2 a_stationary_spread_says_nothing_about_how_fast\n");
    // Two cointegrated pairs, identical in every way except the speed of
    // mean reversion.
    static double yf[kN], xf[kN], ys[kN], xs[kN];
    auto build = [](double phi, std::uint64_t seed, double* y, double* x) {
        Lcg g{seed};
        double px = 50.0, e = 0.0;
        for (std::size_t i = 0; i < kN; ++i) {
            px += g.normal() * 0.9;
            e = phi * e + g.normal() * 1.2;
            x[i] = px;
            y[i] = 2.0 * px + e;
        }
    };
    build(0.70, 0xFA57, yf, xf);        // half-life ~1.9
    // 0.96 gives a true half-life of 17 bars, estimated a little lower. It is
    // deliberately NOT closer to a unit root: at 0.99 the residual is so
    // persistent that Johansen stops calling the pair cointegrated at all, and
    // the pair would then fail `tradeable` for a reason that has nothing to do
    // with speed -- which is the wrong demonstration wearing the right label.
    build(0.96, 0x510C0, ys, xs);

    const auto fast = assess_pair(yf, xf, kN, 1);
    const auto slow = assess_pair(ys, xs, kN, 1);
    check(fast.has_value() && slow.has_value(), "both pairs assess");
    if (!fast || !slow) { return; }

    const double intraday_hold = 8.0;       // bars
    std::printf("    two cointegrated pairs, differing only in reversion"
                " speed:\n"
                "                   EG t-stat   half-life   tradeable on a"
                " %.0f-bar book\n"
                "      fast pair    %+8.3f    %8.2f      %s\n"
                "      slow pair    %+8.3f    %8.2f      %s\n",
                intraday_hold,
                fast->eg.residual_test.t_stat, fast->half_life_bars,
                fast->tradeable(intraday_hold) ? "YES" : "no",
                slow->eg.residual_test.t_stat, slow->half_life_bars,
                slow->tradeable(intraday_hold) ? "YES" : "no");

    check(fast->half_life_bars < slow->half_life_bars,
          "the two half-lives differ by a large factor");
    check(fast->tradeable(intraday_hold),
          "the fast pair is tradeable on an 8-bar book");
    check(slow->eg.cointegrated && slow->joh.cointegrated,
          "the slow pair passes BOTH cointegration tests -- so what follows is"
          " about speed and not about the relationship");
    check(!slow->tradeable(intraday_hold),
          "and it is still NOT tradeable at this holding period, however good"
          " its cointegration statistic"
          " -- the test says the spread is stationary and says nothing about"
          " how fast, so the position would be financed and margined for a"
          " convergence arriving after the strategy was switched off");
    check(slow->tradeable(40.0),
          "on a book that can hold for 40 bars the same pair IS tradeable,"
          " which is why `tradeable` takes the holding period rather than"
          " assuming one");

    // 2b. A PURE RANDOM WALK reports a finite half-life, and that is the
    // reason the cointegration test cannot be skipped.
    static double walk[kN];
    Lcg g{0xA02150};
    double w = 0.0;
    for (std::size_t i = 0; i < kN; ++i) { w += g.normal(); walk[i] = w; }
    const auto hl = half_life(walk, kN);
    const auto wt = adf(walk, kN, 1);
    check(hl.has_value() && wt.has_value(),
          "a pure random walk still produces a half-life and an ADF statistic");
    if (hl && wt) {
        std::printf("    a PURE RANDOM WALK -- no mean reversion whatsoever:\n"
                    "      apparent half-life  %8.2f bars   <- finite, and"
                    " entirely spurious\n"
                    "      ADF t-statistic     %+8.3f       (5%% critical"
                    " %+.3f)  ->  %s\n",
                    *hl, wt->t_stat, eg_critical(kN, 5.0),
                    wt->t_stat < eg_critical(kN, 5.0) ? "stationary"
                                                      : "NOT stationary");
        check(*hl > 0.0 && std::isfinite(*hl),
              "OLS on a unit root returns a slightly negative slope by chance"
              " -- the Dickey-Fuller downward bias -- so a random walk reports"
              " a finite half-life and would pass a 'reverts within 200 bars'"
              " filter on its own");
        check(wt->t_stat > eg_critical(kN, 5.0),
              "while the ADF test correctly says NOT stationary. Which is why"
              " `tradeable` requires cointegration AND a half-life: the"
              " half-life alone cannot tell a slow reverter from a walk");
    }
}

// ── 3 ────────────────────────────────────────────────────────────────────
void the_engle_granger_critical_values_are_not_the_adf_ones()
{
    std::printf("\n3 the_engle_granger_critical_values_are_not_the_adf"
                "_ones\n");
    // The ordinary ADF 5% critical value with a constant is about -2.86 and
    // does not move much with n. The Engle-Granger residual test needs a
    // stricter one, because the residual was produced by a regression that
    // MINIMISED it.
    const double adf_5pct = -2.86;
    std::printf("    5%% critical values:\n"
                "      ordinary ADF (constant, no trend)   %+.4f\n"
                "      Engle-Granger residual, n = 200     %+.4f\n"
                "      Engle-Granger residual, n = 500     %+.4f\n",
                adf_5pct, eg_critical(200, 5.0), eg_critical(500, 5.0));
    check(eg_critical(500, 5.0) < adf_5pct,
          "the Engle-Granger critical value is further from zero than the"
          " ordinary ADF one -- the residual under test was produced by a"
          " regression that minimised it, so it looks more stationary than an"
          " arbitrary series does");
    check(eg_critical(200, 5.0) < eg_critical(500, 5.0),
          "and it is stricter at smaller sample sizes");
    std::printf("    -> using the ordinary ADF table here finds cointegration"
                " roughly twice as often\n       as it exists. The comparison"
                " is done inside `engle_granger`, so a caller never\n"
                "       has to remember which table or which direction the"
                " inequality runs.\n");

    // The direction of the inequality, stated once and tested.
    make_cointegrated(0xD12EC7);
    const auto c = engle_granger(yb, xb, kN, 1);
    check(c.has_value(), "the pair tests");
    if (c) {
        check(c->cointegrated == (c->residual_test.t_stat < c->critical_5pct),
              "cointegrated means the t-statistic is BELOW the critical value"
              " -- more negative is more stationary, which is the one sign"
              " convention everybody gets backwards");
        check(c->residual_test.gamma < 0.0,
              "and the reversion coefficient is negative on a stationary"
              " residual");
    }
}

// ── 4 ────────────────────────────────────────────────────────────────────
void a_broken_relationship_is_not_a_wide_spread()
{
    std::printf("\n4 a_broken_relationship_is_not_a_wide_spread\n");
    static double yk[kN], xk[kN];
    Lcg g{0xB2EA10};
    double x = 50.0, e = 0.0;
    for (std::size_t i = 0; i < kN; ++i) {
        x += g.normal() * 0.9;
        e = 0.90 * e + g.normal() * 1.2;
        xk[i] = x;
        // THE BREAK: the hedge ratio itself changes from 2.0 to 4.5 halfway
        // through. Not a wide spread -- a different relationship.
        const double beta = i < kN / 2 ? 2.0 : 4.5;
        yk[i] = beta * x + e;
    }

    const auto brk = structural_break(yk, xk, kN, 150, 0.25);
    check(brk.has_value(), "the break test runs");

    // A pair whose relationship holds, for comparison.
    make_cointegrated(0x570AB1E);
    const auto stable = structural_break(yb, xb, kN, 150, 0.25);
    check(stable.has_value(), "and so does the stable one");
    if (!brk || !stable) { return; }

    std::printf("    CUSUM of squared residuals against a %.2f band:\n"
                "      hedge ratio changes 2.0 -> 4.5 midway:  excursion %.4f"
                "  ->  %s\n"
                "      hedge ratio stable:                     excursion %.4f"
                "  ->  %s\n",
                0.25, brk->max_excursion, brk->broke ? "BROKE" : "held",
                stable->max_excursion, stable->broke ? "BROKE" : "held");
    check(brk->broke,
          "a change in the hedge RATIO is detected -- the pair is now held at"
          " the OLD ratio against a NEW relationship, which is a directional"
          " position nobody chose");
    check(!stable->broke,
          "and a pair whose relationship holds does not trip it, so the alarm"
          " is not simply firing on volatility");
    check(brk->max_excursion > 1.4 * stable->max_excursion,
          "with a clear margin between the two");
    std::printf("    -> this is the one alarm treated as an EXIT rather than a"
                " signal. A wide spread is\n       what the z-score trades; a"
                " changed beta is the pair no longer existing.\n");

    check(structural_break(yk, xk, kN, 8, 0.25).error()
          == CointError::TooFewSamples,
          "and a burn-in too short to estimate a hedge ratio is refused,"
          " because a break measured against a noisy baseline fires on the"
          " baseline");
}

} // namespace

int main()
{
    std::printf("altair cointegration and pair selection tests\n");
    correlation_is_not_cointegration();
    a_stationary_spread_says_nothing_about_how_fast();
    the_engle_granger_critical_values_are_not_the_adf_ones();
    a_broken_relationship_is_not_a_wide_spread();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
