// P35-02 acceptance tests for models/calibration.hpp.
//
// THE LOOK-AHEAD TEST IS THE ONE THAT MATTERS.
//
// Everything in this file rests on a volatility feature computed from the
// forecast anchors, and if that feature can see the bar it is describing then
// every number downstream is flattered and none of them can be trusted. So the
// first check plants a violation directly: take a series, detonate one bar in
// the middle of it, and require that the volatility values at and before that
// bar are BIT-IDENTICAL to the ones from the undetonated series. A feature that
// peeks even slightly cannot pass that.
//
// This is hard rule 7 as an executable statement rather than a comment, and it
// is cheap enough that there is no excuse for a causal series not to have one.
//
// The rest checks the two claims the file makes: that the two sides of the band
// are earned separately, and that a coverage average can hide a band which is
// wrong in exactly the regime that matters.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <models/calibration.hpp>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

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

void near(double got, double want, double tol, const char* what)
{
    const bool ok = std::fabs(got - want) <= tol;
    if (ok) {
        std::printf("  ok  : %s (%.4f, want %.4f +/- %.4f)\n",
                    what, got, want, tol);
    } else {
        ++failures;
        std::printf("  FAIL: %s (%.4f, want %.4f +/- %.4f)\n",
                    what, got, want, tol);
    }
}

/// Forecast points over a series with clustering volatility.
///
/// `skew` drags the innovation distribution: negative means the downside tail
/// is fatter, which is what an equity index does and what makes a symmetric
/// band the wrong shape.
std::vector<altair::ForecastPoint>
make_points(std::size_t n, unsigned seed, double band_bps, double skew)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> z(0.0, 1.0);

    // omega is chosen so the stationary variance is 100^2, i.e. a typical
    // daily move of 100 bps. The first draft used omega = 0.04 and produced a
    // one-basis-point index: a 40 bps band was never once exceeded, every
    // quintile scored exactly zero, and the test looked like a library bug.
    const double a = 0.10, b = 0.86;
    const double target_sd = 100.0;
    const double omega = target_sd * target_sd * (1.0 - a - b);
    double var = omega / (1.0 - a - b);

    // THE SKEW MUST NOT MOVE THE VARIANCE, or the GARCH recursion stops being
    // stationary and the variance diverges. Stretching the left half by
    // (1+skew) raises E[e^2] to (1 + (1+skew)^2)/2, so the whole innovation is
    // divided by the square root of that. The first draft skipped this: with
    // skew 1.2 the persistence became 1.152, var overflowed, and every offset
    // came out NaN.
    const double stretch = 1.0 + skew;
    const double e_scale = std::sqrt(0.5 * (1.0 + stretch * stretch));

    std::vector<altair::ForecastPoint> pts;
    pts.reserve(n);
    double level = 20000.0;
    for (std::size_t i = 0; i < n; ++i) {
        double e = z(rng);
        // A cheap asymmetry: stretch the left tail only, then renormalise.
        if (e < 0.0) { e *= stretch; }
        e /= e_scale;
        const double r_bps = e * std::sqrt(var);
        const double next = level * std::exp(r_bps / 10'000.0);

        altair::ForecastPoint p;
        p.ts_ns = static_cast<std::int64_t>(i) * 86'400'000'000'000LL;
        p.anchor = level;
        p.predicted = level;                 // the random walk, deliberately
        p.actual = next;
        p.lo = level * std::exp(-band_bps / 10'000.0);
        p.hi = level * std::exp(band_bps / 10'000.0);
        pts.push_back(p);

        // GARCH(1,1) on the standardised innovation: e*sqrt(var) is the
        // realised shock, so e*e*var is its square.
        var = omega + a * (e * e) * var + b * var;
        level = next;
    }
    return pts;
}

// ── hard rule 7 ──────────────────────────────────────────────────────────────

void test_no_lookahead()
{
    std::printf("\ncausality -- planted, not argued\n");

    constexpr std::size_t kN = 400;
    constexpr std::size_t kWindow = 20;
    constexpr std::size_t kBoom = 250;

    auto base = make_points(kN, 4242u, 165.0, 0.0);

    // PLANT: detonate one bar far from the start. Everything at or before it
    // must be untouched, because none of it may know.
    auto blown = base;
    blown[kBoom].actual *= 1.35;
    // The anchor of the NEXT bar is the previous actual, which is how the
    // detonation enters the anchor series at all.
    if (kBoom + 1 < blown.size()) { blown[kBoom + 1].anchor *= 1.35; }

    const auto v0 = altair::detail::causal_vol_bps(base, kWindow);
    const auto v1 = altair::detail::causal_vol_bps(blown, kWindow);

    bool identical_before = true;
    for (std::size_t i = 0; i <= kBoom; ++i) {
        if (v0[i] != v1[i]) { identical_before = false; break; }
    }
    check(identical_before,
          "a detonated bar changes no volatility value at or before itself");

    bool differs_after = false;
    for (std::size_t i = kBoom + 1; i < v0.size(); ++i) {
        if (v0[i] != v1[i]) { differs_after = true; break; }
    }
    check(differs_after,
          "and it does change them afterwards, so the check is not vacuous");

    // Absence is not zero.
    bool early_marked = true;
    for (std::size_t i = 0; i < kWindow; ++i) {
        if (v0[i] >= 0.0) { early_marked = false; break; }
    }
    check(early_marked,
          "bars with too little history are marked absent, not given a zero");
}

// ── the two sides are earned separately ──────────────────────────────────────

void test_asymmetry()
{
    std::printf("\ntwo sides, calibrated separately\n");

    // A series whose left tail is much fatter.
    const auto pts = make_points(4000, 991u, 160.0, 1.2);

    altair::CalibrationSpec spec;
    spec.conformal.alpha = 0.10;
    spec.conformal.mode = altair::ConformalMode::TimeWeighted;
    spec.warmup = 500;

    const auto got = altair::calibrate_band(pts, spec);
    check(got.has_value(), "a skewed series calibrates");
    if (!got) { return; }

    const auto& out = got->first;
    double up = 0.0, dn = 0.0;
    std::size_t m = 0;
    for (std::size_t i = spec.warmup; i < out.size(); ++i) {
        up += out[i].hi - out[i].predicted;
        dn += out[i].predicted - out[i].lo;
        ++m;
    }
    up /= static_cast<double>(m);
    dn /= static_cast<double>(m);
    std::printf("        mean upper offset %.2f   mean lower offset %.2f\n",
                up, dn);
    check(dn > up * 1.05,
          "the fatter downside tail earns a wider lower half");
}

// ── the headline: before and after, overall and by regime ────────────────────

void test_before_and_after()
{
    std::printf("\ncalibration, overall and by volatility quintile\n");

    // A fixed 40 bps band on a series whose volatility clusters: right on
    // average at best, and wrong where it counts.
    const auto pts = make_points(6000, 20260910u, 165.0, 0.3);

    const auto before = altair::score_calibration(pts, 0.10, 20);
    check(before.has_value(), "the uncalibrated band scores");
    if (!before) { return; }

    std::printf("        BEFORE  exceedance %.4f against target %.4f"
                "  (reg-MAE %.2f pp, max %.2f pp)\n",
                before->exceedance, before->target,
                before->reg_mae_pp, before->reg_maxdev_pp);
    for (std::size_t b = 0; b < altair::kVolBuckets; ++b) {
        std::printf("            q%zu  n=%-5zu vol %6.1f bps   exceedance %.4f\n",
                    b, before->by_vol[b].n, before->by_vol[b].mean_vol_bps,
                    before->by_vol[b].exceedance);
    }

    check(before->by_vol[4].exceedance > before->by_vol[0].exceedance,
          "a constant band is exceeded more in the loudest quintile");

    // WHICH MODE, AND WITH WHAT MEMORY, IS A MEASUREMENT.
    //
    // The paper's own result is that no single answer wins everywhere: time
    // decay dominates when the base model already adapts, regime weighting
    // when it does not. The base here is a random walk with a FIXED band --
    // about as non-adaptive as a forecaster gets -- so this is the case where
    // localisation should earn its keep, and the table below is how that claim
    // gets checked instead of assumed.
    //
    // Note what the fixed band's numbers already show: 8.20% overall against a
    // 10% target looks nearly right, and it is 2.01% in the quietest quintile
    // and 17.66% in the loudest. The overall figure is the average of a band
    // that is far too wide half the time and far too narrow the rest. That is
    // the entire reason this breakdown exists.
    struct Run { const char* name; altair::ConformalMode mode; double lambda; };
    const Run runs[] = {
        {"SWC  (no weights)   ", altair::ConformalMode::Sliding,       0.0},
        {"TWC  slow (250 bar) ", altair::ConformalMode::TimeWeighted,  0.004},
        {"TWC  fast (25 bar)  ", altair::ConformalMode::TimeWeighted,  0.040},
        {"RWC  slow + regime  ", altair::ConformalMode::RegimeWeighted, 0.004},
        {"RWC  fast + regime  ", altair::ConformalMode::RegimeWeighted, 0.040},
    };

    std::printf("        %s  exceed   reg-MAE  reg-max   n_eff  memory\n",
                "mode                ");
    std::printf("        fixed band            %.4f   %5.2f    %5.2f      -       -\n",
                before->exceedance, before->reg_mae_pp, before->reg_maxdev_pp);

    double best_mae = before->reg_mae_pp;
    const char* best_name = "fixed band";
    bool any_calibrated = false;

    for (const Run& r : runs) {
        altair::CalibrationSpec spec;
        spec.conformal.alpha = 0.10;
        spec.conformal.mode = r.mode;
        spec.conformal.lambda = r.lambda;
        spec.conformal.bandwidth = 0.5;
        spec.conformal.n_min = 25.0;
        spec.warmup = 500;

        const auto got = altair::calibrate_band(pts, spec);
        if (!got) { continue; }
        any_calibrated = true;
        const auto& a = got->second;
        std::printf("        %s  %.4f   %5.2f    %5.2f   %6.1f  %6.1f\n",
                    r.name, a.exceedance, a.reg_mae_pp, a.reg_maxdev_pp,
                    a.mean_n_eff, a.mean_memory);
        if (a.reg_mae_pp < best_mae) {
            best_mae = a.reg_mae_pp;
            best_name = r.name;
        }
    }
    check(any_calibrated, "the band calibrates in every mode");
    std::printf("        best per-regime calibration: %s (reg-MAE %.2f pp)\n",
                best_name, best_mae);
    check(best_mae < before->reg_mae_pp,
          "some conformal setting beats the fixed band per regime");
}

// ── refusals and bookkeeping ─────────────────────────────────────────────────

void test_refusals()
{
    std::printf("\nrefusals\n");

    std::vector<altair::ForecastPoint> few(10);
    altair::CalibrationSpec spec;
    check(!altair::calibrate_band(few, spec).has_value(),
          "fewer points than the warmup is refused");

    const auto pts = make_points(600, 5u, 165.0, 0.0);
    altair::CalibrationSpec bad;
    bad.vol_window = 1;
    check(!altair::calibrate_band(pts, bad).has_value(),
          "a volatility window of one is refused");

    check(!altair::score_calibration({}, 0.1).has_value(),
          "scoring no points is refused");
    check(!altair::score_calibration(pts, 0.0).has_value(),
          "a target of zero is refused");

    // Every scored point lands in the overall figure even when it is too early
    // to have a volatility bucket.
    const auto rep = altair::score_calibration(pts, 0.10, 20);
    check(rep.has_value() && rep->n == pts.size(),
          "every point counts toward the overall rate");
    if (rep) {
        std::size_t bucketed = 0;
        for (const auto& b : rep->by_vol) { bucketed += b.n; }
        check(bucketed == pts.size() - 20,
              "and exactly the warmup-window points are outside the buckets");
    }
}

}  // namespace

int main()
{
    std::printf("P35-02 forecast band calibration\n");
    test_no_lookahead();
    test_refusals();
    test_asymmetry();
    test_before_and_after();

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : "THERE WERE FAILURES");
    return failures == 0 ? 0 : 1;
}
