// models/event_study.hpp -- M23 point-in-time event/news study mechanics.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace altair {

struct EventObservation {
    std::uint64_t available_at_ns = 0; // publication/availability, never event date alone
    std::uint64_t entity = 0;
};

struct EntityReturn {
    std::uint64_t bar_close_ns = 0;
    std::uint64_t entity = 0;
    double asset_return = 0.0;
    double benchmark_return = 0.0;
};

enum class EventStudyError : std::uint8_t {
    BadParameter,
    UnsortedReturns,
    NonFinite,
    NoUsableEvents
};

struct EventWindowResult {
    std::uint64_t available_at_ns = 0;
    std::uint64_t entity = 0;
    std::size_t first_bar = 0;
    std::vector<double> abnormal_return;
    double cumulative_abnormal_return = 0.0;
};

struct EventStudyResult {
    std::vector<EventWindowResult> windows;
    double mean_cumulative_abnormal_return = 0.0;
    double standard_error = 0.0;
    std::size_t excluded_overlap = 0;
    std::size_t excluded_missing_window = 0;
};

/// Align events by the first bar CLOSE at or after availability. Pre/post are
/// bar counts. Same-entity windows that overlap after an embargo are refused,
/// so one price move cannot be counted as several independent events.
[[nodiscard]] inline std::expected<EventStudyResult, EventStudyError>
event_study(std::span<const EventObservation> events,
            std::span<const EntityReturn> returns,
            std::size_t pre_bars, std::size_t post_bars,
            std::size_t embargo_bars) {
    if (events.empty() || returns.empty() || pre_bars + post_bars == 0)
        return std::unexpected(EventStudyError::BadParameter);
    for (std::size_t i = 0; i < returns.size(); ++i) {
        if (!std::isfinite(returns[i].asset_return)
            || !std::isfinite(returns[i].benchmark_return))
            return std::unexpected(EventStudyError::NonFinite);
        if (i != 0 && (returns[i].entity < returns[i - 1].entity
            || (returns[i].entity == returns[i - 1].entity
                && returns[i].bar_close_ns <= returns[i - 1].bar_close_ns)))
            return std::unexpected(EventStudyError::UnsortedReturns);
    }
    std::vector<EventObservation> ordered(events.begin(), events.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        return a.entity != b.entity ? a.entity < b.entity
                                    : a.available_at_ns < b.available_at_ns;
    });
    EventStudyResult out;
    std::uint64_t last_entity = 0;
    std::size_t last_window_end = 0;
    bool have_last = false;
    for (const auto& event : ordered) {
        const auto entity_begin = std::lower_bound(returns.begin(), returns.end(),
            event.entity, [](const EntityReturn& row, std::uint64_t entity) {
                return row.entity < entity;
            });
        const auto entity_end = std::upper_bound(entity_begin, returns.end(),
            event.entity, [](std::uint64_t entity, const EntityReturn& row) {
                return entity < row.entity;
            });
        const auto first = std::lower_bound(entity_begin, entity_end,
            event.available_at_ns, [](const EntityReturn& row, std::uint64_t time) {
                return row.bar_close_ns < time;
            });
        if (first == entity_end) { ++out.excluded_missing_window; continue; }
        const std::size_t local = static_cast<std::size_t>(first - entity_begin);
        const std::size_t count = static_cast<std::size_t>(entity_end - entity_begin);
        if (local < pre_bars || local + post_bars >= count) {
            ++out.excluded_missing_window;
            continue;
        }
        const std::size_t begin = local - pre_bars;
        const std::size_t end = local + post_bars;
        if (have_last && event.entity == last_entity
            && begin <= last_window_end + embargo_bars) {
            ++out.excluded_overlap;
            continue;
        }
        EventWindowResult window;
        window.available_at_ns = event.available_at_ns;
        window.entity = event.entity;
        window.first_bar = local;
        window.abnormal_return.reserve(end - begin + 1);
        for (std::size_t i = begin; i <= end; ++i) {
            const auto& row = entity_begin[static_cast<std::ptrdiff_t>(i)];
            const double abnormal = row.asset_return - row.benchmark_return;
            window.abnormal_return.push_back(abnormal);
            window.cumulative_abnormal_return += abnormal;
        }
        out.windows.push_back(std::move(window));
        last_entity = event.entity;
        last_window_end = end;
        have_last = true;
    }
    if (out.windows.empty())
        return std::unexpected(EventStudyError::NoUsableEvents);
    for (const auto& window : out.windows)
        out.mean_cumulative_abnormal_return += window.cumulative_abnormal_return;
    out.mean_cumulative_abnormal_return /= static_cast<double>(out.windows.size());
    if (out.windows.size() > 1) {
        double variance = 0.0;
        for (const auto& window : out.windows) {
            const double d = window.cumulative_abnormal_return
                           - out.mean_cumulative_abnormal_return;
            variance += d * d;
        }
        variance /= static_cast<double>(out.windows.size() - 1);
        out.standard_error = std::sqrt(variance / static_cast<double>(out.windows.size()));
    }
    return out;
}

} // namespace altair
