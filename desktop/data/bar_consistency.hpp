// Cold-path audit only. No loader, engine, broker or source-file mutation.
#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QTimeZone>
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <optional>
#include <set>

namespace altair::ui::barcheck {

struct Bar {
    qint64 start = 0;                 ///< UTC epoch seconds, interval open.
    std::array<qint64, 4> price{};      ///< Open/high/low/close, integer paise.
    std::optional<qint64> volume;       ///< Nonnegative source units, null unknown.
};
using Series = std::map<qint64, Bar>;

struct Report {
    QMap<QString, qint64> counts;
    QJsonArray diagnostics;
    qsizetype detail_limit = 200;
    qint64 omitted_details = 0;
    qint64 loaded_rows = 0;
    qint64 rounded_fields = 0;
    qint64 compared_windows = 0;
    qint64 matched_windows = 0;
    qint64 prices_compared = 0;
    qint64 volumes_compared = 0;
    qint64 volumes_unverified = 0;
    qint64 session_dates = 0;

    /// Count every issue; cap only details, visibly. Call only on cold paths.
    void issue(const QString& code, const QString& message,
               const QString& source = {}, qint64 row = 0) {
        ++counts[code];
        if (diagnostics.size() < std::max<qsizetype>(0, detail_limit))
            diagnostics.append(QJsonObject{{"code", code}, {"message", message},
                                           {"source", source}, {"row", row}});
        else
            ++omitted_details;
    }

    /// Scoped consistency only; unreported volume is never verified equality.
    [[nodiscard]] bool ok() const { return counts.isEmpty() && matched_windows > 0; }

    /// Serialize audit counts; exact price/volume diagnostics are text, not doubles.
    [[nodiscard]] QJsonObject json() const {
        QJsonObject totals;
        for (auto it = counts.cbegin(); it != counts.cend(); ++it)
            totals.insert(it.key(), it.value());
        return {{"consistent", ok()},
                {"ohlcv_fully_verified", ok() && volumes_unverified == 0},
                {"issue_counts", totals},
                {"diagnostics", diagnostics}, {"omitted_details", omitted_details},
                {"loaded_rows", loaded_rows}, {"rounded_fields", rounded_fields},
                {"compared_windows", compared_windows}, {"matched_windows", matched_windows},
                {"price_fields_compared", prices_compared},
                {"volume_windows_compared", volumes_compared},
                {"volume_windows_unverified", volumes_unverified},
                {"session_dates", session_dates},
                {"scope", "Regular 09:15-15:30 IST intraday interval-open bars. "
                          "All dates present on either side require complete coverage. "
                          "Entire dates absent on both sides are not detectable without a calendar. "
                          "Provider, instrument, futures contract/roll and adjustment equivalence "
                          "must be established independently. This is not financial-truth certification."}};
    }
};

namespace detail {
constexpr qint64 day_seconds = 86400;
constexpr qint64 ist_offset = 19800;
constexpr qint64 open_seconds = 9 * 3600 + 15 * 60;
constexpr qint64 session_seconds = 375 * 60;

inline bool durations_ok(int base_minutes, int target_minutes) {
    return base_minutes > 0 && target_minutes > base_minutes &&
           target_minutes <= 375 && target_minutes % base_minutes == 0;
}

inline QString stamp(qint64 seconds) {
    return QDateTime::fromSecsSinceEpoch(seconds,
        QTimeZone::fromSecondsAheadOfUtc(ist_offset)).toString(Qt::ISODate);
}

inline qint64 day(qint64 seconds) { return (seconds + ist_offset) / day_seconds; }
inline qint64 session_start(qint64 day_number) {
    return day_number * day_seconds - ist_offset + open_seconds;
}

inline bool valid_bar(const Bar& bar, int minutes, Report& report,
                      const QString& source) {
    // Four-digit ISO years and checked timestamp arithmetic. No wrap at int64 extremes.
    if (bar.start < 0 || bar.start > 253402280999LL) {
        report.issue("invalid_timestamp", "Timestamp outside 1970-9999 range", source);
        return false;
    }
    const qint64 offset = bar.start - session_start(day(bar.start));
    bool valid = true;
    if (offset < 0 || offset >= session_seconds || offset % (minutes * 60LL) != 0) {
        report.issue("misaligned_timestamp", stamp(bar.start) +
            " is not a regular-session interval open", source);
        valid = false;
    }
    const auto& p = bar.price;
    if (p[0] <= 0 || p[1] <= 0 || p[2] <= 0 || p[3] <= 0 ||
        p[2] > std::min(p[0], p[3]) || p[1] < std::max(p[0], p[3])) {
        report.issue("invalid_ohlc", stamp(bar.start) + " has invalid positive OHLC range", source);
        valid = false;
    }
    if (bar.volume && *bar.volume < 0) {
        report.issue("invalid_volume", stamp(bar.start) + " has negative volume", source);
        valid = false;
    }
    return valid;
}

inline Series valid_series(const Series& input, int minutes, Report& report,
                           const QString& source, std::set<qint64>& dates) {
    Series result;
    for (const auto& [key, bar] : input) {
        if (key != bar.start) {
            report.issue("invalid_key", "Map key differs from interval start", source);
            continue;
        }
        if (valid_bar(bar, minutes, report, source)) {
            result.emplace(key, bar);
            dates.insert(day(key));
        }
    }
    return result;
}

inline void compare_window(const Series& base, const Series& target, qint64 start,
                           qint64 end, qint64 base_seconds, Report& report) {
    const auto target_it = target.find(start);
    if (target_it == target.end())
        report.issue("missing_target", stamp(start) + " has no target bar");
    Bar expected;
    expected.start = start;
    bool first = true;
    bool complete = true;
    bool known = true;
    qint64 total = 0;
    for (qint64 at = start; at < end; at += base_seconds) {
        const auto it = base.find(at);
        if (it == base.end()) {
            report.issue("missing_base", stamp(at) + " required by target " + stamp(start));
            complete = false;
            continue;
        }
        const Bar& b = it->second;
        if (first) {
            expected.price = b.price;
            first = false;
        } else {
            expected.price[1] = std::max(expected.price[1], b.price[1]);
            expected.price[2] = std::min(expected.price[2], b.price[2]);
            expected.price[3] = b.price[3];
        }
        if (!b.volume) {
            known = false;
        } else if (total > std::numeric_limits<qint64>::max() - *b.volume) {
            report.issue("volume_overflow", stamp(start) + " exceeds int64 volume capacity");
            complete = false;
            break;
        } else {
            total += *b.volume;
        }
    }
    if (!complete || first || target_it == target.end()) return;
    ++report.compared_windows;
    report.prices_compared += 4;
    const Bar& actual = target_it->second;
    bool matches = true;
    constexpr std::array<const char*, 4> fields{"open", "high", "low", "close"};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (expected.price[i] != actual.price[i]) {
            report.issue("price_mismatch", stamp(start) + " " + fields[i] +
                " expected_paise=" + QString::number(expected.price[i]) +
                " actual_paise=" + QString::number(actual.price[i]));
            matches = false;
        }
    }
    if (known != actual.volume.has_value()) {
        report.issue("volume_availability_mismatch", stamp(start) +
            " known versus unknown volume cannot be compared");
        matches = false;
    } else if (known) {
        ++report.volumes_compared;
        if (total != *actual.volume) {
            report.issue("volume_mismatch", stamp(start) + " expected=" +
                QString::number(total) + " actual=" + QString::number(*actual.volume));
            matches = false;
        }
    } else {
        ++report.volumes_unverified;
    }
    if (matches) ++report.matched_windows;
}
} // namespace detail

/// Reconcile full regular sessions, exact integer paise and volume; cold-path only.
/// Input interval-open durations are minutes; target must be a larger base multiple.
inline void compare(const Series& base, const Series& target,
                    int base_minutes, int target_minutes, Report& report) {
    if (!detail::durations_ok(base_minutes, target_minutes)) {
        report.issue("invalid_duration", "Require 0 < base < target <= 375 minutes and target % base == 0");
        return;
    }
    std::set<qint64> dates;
    const Series valid_base = detail::valid_series(base, base_minutes, report, "base", dates);
    const Series valid_target = detail::valid_series(target, target_minutes, report, "target", dates);
    if (valid_base.empty() || valid_target.empty())
        report.issue("empty_input", "Both inputs must contain valid bars");
    if (dates.size() > 20000) {
        report.issue("session_limit", "More than 20000 dates; comparison refused, not truncated");
        return;
    }
    report.session_dates += static_cast<qint64>(dates.size());
    for (const qint64 date : dates) {
        const qint64 begin = detail::session_start(date);
        const qint64 close = begin + detail::session_seconds;
        for (qint64 at = begin; at < close; at += target_minutes * 60LL)
            detail::compare_window(valid_base, valid_target, at,
                std::min(close, at + target_minutes * 60LL), base_minutes * 60LL, report);
    }
}

} // namespace altair::ui::barcheck
