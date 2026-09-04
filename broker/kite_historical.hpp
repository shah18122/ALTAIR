// broker/kite_historical.hpp -- Kite's historical candle API.
//
// P2-12. The card that turns "we have no intraday history" into a fetch.
//
// P8-15 measured that the binding constraint on every model in this tree is
// DATA, not compute: 3,131 hourly bars and four partial days of one-minute is
// not enough to identify anything, and eleven of twelve rows on the Models
// panel say so. This is the request that fills `dataset/`.
//
// SHAPE TAKEN FROM ZERODHA'S OWN CLIENT, NOT FROM MEMORY.
//
// `research/reference/gokiteconnect/market.go` is the authority for what comes
// back, and reading it settled two things that a plausible guess gets wrong.
//
// TRAP 1: THE OFFSET HAS NO COLON, AND OURS DOES.
//
// Kite's layout is `2006-01-02T15:04:05-0700` (market.go:236), so a candle
// stamp is `2026-08-31T09:15:00+0530`. Every CSV already in `dataset/` writes
// `+05:30` WITH a colon. Neither is wrong; they are different, and a parser
// written against one silently rejects every row of the other. This file
// therefore ACCEPTS BOTH on read and EMITS the colon form on write, so the
// existing loaders in `desktop/data/bar_csv.hpp` and the model tests keep
// working against a freshly fetched file without being touched.
//
// TRAP 2: A CANDLE IS A HETEROGENEOUS ARRAY AND OI IS OPTIONAL.
//
// Not an object -- `[date, open, high, low, close, volume]`, with open
// interest as a SEVENTH element that is present for derivatives and absent for
// cash (market.go:224, `if len(i) > 6`). Absent is not zero: an index has no
// open interest to report, and writing 0 would make "no OI" and "OI is zero"
// the same value, which is the confusion P11Q-06 already had to unpick for
// volume. So `oi_known` is carried beside it.
//
// Volume arrives as a JSON NUMBER and Zerodha's own client reads it as a
// float64 before narrowing (market.go:216). A parser demanding an integer
// token would reject `12345.0`, which is legal JSON for the same value.
//
// THE 60-DAY WINDOW IS AN API LIMIT, AND EXCEEDING IT LOSES DATA QUIETLY.
//
// Kite caps a single historical request by interval -- around 60 days for
// one-minute. Ask for two years in one call and the answer is not an error, it
// is a SHORTER LIST. So two years must be fetched in chunks, and the chunking
// is where the data loss hides.
//
// The defence is not to trust the limit. `chunk_requests` splits
// conservatively, and `verify_coverage` then checks that what came back
// actually spans what was asked for -- so a limit that is wrong, or that
// Zerodha changes, surfaces as a refusal naming the gap instead of a file that
// is quietly missing three weeks. Rule 9: the numbers below are the best
// available and they are CHECKED rather than believed.

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
    /// The document is not the shape market.go describes.
    Malformed,
    /// `status` was not "success" -- Kite puts the reason in `message`.
    ApiError,
    /// A candle field was present but not parseable as its type.
    BadField,
    /// Chunks came back covering less than was asked for. NOT tolerated: see
    /// the header.
    IncompleteCoverage
};

/// The intervals Kite serves, with the per-request day cap for each.
///
/// The caps are Kite's, they are not in any header we control, and they have
/// changed before. `verify_coverage` exists because of that: these are the
/// starting assumption, not the guarantee.
struct Interval {
    const char* name;
    std::int32_t max_days;
};

inline constexpr Interval kMinute   {"minute",    60};
inline constexpr Interval kMinute3  {"3minute",  100};
inline constexpr Interval kMinute5  {"5minute",  100};
inline constexpr Interval kMinute10 {"10minute", 100};
inline constexpr Interval kMinute15 {"15minute", 200};
inline constexpr Interval kMinute30 {"30minute", 200};
inline constexpr Interval kMinute60 {"60minute", 400};
inline constexpr Interval kDay      {"day",     2000};

[[nodiscard]] inline const Interval* interval_by_name(std::string_view n) {
    for (const Interval* i : {&kMinute, &kMinute3, &kMinute5, &kMinute10,
                              &kMinute15, &kMinute30, &kMinute60, &kDay}) {
        if (n == i->name) { return i; }
    }
    return nullptr;
}

/// One candle, as it comes off the wire.
///
/// Prices stay DOUBLE here and only here. This is the decode boundary: Kite
/// sends JSON numbers, and converting to integer paise is the caller's step
/// against the instrument's own price scale (the P0-01 carried-debt note --
/// NSE-CD is 10^-7 rupees, not paise). A struct that claimed `Price` here
/// would be asserting a scale this layer does not know.
struct RawCandle {
    /// Nanoseconds since the Unix epoch, UTC. The +0530 offset is applied on
    /// parse, so nothing downstream has to know the stamp was ever local.
    std::int64_t ts_ns = 0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
    double oi = 0.0;
    /// False for cash instruments, which report no open interest at all.
    /// Absence is not zero -- see the header.
    bool oi_known = false;
};

/// A [from, to] window for one request, inclusive of both dates.
struct Chunk {
    /// Dates as `YYYY-MM-DD`, which is what the API takes.
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

/// Parse a Kite candle stamp to UTC nanoseconds.
///
/// ACCEPTS BOTH `+0530` and `+05:30`. Kite emits the first (market.go's
/// `-0700` layout); every CSV in `dataset/` carries the second. Accepting one
/// and not the other is how a fetched file and a stored file stop being the
/// same format -- see the header.
[[nodiscard]] inline bool parse_stamp(std::string_view s, std::int64_t& ns) {
    // YYYY-MM-DDTHH:MM:SS then an offset.
    if (s.size() < 19 || s[10] != 'T' || s[13] != ':' || s[16] != ':') {
        return false;
    }
    std::int64_t day = 0;
    if (!parse_date(s.substr(0, 10), day)) { return false; }
    const auto num2 = [&s](std::size_t i, std::int64_t& out) {
        if (s[i] < '0' || s[i] > '9' || s[i + 1] < '0' || s[i + 1] > '9') {
            return false;
        }
        out = (s[i] - '0') * 10 + (s[i + 1] - '0');
        return true;
    };
    std::int64_t h = 0, mi = 0, se = 0;
    if (!num2(11, h) || !num2(14, mi) || !num2(17, se)) { return false; }
    if (h > 23 || mi > 59 || se > 60) { return false; }

    std::int64_t off_ns = 0;
    if (s.size() > 19) {
        std::string_view o = s.substr(19);
        if (o == "Z") {
            off_ns = 0;
        } else {
            if (o.size() != 5 && o.size() != 6) { return false; }
            const char sign = o[0];
            if (sign != '+' && sign != '-') { return false; }
            // "+0530" or "+05:30" -- the colon is the only difference.
            const std::size_t mpos = (o.size() == 6) ? 4 : 3;
            if (o.size() == 6 && o[3] != ':') { return false; }
            std::int64_t oh = 0, om = 0;
            for (std::size_t i = 1; i < 3; ++i) {
                if (o[i] < '0' || o[i] > '9') { return false; }
                oh = oh * 10 + (o[i] - '0');
            }
            for (std::size_t i = mpos; i < mpos + 2; ++i) {
                if (o[i] < '0' || o[i] > '9') { return false; }
                om = om * 10 + (o[i] - '0');
            }
            off_ns = (oh * 3600 + om * 60) * 1'000'000'000LL;
            if (sign == '-') { off_ns = -off_ns; }
        }
    }
    ns = day * detail::kNsPerDay
       + (h * 3600 + mi * 60 + se) * 1'000'000'000LL - off_ns;
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

/// Split [from, to] into requests no longer than the interval allows.
///
/// Inclusive of both ends, contiguous, non-overlapping. An overlap would
/// duplicate candles at the seam and a gap would lose them, so the seam is
/// `previous_to + 1 day` exactly and the test checks it.
[[nodiscard]] inline std::expected<std::vector<Chunk>, HistError>
chunk_requests(std::string_view from, std::string_view to,
               std::string_view interval) {
    const Interval* iv = interval_by_name(interval);
    if (iv == nullptr) { return std::unexpected(HistError::BadInterval); }
    std::int64_t f = 0, t = 0;
    if (!parse_date(from, f) || !parse_date(to, t) || t < f) {
        return std::unexpected(HistError::BadRange);
    }
    std::vector<Chunk> out;
    for (std::int64_t s = f; s <= t; s += iv->max_days) {
        const std::int64_t e = std::min(t, s + iv->max_days - 1);
        out.push_back(Chunk{date_string(s), date_string(e)});
    }
    return out;
}

/// The path and query for one request. The host and the Authorization header
/// are the caller's -- this file never sees a credential.
[[nodiscard]] inline std::expected<std::string, HistError>
historical_uri(std::int64_t instrument_token, std::string_view interval,
               const Chunk& c, bool continuous, bool oi) {
    if (instrument_token <= 0) { return std::unexpected(HistError::BadToken); }
    if (interval_by_name(interval) == nullptr) {
        return std::unexpected(HistError::BadInterval);
    }
    std::int64_t a = 0, b = 0;
    if (!parse_date(c.from, a) || !parse_date(c.to, b) || b < a) {
        return std::unexpected(HistError::BadRange);
    }
    std::string u = "/instruments/historical/";
    u += std::to_string(instrument_token);
    u += '/';
    u.append(interval);
    u += "?from=";
    u += c.from;
    u += "&to=";
    u += c.to;
    u += "&continuous=";
    u += continuous ? '1' : '0';
    u += "&oi=";
    u += oi ? '1' : '0';
    return u;
}

// ---------------------------------------------------------------------------
// The response
// ---------------------------------------------------------------------------

namespace detail {

inline void skip_ws(std::string_view s, std::size_t& i) {
    while (i < s.size()
           && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        ++i;
    }
}

/// A JSON number. Strict: it does not skip what it cannot read.
inline bool read_number(std::string_view s, std::size_t& i, double& out) {
    skip_ws(s, i);
    const std::size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) { ++i; }
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; any = true; }
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; any = true; }
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) { ++i; }
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; }
    }
    if (!any) { return false; }
    out = std::strtod(std::string(s.substr(start, i - start)).c_str(), nullptr);
    return true;
}

/// A JSON string with no escape handling, which is correct ONLY because the
/// candle payload's one string is an ISO stamp. Anything containing a
/// backslash is REFUSED rather than mis-read -- a scanner that silently
/// mishandles an escape is worse than one that admits it cannot.
inline bool read_string(std::string_view s, std::size_t& i,
                        std::string_view& out) {
    skip_ws(s, i);
    if (i >= s.size() || s[i] != '"') { return false; }
    ++i;
    const std::size_t start = i;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\') { return false; }
        ++i;
    }
    if (i >= s.size()) { return false; }
    out = s.substr(start, i - start);
    ++i;
    return true;
}

} // namespace detail

/// Parse a `/instruments/historical` response body.
///
/// Deliberately a purpose-built scanner rather than simdjson: simdjson only
/// exists under the vcpkg `net` feature, and a parser that cannot be tested in
/// the default preset is a parser gate 4 does not cover. The grammar here is
/// tiny and closed -- numbers and one ISO stamp, no escapes, no unicode -- and
/// every branch REFUSES what it does not expect instead of skipping it.
[[nodiscard]] inline std::expected<std::vector<RawCandle>, HistError>
parse_candles(std::string_view body) {
    // Kite wraps everything: {"status":"success","data":{"candles":[...]}}.
    // A failed call is 200 with status "error", so the status is checked
    // BEFORE the data -- an error body has no candles and "no candles" would
    // otherwise read as "no trading that day".
    const std::size_t st = body.find("\"status\"");
    if (st == std::string_view::npos) {
        return std::unexpected(HistError::Malformed);
    }
    const std::size_t ok = body.find("\"success\"", st);
    const std::size_t cand = body.find("\"candles\"");
    if (ok == std::string_view::npos || (cand != std::string_view::npos
                                         && ok > cand)) {
        return std::unexpected(HistError::ApiError);
    }
    if (cand == std::string_view::npos) {
        return std::unexpected(HistError::Malformed);
    }

    std::size_t i = cand + 9;
    detail::skip_ws(body, i);
    if (i >= body.size() || body[i] != ':') {
        return std::unexpected(HistError::Malformed);
    }
    ++i;
    detail::skip_ws(body, i);
    if (i >= body.size() || body[i] != '[') {
        return std::unexpected(HistError::Malformed);
    }
    ++i;

    std::vector<RawCandle> out;
    detail::skip_ws(body, i);
    if (i < body.size() && body[i] == ']') {
        return out;                       // legitimately empty: a holiday
    }

    while (i < body.size()) {
        detail::skip_ws(body, i);
        if (i >= body.size() || body[i] != '[') {
            return std::unexpected(HistError::Malformed);
        }
        ++i;

        RawCandle c{};
        std::string_view stamp;
        if (!detail::read_string(body, i, stamp)
            || !parse_stamp(stamp, c.ts_ns)) {
            return std::unexpected(HistError::BadField);
        }
        double v[5] = {};
        for (double& x : v) {
            detail::skip_ws(body, i);
            if (i >= body.size() || body[i] != ',') {
                return std::unexpected(HistError::Malformed);
            }
            ++i;
            if (!detail::read_number(body, i, x)) {
                return std::unexpected(HistError::BadField);
            }
        }
        c.open = v[0]; c.high = v[1]; c.low = v[2];
        c.close = v[3]; c.volume = v[4];

        detail::skip_ws(body, i);
        if (i < body.size() && body[i] == ',') {
            // The SEVENTH element. Present for derivatives, absent for cash --
            // and absent is not zero.
            ++i;
            double oi = 0.0;
            if (!detail::read_number(body, i, oi)) {
                return std::unexpected(HistError::BadField);
            }
            c.oi = oi;
            c.oi_known = true;
            detail::skip_ws(body, i);
        }
        if (i >= body.size() || body[i] != ']') {
            return std::unexpected(HistError::Malformed);
        }
        ++i;
        out.push_back(c);

        detail::skip_ws(body, i);
        if (i < body.size() && body[i] == ',') { ++i; continue; }
        if (i < body.size() && body[i] == ']') { break; }
        return std::unexpected(HistError::Malformed);
    }
    return out;
}

/// Did the candles actually cover the window that was asked for?
///
/// THE POINT OF THE WHOLE FILE. Kite truncates an over-long request rather
/// than refusing it, so a wrong day-cap produces a short list and not an
/// error. `max_gap_days` is the largest run of missing SESSIONS tolerated --
/// Indian markets close for weekends and a long holiday stretch, so three or
/// four days is normal and a fortnight is not.
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
