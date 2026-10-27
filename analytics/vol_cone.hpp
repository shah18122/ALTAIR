// analytics/vol_cone.hpp -- the volatility cone: where does today's IV sit
// among the realised vols this index has actually delivered over the same
// horizon?
//
// From Ianieri, Option Theory and Trading (Ch. 13): "volatility cones ... aid
// in determining the mean, mode and standard deviations" of vol, so a
// position's vol can be called rich or cheap against the stock's own history
// instead of against a single forecast. The straddle seller's IV >= RV rule
// compares IV with ONE forecast; the cone says how often the market has ever
// moved that much in that many sessions. An IV at the 90th percentile of the
// cone is rich by history whatever the forecast says; one at the 20th is not.
//
// Realised vol here is close-to-close, zero-mean (sum of squared log returns),
// annualised with `sessions_per_year`, over every overlapping window of `h`
// returns. Point in time: `cone_rank` at day `end` reads closes[0..end] only.
// NaN in, NaN out; nothing throws.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace altair {

namespace vol_cone_detail {
inline constexpr double kConeNaN = std::numeric_limits<double>::quiet_NaN();

/// Prefix sums of squared log returns of closes[0..end]; out[k] = sum of the
/// first k returns, bad[k] = how many of them touch a non-positive or
/// non-finite close. A bad return adds 0 to the sum and 1 to the count, so it
/// drops only the windows that span it, not every window after it.
inline void squared_return_prefix(std::span<const double> closes, std::size_t end, std::vector<double>& out,
                                  std::vector<std::size_t>& bad) {
    out.assign(1, 0.0);
    bad.assign(1, 0);
    out.reserve(end + 1);
    bad.reserve(end + 1);
    for (std::size_t k = 1; k <= end; ++k) {
        const double a = closes[k - 1], b = closes[k];
        const bool ok = a > 0.0 && b > 0.0 && std::isfinite(a) && std::isfinite(b);
        const double r = ok ? std::log(b / a) : 0.0;
        out.push_back(out.back() + r * r);
        bad.push_back(bad.back() + (ok ? 0u : 1u));
    }
}
}  // namespace vol_cone_detail

/// Annualised realised vol of every window of `h` returns ending at or before
/// `end` (closes[0..end]), oldest first. Windows spanning a bad close are left out.
[[nodiscard]] inline std::vector<double> realised_vol_windows(std::span<const double> closes, std::size_t end,
                                                              std::size_t h, double sessions_per_year = 252.0) {
    std::vector<double> out;
    if (h == 0 || end >= closes.size() || end < h || !(sessions_per_year > 0.0)) { return out; }
    std::vector<double> pre;
    std::vector<std::size_t> bad;
    vol_cone_detail::squared_return_prefix(closes, end, pre, bad);
    out.reserve(end - h + 1);
    for (std::size_t k = h; k <= end; ++k) {
        const double ss = std::max(0.0, pre[k] - pre[k - h]);   // rounding can dip a flat window below 0
        if (bad[k] == bad[k - h]) { out.push_back(std::sqrt(ss * sessions_per_year / static_cast<double>(h))); }
    }
    return out;
}

/// Share of `samples` at or below `value`, in [0, 1]. NaN if there are none.
[[nodiscard]] inline double percentile_rank(std::span<const double> samples, double value) noexcept {
    if (samples.empty() || !std::isfinite(value)) { return vol_cone_detail::kConeNaN; }
    std::size_t at_or_below = 0;
    for (const double s : samples) { at_or_below += (s <= value) ? 1u : 0u; }
    return static_cast<double>(at_or_below) / static_cast<double>(samples.size());
}

/// Where `iv` sits in the cone for horizon `h` at day `end`: the share of past
/// `h`-session realised vols at or below it. NaN with fewer than `min_windows`.
[[nodiscard]] inline double cone_rank(std::span<const double> closes, std::size_t end, std::size_t h, double iv,
                                      std::size_t min_windows = 60, double sessions_per_year = 252.0) {
    const auto w = realised_vol_windows(closes, end, h, sessions_per_year);
    if (w.size() < min_windows) { return vol_cone_detail::kConeNaN; }
    return percentile_rank(w, iv);
}

/// The cone's shape at one horizon: min, quartiles, max of the realised vols.
struct ConeSlice {
    std::size_t horizon = 0, windows = 0;
    double min = vol_cone_detail::kConeNaN, q25 = vol_cone_detail::kConeNaN, median = vol_cone_detail::kConeNaN,
           q75 = vol_cone_detail::kConeNaN, max = vol_cone_detail::kConeNaN;
};

/// One slice per horizon, for display. Quantiles by nearest rank.
[[nodiscard]] inline std::vector<ConeSlice> vol_cone(std::span<const double> closes, std::span<const std::size_t> horizons,
                                                     double sessions_per_year = 252.0) {
    std::vector<ConeSlice> out;
    if (closes.empty()) { return out; }
    for (const std::size_t h : horizons) {
        auto w = realised_vol_windows(closes, closes.size() - 1, h, sessions_per_year);
        ConeSlice s;
        s.horizon = h;
        s.windows = w.size();
        if (!w.empty()) {
            std::sort(w.begin(), w.end());
            const auto q = [&](double p) { return w[static_cast<std::size_t>(p * static_cast<double>(w.size() - 1) + 0.5)]; };
            s.min = w.front();
            s.q25 = q(0.25);
            s.median = q(0.5);
            s.q75 = q(0.75);
            s.max = w.back();
        }
        out.push_back(s);
    }
    return out;
}

}  // namespace altair
