// P11Q-07 acceptance tests -- fitting on the REAL series.
//
// Test 1: the Markov chain fits on real daily NIFTY through the loader the UI
// uses, and reproduces what models/tests/test_markov.cpp measured through its
// own reader. Two independent paths to the same number.
//
// Test 2: THE INTERESTING ONE. P10-07's finding about level-space vs log-space
// VIX intervals was measured on synthetic data where the truth was known by
// construction. This refits both on real India VIX and reports whether it
// survives.
//
// Test 3: a fit is refused when the data is too short, rather than returning
// parameters estimated from nothing.
//
// No check description here may contain the substring FAIL.

#include "../data/fits.hpp"

#include <QCoreApplication>

#include <cstdio>

#ifndef ALTAIR_DATASET_DIR
#  define ALTAIR_DATASET_DIR "dataset"
#endif

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

using namespace altair;
using namespace altair::ui;

constexpr std::int64_t kDay = 24LL * 3600 * 1'000'000'000LL;

QString dataset(const char* rel)
{
    return QStringLiteral("%1/%2").arg(QStringLiteral(ALTAIR_DATASET_DIR),
                                       QString::fromUtf8(rel));
}

} // namespace

// ---------------------------------------------------------------------------

static void test_markov_on_real_nifty()
{
    std::printf("\n[1] the Markov chain, through the UI's own loader\n");

    const LoadResult r = load_bars_csv(dataset("spot/nifty/1d/all.csv"), kDay,
                                       DailyStamp::SessionClose, true);
    check(r.ok() && r.bars.size() > 5000,
          "the 35-year daily series loads through the same path the chart uses");
    if (!r.ok()) {
        std::printf("    %s\n", r.error.toUtf8().constData());
        return;
    }

    const MarkovFit f = fit_markov(r.bars);
    check(f.ok, "and the chain fits on it");
    if (!f.ok) {
        std::printf("    %s\n", f.error.toUtf8().constData());
        return;
    }

    std::printf("    %zu returns -> %zu transitions over %zu states"
                " (thinnest cell %u, %zu empty)\n",
                f.returns, f.transitions, f.states, f.thinnest_cell,
                f.empty_cells);
    std::printf("    look-ahead: %zu of the labelled days change state when"
                " boundaries come from the past only (%.1f%%)\n",
                f.relabelled, f.relabelled_pct);
    std::printf("    chi2 real %.2f vs shuffled %.2f, critical %.2f\n",
                f.chi_square, f.chi_square_shuffled, f.critical_5pct);

    check(f.transitions > 5000, "thousands of transitions, as before");
    check(f.relabelled_pct > 5.0 && f.relabelled_pct < 40.0,
          "the look-ahead is material and not total -- which is what makes it"
          " survive review");
    check(f.rejects,
          "the real series rejects independence, as it did through the engine"
          " test's own reader: two independent paths to the same conclusion");
    check(!f.shuffled_rejects,
          "and the shuffled control does not, so the statistic is responding"
          " to temporal structure rather than to sample size");

    double total = 0.0;
    for (double x : f.stationary_dist) {
        total += x;
    }
    check(std::fabs(total - 1.0) < 1e-6,
          "the stationary distribution sums to one");
}

static void test_vix_synthetic_finding_against_reality()
{
    std::printf("\n[2] P10-07's interval finding, refitted on real India VIX\n");

    const LoadResult r = load_bars_csv(dataset("spot/indiavix/1d/all.csv"),
                                       kDay, DailyStamp::SessionClose, true);
    check(r.ok(), "the India VIX daily series loads");
    if (!r.ok()) {
        std::printf("    %s\n", r.error.toUtf8().constData());
        return;
    }
    std::printf("    %zu India VIX daily bars\n", r.bars.size());

    const VixFit f = fit_vix_both_spaces(r.bars);
    check(f.ok, "and both spaces fit");
    if (!f.ok) {
        std::printf("    %s\n", f.error.toUtf8().constData());
        return;
    }

    std::printf("    level: b = %.4f, half-life %.1f obs, residual sd %.4f\n",
                f.level_b, f.level_half_life, f.level_sd);
    std::printf("    log  : b = %.4f, half-life %.1f obs, residual sd %.4f\n",
                f.log_b, f.log_half_life, f.log_sd);
    std::printf("    regimes split at VIX %.2f (quiet) and %.2f (stressed)\n",
                f.quiet_threshold, f.stressed_threshold);
    std::printf("    LEVEL residual sd: %.4f quiet (%zu obs) vs %.4f stressed"
                " (%zu obs)  = %.2fx\n",
                f.level_sd_quiet, f.quiet_n, f.level_sd_stressed, f.stressed_n,
                f.level_sd_quiet > 0.0 ? f.level_sd_stressed / f.level_sd_quiet
                                       : 0.0);
    std::printf("    coverage of a nominal 95.4%% band:\n");
    std::printf("      level  %.1f%% quiet   %.1f%% stressed\n",
                f.level_cover_quiet, f.level_cover_stressed);
    std::printf("      log    %.1f%% quiet   %.1f%% stressed\n",
                f.log_cover_quiet, f.log_cover_stressed);

    check(f.level_b > 0.0 && f.level_b < 1.0,
          "the level AR(1) is mean-reverting on real data, as it was on"
          " synthetic");
    check(f.log_b > 0.0 && f.log_b < 1.0,
          "and so is the log one");
    check(f.quiet_n > 100 && f.stressed_n > 100,
          "both regimes carry enough observations for the comparison to mean"
          " something");

    // The synthetic finding was that the LEVEL model's single residual sd is
    // wrong in both regimes because VIX moves proportionally. Whether that
    // holds here is measured, not asserted -- a test that demanded it would be
    // a test that demanded the synthetic result generalise.
    const double ratio = f.level_sd_quiet > 0.0
                           ? f.level_sd_stressed / f.level_sd_quiet
                           : 0.0;
    if (ratio > 1.5) {
        std::printf("    -> the level model's residual spread really is"
                    " regime-dependent on real data: P10-07's synthetic finding"
                    " holds\n");
    } else {
        std::printf("    -> the level model's residual spread is NOT strongly"
                    " regime-dependent here; the synthetic finding does not"
                    " reproduce on this series\n");
    }
    check(ratio > 0.0,
          "and whichever it is, the ratio is reported -- refitting a synthetic"
          " finding on real data is the only thing that says whether it"
          " generalises");
}

static void test_a_short_series_is_refused()
{
    std::printf("\n[3] too little data is refused, not estimated from\n");

    std::vector<Candle> few;
    for (int i = 0; i < 40; ++i) {
        Candle c{};
        c.start_ns = static_cast<std::int64_t>(i) * kDay;
        c.end_ns = c.start_ns + kDay;
        c.close = 100000 + i * 10;
        c.complete = true;
        few.push_back(c);
    }

    const MarkovFit m = fit_markov(few);
    check(!m.ok && !m.error.isEmpty(),
          "a forty-bar series produces no Markov chain and says how many bars"
          " it needed, rather than fitting twenty-five cells from thirty-nine"
          " transitions");

    const VixFit v = fit_vix_both_spaces(few);
    check(!v.ok && !v.error.isEmpty(),
          "and no VIX fit either -- an AR(1) estimated from forty points has a"
          " standard error nobody would act on if they saw it");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    std::printf("P11Q-07 -- fitting on the real series\n");
    test_markov_on_real_nifty();
    test_vix_synthetic_finding_against_reality();
    test_a_short_series_is_refused();

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : "checks did not pass");
    return failures == 0 ? 0 : 1;
}
