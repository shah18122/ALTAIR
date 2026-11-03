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
using Estimate = std::expected<altair::ForwardEstimate, altair::StreamForecastError>;
Estimate refused() { return std::unexpected(altair::StreamForecastError::FitFailed); }

constexpr std::int64_t minute = 60'000'000'000LL;
constexpr std::int64_t day = 86'400'000'000'000LL;
// 2024-01-01 09:15 IST in UTC ns (03:45 UTC). 2024-01-01 is a Monday.
constexpr std::int64_t first_open = 1'704'080'700'000'000'000LL;

double wiggle(std::size_t i) {
    return (2.0 * std::sin(static_cast<double>(i) * 0.17) + std::cos(static_cast<double>(i) * 0.71)) / 10000.0;
}

/// NSE weekdays, 09:15-15:30 IST: every COMPLETE bar of `width`.
void nse_sessions(std::size_t days, std::int64_t width, std::vector<double>& closes, std::vector<std::int64_t>& ends) {
    closes.clear(); ends.clear();
    double p = 23000.0;
    std::size_t made = 0;
    for (std::int64_t d = 0; made < days; ++d) {
        if (d % 7 >= 5) continue;
        ++made;
        const std::int64_t open = first_open + d * day, close = open + 375 * minute;
        p *= std::exp(0.002 * std::sin(static_cast<double>(d)));   // the overnight gap
        for (std::int64_t e = open + width; e <= close; e += width) {
            p *= std::exp(wiggle(closes.size()));
            closes.push_back(p);
            ends.push_back(e);
        }
    }
}

altair::ReturnWindow window_of(const std::vector<double>& closes, const std::vector<std::int64_t>& ends,
                               std::int64_t width) {
    altair::ReturnWindow w(width);
    for (std::size_t i = 0; i < closes.size(); ++i) (void)w.push(closes[i], ends[i]);
    return w;
}
}

int main() {
    std::vector<double> prices(1200);
    std::vector<std::int64_t> ends(prices.size());
    prices[0] = 23000.0;
    for (std::size_t i = 0; i < prices.size(); ++i) {
        ends[i] = (static_cast<std::int64_t>(i) + 1) * minute;
        if (i) prices[i] = prices[i - 1] * std::exp(wiggle(i));
    }
    const auto fit = altair::fit_stream_forecast(prices, ends, minute);
    check(fit.has_value(), "causal chronological fit succeeds");
    if (!fit) return 1;
    const auto window = window_of(prices, ends, minute);
    std::array<double, 20> tail{};
    window.copy(tail);
    const auto predicted = fit->predict(tail, prices.back(), ends.back());
    check(predicted && predicted->issued_ns == ends.back() && predicted->target_ns == ends.back() + minute,
          "one-minute prediction targets the future, not the observed bar");
    check(fit->training_rows > fit->scored_rows && fit->scored_rows > 100 && fit->validation_rows > 100,
          "fit, model selection and calibration use separate chronological partitions");
    check(fit->purged_rows == 0 && fit->embargoed_rows == 2,
          "one-bar labels only touch the boundary (nothing to purge); one row embargoed at each boundary");
    const auto small = fit->predict(tail, prices.back() / 1000.0, ends.back());
    check(predicted && small && std::fabs(small->predicted * 1000.0 - predicted->predicted) < 1e-6,
          "returns are scale-free: a downscaled price gives the downscaled forecast");
    std::array<double, 20> altered{};
    const bool provisional = window.observe(prices.back() * 1.001, ends.back() + minute, altered);
    const auto tick = fit->predict(altered, prices.back() * 1.001, ends.back() + 1);
    check(provisional && tick && tick->current == prices.back() * 1.001 && tick->target_ns == tick->issued_ns + minute,
          "each new tick is the observed anchor and targets one minute ahead");
    check(!fit->predict(tail, prices.back(), ends.back() - 1), "inference refuses observations before fitted history");
    check(!fit->predict(tail, prices.back(), std::numeric_limits<std::int64_t>::max()), "target-time overflow refused");
    const std::array<double, 6> stray{};
    check(!fit->predict(tail, prices.back(), ends.back(), stray), "a returns-only model refuses a depth row");
    altered[2] = std::numeric_limits<double>::infinity();
    check(!fit->predict(altered, prices.back(), ends.back()), "nonfinite returns refused");
    auto bad = ends;
    bad[100] = bad[99];
    check(!altair::fit_stream_forecast(prices, bad, minute), "duplicate timestamps refused");
    auto changed = prices;
    for (std::size_t i = 1000; i < changed.size(); ++i) changed[i] *= 1.002;
    const auto other = altair::fit_stream_forecast(changed, ends, minute);
    const auto untouched = other ? other->predict(tail, prices.back(), ends.back()) : refused();
    check(untouched && predicted && untouched->predicted == predicted->predicted,
          "changing calibration outcomes cannot change the chosen model prediction");

    std::vector<std::int64_t> daily_ends(600);
    for (std::size_t i = 0; i < daily_ends.size(); ++i)
        daily_ends[i] = (static_cast<std::int64_t>(i) + 1) * day;
    const std::span<const double> daily_prices(prices.data(), daily_ends.size());
    const auto daily_fit = altair::fit_stream_forecast(daily_prices, daily_ends, day);
    altair::ReturnWindow dw(day);
    for (std::size_t i = 0; i < daily_ends.size(); ++i) (void)dw.push(daily_prices[i], daily_ends[i]);
    std::array<double, 20> daily_tail{};
    dw.copy(daily_tail);
    const auto daily = daily_fit ? daily_fit->predict(daily_tail, daily_prices.back(), daily_ends.back()) : refused();
    check(daily && daily->target_ns == 0,
          "daily horizon does not invent a calendar-date target without a session calendar");

    constexpr std::int64_t quarter = 15 * minute;
    std::vector<std::int64_t> q_ends(prices.size());
    for (std::size_t i = 0; i < q_ends.size(); ++i)
        q_ends[i] = (static_cast<std::int64_t>(i) + 1) * quarter;
    const auto hour = altair::fit_stream_forecast(prices, q_ends, quarter, 4);
    check(hour && hour->horizon_bars == 4 && hour->chosen_depth >= 2 && hour->chosen_depth <= 5,
          "15m bars fit a 4-bar (one hour) target and select a tree depth from 2 to 5");
    check(hour && hour->purged_rows == 6 && hour->embargoed_rows == 8,
          "four-bar labels: 3 overlapping rows purged and 4 rows embargoed at each boundary");
    const auto ahead = hour ? hour->predict(tail, prices.back(), q_ends.back()) : refused();
    check(ahead && ahead->target_ns == q_ends.back() + 60 * minute,
          "15m four-bar forecast targets one hour after issue");
    auto gapped = q_ends;
    for (std::size_t i = 600; i < gapped.size(); ++i) gapped[i] += quarter;
    const auto one_gap = altair::fit_stream_forecast(prices, gapped, quarter, 1);
    const auto four_gap = altair::fit_stream_forecast(prices, gapped, quarter, 4);
    check(one_gap && four_gap && four_gap->skipped_windows == one_gap->skipped_windows + 3,
          "a missing bar skips every multi-bar target that would straddle it");
    check(!altair::fit_stream_forecast(prices, q_ends, quarter, 0), "zero-bar horizon refused");
    check(!altair::fit_stream_forecast(daily_prices, daily_ends, day, 4),
          "daily fit refuses a multi-session horizon it cannot date");

    // THE BUG THIS FILE NOW GUARDS: real NSE sessions. 25 fifteen-minute bars
    // a day; 6 complete sixty-minute bars. The old rule needed 25 contiguous
    // bars for a 15m one-hour window and 21 for any 60m window.
    std::vector<double> sc;
    std::vector<std::int64_t> se;
    nse_sessions(500, quarter, sc, se);
    const auto rows15 = altair::build_stream_rows(sc, se, quarter, 4);
    check(rows15 && rows15->rows.size() >= 500 * 20,
          "15m sessions: a one-hour target has a row at 21 of 25 bars every day");
    bool inside = rows15.has_value();
    if (rows15)
        for (const auto& r : rows15->rows) inside = inside && r.label_end_ns - r.anchor_ns == 60 * minute;
    check(inside, "15m sessions: every one-hour label ends exactly one hour after its anchor, same session");
    const auto fit15 = altair::fit_stream_forecast(sc, se, quarter, 4);
    check(fit15.has_value(), "15m + next 1 hour fits on 500 NSE sessions (it was refused before)");

    constexpr std::int64_t hour_bar = 60 * minute;
    nse_sessions(1500, hour_bar, sc, se);
    check(se.size() == 1500 * 6, "60m sessions: six complete bars a day (15:15-15:30 is partial and left out)");
    const auto fit60 = altair::fit_stream_forecast(sc, se, hour_bar, 1);
    check(fit60 && fit60->training_rows > 1000, "60m bars fit a next-hour target (no window was possible before)");
    const auto w60 = window_of(sc, se, hour_bar);
    std::array<double, 20> r60{};
    w60.copy(r60);
    check(w60.ready(se.back() + day), "60m: 20 same-session returns span past sessions, ready at the next open");
    const auto f60 = fit60 ? fit60->predict(r60, sc.back(), se.back() + day - 5 * hour_bar) : refused();
    check(f60 && f60->target_ns == f60->issued_ns + hour_bar, "60m forecast at the next session's open targets one hour on");
    check(!w60.ready(se.back() + 15 * day), "returns older than 14 days do not make a live window");

    const auto begin = std::chrono::steady_clock::now();
    double accumulator = 0.0;
    for (int i = 0; i < 10000; ++i) accumulator += fit->predict(tail, prices.back(), ends.back() + i)->predicted;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    std::printf("Cached inference mean %lld ns, checksum %.2f (synthetic fixture; no tail-latency claim)\n",
                static_cast<long long>(elapsed / 10000), accumulator);
    return failures ? 1 : 0;
}
