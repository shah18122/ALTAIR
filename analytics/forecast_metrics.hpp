// analytics/forecast_metrics.hpp -- the scores forecasting papers report, and
// the two tests that say whether a score means anything.
//
// Charithra C. M., Hawaldar, Vinish, Pinto & Shetty, "Deep learning-based
// stock market forecasting: a comparative analysis of ANN, CNN, and LSTM",
// Investment Management and Financial Innovations 23(2), 2026, 219-234,
// doi 10.21511/imfi.23(2).2026.17. NIFTY 50 closes 2017-06/2025, 80/20 split,
// one-day-ahead PRICE forecasts scored by MAPE, RMSE and directional
// accuracy, compared by the Diebold-Mariano test. Table 2:
//     CNN 0.63 %  LSTM 0.72 %  ANN 0.89 %  ARIMA 1.21 %  random walk 1.47 %
//
// So that ALTAIR's own models can be quoted in the same units, this header
// has those scores -- with direction measured the only way a trader can use
// it, against the LAST KNOWN close -- and the two tests:
//   * Diebold-Mariano, with the Newey-West variance for h-step forecasts and
//     the Harvey-Leybourne-Newbold small-sample factor;
//   * Pesaran-Timmermann, for whether a hit rate beats what the same up/down
//     frequencies would give by chance.
//
// WHY THE PAPER'S TABLE IS NOT A BENCHMARK TO BEAT.
//
//   * A random walk's one-day MAPE on a price level is just the mean absolute
//     daily return. On ALTAIR's own NIFTY closes (dataset/spot/nifty/1d, which
//     match the paper's Table 4 to the paisa) "tomorrow = today" scores
//     0.70 % over 2017 - June 2025 and 0.62 % on the last fifth -- not 1.47 %.
//     So the random walk beats the paper's LSTM (0.72) and ANN (0.89) and
//     ties its CNN (0.63). The test measures this when the data is present.
//   * Its own Table 4 rows disagree with Table 2: on the eight rows with a
//     previous close, CNN's MAPE is 0.54 % against the random walk's 0.59 %
//     (close to a tie), and LSTM's is 0.79 % -- worse than the random walk.
//     LSTM calls direction right on 2 of 8.
//   * The min-max scaling and "spike smoothing" are not said to be fitted on
//     the training part only; a centred smoother leaks the next close. The
//     test shows how good a leaked forecast looks under these same scores.
//   * No trading test and no costs. A MAPE is not a P&L.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace altair {

/// Mean absolute percentage error, in percent. Rows with actual == 0 skipped.
[[nodiscard]] inline double forecast_mape(std::span<const double> actual,
                                          std::span<const double> predicted) noexcept {
    const std::size_t n = std::min(actual.size(), predicted.size());
    double s = 0.0;
    std::size_t k = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (actual[i] == 0.0) { continue; }
        s += std::fabs((predicted[i] - actual[i]) / actual[i]);
        ++k;
    }
    return k > 0 ? 100.0 * s / static_cast<double>(k) : 0.0;
}

[[nodiscard]] inline double forecast_rmse(std::span<const double> actual,
                                          std::span<const double> predicted) noexcept {
    const std::size_t n = std::min(actual.size(), predicted.size());
    if (n == 0) { return 0.0; }
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double e = predicted[i] - actual[i];
        s += e * e;
    }
    return std::sqrt(s / static_cast<double>(n));
}

/// Hits and calls for one-step price forecasts: predicted[i] and actual[i]
/// are both for day i, and direction is taken against actual[i - 1], the
/// close known when the forecast was made. A forecast equal to that close
/// names no direction and is not counted.
struct DirectionTally {
    std::size_t hits = 0, calls = 0;
    std::size_t actual_up = 0, predicted_up = 0;   ///< for Pesaran-Timmermann
    [[nodiscard]] double accuracy() const noexcept {
        return calls > 0 ? static_cast<double>(hits) / static_cast<double>(calls) : 0.0;
    }
};

[[nodiscard]] inline DirectionTally
forecast_direction(std::span<const double> actual, std::span<const double> predicted) noexcept {
    DirectionTally t;
    const std::size_t n = std::min(actual.size(), predicted.size());
    for (std::size_t i = 1; i < n; ++i) {
        const double p = predicted[i] - actual[i - 1], a = actual[i] - actual[i - 1];
        if (p == 0.0 || a == 0.0) { continue; }
        ++t.calls;
        t.hits += (p > 0.0) == (a > 0.0) ? 1 : 0;
        t.actual_up += a > 0.0 ? 1 : 0;
        t.predicted_up += p > 0.0 ? 1 : 0;
    }
    return t;
}

struct ForecastTest {
    double stat = 0.0;      ///< approximately standard normal under the null
    double p_value = 1.0;   ///< two-sided for DM, one-sided (better than chance) for PT
};

namespace forecast_detail {
inline double normal_upper(double z) noexcept { return 0.5 * std::erfc(z / std::sqrt(2.0)); }
} // namespace forecast_detail

/// Diebold-Mariano on two forecasts' errors for the same targets, squared-
/// error loss. stat > 0 means the SECOND forecast is more accurate. h is the
/// horizon: autocovariances of the loss differential up to lag h - 1 enter
/// the variance, and the Harvey-Leybourne-Newbold factor is applied.
[[nodiscard]] inline ForecastTest
diebold_mariano(std::span<const double> err_a, std::span<const double> err_b,
                std::size_t h = 1) noexcept {
    const std::size_t n = std::min(err_a.size(), err_b.size());
    if (n < 3 || h == 0 || h >= n) { return {}; }
    const auto d = [&](std::size_t i) { return err_a[i] * err_a[i] - err_b[i] * err_b[i]; };
    double mean = 0.0;
    for (std::size_t i = 0; i < n; ++i) { mean += d(i); }
    mean /= static_cast<double>(n);
    double var = 0.0;
    for (std::size_t k = 0; k < h; ++k) {
        double g = 0.0;
        for (std::size_t i = k; i < n; ++i) { g += (d(i) - mean) * (d(i - k) - mean); }
        g /= static_cast<double>(n);
        var += k == 0 ? g : 2.0 * g;
    }
    if (!(var > 0.0)) { return {}; }
    const double nn = static_cast<double>(n), hh = static_cast<double>(h);
    const double hln = std::sqrt((nn + 1.0 - 2.0 * hh + hh * (hh - 1.0) / nn) / nn);
    ForecastTest r;
    r.stat = hln * mean / std::sqrt(var / nn);
    r.p_value = 2.0 * forecast_detail::normal_upper(std::fabs(r.stat));
    return r;
}

/// Pesaran-Timmermann (1992): is the hit rate above what a forecaster with
/// the same share of "up" calls would get against these outcomes by chance?
[[nodiscard]] inline ForecastTest pesaran_timmermann(const DirectionTally& t) noexcept {
    if (t.calls < 2) { return {}; }
    const double n = static_cast<double>(t.calls);
    const double p = t.accuracy();
    const double py = static_cast<double>(t.actual_up) / n;
    const double px = static_cast<double>(t.predicted_up) / n;
    const double ps = py * px + (1.0 - py) * (1.0 - px);
    const double v_p = ps * (1.0 - ps) / n;
    const double v_ps = (2.0 * py - 1.0) * (2.0 * py - 1.0) * px * (1.0 - px) / n
                      + (2.0 * px - 1.0) * (2.0 * px - 1.0) * py * (1.0 - py) / n
                      + 4.0 * py * px * (1.0 - py) * (1.0 - px) / (n * n);
    const double v = v_p - v_ps;
    if (!(v > 0.0)) { return {}; }   // e.g. every call the same way: untestable
    ForecastTest r;
    r.stat = (p - ps) / std::sqrt(v);
    r.p_value = forecast_detail::normal_upper(r.stat);
    return r;
}

} // namespace altair
