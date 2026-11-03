// broker/candles.hpp -- one OHLCV candle off a broker's history API, the IST
// date and stamp helpers every history fetch shares, and the coverage check.
//
// broker/fyers_historical.hpp parses FYERS's /data/history into RawCandle;
// app/dataset_merge.hpp merges candles into dataset/ and refuses a fetch that
// does not cover the window it asked for (verify_coverage).
//
// Prices stay DOUBLE here and only here: this is the decode boundary, and the
// conversion to integer paise is the caller's step against the instrument's
// own price scale. Open interest is optional (`oi_known`): an index or a cash
// scrip has none, and "no OI" must not read as "OI is zero".

#pragma once

#include <core/time/timestamp.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace altair {

enum class HistError : std::uint8_t {
    BadInterval,
    BadRange,        // to <= from, or either unset
    BadToken,
    /// The document is not the shape the broker documents.
    Malformed,
    /// The broker answered with an error status.
    ApiError,
    /// A candle field was present but not parseable as its type.
    BadField,
    /// Chunks came back covering less than was asked for. NOT tolerated.
    IncompleteCoverage
};

/// One candle, as it comes off the wire.
struct RawCandle {
    /// Nanoseconds since the Unix epoch, UTC.
    std::int64_t ts_ns = 0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
    double oi = 0.0;
    /// False for cash instruments, which report no open interest at all.
    bool oi_known = false;
};

/// A [from, to] window for one request, inclusive of both dates.
struct Chunk {
    /// Dates as `YYYY-MM-DD`.
    std::string from;
    std::string to;
};

// ---------------------------------------------------------------------------
// Dates
// ---------------------------------------------------------------------------

namespace detail {

// kNsPerDay is NOT defined here. `core/time/timestamp.hpp` already has one in
// this exact namespace, and a second definition is both a compile error and,
// worse, the kind of duplicate constant that survives as two subtly different
// values once somebody edits one of them.
//
/// IST is UTC+5:30, fixed. India has no daylight saving, which is the only
/// reason a constant offset is correct here rather than a lookup.
constexpr std::int64_t kIstOffsetNs = (5 * 3600 + 30 * 60) * 1'000'000'000LL;

constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m,
                                       unsigned d) noexcept {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

constexpr void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m,
                               unsigned& d) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe =
        (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t yr = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = yr + (m <= 2);
}

/// Two digits, zero padded.
inline void two(std::string& s, unsigned v) {
    s.push_back(static_cast<char>('0' + (v / 10) % 10));
    s.push_back(static_cast<char>('0' + v % 10));
}

} // namespace detail

/// `YYYY-MM-DD` for a day number since the epoch.
[[nodiscard]] inline std::string date_string(std::int64_t day) {
    std::int64_t y = 0;
    unsigned m = 0, d = 0;
    detail::civil_from_days(day, y, m, d);
    std::string s;
    s.reserve(10);
    s += std::to_string(y);
    s.push_back('-');
    detail::two(s, m);
    s.push_back('-');
    detail::two(s, d);
    return s;
}

/// Parse `YYYY-MM-DD` to a day number. Returns false on anything else.
[[nodiscard]] inline bool parse_date(std::string_view s, std::int64_t& day) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') { return false; }
    std::int64_t v[3] = {0, 0, 0};
    const std::size_t at[3] = {0, 5, 8};
    const std::size_t len[3] = {4, 2, 2};
    for (int f = 0; f < 3; ++f) {
        for (std::size_t i = 0; i < len[f]; ++i) {
            const char c = s[at[f] + i];
            if (c < '0' || c > '9') { return false; }
            v[f] = v[f] * 10 + (c - '0');
        }
    }
    if (v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31) { return false; }
    day = detail::days_from_civil(v[0], static_cast<unsigned>(v[1]),
                                  static_cast<unsigned>(v[2]));
    return true;
}

/// Format UTC nanoseconds as `YYYY-MM-DDTHH:MM:SS+05:30`.
///
/// THE COLON FORM, deliberately: it is what every file already in `dataset/`
/// uses, so a fetched month drops in beside a stored one and every existing
/// loader reads both.
[[nodiscard]] inline std::string format_ist(std::int64_t ns) {
    const std::int64_t local = ns + detail::kIstOffsetNs;
    std::int64_t day = local / detail::kNsPerDay;
    std::int64_t rem = local % detail::kNsPerDay;
    if (rem < 0) { rem += detail::kNsPerDay; --day; }
    const std::int64_t secs = rem / 1'000'000'000LL;
    std::string s = date_string(day);
    s.push_back('T');
    detail::two(s, static_cast<unsigned>(secs / 3600));
    s.push_back(':');
    detail::two(s, static_cast<unsigned>((secs / 60) % 60));
    s.push_back(':');
    detail::two(s, static_cast<unsigned>(secs % 60));
    s += "+05:30";
    return s;
}

// ---------------------------------------------------------------------------
// The request
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Coverage
// ---------------------------------------------------------------------------

/// Did the candles actually cover the window that was asked for?
///
/// A history API that truncates an over-long request rather than refusing it
/// produces a short list and not an error. `max_gap_days` is the largest run
/// of missing SESSIONS tolerated -- Indian markets close for weekends and a
/// long holiday stretch, so three or four days is normal and a fortnight is not.
///
/// Returns the largest gap found, in days, or an error if coverage fails.
[[nodiscard]] inline std::expected<std::int64_t, HistError>
verify_coverage(const std::vector<RawCandle>& candles, std::string_view from,
                std::string_view to, std::int64_t max_gap_days) {
    std::int64_t f = 0, t = 0;
    if (!parse_date(from, f) || !parse_date(to, t) || t < f) {
        return std::unexpected(HistError::BadRange);
    }
    if (candles.empty()) {
        return std::unexpected(HistError::IncompleteCoverage);
    }
    const std::int64_t first_day =
        (candles.front().ts_ns + detail::kIstOffsetNs) / detail::kNsPerDay;
    const std::int64_t last_day =
        (candles.back().ts_ns + detail::kIstOffsetNs) / detail::kNsPerDay;

    // The window may legitimately start on a weekend, so the FIRST candle is
    // allowed to be a few days late; it may not be weeks late.
    if (first_day - f > max_gap_days || t - last_day > max_gap_days) {
        return std::unexpected(HistError::IncompleteCoverage);
    }

    std::int64_t worst = 0;
    std::int64_t prev = first_day;
    for (const RawCandle& c : candles) {
        const std::int64_t d =
            (c.ts_ns + detail::kIstOffsetNs) / detail::kNsPerDay;
        if (d < prev) {
            return std::unexpected(HistError::Malformed);   // not ascending
        }
        if (d - prev > worst) { worst = d - prev; }
        prev = d;
    }
    if (worst > max_gap_days) {
        return std::unexpected(HistError::IncompleteCoverage);
    }
    return worst;
}

} // namespace altair
