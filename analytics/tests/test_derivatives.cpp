// P3-07 acceptance tests for analytics/derivatives.hpp.
//
// The load-bearing demonstration is test 2: a series whose true velocity is
// EXACTLY ZERO -- a constant price plus a one-tick bid-ask bounce -- run
// through the naive two-point difference and through the polynomial fit. One
// of them reports the bounce as a velocity of a thousand paise per second. If
// this file establishes nothing else, it establishes that.
//
// Everything else is checked against series with an analytically known
// derivative, so there is a right answer rather than a plausible one.
//
// No check description here may contain the substring FAIL.

#include <analytics/derivatives.hpp>

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

} // namespace

using namespace altair;

namespace {

constexpr std::int64_t kMs = 1'000'000;          // ns per millisecond
constexpr std::size_t kMaxPts = 4000;
TimedPoint g_pts[kMaxPts];

/// Build a series sampled every `step_ms` over `span_ms` ending at t_eval = 0.
/// `f` takes seconds-before-now (negative) and returns the value.
template <typename F>
std::size_t build(std::int64_t step_ms, std::int64_t span_ms, F f)
{
    std::size_t n = 0;
    for (std::int64_t ms = -span_ms; ms <= 0 && n < kMaxPts; ms += step_ms) {
        const double t = static_cast<double>(ms) / 1000.0;   // seconds
        g_pts[n].ts = Timestamp{ms * kMs};
        g_pts[n].value = f(t);
        ++n;
    }
    return n;
}

const Timestamp kNow{0};

// ── 1 ────────────────────────────────────────────────────────────────────
// Exact recovery on a polynomial with known derivatives. If a cubic through
// cubic data does not return the cubic's own coefficients, nothing else here
// means anything.
void a_known_polynomial_is_recovered_exactly()
{
    std::printf("\n1 a_known_polynomial_is_recovered_exactly\n");
    // p(t) = 2408000 + 300*t + 40*t^2 + 6*t^3, t in seconds.
    // p'(0) = 300, p''(0) = 80, p'''(0) = 36.
    const std::size_t n = build(10, 2000, [](double t) {
        return 2'408'000.0 + 300.0 * t + 40.0 * t * t + 6.0 * t * t * t;
    });
    const auto d = derivatives_at(g_pts, n, kNow, duration::seconds(2), 3);
    check(d.has_value(), "a cubic fit over 2 seconds of 10 ms ticks succeeds");
    if (!d) { return; }
    std::printf("    value %.6f  velocity %.9f  acceleration %.9f"
                "  jerk %.9f\n", d->value, d->velocity, d->acceleration,
                d->jerk);
    check(rel_near(d->value, 2'408'000.0, 1e-9), "the fitted value is p(0)");
    check(rel_near(d->velocity, 300.0, 1e-9), "velocity is p'(0) = 300");
    check(rel_near(d->acceleration, 80.0, 1e-8),
          "acceleration is p''(0) = 2*c2 = 80 -- the factor of 2 is present");
    check(rel_near(d->jerk, 36.0, 1e-7),
          "jerk is p'''(0) = 6*c3 = 36 -- the factor of 6 is present");
    check(d->residual_sd < 1e-6, "and the fit is exact, so residuals vanish");
    check(d->order == 3 && d->scale.raw() == duration::seconds(2).raw(),
          "the result carries the order and the scale that produced it");

    // A QUADRATIC fit to cubic data must be biased. Reported rather than
    // treated as a defect: it is the cost of choosing an order.
    const auto q = derivatives_at(g_pts, n, kNow, duration::seconds(2), 2);
    if (q) {
        std::printf("    quadratic fit to cubic data: velocity %.4f"
                    " (truth 300)\n", q->velocity);
        check(std::fabs(q->velocity - 300.0) > 1.0,
              "a quadratic through cubic data reports a biased velocity --"
              " the order is a real choice, not a detail");
    }
}

// ── 2 ────────────────────────────────────────────────────────────────────
// THE demonstration. True velocity exactly zero, plus a one-tick bid-ask
// bounce. The naive difference measures the spread.
void the_naive_difference_measures_the_bid_ask_bounce()
{
    std::printf("\n2 the_naive_difference_measures_the_bid_ask_bounce\n");
    // A constant Rs 24,080 quote, alternating between bid and ask by one
    // 5-paise tick. True dP/dt is exactly 0.
    const std::size_t n = build(10, 2000, [](double t) {
        const long k = std::lround(t * 100.0);
        return 2'408'000.0 + ((k % 2 == 0) ? 2.5 : -2.5);
    });

    // The naive two-point difference over the last 10 ms.
    const double dt = 0.010;
    const double naive = (g_pts[n - 1].value - g_pts[n - 2].value) / dt;

    const auto d = derivatives_at(g_pts, n, kNow, duration::seconds(1), 2);
    check(d.has_value(), "the polynomial fit succeeds on bouncing data");
    if (!d) { return; }
    std::printf("    true velocity 0.000 paise/s\n");
    std::printf("    two-point difference at 10 ms: %+.1f paise/s\n", naive);
    std::printf("    polynomial fit over 1 s:       %+.4f paise/s"
                "  (residual sd %.3f paise)\n", d->velocity, d->residual_sd);
    check(std::fabs(naive) > 100.0,
          "the two-point difference reports a velocity of hundreds of paise"
          " per second on a series that is not moving at all");
    check(std::fabs(d->velocity) < 1.0,
          "the polynomial fit reports essentially zero");
    check(std::fabs(naive) > 100.0 * std::fabs(d->velocity),
          "the naive estimator is more than a hundred times further from the"
          " truth -- it is measuring the spread, accurately");

    // And the error bar says so: a velocity inside its own noise is not a
    // velocity.
    std::printf("    velocity %.4f, standard error %.4f -> %s\n",
                d->velocity, velocity_std_error(*d),
                velocity_is_significant(*d) ? "SIGNIFICANT" : "not significant");
    check(!velocity_is_significant(*d),
          "and the error bar correctly refuses to call it a signal");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// The noise/responsiveness tradeoff, measured rather than asserted.
void a_longer_window_trades_noise_against_lag()
{
    std::printf("\n3 a_longer_window_trades_noise_against_lag\n");
    // A pure drift of 500 paise/s plus Gaussian noise of 20 paise.
    Gauss g{0xD3D3D3ULL};
    const std::size_t n = build(10, 4000, [&](double t) {
        return 2'408'000.0 + 500.0 * t + 20.0 * g.next();
    });

    std::printf("    truth 500.000 paise/s, noise sd 20 paise\n");
    struct Row { double win_s; double v; double se; };
    Row rows[4];
    int i = 0;
    for (std::int64_t ms : {200, 500, 1000, 4000}) {
        const auto d = derivatives_at(g_pts, n, kNow, duration::millis(ms), 2);
        if (!d) { check(false, "window fits"); return; }
        rows[i] = Row{static_cast<double>(ms) / 1000.0, d->velocity,
                      velocity_std_error(*d)};
        std::printf("    window %5.2f s, %4d points: velocity %+9.2f"
                    "   std error %8.2f\n",
                    rows[i].win_s, d->points, rows[i].v, rows[i].se);
        ++i;
    }
    check(rows[3].se < rows[0].se,
          "a longer window gives a smaller standard error");
    check(rows[0].se > 4.0 * rows[3].se,
          "and the reduction is large -- se falls roughly as 1/(sqrt(N)*h),"
          " so a 20x longer window is far more than 20x quieter");
    check(std::fabs(rows[3].v - 500.0) < std::fabs(rows[0].v - 500.0),
          "the long window is also closer to the truth on a pure drift");

    // The other half of the trade: on a TURNING price the long window lags.
    // A sine wave has a known derivative everywhere, so the lag is measurable.
    const double omega = 2.0 * 3.14159265358979323846 / 2.0;   // 0.5 Hz
    build(10, 4000, [&](double t) {
        return 2'408'000.0 + 10'000.0 * std::sin(omega * t);
    });
    const double truth = 10'000.0 * omega * std::cos(0.0);   // at t = 0
    std::printf("    on a 0.5 Hz sine, true velocity at t=0 is %+.1f paise/s\n",
                truth);
    for (std::int64_t ms : {200, 1000, 4000}) {
        const auto d = derivatives_at(g_pts, kMaxPts > 401 ? 401 : kMaxPts,
                                      kNow, duration::millis(ms), 2);
        if (d) {
            std::printf("      window %5.2f s: velocity %+9.1f"
                        "  (%.1f%% of truth)\n",
                        static_cast<double>(ms) / 1000.0, d->velocity,
                        100.0 * d->velocity / truth);
        }
    }
    const auto shortw = derivatives_at(g_pts, 401, kNow,
                                       duration::millis(200), 2);
    const auto longw = derivatives_at(g_pts, 401, kNow,
                                      duration::millis(4000), 2);
    check(shortw && longw, "both windows fit the sine");
    if (shortw && longw) {
        check(std::fabs(shortw->velocity - truth)
              < std::fabs(longw->velocity - truth),
              "on a turning price the SHORT window is the accurate one --"
              " the opposite of the drift case, which is the whole tradeoff");
    }
}

// ── 4 ────────────────────────────────────────────────────────────────────
// Irregular sampling. Ticks do not arrive on a grid, and a fixed-lag
// difference silently assumes they do.
void irregular_arrival_is_handled_by_construction()
{
    std::printf("\n4 irregular_arrival_is_handled_by_construction\n");
    // Same drift, but with wildly uneven gaps: 1 ms bursts and 200 ms lulls,
    // the shape of a real session.
    std::size_t n = 0;
    std::int64_t ms = -3000;
    int burst = 0;
    while (ms <= 0 && n < kMaxPts) {
        const double t = static_cast<double>(ms) / 1000.0;
        g_pts[n].ts = Timestamp{ms * kMs};
        g_pts[n].value = 2'408'000.0 + 500.0 * t;
        ++n;
        ++burst;
        ms += (burst % 20 < 15) ? 1 : 200;   // bursts, then a lull
    }
    std::printf("    %zu points over 3 s, gaps alternating 1 ms and 200 ms\n",
                n);
    const auto d = derivatives_at(g_pts, n, kNow, duration::seconds(3), 2);
    check(d.has_value(), "an irregular series fits");
    if (!d) { return; }
    std::printf("    velocity %.6f paise/s (truth 500)\n", d->velocity);
    check(rel_near(d->velocity, 500.0, 1e-6),
          "the fit recovers the true drift exactly despite 200x variation in"
          " the sampling interval -- the timestamps do the work");

    // A fixed-LAG difference on the same data does not, because it assumes a
    // uniform grid that this series does not have.
    const double lag_naive = (g_pts[n - 1].value - g_pts[n - 21].value) / 0.020;
    std::printf("    a fixed 20-tick-lag difference assuming 1 ms spacing:"
                " %.1f paise/s\n", lag_naive);
    check(std::fabs(lag_naive - 500.0) > 100.0,
          "while a fixed-lag difference that assumes a uniform grid is badly"
          " wrong -- the hazard is real, not hypothetical");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void multiscale_reports_every_scale_it_was_asked_for()
{
    std::printf("\n5 multiscale_reports_every_scale_it_was_asked_for\n");
    // A price rising over the last 5 s but pulling back over the last 200 ms.
    // No single-scale estimate can describe that; the point of the primitive
    // is that it does not have to.
    // 20000*t is a steady rise across the whole 5 s; -3000*exp(20t) is a
    // pullback that is negligible before the last ~200 ms and then dominates.
    // At t = 0 the instantaneous slope is 20000 - 60000 = -40000, while the
    // average slope over the 5 s is about +19400. Both are true at once.
    const std::size_t n = build(10, 5000, [](double t) {
        return 2'408'000.0 + 20'000.0 * t - 3'000.0 * std::exp(20.0 * t);
    });
    const Duration windows[4] = {duration::millis(200), duration::millis(500),
                                 duration::seconds(2), duration::seconds(5)};
    Derivatives out[4];
    const auto k = derivatives_multiscale(g_pts, n, kNow, windows, 4, out, 2);
    check(k.has_value() && *k == 4, "all four scales produce a result");
    if (!k) { return; }
    for (int i = 0; i < *k; ++i) {
        std::printf("    scale %6.2f s: velocity %+10.2f paise/s"
                    "   acceleration %+12.2f\n",
                    static_cast<double>(out[i].scale.raw()) / 1e9,
                    out[i].velocity, out[i].acceleration);
    }
    check(out[0].velocity < 0.0,
          "the shortest scale sees the pullback");
    check(out[3].velocity > 0.0,
          "the longest scale sees the rise -- both are true, which is why"
          " asking for THE velocity is a malformed question");
    bool scales_kept = true;
    for (int i = 0; i < 4; ++i) {
        if (out[i].scale.raw() != windows[i].raw()) { scales_kept = false; }
    }
    check(scales_kept,
          "every result carries its own scale, so two cannot be compared"
          " across scales by accident");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void degenerate_input_is_refused()
{
    std::printf("\n6 degenerate_input_is_refused\n");
    const std::size_t n = build(10, 1000, [](double t) {
        return 2'408'000.0 + 100.0 * t;
    });
    auto err = [&](std::size_t cnt, Duration w, int order) {
        const auto r = derivatives_at(g_pts, cnt, kNow, w, order);
        return r ? DerivativeError::NotFinite : r.error();
    };
    check(err(n, Duration{0}, 2) == DerivativeError::BadWindow,
          "a zero window is refused");
    check(err(n, Duration{-1}, 2) == DerivativeError::BadWindow,
          "and a negative one");
    check(err(n, duration::seconds(1), 0) == DerivativeError::BadWindow,
          "order 0 is refused -- there is no derivative to report");
    check(err(n, duration::seconds(1), 4) == DerivativeError::BadWindow,
          "and order 4 exceeds what market data supports");
    check(err(2, duration::seconds(1), 2) == DerivativeError::TooFewPoints,
          "a quadratic through two points is refused, not interpolated");

    // All points at one instant: no slope exists.
    for (std::size_t i = 0; i < 10; ++i) {
        g_pts[i].ts = Timestamp{0};
        g_pts[i].value = 2'408'000.0 + static_cast<double>(i);
    }
    check(err(10, duration::seconds(1), 2) == DerivativeError::ZeroTimeSpan,
          "ten points at one timestamp have no velocity, and saying so beats"
          " dividing by zero");

    // Points entirely outside the window are ignored, leaving too few.
    build(10, 1000, [](double t) { return 2'408'000.0 + 100.0 * t; });
    check(err(n, duration::millis(5), 2) == DerivativeError::TooFewPoints,
          "a window shorter than the tick spacing catches too few points");

    // A future-stamped point must not enter the fit: reading it would be
    // look-ahead (rule 7).
    const std::size_t m = build(10, 1000, [](double t) {
        return 2'408'000.0 + 100.0 * t;
    });
    g_pts[m - 1].ts = Timestamp{500 * kMs};        // 500 ms in the FUTURE
    g_pts[m - 1].value = 9'999'999.0;              // and wildly wrong
    const auto d = derivatives_at(g_pts, m, kNow, duration::seconds(1), 2);
    check(d.has_value(), "the fit still succeeds with a future point present");
    if (d) {
        std::printf("    with a future-stamped outlier present:"
                    " velocity %.4f (truth 100)\n", d->velocity);
        check(rel_near(d->velocity, 100.0, 1e-6),
              "and the future point is EXCLUDED -- a derivative that reads"
              " past t_eval is look-ahead, which rule 7 forbids outright");
    }
}

} // namespace

int main()
{
    std::printf("altair analytics derivatives tests\n");
    a_known_polynomial_is_recovered_exactly();
    the_naive_difference_measures_the_bid_ask_bounce();
    a_longer_window_trades_noise_against_lag();
    irregular_arrival_is_handled_by_construction();
    multiscale_reports_every_scale_it_was_asked_for();
    degenerate_input_is_refused();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
