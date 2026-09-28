#include <models/stream_forecast.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
int failures = 0;
void check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}
}
int main() {
    constexpr std::int64_t minute = 60'000'000'000LL;
    std::vector<double> prices(1200);
    std::vector<std::int64_t> ends(prices.size());
    prices[0] = 23000.0;
    for (std::size_t i = 0; i < prices.size(); ++i) {
        ends[i] = (static_cast<std::int64_t>(i) + 1) * minute;
        if (i) prices[i] = prices[i - 1] * std::exp((2.0 * std::sin(static_cast<double>(i) * 0.17) +
                                                   std::cos(static_cast<double>(i) * 0.71)) / 10000.0);
    }
    const auto fit = altair::fit_stream_forecast(prices, ends, minute);
    check(fit.has_value(), "causal chronological fit succeeds");
    if (!fit) return 1;
    const auto tail = std::span<const double, 21>(prices.data() + prices.size() - 21, 21);
    const auto predicted = fit->predict(tail, ends.back());
    check(predicted && predicted->issued_ns == ends.back() && predicted->target_ns == ends.back() + minute,
          "one-minute prediction targets the future, not the observed bar");
    check(fit->training_rows > fit->scored_rows && fit->scored_rows > 100 && fit->validation_rows > 100,
          "fit, model selection and calibration use separate chronological partitions");
    std::array<double, 21> scaled{}, altered{};
    for (std::size_t i = 0; i < 21; ++i) scaled[i] = tail[i] / 1000.0;
    const auto small = fit->predict(scaled, ends.back());
    check(predicted && small && std::fabs(small->predicted * 1000.0 - predicted->predicted) < 1e-6,
          "downscaling then upscaling preserves forecast price");
    std::copy(tail.begin(), tail.end(), altered.begin());
    altered.back() *= 1.001;
    const auto tick = fit->predict(altered, ends.back() + 1);
    check(tick && tick->current == altered.back() && tick->target_ns == tick->issued_ns + minute,
          "each new tick is the observed anchor and targets one minute ahead");
    check(!fit->predict(tail, ends.back() - 1), "inference refuses observations before fitted history");
    check(!fit->predict(tail, std::numeric_limits<std::int64_t>::max()), "target-time overflow refused");
    altered[2] = std::numeric_limits<double>::infinity();
    check(!fit->predict(altered, ends.back()), "nonfinite prices refused");
    auto bad = ends;
    bad[100] = bad[99];
    check(!altair::fit_stream_forecast(prices, bad, minute), "duplicate timestamps refused");
    auto changed = prices;
    for (std::size_t i = 1000; i < changed.size(); ++i) changed[i] *= 1.002;
    const auto other = altair::fit_stream_forecast(changed, ends, minute);
    const auto untouched = other ? other->predict(tail, ends.back()) : std::expected<altair::ForwardEstimate, altair::StreamForecastError>{std::unexpected(altair::StreamForecastError::FitFailed)};
    check(untouched && predicted && untouched->predicted == predicted->predicted,
          "changing calibration outcomes cannot change the chosen model prediction");
    std::vector<std::int64_t> daily_ends(600);
    constexpr std::int64_t day = 86'400'000'000'000LL;
    for (std::size_t i = 0; i < daily_ends.size(); ++i)
        daily_ends[i] = (static_cast<std::int64_t>(i) + 1) * day;
    const auto daily_fit = altair::fit_stream_forecast(
        std::span<const double>(prices.data(), daily_ends.size()), daily_ends, day);
    const auto daily_tail = std::span<const double, 21>(prices.data() + daily_ends.size() - 21, 21);
    const auto daily = daily_fit ? daily_fit->predict(daily_tail, daily_ends.back())
        : std::expected<altair::ForwardEstimate, altair::StreamForecastError>{
              std::unexpected(altair::StreamForecastError::FitFailed)};
    check(daily && daily->target_ns == 0,
          "daily horizon does not invent a calendar-date target without a session calendar");
    const auto begin = std::chrono::steady_clock::now();
    double accumulator = 0.0;
    for (int i = 0; i < 10000; ++i) accumulator += fit->predict(tail, ends.back() + i)->predicted;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    std::printf("Cached inference mean %lld ns, checksum %.2f (synthetic fixture; no tail-latency claim)\n",
                static_cast<long long>(elapsed / 10000), accumulator);
    return failures ? 1 : 0;
}
