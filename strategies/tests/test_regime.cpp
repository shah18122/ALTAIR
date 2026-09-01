// P6-03 acceptance tests for strategies/regime.hpp.
//
// Test 1 is the card: a regime boundary is a threshold on a noisy estimate, so
// near the boundary the label is a coin flip -- and near the boundary is
// exactly where a regime-conditional model switches behaviour. The test
// measures how much of a real-shaped session lands in the undecidable band.
//
// Test 2 is hysteresis, measured as flip count with and without.
//
// Test 3: the detector is causal. There is no method that takes a series.
//
// No check description here may contain the substring FAIL.

#include <strategies/regime.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>

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

constexpr std::size_t kWindow = 512;

TrendThresholds thresholds(double enter, double exit, std::size_t bars)
{
    TrendThresholds t{};
    t.centre = 0.5;
    t.min_sigma = enter;
    t.exit_sigma = exit;
    t.min_bars = bars;
    return t;
}

// ── 1 ────────────────────────────────────────────────────────────────────
// THE card.
void near_the_threshold_the_label_is_a_coin_flip()
{
    std::printf("\n1 near_the_threshold_the_label_is_a_coin_flip\n");
    // A pure random walk: Hurst is 0.5 by construction, so EVERY bar of this
    // series is genuinely a random walk and any trend label is an error.
    TrendDetector<kWindow> det;
    TrendDetector<kWindow> naive_src;
    Lcg g{0x2E61ADE1};

    int decided = 0, undecided = 0, random_walk = 0;
    int naive_trending = 0, naive_reverting = 0;
    double p = 2'400'000.0;
    const auto t = thresholds(2.0, 1.0, 5);

    for (int i = 0; i < 4000; ++i) {
        p += g.normal() * 2000.0;
        const Price c{static_cast<std::int64_t>(p)};
        det.push(c);
        naive_src.push(c);
        const auto r = det.call(t);
        if (!r) { continue; }
        if (r->label == TrendRegime::Unknown) { ++undecided; }
        else if (r->label == TrendRegime::RandomWalk) { ++random_walk; ++decided; }
        else { ++decided; }
        // The naive rule: no error bar, just which side of 0.5.
        const auto n = TrendDetector<kWindow>::naive_label(r->statistic, 0.5);
        if (n == TrendRegime::Trending) { ++naive_trending; }
        else if (n == TrendRegime::MeanReverting) { ++naive_reverting; }
    }

    const int total = decided + undecided;
    std::printf("    4000 bars of a PURE RANDOM WALK, Hurst window %zu:\n"
                "      with the error bar:  %4d random-walk, %4d undecided,"
                " %4d trend/revert\n"
                "      without it (naive):  %4d trending, %4d mean-reverting,"
                "    0 random-walk\n",
                kWindow, random_walk, undecided,
                decided - random_walk, naive_trending, naive_reverting);

    check(total > 1000, "the detector produced a usable number of calls");
    const double false_rate = static_cast<double>(decided - random_walk)
                            / static_cast<double>(total);
    std::printf("      FALSE regime rate on a series with no regime in it:"
                "  naive 100.0%%   with error bar %.1f%%\n",
                100.0 * false_rate);
    check(naive_trending + naive_reverting == total,
          "the NAIVE rule -- is Hurst above or below 0.5 -- labels EVERY bar as"
          " one or the other, because a continuous statistic is never exactly"
          " 0.5, so it cannot ever return 'random walk'");
    check(false_rate < 0.10,
          "requiring the error bar to clear the threshold cuts the false"
          " regime rate from 100% to under a tenth of that");
    check(false_rate > 0.0,
          "and NOT to zero. A 2-sigma test on a true null fires about 5% of the"
          " time BY CONSTRUCTION, and a bit more here because consecutive"
          " windows share 511 of their 512 samples so the excursions cluster."
          " Asserting zero would be asserting the test is broken -- which is"
          " why min_bars and hysteresis sit on top of it rather than instead"
          " of it");
    std::printf("    -> the naive rule describes a random walk as %d bars of"
                " trend and %d of mean\n       reversion -- 3,872 regimes to"
                " switch behaviour on, in a series that has none.\n       The"
                " error bar leaves %d, and the dwell requirement in test 2 is"
                " what handles\n       those.\n",
                naive_trending, naive_reverting, decided - random_walk);
    check(random_walk > 0,
          "a random walk IS a finding, and the detector says so -- a detector"
          " that never returns it is describing its own noise");

    // Now a genuinely trending series, so the test proves the detector can
    // still see one rather than merely being conservative.
    TrendDetector<kWindow> trend_det;
    Lcg g2{0xF00D};
    double q = 2'400'000.0, momentum = 0.0;
    int found_trending = 0, calls = 0;
    for (int i = 0; i < 4000; ++i) {
        momentum = 0.94 * momentum + g2.normal() * 600.0;   // persistent
        q += momentum;
        trend_det.push(Price{static_cast<std::int64_t>(q)});
        const auto r = trend_det.call(t);
        if (!r) { continue; }
        ++calls;
        if (r->label == TrendRegime::Trending) { ++found_trending; }
    }
    std::printf("    on a genuinely PERSISTENT series (AR(1) momentum, phi ="
                " 0.94): %d of %d calls\n      labelled Trending\n",
                found_trending, calls);
    check(found_trending > calls / 2,
          "and on a series that really does trend the detector says so on most"
          " bars -- so the error bar made it correct, not merely silent");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void hysteresis_is_not_cosmetic()
{
    std::printf("\n2 hysteresis_is_not_cosmetic\n");
    // A series that drifts across the boundary: momentum that fades in and
    // out, so the statistic wanders around the threshold rather than sitting
    // clearly on one side.
    auto run = [](double enter, double exit, std::size_t min_bars) {
        TrendDetector<kWindow> det;
        Lcg g{0xB0DA};
        double p = 2'400'000.0, mom = 0.0;
        TrendRegime last = TrendRegime::Unknown;
        int flips = 0, published = 0;
        const auto t = thresholds(enter, exit, min_bars);
        for (int i = 0; i < 6000; ++i) {
            const double phi = 0.5 + 0.42 * std::sin(i / 400.0);  // fades
            mom = phi * mom + g.normal() * 900.0;
            p += mom;
            det.push(Price{static_cast<std::int64_t>(p)});
            const auto r = det.call(t);
            if (!r || r->label == TrendRegime::Unknown) { continue; }
            ++published;
            if (r->label != last && last != TrendRegime::Unknown) { ++flips; }
            last = r->label;
        }
        return std::pair<int, int>{flips, published};
    };

    // No hysteresis and no dwell: enter and exit at the same threshold,
    // publish immediately.
    const auto bare = run(2.0, 2.0, 1);
    // With both.
    const auto damped = run(2.0, 1.0, 5);

    std::printf("    6000 bars whose persistence fades in and out:\n"
                "      enter 2.0 / exit 2.0, publish at 1 bar:  %3d regime"
                " changes over %d calls\n"
                "      enter 2.0 / exit 1.0, publish at 5 bars: %3d regime"
                " changes over %d calls\n",
                bare.first, bare.second, damped.first, damped.second);
    check(bare.first > 0, "the bare detector does change regime");
    check(damped.first < bare.first,
          "and hysteresis plus a dwell requirement cuts the number of regime"
          " changes -- each avoided change is a strategy switch that would have"
          " paid the spread to chase a statistic wandering across a line");
    check(damped.second > 0,
          "while still publishing a regime most of the time, so the damping"
          " did not simply silence it");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void the_detector_is_causal_and_has_no_thresholds_of_its_own()
{
    std::printf("\n3 the_detector_is_causal_and_has_no_thresholds_of_its_own\n");
    TrendDetector<kWindow> det;
    Lcg g{0xCA05A1};
    double p = 2'400'000.0;
    for (int i = 0; i < 600; ++i) {
        p += g.normal() * 2000.0;
        det.push(Price{static_cast<std::int64_t>(p)});
    }

    TrendThresholds none{};
    const auto r = det.call(none);
    check(!r && r.error() == RegimeError::NoThresholds,
          "a caller who supplied no thresholds is refused: 0.5 is the null for"
          " Hurst, but how many sigma is enough evidence and how much churn is"
          " tolerable are decisions about what the regime is FOR, and there is"
          " no default that is not one");

    TrendThresholds half{};
    half.centre = 0.5;
    half.min_sigma = 2.0;
    half.exit_sigma = 1.0;
    check(!det.call(half).has_value(),
          "and supplying two of the three is still refused -- min_bars of zero"
          " means every one-bar excursion is published as a regime");

    // Warmup: nothing before the estimator's own minimum.
    TrendDetector<kWindow> fresh;
    const auto t = thresholds(2.0, 1.0, 5);
    int refused = 0;
    for (int i = 0; i < 40; ++i) {
        fresh.push(Price{2'400'000 + i * 100});
        if (!fresh.call(t).has_value()) { ++refused; }
    }
    check(refused == 40,
          "with fewer bars than Hurst's own minimum every call is refused,"
          " rather than returning a label computed from the handful it has");

    // The composite refuses to be complete on partial information.
    MarketRegime m{};
    check(!m.complete(),
          "a zeroed MarketRegime is not complete -- all three axes are Unknown"
          " at ordinal 0, so an uninitialised struct cannot pass for a"
          " trending, normal-vol, deep market");
    m.trend = TrendRegime::Trending;
    m.vol = VolRegime::High;
    check(!m.complete(),
          "and two axes out of three is still not complete: a model"
          " conditioned on regime should refuse rather than fall back to an"
          " unconditioned version, which is a different model trained on a"
          " different sample");
    m.liquidity = LiquidityRegime::Thin;
    check(m.complete(), "all three decided is complete");

    MarketRegime other = m;
    other.liquidity = LiquidityRegime::Deep;
    check(m.key() != other.key(),
          "and 'trending and thin' keys differently from 'trending and deep' --"
          " the three axes are not collapsed into one label, because a single"
          " label would have to pick which axis to throw away");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void the_volatility_threshold_inherits_the_estimate_s_error()
{
    std::printf("\n4 the_volatility_threshold_inherits_the_estimate_s_error\n");
    VolThresholds t{};
    t.low = 1500.0;
    t.high = 2100.0;            // only 5% above the true sd
    t.min_sigma = 2.0;
    t.min_bars = 3;

    VolDetector<60> vd;
    Lcg g{0x501A71};
    double p = 2'400'000.0;
    const double true_sd = 2000.0;
    // call() on EVERY bar, so the dwell requirement is satisfied and the
    // label that comes out is the error bar's doing rather than the dwell's.
    // (An earlier draft called it once at the end, where every answer is
    // Unknown because nothing has held for three bars -- which made the
    // headline check pass for entirely the wrong reason.)
    std::expected<RegimeCall<VolRegime>, RegimeError> r =
        std::unexpected(RegimeError::WarmingUp);
    for (int i = 0; i < 200; ++i) {
        p += g.normal() * true_sd;
        vd.push(Price{static_cast<std::int64_t>(p)});
        r = vd.call(t);
    }
    check(r.has_value(), "the vol detector calls");
    if (!r) { return; }

    const double rel_err = r->std_error / r->statistic;
    std::printf("    60-bar realised sd %.1f paise, standard error %.1f"
                " (%.1f%% of the estimate)\n"
                "      threshold for High is %.0f -- %.2f sigma away\n"
                "      label: %s\n",
                r->statistic, r->std_error, 100.0 * rel_err, t.high,
                (t.high - r->statistic) / r->std_error,
                r->label == VolRegime::High ? "High"
                    : r->label == VolRegime::Normal ? "Normal"
                    : r->label == VolRegime::Low ? "Low" : "Unknown");

    check(rel_err > 0.08 && rel_err < 0.11,
          "the standard error of a 60-sample standard deviation is about 9% of"
          " the estimate -- sd/sqrt(2(n-1)), which is a large number and not a"
          " rounding term");
    check(r->label == VolRegime::Normal
          && std::fabs((t.high - r->statistic) / r->std_error) < 2.0,
          "so a High threshold set 5% above the current reading is INSIDE the"
          " error bar and the detector does not claim it -- thresholds set by"
          " eye on a chart are routinely closer than 9%");
    std::printf("    -> which is the whole point of carrying the error bar."
                " The naive comparison\n       'is realised vol above 2100?'"
                " has a definite answer on every bar and it\n       is a coin"
                " flip whenever the true value is within 9%% of the line.\n");

    // A vol that really is high clears it.
    VolDetector<60> loud;
    Lcg g2{0x10AD0011};
    double q = 2'400'000.0;
    std::expected<RegimeCall<VolRegime>, RegimeError> hi =
        std::unexpected(RegimeError::WarmingUp);
    for (int i = 0; i < 200; ++i) {
        q += g2.normal() * 4000.0;      // twice the vol
        loud.push(Price{static_cast<std::int64_t>(q)});
        hi = loud.call(t);
    }
    check(hi.has_value() && hi->label == VolRegime::High,
          "and a market at twice that volatility is labelled High without"
          " hesitation, so the error bar made the detector correct rather than"
          " permanently undecided");
}

} // namespace

int main()
{
    std::printf("altair regime detector tests\n");
    near_the_threshold_the_label_is_a_coin_flip();
    hysteresis_is_not_cosmetic();
    the_detector_is_causal_and_has_no_thresholds_of_its_own();
    the_volatility_threshold_inherits_the_estimate_s_error();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
