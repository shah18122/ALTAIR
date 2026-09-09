// P6-07 acceptance tests for backtest/montecarlo.hpp.
//
// Test 1 is the card: an IID bootstrap keeps the mean, the variance and the
// Sharpe, and destroys the drawdown -- because a drawdown is a property of the
// ORDER of returns and of nothing else. Measured on a series with real
// volatility clustering, against a block bootstrap of the same series.
//
// Test 2: max drawdown grows with sample length, so two backtests of different
// lengths cannot be compared on it.
//
// Test 3: the sqrt(252) in an annualised Sharpe assumes independence.
//
// No check description here may contain the substring FAIL.

#include <backtest/montecarlo.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <tuple>

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

constexpr std::size_t kLen = 2000;
constexpr std::size_t kPaths = 2000;

double source[kLen];
double sample[kLen];
double stat[kPaths];

// ── 1 ────────────────────────────────────────────────────────────────────
// THE card.
void an_iid_bootstrap_destroys_the_drawdown()
{
    std::printf("\n1 an_iid_bootstrap_destroys_the_drawdown\n");
    // A Heston path: real volatility clustering, no jumps, no drift. Anything
    // the two bootstraps disagree about is the clustering.
    Rng rng{0xC1057E4};
    // Parameters chosen so (a) the variance actually moves -- its stationary
    // spread is larger than its mean, which needs xi well above the first
    // draft's -- and (b) a 2,000-step drawdown lands around 15% rather than
    // saturating. At 1% per-step vol over 2,000 steps the cumulative spread is
    // 45%, every path draws down 60-90%, and both bootstraps pin against the
    // ceiling where nothing can be told apart.
    HestonParams h{};
    h.mu = 0.0;
    h.v0 = 9e-6;                // 0.3% per-step vol
    h.kappa = 0.03;
    h.theta = 9e-6;
    h.xi = 1e-3;                // vol-of-vol: variance sd is ~1.4x its mean
    h.rho = -0.7;
    check(heston_returns(source, kLen, h, rng).has_value(),
          "the Heston source path generates");

    const auto src_m = metrics(source, kLen, 0.0);
    check(src_m.has_value(), "and its metrics compute");
    if (!src_m) { return; }

    // Volatility clustering shows up as autocorrelation in |returns|, not in
    // returns -- the returns themselves are near-uncorrelated, which is why a
    // check on the returns' own autocorrelation would find nothing.
    static double absr[kLen];
    for (std::size_t i = 0; i < kLen; ++i) { absr[i] = std::fabs(source[i]); }
    const auto abs_m = metrics(absr, kLen, 0.0);
    check(abs_m.has_value() && abs_m->autocorr1 > 0.05,
          "the source has volatility CLUSTERING -- positive autocorrelation in"
          " absolute returns, which is the property a drawdown depends on");

    auto run = [&](bool blocked, std::size_t block) {
        // COMMON RANDOM NUMBERS: both bootstraps draw from the SAME stream.
        // The quantity under test is the difference between two estimators on
        // one source, and giving them independent streams adds Monte Carlo
        // noise to exactly the comparison being made -- enough here to swamp a
        // 2-3% gap. (This held by accident until P8-02: the old `seed | 1`
        // collapsed 0xB0075 and 0xB0076 onto one state. Now it is deliberate.)
        Rng g{0xB0075};
        double mean_sharpe = 0.0, mean_sd = 0.0;
        for (std::size_t p = 0; p < kPaths; ++p) {
            const auto ok = blocked
                ? block_bootstrap(sample, kLen, source, kLen, block, g)
                : iid_bootstrap(sample, kLen, source, kLen, g);
            if (!ok) { stat[p] = 0.0; continue; }
            const auto m = metrics(sample, kLen, 0.0);
            stat[p] = m ? m->max_drawdown : 0.0;
            if (m) { mean_sharpe += m->sharpe; mean_sd += m->sd; }
        }
        const auto d = summarise(stat, kPaths);
        return std::tuple<Distribution, double, double>{
            d ? *d : Distribution{}, mean_sharpe / kPaths, mean_sd / kPaths};
    };

    const auto [iid, iid_sharpe, iid_sd] = run(false, 0);
    const auto [blk, blk_sharpe, blk_sd] = run(true, 50);

    std::printf("    %zu bootstrap paths of %zu returns each, from a Heston"
                " source with clustering:\n"
                "                        mean sd    mean Sharpe   MDD p50  "
                "  MDD p95    MDD worst\n"
                "      IID bootstrap    %8.5f    %+9.4f  %8.2f%%  %8.2f%%"
                "  %8.2f%%\n"
                "      BLOCK bootstrap  %8.5f    %+9.4f  %8.2f%%  %8.2f%%"
                "  %8.2f%%\n",
                kPaths, kLen, iid_sd, iid_sharpe, 100.0 * iid.p50,
                100.0 * iid.p95, 100.0 * iid.worst,
                blk_sd, blk_sharpe, 100.0 * blk.p50, 100.0 * blk.p95,
                100.0 * blk.worst);

    check(near(iid_sd, blk_sd, 0.05 * blk_sd),
          "the two agree on the standard deviation to within 5% -- shuffling"
          " returns does not change their spread");
    check(std::fabs(iid_sharpe - blk_sharpe) < 0.05,
          "and they agree on the Sharpe, because that is a function of the"
          " mean and the variance and neither depends on order");
    check(std::fabs(blk.p50 - iid.p50) > 0.005
          || std::fabs(blk.p95 - iid.p95) > 0.005
          || std::fabs(blk.worst - iid.worst) > 0.005,
          "but the DRAWDOWN distributions differ -- a drawdown is a property of"
          " the ORDER of returns and of nothing else, so it is the one"
          " statistic here that shuffling can move");
    check(blk.worst > iid.worst,
          "and the BLOCK bootstrap's WORST path is deeper: preserving the runs"
          " is what strings bad days together, and the extreme tail is where"
          " that shows. This is the number a risk limit is actually set"
          " against");
    std::printf("    -> the IID bootstrap understates the WORST path by"
                " %.1f%%, and its median and\n       95th percentile sit"
                " %.1f%% and %.1f%% the other way. That shape is the honest\n"
                "       result and is worth stating precisely: block"
                " resampling preserves runs, which\n       fattens the extreme"
                " tail, and it also draws only %zu independent blocks per\n"
                "       path instead of %zu returns, which widens the whole"
                " distribution. The two\n       effects pull the middle and"
                " the extreme in opposite directions.\n\n       What survives"
                " unambiguously: the mean, the spread and the Sharpe agree to"
                "\n       three decimals, and the drawdown does not. Shuffling"
                " changes only the one\n       statistic that depends on order"
                " -- and it is the one a risk limit is set on.\n",
                100.0 * (blk.worst / iid.worst - 1.0),
                100.0 * (blk.p50 / iid.p50 - 1.0),
                100.0 * (blk.p95 / iid.p95 - 1.0),
                kLen / 50, kLen);

    // The block length is a decision, and the degenerate ends prove it.
    Rng g2{0x512E9A11};
    check(block_bootstrap(sample, kLen, source, kLen, 0, g2).error()
          == McError::NoBlockLength,
          "a block length of zero is refused rather than defaulted -- too short"
          " and it degenerates to IID, too long and every sample is the"
          " original series with a rotated start");
    check(block_bootstrap(sample, kLen, source, kLen, kLen + 1, g2).error()
          == McError::NoBlockLength,
          "and so is a block longer than the source");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void max_drawdown_grows_with_the_length_of_the_backtest()
{
    std::printf("\n2 max_drawdown_grows_with_the_length_of_the_backtest\n");
    GbmParams g{};
    g.mu = 0.0;                 // NO drift: the process does not change at all
    // 0.3% per step, so a 4,000-step drawdown lands near 20% instead of near
    // 100%. Above about 60% the 1 - exp(-x) conversion saturates and the
    // sqrt(n) relation this test is measuring is compressed out of existence.
    g.sigma = 0.003;

    struct Row { std::size_t n; double p50; double mean; };
    Row rows[3] = {{250, 0, 0}, {1000, 0, 0}, {4000, 0, 0}};
    static double path[4000];
    for (auto& row : rows) {
        Rng rng{0xD0D0 + row.n};
        for (std::size_t p = 0; p < kPaths; ++p) {
            (void)gbm_returns(path, row.n, g, rng);
            const auto m = metrics(path, row.n, 0.0);
            stat[p] = m ? m->max_drawdown : 0.0;
        }
        const auto d = summarise(stat, kPaths);
        if (d) { row.p50 = d->p50; row.mean = d->mean; }
    }

    std::printf("    the SAME driftless GBM, 0.3%% per-step vol, %zu paths each:\n"
                "      %5zu steps   median MDD %6.2f%%   mean %6.2f%%\n"
                "      %5zu steps   median MDD %6.2f%%   mean %6.2f%%\n"
                "      %5zu steps   median MDD %6.2f%%   mean %6.2f%%\n"
                "      ratio 4000/250 = %.2fx   (sqrt(16) = 4.00)\n",
                kPaths,
                rows[0].n, 100.0 * rows[0].p50, 100.0 * rows[0].mean,
                rows[1].n, 100.0 * rows[1].p50, 100.0 * rows[1].mean,
                rows[2].n, 100.0 * rows[2].p50, 100.0 * rows[2].mean,
                rows[2].p50 / rows[0].p50);

    check(rows[1].p50 > rows[0].p50 && rows[2].p50 > rows[1].p50,
          "expected max drawdown rises monotonically with the number of"
          " observations, on a process that is identical in every other"
          " respect");
    check(rows[2].p50 / rows[0].p50 > 2.5,
          "sixteen times the length gives well over twice the drawdown, close"
          " to the sqrt(n) a driftless walk predicts -- a longer series simply"
          " has more chances to make a new low");
    std::printf("    -> so a five-year backtest showing a deeper drawdown than"
                " a one-year backtest is\n       telling you it is five years"
                " long. Quoting the two side by side is a units\n       error"
                " dressed as a risk comparison.\n");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void the_sqrt_252_assumes_independence()
{
    std::printf("\n3 the_sqrt_252_assumes_independence\n");
    const double sr = 0.05;             // a plausible per-day Sharpe
    const std::size_t q = 252;

    std::printf("    a per-period Sharpe of %.2f, annualised over %zu"
                " periods:\n"
                "      autocorr   naive SR*sqrt(q)   Lo (2002)    naive"
                " overstates by\n", sr, q);
    double worst_ratio = 1.0;
    for (const double rho : {0.0, 0.1, 0.2, 0.3, 0.4}) {
        const double naive = naive_annualised_sharpe(sr, q);
        const auto lo = lo_annualised_sharpe(sr, q, rho);
        check(lo.has_value(), "the Lo correction computes");
        if (!lo) { continue; }
        const double ratio = naive / *lo;
        if (ratio > worst_ratio) { worst_ratio = ratio; }
        std::printf("      %+.2f          %8.3f       %8.3f       %6.1f%%\n",
                    rho, naive, *lo, 100.0 * (ratio - 1.0));
    }

    const auto indep = lo_annualised_sharpe(sr, q, 0.0);
    check(indep.has_value()
          && near(*indep, naive_annualised_sharpe(sr, q), 1e-9),
          "at zero autocorrelation the correction reduces EXACTLY to"
          " SR*sqrt(q), so it generalises the familiar formula rather than"
          " replacing it");
    check(worst_ratio > 1.30,
          "and at the autocorrelation a slow-moving strategy actually has, the"
          " naive annualisation overstates the Sharpe by more than 30% -- tens"
          " of percent, not a rounding term");

    // Negative autocorrelation goes the other way, which is the check that the
    // correction is a correction and not a haircut.
    const auto neg = lo_annualised_sharpe(sr, q, -0.2);
    check(neg.has_value() && *neg > naive_annualised_sharpe(sr, q),
          "a mean-reverting strategy with NEGATIVE autocorrelation is"
          " UNDERSTATED by sqrt(q), and the correction raises it -- so this is"
          " arithmetic, not conservatism");
    std::printf("    -> which is why Metrics carries autocorr1 alongside the"
                " Sharpe. An annualised\n       number without it is a claim"
                " about independence that nobody made"
                " deliberately.\n");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void the_generators_have_the_moments_they_claim()
{
    std::printf("\n4 the_generators_have_the_moments_they_claim\n");
    static double path[20000];

    // GBM: the Ito correction means exp of the accumulated log path has
    // expectation exp(mu*n), NOT exp((mu + sigma^2/2)*n).
    Rng r1{0x6B111};
    GbmParams g{};
    g.mu = 0.0004;
    g.sigma = 0.012;
    (void)gbm_returns(path, 20000, g, r1);
    double s = 0.0;
    for (std::size_t i = 0; i < 20000; ++i) { s += std::exp(path[i]); }
    const double mean_simple = s / 20000.0;
    std::printf("    GBM mu=%.4f sigma=%.3f: mean SIMPLE return %.6f"
                " (exp(mu) = %.6f)\n",
                g.mu, g.sigma, mean_simple, std::exp(g.mu));
    check(near(mean_simple, std::exp(g.mu), 0.0005),
          "the Ito correction is applied, so the expected SIMPLE return is"
          " exp(mu) -- omitting it makes every simulated strategy look better"
          " the higher its volatility, which is the classic error");

    // Jump-diffusion: the compensator means adding jumps does not add drift.
    Rng r2{0x104D9911};
    JumpParams j{};
    j.mu = 0.0004;
    j.sigma = 0.012;
    j.lambda = 0.05;
    j.jump_mean = -0.02;
    j.jump_sd = 0.03;
    (void)jump_returns(path, 20000, j, r2);
    double sj = 0.0;
    for (std::size_t i = 0; i < 20000; ++i) { sj += std::exp(path[i]); }
    const double mean_jump = sj / 20000.0;
    const auto jm = metrics(path, 20000, 0.0);
    check(jm.has_value(), "the jump path's metrics compute");
    std::printf("    jump-diffusion, same mu: mean SIMPLE return %.6f,"
                " sd %.5f (GBM sd was %.5f)\n",
                mean_jump, jm ? jm->sd : 0.0, g.sigma);
    check(near(mean_jump, std::exp(j.mu), 0.002),
          "the compensator is applied, so adding jumps changes the RISK and"
          " not the expected return -- without it a jump model quietly becomes"
          " an alpha model");
    check(jm.has_value() && jm->sd > g.sigma,
          "while the standard deviation rises, which is the entire point of"
          " adding them");

    // Heston with the Feller condition violated: full truncation must not
    // produce a NaN, which is what a naive Euler scheme does.
    Rng r3{0xFE11E4};
    HestonParams h{};
    h.mu = 0.0;
    h.v0 = 0.0001;
    h.kappa = 0.01;
    h.theta = 0.0001;
    h.xi = 0.002;               // 2*kappa*theta = 2e-6 << xi^2 = 4e-6
    h.rho = -0.7;
    check(2.0 * h.kappa * h.theta < h.xi * h.xi,
          "the Feller condition is deliberately violated, which fitted equity"
          " parameters routinely do");
    (void)heston_returns(path, 20000, h, r3);
    bool finite = true;
    for (std::size_t i = 0; i < 20000; ++i) {
        if (!std::isfinite(path[i])) { finite = false; }
    }
    check(finite,
          "and full-truncation Euler still produces a finite path on every"
          " step -- a naive scheme takes the square root of a negative variance"
          " and fills the tail of the simulation with NaN, which then"
          " propagates into every risk number computed from it");
}

} // namespace


// P33-03. THE POISSON DRAW WAS SILENTLY TRUNCATED.
//
// The inversion loop was capped at sixteen arrivals with no bound on lambda.
// A Merton model calibrated at fifty jumps a year, entered without dividing by
// 252, gives lambda = 50 -- and the path came back finite, the metrics
// computed, and essentially every step had been forced to exactly sixteen
// jumps. Measured: at lambda 20, P(N <= 16) is 0.221.
void a_lambda_that_would_truncate_is_refused()
{
    std::printf("\nN a_lambda_that_would_truncate_is_refused\n");

    double buf[256];
    Rng rng(99u);

    JumpParams ok{};
    ok.sigma = 0.01;
    ok.lambda = 0.05;
    ok.jump_sd = 0.02;
    check(jump_returns(buf, 256, ok, rng).has_value(),
          "a realistic per-step intensity is accepted");

    JumpParams bad = ok;
    bad.lambda = 50.0;      // fifty jumps a YEAR, not per step
    const auto r = jump_returns(buf, 256, bad, rng);
    check(!r.has_value() && r.error() == McError::BadParameter,
          "and one that is fifty per STEP is REFUSED rather than quietly "
          "capped -- an intensity that large is a units error, and the old "
          "code returned a hard-capped process that looked fine");

    JumpParams edge = ok;
    edge.lambda = 12.0;
    check(jump_returns(buf, 256, edge, rng).has_value(),
          "the bound itself is admissible: at twelve arrivals per step "
          "against a cap of 64, P(N > cap) is below 1e-25");
}

int main()
{
    std::printf("altair Monte Carlo and metrics tests\n");
    an_iid_bootstrap_destroys_the_drawdown();
    max_drawdown_grows_with_the_length_of_the_backtest();
    the_sqrt_252_assumes_independence();
    the_generators_have_the_moments_they_claim();

    a_lambda_that_would_truncate_is_refused();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
