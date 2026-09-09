// P35-01 acceptance tests for models/conformal.hpp.
//
// THE CLAIM UNDER TEST IS A NUMBER, NOT A SHAPE.
//
// A calibrator is easy to write and hard to know you got right, because a
// wrong one still returns a plausible band. So the test is the paper's own
// experiment reduced to synthetic data where the truth is known by
// construction: take a forecaster whose band is deliberately too narrow, and
// require that the wrapper moves its exceedance rate from badly wrong to close
// to target.
//
// WHAT THESE CHECKS ACTUALLY CATCH, MEASURED BY PLANTING THE DEFECT.
//
// Every line below was established by breaking conformal.hpp on purpose and
// recording which checks fired -- not by reasoning about which ones ought to.
// Three checks in this repo have shipped reporting clean while broken (P33),
// and the first draft of this comment asserted a separation the plants then
// disproved.
//
//   plant                        result
//   ---------------------------  --------------------------------------------
//   buffer = 0        (no-op)    5 checks fired; exceedance stayed at 19.12%
//   buffer * 1.6      (scaled)   3 fired; miscalibrated case over-corrected to
//                                2.26% against a 5% target. NOTE the control's
//                                rate check did NOT fire (5.86%) -- scaling a
//                                near-zero buffer barely moves a correct band
//   buffer + 0.8      (padded)   6 fired, including the buffer-size check at
//                                43.1% of the band half-width against a 35%
//                                threshold
//
// So the exceedance rates are the primary detector and they are sensitive in
// both directions. The buffer-size check is NOT independent of them -- the pad
// that trips it trips them too -- and it is kept anyway because it states the
// property directly. "The correction applied to an already-correct band was 5%
// of that band" is a claim a reader can check; "the exceedance rate stayed near
// 5%" is a symptom they have to reason backwards from.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <models/conformal.hpp>

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
        std::printf("  ok  : %s (%.4f, want %.4f +/- %.4f)\n", what, got, want, tol);
    } else {
        ++failures;
        std::printf("  FAIL: %s (%.4f, want %.4f +/- %.4f)\n",
                    what, got, want, tol);
    }
}

// ── the weighted quantile, against cases computed by hand ────────────────────

void test_weighted_quantile()
{
    std::printf("\nweighted_quantile\n");

    // Uniform weights reduce to the ordinary quantile. Values 1..10, level 0.5
    // wants the smallest value whose cumulative weight reaches 5 of 10 -- the
    // fifth value, 5.
    {
        std::vector<double> v{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        std::vector<double> w(10, 1.0);
        const auto q = altair::weighted_quantile(v, w, 0.5);
        check(q.has_value() && *q == 5.0, "uniform weights give the plain quantile");
        const auto hi = altair::weighted_quantile(v, w, 0.95);
        check(hi.has_value() && *hi == 10.0, "level 0.95 of ten reaches the tenth");
    }

    // INPUT ORDER MUST NOT MATTER. The same multiset shuffled must give the
    // same answer -- this is the bug that a sorted-input-only implementation
    // hides, and real score buffers are in time order, not value order.
    {
        std::vector<double> v{7, 2, 9, 1, 5};
        std::vector<double> w(5, 1.0);
        const auto a = altair::weighted_quantile(v, w, 0.6);
        std::vector<double> v2{1, 5, 9, 7, 2};
        const auto b = altair::weighted_quantile(v2, w, 0.6);
        check(a.has_value() && b.has_value() && *a == *b,
              "unsorted input gives the same quantile as sorted");
    }

    // One dominant weight drags the quantile onto its value.
    {
        std::vector<double> v{1, 2, 3, 100};
        std::vector<double> w{0.001, 0.001, 0.001, 1000.0};
        const auto q = altair::weighted_quantile(v, w, 0.5);
        check(q.has_value() && *q == 100.0,
              "a dominant weight carries the quantile to its own value");
    }

    // Refusals.
    {
        std::vector<double> v{1, 2};
        std::vector<double> w{1.0};
        check(!altair::weighted_quantile(v, w, 0.5).has_value(),
              "mismatched lengths are refused");
        std::vector<double> z{0.0, 0.0};
        std::vector<double> v2{1, 2};
        check(!altair::weighted_quantile(v2, z, 0.5).has_value(),
              "all-zero weights are refused rather than divided by");
        std::vector<double> neg{-1.0, 1.0};
        check(!altair::weighted_quantile(v2, neg, 0.5).has_value(),
              "a negative weight is refused");
    }
}

// ── the experiment ───────────────────────────────────────────────────────────

/// A forecaster with a deliberately chosen error scale.
///
/// The truth is a random walk with volatility-clustering innovations. The base
/// forecaster predicts "no change", which is optimal in the mean, and publishes
/// a one-sided upper bound at `claimed_sigma` multiples. When `claimed_sigma` is
/// the true unconditional sigma the band is right on average and still wrong
/// per-regime; when it is half of it the band is wrong outright.
struct Experiment {
    std::vector<double> actual;    // realised next value
    std::vector<double> forecast;  // base point forecast
    std::vector<double> bound;     // base one-sided upper bound
    std::vector<double> regime;    // one regime feature: recent volatility
    /// Half-width of the base band. The buffer is only meaningful relative to
    /// the thing it is correcting.
    double half_width = 0.0;
};

Experiment make(std::size_t n, unsigned seed, double sigma_scale, double alpha)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> z(0.0, 1.0);

    // GARCH-ish variance so volatility clusters, which is what makes a constant
    // band wrong in a way a conformal wrapper can see and fix.
    const double omega = 0.05, a = 0.10, b = 0.85;
    double var = omega / (1.0 - a - b);
    const double uncond = std::sqrt(var);

    // The one-sided normal multiplier for the target level.
    // alpha 0.05 -> 1.6449.
    const double k = alpha > 0.049 && alpha < 0.051 ? 1.6449 : 2.3263;

    Experiment e;
    e.half_width = k * sigma_scale * uncond;
    e.actual.reserve(n);
    e.forecast.reserve(n);
    e.bound.reserve(n);
    e.regime.reserve(n);

    double level = 100.0;
    double recent = uncond;
    for (std::size_t i = 0; i < n; ++i) {
        const double eps = z(rng) * std::sqrt(var);
        const double next = level + eps;

        e.forecast.push_back(level);
        // The base model's band uses a CONSTANT sigma -- it does not know the
        // variance is clustering. sigma_scale 1.0 is "right on average",
        // 0.5 is "badly too narrow".
        e.bound.push_back(level + k * sigma_scale * uncond);
        e.actual.push_back(next);
        // The regime feature is the volatility the model could have seen, i.e.
        // computed from bars strictly before this one. Hard rule 7.
        e.regime.push_back(recent);

        recent = std::sqrt(var);
        var = omega + a * eps * eps + b * var;
        level = next;
    }
    return e;
}

/// Exceedance rate of a set of bounds: how often the truth went above.
double exceedance(const std::vector<double>& actual,
                  const std::vector<double>& bound, std::size_t from)
{
    std::size_t over = 0, n = 0;
    for (std::size_t i = from; i < actual.size(); ++i) {
        ++n;
        if (actual[i] > bound[i]) { ++over; }
    }
    return n > 0 ? static_cast<double>(over) / static_cast<double>(n) : 0.0;
}

/// Run the wrapper over an experiment and return the calibrated bounds.
std::vector<double> wrap(const Experiment& e, altair::ConformalSpec spec,
                         std::size_t warmup, std::size_t* fallbacks = nullptr,
                         std::size_t* dropped = nullptr,
                         double* mean_buffer = nullptr)
{
    // The scaler is fitted on the WARMUP ONLY -- never on the scored period.
    altair::RegimeScaler scaler;
    std::vector<double> train(e.regime.begin(),
                              e.regime.begin()
                                  + static_cast<std::ptrdiff_t>(warmup));
    (void)scaler.fit(train, 1);

    altair::Conformal c(spec);
    std::vector<double> out(e.actual.size(), 0.0);
    std::vector<double> zs(1, 0.0);

    for (std::size_t i = 0; i < e.actual.size(); ++i) {
        scaler.transform(std::span(&e.regime[i], 1), zs);

        const auto step = c.calibrate(zs);
        // Before there is any history the base bound stands, uncalibrated.
        out[i] = step ? e.bound[i] + step->buffer : e.bound[i];

        // Only after the bar has closed. `bound` already contains the base
        // band, so the score is measured against the BOUND, not the point --
        // the buffer then corrects the bound directly.
        (void)c.observe(e.actual[i] - e.bound[i], zs);
    }
    if (fallbacks != nullptr) { *fallbacks = c.fallbacks(); }
    if (dropped != nullptr) { *dropped = c.dropped(); }
    if (mean_buffer != nullptr) {
        double sum = 0.0;
        std::size_t m = 0;
        for (std::size_t i = warmup; i < out.size(); ++i) {
            sum += out[i] - e.bound[i];
            ++m;
        }
        *mean_buffer = m > 0 ? sum / static_cast<double>(m) : 0.0;
    }
    return out;
}

void test_calibration()
{
    std::printf("\ncalibration -- the paper's experiment\n");

    constexpr std::size_t kN = 6000;
    constexpr std::size_t kWarm = 1000;
    constexpr double kAlpha = 0.05;

    // ── the miscalibrated base ───────────────────────────────────────────
    {
        const Experiment e = make(kN, 20260910u, 0.5, kAlpha);
        const double raw = exceedance(e.actual, e.bound, kWarm);
        check(raw > 0.12,
              "a band at half the needed width is exceeded far above target");
        std::printf("        uncalibrated exceedance %.4f against target %.4f\n",
                    raw, kAlpha);

        altair::ConformalSpec spec;
        spec.alpha = kAlpha;
        spec.mode = altair::ConformalMode::TimeWeighted;
        spec.m = 500;
        spec.lambda = 0.004;
        const auto fixed = wrap(e, spec, kWarm);
        const double got = exceedance(e.actual, fixed, kWarm);
        std::printf("        TWC exceedance          %.4f\n", got);
        near(got, kAlpha, 0.02, "the wrapper brings exceedance back to target");
        check(got < raw * 0.5, "and it is a large move, not a nudge");
    }

    // ── THE CONTROL: an already-calibrated base must stay calibrated ─────
    //
    // This is the check that a calibrator which merely widens everything
    // cannot pass.
    {
        const Experiment e = make(kN, 7788u, 1.0, kAlpha);
        const double raw = exceedance(e.actual, e.bound, kWarm);
        std::printf("        control raw exceedance  %.4f\n", raw);

        altair::ConformalSpec spec;
        spec.alpha = kAlpha;
        spec.mode = altair::ConformalMode::TimeWeighted;
        double buf = 0.0;
        const auto fixed = wrap(e, spec, kWarm, nullptr, nullptr, &buf);
        const double got = exceedance(e.actual, fixed, kWarm);
        std::printf("        control TWC exceedance  %.4f\n", got);
        near(got, kAlpha, 0.025,
             "an already-calibrated band is left near target");

        // THE PADDING CHECK. When the base band is already right the honest
        // buffer is close to zero, so a calibrator that quietly adds width
        // shows up here and nowhere else -- making a correct band slightly
        // wider barely moves its exceedance rate, which is exactly why this
        // needs its own check rather than trusting the rate to notice.
        const double ratio = std::fabs(buf) / e.half_width;
        std::printf("        control mean buffer     %+.4f  (%.1f%% of band "
                    "half-width %.4f)\n", buf, 100.0 * ratio,
                    e.half_width);
        check(ratio < 0.35,
              "and the buffer applied to it is small beside the band itself");
    }

    // ── the three modes all calibrate; none is allowed to be wild ────────
    {
        const Experiment e = make(kN, 5150u, 0.5, kAlpha);
        for (auto mode : {altair::ConformalMode::Sliding,
                          altair::ConformalMode::TimeWeighted,
                          altair::ConformalMode::RegimeWeighted}) {
            altair::ConformalSpec spec;
            spec.alpha = kAlpha;
            spec.mode = mode;
            spec.bandwidth = 1.0;
            spec.n_min = 20.0;
            std::size_t fb = 0;
            const auto fixed = wrap(e, spec, kWarm, &fb);
            const double got = exceedance(e.actual, fixed, kWarm);
            const char* name = mode == altair::ConformalMode::Sliding ? "SWC"
                             : mode == altair::ConformalMode::TimeWeighted ? "TWC"
                                                                           : "RWC";
            std::printf("        %s exceedance %.4f  (fallbacks %zu)\n",
                        name, got, fb);
            near(got, kAlpha, 0.025, "this mode calibrates to target");
        }
    }
}

// ── rule 11: both bounds must be visible ─────────────────────────────────────

void test_rule11()
{
    std::printf("\nrule 11 -- the bounds report themselves\n");

    // The buffer cap TRUNCATES, so it must count what it evicted.
    {
        altair::ConformalSpec spec;
        spec.m = 50;
        altair::Conformal c(spec);
        std::vector<double> z;
        for (int i = 0; i < 200; ++i) {
            (void)c.observe(static_cast<double>(i), z);
        }
        check(c.size() == 50, "the buffer holds exactly m scores");
        check(c.dropped() == 150, "and every evicted score is counted");
    }

    // PLANTED: a regime far from everything in the buffer must trip the
    // effective-sample floor and be COUNTED, not silently absorbed.
    {
        altair::ConformalSpec spec;
        spec.mode = altair::ConformalMode::RegimeWeighted;
        spec.bandwidth = 0.5;
        spec.n_min = 30.0;
        altair::Conformal c(spec);

        std::vector<double> z(1, 0.0);
        for (int i = 0; i < 200; ++i) {
            z[0] = 0.001 * static_cast<double>(i % 5);
            (void)c.observe(static_cast<double>(i % 7), z);
        }
        check(c.fallbacks() == 0, "a familiar regime does not trip the floor");

        std::vector<double> far(1, 500.0);   // nothing in the buffer is near this
        const auto step = c.calibrate(far);
        check(step.has_value(), "an unfamiliar regime still publishes a bound");
        check(step && step->fell_back,
              "and it says on the step that it fell back to time-only weights");
        check(c.fallbacks() == 1, "and the fallback is counted");
    }

    // n_eff must actually respond to concentration.
    {
        altair::ConformalSpec spec;
        spec.mode = altair::ConformalMode::Sliding;
        altair::Conformal c(spec);
        std::vector<double> z;
        for (int i = 0; i < 100; ++i) { (void)c.observe(1.0, z); }
        const auto step = c.calibrate(z);
        check(step.has_value(), "uniform weights calibrate");
        near(step ? step->n_eff : 0.0, 100.0, 1e-6,
             "uniform weights give n_eff equal to the count");
    }
    {
        altair::ConformalSpec spec;
        spec.mode = altair::ConformalMode::TimeWeighted;
        spec.lambda = 1.0;   // forgets almost instantly
        altair::Conformal c(spec);
        std::vector<double> z;
        for (int i = 0; i < 100; ++i) { (void)c.observe(1.0, z); }
        const auto step = c.calibrate(z);
        check(step && step->n_eff < 5.0,
              "a hard decay concentrates the weights and n_eff collapses");
        check(step && step->effective_memory < 5.0,
              "and the effective memory is a few bars, not a hundred");
    }
}

// ── refusals ─────────────────────────────────────────────────────────────────

void test_refusals()
{
    std::printf("\nrefusals\n");

    {
        altair::Conformal c{altair::ConformalSpec{}};
        std::vector<double> z;
        const auto step = c.calibrate(z);
        check(!step.has_value()
                  && step.error() == altair::ConformalError::NoScores,
              "an empty buffer refuses rather than returning a zero buffer");
    }
    {
        altair::ConformalSpec spec;
        spec.mode = altair::ConformalMode::RegimeWeighted;
        altair::Conformal c(spec);
        std::vector<double> wide(altair::kMaxRegimeDim + 1, 1.0);
        const auto r = c.observe(1.0, wide);
        check(!r.has_value()
                  && r.error() == altair::ConformalError::EmbeddingTooWide,
              "an over-wide embedding is refused, not truncated");
    }
    {
        altair::ConformalSpec spec;
        spec.mode = altair::ConformalMode::RegimeWeighted;
        altair::Conformal c(spec);
        std::vector<double> two{1.0, 2.0};
        check(c.observe(1.0, two).has_value(), "a two-wide embedding is accepted");
        std::vector<double> three{1.0, 2.0, 3.0};
        const auto r = c.observe(1.0, three);
        check(!r.has_value()
                  && r.error() == altair::ConformalError::EmbeddingWidthChanged,
              "changing the embedding width mid-run is refused");
    }
    {
        altair::ConformalSpec bad;
        bad.alpha = 0.0;
        altair::Conformal c(bad);
        std::vector<double> z;
        (void)c.observe(1.0, z);
        check(!c.calibrate(z).has_value(), "alpha of zero is refused");
    }
}

// ── the scaler ───────────────────────────────────────────────────────────────

void test_scaler()
{
    std::printf("\nRegimeScaler\n");

    {
        // mean 3, population values 1..5 -> sample sd 1.5811
        std::vector<double> rows{1, 2, 3, 4, 5};
        altair::RegimeScaler s;
        check(s.fit(rows, 1).has_value(), "fits a one-column matrix");
        std::vector<double> out(1, 0.0);
        const double v = 5.0;
        s.transform(std::span(&v, 1), out);
        near(out[0], (5.0 - 3.0) / 1.5811388, 1e-5,
             "standardises using the fitted mean and sd");
    }
    {
        // A CONSTANT COLUMN MUST NOT DIVIDE BY ZERO.
        std::vector<double> rows{7, 7, 7, 7};
        altair::RegimeScaler s;
        check(s.fit(rows, 1).has_value(), "fits a constant column");
        std::vector<double> out(1, 0.0);
        const double v = 7.0;
        s.transform(std::span(&v, 1), out);
        check(std::isfinite(out[0]),
              "a constant column produces a finite value, not a division by zero");
    }
    {
        std::vector<double> rows(altair::kMaxRegimeDim + 1, 1.0);
        altair::RegimeScaler s;
        const auto r = s.fit(rows, altair::kMaxRegimeDim + 1);
        check(!r.has_value()
                  && r.error() == altair::ConformalError::EmbeddingTooWide,
              "an over-wide fit is refused");
    }
}

}  // namespace

int main()
{
    std::printf("P35-01 conformal calibration\n");
    test_weighted_quantile();
    test_scaler();
    test_rule11();
    test_refusals();
    test_calibration();

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : "THERE WERE FAILURES");
    return failures == 0 ? 0 : 1;
}
