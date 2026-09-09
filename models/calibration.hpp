// models/calibration.hpp -- conformal calibration applied to a price forecast.
//
// P35-02. conformal.hpp is the generic primitive: give it conformity scores and
// it returns a weighted quantile of them. This file is the part that knows what
// a price forecast is, and it does two things the primitive deliberately does
// not.
//
// FIRST: THE BAND HAS TWO SIDES, AND THEY ARE NOT THE SAME SIZE.
//
// The paper calibrates a one-sided VaR bound, which is the right object for a
// loss quantile. A price forecast wants an interval, and the lazy way to get one
// is to take the one-sided bound and mirror it. That would be wrong on any
// financial series: the downside tail is fatter than the upside, so a symmetric
// band is simultaneously too wide above and too narrow below, and the half that
// is too narrow is the half that costs money.
//
// So two calibrators run side by side over the same forecasts -- one on
// `actual - predicted`, one on `predicted - actual` -- each at level
// 1 - alpha/2. The result is an asymmetric interval whose two sides are earned
// separately.
//
// SECOND: AN AVERAGE COVERAGE NUMBER CAN HIDE A BAND THAT IS ALWAYS WRONG.
//
// A band that covers 95% overall by covering 99% in quiet markets and 85% in
// volatile ones is not a 95% band. It is two different bands, and the one that
// applies when it matters is the bad one. The paper's Table 2 is the fix:
// exceedance broken out by realised-volatility quintile, plus the three
// summary statistics of Table 3 -- mean absolute deviation from target, worst
// deviation, and spread -- all in percentage points.
//
// This is the same instinct as CLAUDE.md's "report per regime, never only in
// aggregate", which is there because markets are not ergodic. An average over a
// period containing two regimes describes neither.
//
// THE VOLATILITY FEATURE IS DERIVED HERE, AND CAUSALLY.
//
// The regime feature is computed from the forecast anchors, which are closes the
// model had already seen. Bucketing is a diagnostic and is allowed to use the
// whole scored period to place the quintile edges; the WEIGHTS are not, and the
// feature feeding them at bar i uses only bars before i. Getting that backwards
// would put the answer into the thing being scored.

#pragma once

#include <models/conformal.hpp>
#include <models/forecast_scorecard.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <expected>
#include <utility>
#include <vector>

namespace altair {

/// Volatility buckets. Five, matching the paper's quintiles.
inline constexpr std::size_t kVolBuckets = 5;

struct CalibrationSpec {
    /// The primitive's settings. `alpha` here is the TOTAL two-sided
    /// miscoverage: 0.10 asks for a 90% interval, and each side is calibrated
    /// at 1 - alpha/2.
    ConformalSpec conformal{};

    /// Bars of history in the realised-volatility regime feature.
    std::size_t vol_window = 20;

    /// Forecasts to observe before the band is calibrated at all. Below this
    /// the point's own published band is passed through UNCHANGED and counted
    /// as uncalibrated, rather than corrected from a handful of scores.
    std::size_t warmup = 250;

    [[nodiscard]] bool valid() const noexcept {
        return conformal.valid() && vol_window >= 2 && warmup >= 20;
    }
};

/// One volatility bucket's record.
struct VolBucket {
    std::size_t n = 0;
    /// Fraction of actuals that fell OUTSIDE the band in this bucket.
    double exceedance = 0.0;
    /// Mean realised volatility in the bucket, in basis points, so the reader
    /// can see what "quintile 4" actually means on this series.
    double mean_vol_bps = 0.0;
};

/// What the band did, overall and where it matters.
struct CalibrationReport {
    std::size_t n = 0;
    /// Target miscoverage the band was aiming at.
    double target = 0.0;
    /// Fraction of actuals outside the band, overall.
    double exceedance = 0.0;
    /// 1 - exceedance. The number that should match the band's claim.
    double coverage = 0.0;

    std::array<VolBucket, kVolBuckets> by_vol{};

    /// Paper Table 3, all in PERCENTAGE POINTS relative to target.
    /// Mean absolute deviation of the per-bucket exceedance from target.
    double reg_mae_pp = 0.0;
    /// Worst single bucket's deviation.
    double reg_maxdev_pp = 0.0;
    /// Spread of the per-bucket deviations.
    double reg_std_pp = 0.0;

    /// Paper Table 4: how much history the weights actually used.
    double mean_n_eff = 0.0;
    double mean_memory = 0.0;

    /// Rule-11 counters, carried out of the calibrators so a caller can see
    /// them without reaching into either one.
    std::size_t fallbacks = 0;
    std::size_t dropped = 0;
    /// Forecasts that passed through uncalibrated because the warmup had not
    /// finished. They are still scored -- excluding them would flatter the
    /// result by dropping exactly the period the band was worst.
    std::size_t uncalibrated = 0;

    /// Deviation from target in percentage points, positive = too many
    /// exceedances, i.e. the band is too NARROW.
    [[nodiscard]] double deviation_pp() const noexcept {
        return 100.0 * (exceedance - target);
    }

    /// The offsets that apply to the NEXT bar -- the one that has not happened.
    ///
    /// Every other field here describes the past. These two are the only
    /// forward-looking numbers in the report, and they are what a page
    /// publishes as the live band. They are produced by one further calibration
    /// step after the whole record has been observed, so they use every score
    /// available and none that is not.
    ///
    /// `next_ready` is false when there was not enough history to calibrate at
    /// all. A caller must then publish the model's own uncalibrated band and
    /// SAY it is uncalibrated -- zeros here would read as a band of zero width.
    double next_up = 0.0;
    double next_lo = 0.0;
    bool next_ready = false;
};

namespace detail {

/// Causal realised volatility, in bps, from the forecast anchors.
///
/// Element i uses returns strictly before bar i. The first `window` entries
/// have too little history and are marked with a negative value rather than a
/// zero -- absence is not a volatility of zero, and a zero would sort into the
/// quietest bucket and corrupt it.
[[nodiscard]] inline std::vector<double>
causal_vol_bps(const std::vector<ForecastPoint>& pts, std::size_t window) {
    std::vector<double> out(pts.size(), -1.0);
    if (pts.size() < 2 || window < 2) { return out; }

    std::vector<double> r;
    r.reserve(pts.size());
    r.push_back(0.0);   // no return into the first anchor
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const double a = pts[i - 1].anchor, b = pts[i].anchor;
        r.push_back(a > 0.0 && b > 0.0 ? 10'000.0 * std::log(b / a) : 0.0);
    }

    for (std::size_t i = window; i < pts.size(); ++i) {
        // Returns r[i-window+1 .. i] all landed on or before anchor i, which
        // the model saw. Nothing here touches bar i's outcome.
        double m = 0.0;
        for (std::size_t j = i - window + 1; j <= i; ++j) { m += r[j]; }
        m /= static_cast<double>(window);
        double v = 0.0;
        for (std::size_t j = i - window + 1; j <= i; ++j) {
            const double e = r[j] - m;
            v += e * e;
        }
        out[i] = std::sqrt(v / static_cast<double>(window - 1));
    }
    return out;
}

}  // namespace detail

/// Score a band that already exists, without changing it.
///
/// This is what makes the before/after comparison possible: run it on the
/// forecaster's own published band, then on the calibrated one, and the two
/// reports are directly comparable because nothing else differs.
[[nodiscard]] inline std::expected<CalibrationReport, ConformalError>
score_calibration(const std::vector<ForecastPoint>& pts, double target,
                  std::size_t vol_window = 20) {
    if (pts.empty() || !(target > 0.0) || target >= 1.0) {
        return std::unexpected(ConformalError::BadParameter);
    }
    const std::vector<double> vol = detail::causal_vol_bps(pts, vol_window);

    CalibrationReport rep;
    rep.target = target;
    rep.n = pts.size();

    std::size_t out_of_band = 0;
    for (const auto& p : pts) {
        if (!p.inside_band()) { ++out_of_band; }
    }
    rep.exceedance = static_cast<double>(out_of_band)
                     / static_cast<double>(pts.size());
    rep.coverage = 1.0 - rep.exceedance;

    // Quintile edges from the scored period's own volatility distribution.
    // Points without enough history to have a volatility are left out of the
    // BUCKETS and stay in the overall figure -- the alternative is to drop
    // them entirely, which quietly shortens the sample.
    std::vector<double> known;
    known.reserve(vol.size());
    for (double v : vol) {
        if (v >= 0.0) { known.push_back(v); }
    }
    if (known.size() >= kVolBuckets) {
        std::vector<double> sorted = known;
        std::sort(sorted.begin(), sorted.end());
        std::array<double, kVolBuckets - 1> edge{};
        for (std::size_t q = 1; q < kVolBuckets; ++q) {
            const std::size_t at = sorted.size() * q / kVolBuckets;
            edge[q - 1] = sorted[std::min(at, sorted.size() - 1)];
        }
        std::array<std::size_t, kVolBuckets> over{}, cnt{};
        std::array<double, kVolBuckets> vsum{};
        for (std::size_t i = 0; i < pts.size(); ++i) {
            if (vol[i] < 0.0) { continue; }
            std::size_t b = 0;
            while (b < kVolBuckets - 1 && vol[i] > edge[b]) { ++b; }
            ++cnt[b];
            vsum[b] += vol[i];
            if (!pts[i].inside_band()) { ++over[b]; }
        }
        double mae = 0.0, maxdev = 0.0;
        std::vector<double> devs;
        for (std::size_t b = 0; b < kVolBuckets; ++b) {
            rep.by_vol[b].n = cnt[b];
            if (cnt[b] == 0) { continue; }
            rep.by_vol[b].exceedance = static_cast<double>(over[b])
                                       / static_cast<double>(cnt[b]);
            rep.by_vol[b].mean_vol_bps = vsum[b] / static_cast<double>(cnt[b]);
            const double d = 100.0 * (rep.by_vol[b].exceedance - target);
            mae += std::fabs(d);
            maxdev = std::max(maxdev, std::fabs(d));
            devs.push_back(d);
        }
        if (!devs.empty()) {
            rep.reg_mae_pp = mae / static_cast<double>(devs.size());
            rep.reg_maxdev_pp = maxdev;
            double m = 0.0;
            for (double d : devs) { m += d; }
            m /= static_cast<double>(devs.size());
            double v = 0.0;
            for (double d : devs) { v += (d - m) * (d - m); }
            rep.reg_std_pp = devs.size() > 1
                                 ? std::sqrt(v / (devs.size() - 1.0)) : 0.0;
        }
    }
    return rep;
}

/// Replace each forecast's band with a conformally calibrated one.
///
/// Returns the rewritten points and the report for the NEW band. Score the
/// input separately with `score_calibration` to get the before.
///
/// The two sides are calibrated independently, so the returned interval is
/// asymmetric whenever the series is -- which is nearly always.
[[nodiscard]] inline std::expected<
    std::pair<std::vector<ForecastPoint>, CalibrationReport>, ConformalError>
calibrate_band(const std::vector<ForecastPoint>& pts,
               const CalibrationSpec& spec) {
    if (!spec.valid()) { return std::unexpected(ConformalError::BadParameter); }
    if (pts.size() <= spec.warmup) {
        return std::unexpected(ConformalError::NoScores);
    }

    // Each side carries half the miscoverage.
    ConformalSpec side = spec.conformal;
    side.alpha = spec.conformal.alpha * 0.5;

    Conformal up(side), down(side);

    const std::vector<double> vol = detail::causal_vol_bps(pts, spec.vol_window);

    // The regime scaler is fitted on the WARMUP ONLY. Fitting it over
    // everything would let the scored period's own volatility distribution set
    // the weights that judge it.
    RegimeScaler scaler;
    {
        std::vector<double> train;
        train.reserve(spec.warmup);
        for (std::size_t i = 0; i < spec.warmup && i < vol.size(); ++i) {
            if (vol[i] >= 0.0) { train.push_back(vol[i]); }
        }
        if (train.size() >= 2) { (void)scaler.fit(train, 1); }
    }

    std::vector<ForecastPoint> out;
    out.reserve(pts.size());

    CalibrationReport rep;
    rep.target = spec.conformal.alpha;

    double neff_sum = 0.0, mem_sum = 0.0;
    std::size_t steps = 0;

    std::vector<double> z(1, 0.0);
    for (std::size_t i = 0; i < pts.size(); ++i) {
        ForecastPoint p = pts[i];

        const double raw = vol[i] >= 0.0 ? vol[i] : 0.0;
        scaler.transform(std::span(&raw, 1), z);

        if (i >= spec.warmup) {
            const auto a = up.calibrate(z);
            const auto b = down.calibrate(z);
            if (a && b) {
                // The buffer is an offset in PRICE, because the scores were.
                // Clamping at zero is not a rule-11 clamp: a negative offset
                // would be a band narrower than the point forecast on that
                // side, which is not an interval.
                p.hi = p.predicted + std::max(0.0, a->buffer);
                p.lo = p.predicted - std::max(0.0, b->buffer);
                neff_sum += 0.5 * (a->n_eff + b->n_eff);
                mem_sum += 0.5 * (a->effective_memory + b->effective_memory);
                ++steps;
            } else {
                ++rep.uncalibrated;
            }
        } else {
            ++rep.uncalibrated;
        }
        out.push_back(p);

        // Only now that bar i has closed. Hard rule 7 lives in this ordering.
        (void)up.observe(pts[i].actual - pts[i].predicted, z);
        (void)down.observe(pts[i].predicted - pts[i].actual, z);
    }

    // ONE MORE STEP, FOR THE BAR THAT HAS NOT HAPPENED.
    //
    // The loop above always calibrated before observing, so after it ends the
    // calibrators hold every score and have published nothing for the next bar.
    // This is that publication, and it uses the most recent regime because that
    // is the one the next bar will be drawn from.
    CalibrationReport head;
    if (!pts.empty()) {
        const double raw = vol.back() >= 0.0 ? vol.back() : 0.0;
        scaler.transform(std::span(&raw, 1), z);
        const auto a = up.calibrate(z);
        const auto b = down.calibrate(z);
        if (a && b) {
            head.next_up = std::max(0.0, a->buffer);
            head.next_lo = std::max(0.0, b->buffer);
            head.next_ready = true;
        }
    }

    auto scored = score_calibration(out, spec.conformal.alpha, spec.vol_window);
    if (!scored) { return std::unexpected(scored.error()); }

    CalibrationReport final_rep = *scored;
    final_rep.next_up = head.next_up;
    final_rep.next_lo = head.next_lo;
    final_rep.next_ready = head.next_ready;
    final_rep.uncalibrated = rep.uncalibrated;
    final_rep.mean_n_eff = steps > 0 ? neff_sum / static_cast<double>(steps) : 0.0;
    final_rep.mean_memory = steps > 0 ? mem_sum / static_cast<double>(steps) : 0.0;
    final_rep.fallbacks = up.fallbacks() + down.fallbacks();
    final_rep.dropped = up.dropped() + down.dropped();
    return std::make_pair(std::move(out), final_rep);
}

}  // namespace altair
