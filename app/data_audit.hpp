// app/data_audit.hpp -- cross-check dataset/ against itself and against brokers.
//
// Two questions, asked of every instrument in dataset/:
//
//   1. Does each timeframe agree with the others? 5m, 15m, 60m and 1d bars
//      are rebuilt from every finer timeframe the dataset holds (open = first,
//      high = max, low = min, close = last, volume = sum, OI = last) and
//      compared bar by bar with the stored ones. Only COMPLETE buckets are
//      compared: a 60m bar is rebuilt from 1m only when all 60 minutes (15 for
//      the 15:15 bar) are present, so a gap in 1m is reported as a gap, not as
//      a wrong 60m bar.
//   2. Does it agree with the broker? Candles fetched from FYERS and/or Kite
//      into a directory laid out like dataset/ are compared bar by bar.
//
// Each series is also checked on its own: unparseable rows, duplicate and
// conflicting timestamps, impossible OHLC (low above open/close, high below),
// bars outside 09:15-15:30 IST or off the timeframe grid, weekend sessions,
// and trading days with missing intraday bars.
//
// TOLERANCE. A price is EXACT when it differs by at most half a paisa, a
// ROUNDING difference when within 1 basis point (the 1 bp rule of
// app/kite_update_main.cpp: dataset/ keeps two decimals, some sources one),
// and a MISMATCH beyond that. Volume is compared only when both sides carry it.
//
// KNOWN, EXPECTED DIFFERENCE. NSE's official daily close for an index is not
// its last traded value (it is computed from constituents' closing prices,
// themselves a 30-minute VWAP), so a 1d close rebuilt from 1m bars routinely
// differs from the published 1d close. The report counts close separately
// for that reason.
//
// All times are handled as IST wall-clock seconds (UTC + 5:30). Nothing here
// writes to dataset/.
#pragma once

#include <app/price_text.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <istream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace altair::data_audit {

inline constexpr std::int64_t kSessionOpenMin = 9 * 60 + 15;    // 09:15 IST
inline constexpr std::int64_t kSessionCloseMin = 15 * 60 + 30;  // 15:30 IST, cash and index
/// F&O bars run to 15:40 in both the dataset and FYERS' symbol master
/// ("0915-1540"); a futures series is audited against that session.
inline constexpr std::int64_t kFnoSessionCloseMin = 15 * 60 + 40;
inline constexpr std::int64_t kDaySec = 86'400;
inline constexpr int kDailyTf = 1440;
inline constexpr double kExactAbs = 0.005 + 1e-9;               // half a paisa
inline constexpr double kRoundingBp = 1.0;

/// One bar. `t` is the bar START in IST wall-clock seconds; a daily bar sits
/// at IST midnight. Absent volume / OI are NaN, never zero.
struct AuditBar {
    std::int64_t t{};
    double o{}, h{}, l{}, c{};
    double v{std::numeric_limits<double>::quiet_NaN()};
    double oi{std::numeric_limits<double>::quiet_NaN()};
    std::uint32_t file{};
};

struct AuditNote {
    std::string where;    ///< file:line or timestamp
    std::string what;
};

struct AuditSeries {
    int tf{};                                ///< minutes; 1440 = daily
    std::int64_t session_close{kSessionCloseMin};  ///< minute of day the session ends
    std::vector<std::string> files;
    std::vector<AuditBar> bars;              ///< sorted, one per timestamp
    std::size_t rows_read{};
    std::size_t parse_errors{};
    std::size_t duplicates{};                ///< same timestamp, same bar
    std::size_t conflicts{};                 ///< same timestamp, different bar
    std::size_t unsorted{};                  ///< rows not after the previous row of their file
    std::size_t seconds_floored{};           ///< intraday stamps like 09:21:01 moved to 09:21:00
    std::vector<AuditNote> notes;            ///< first problems, for the report
    bool has_volume{};
    bool has_oi{};
};

// ---- time ---------------------------------------------------------------------

[[nodiscard]] constexpr std::int64_t audit_days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

struct AuditCivil { std::int64_t y; unsigned m, d; };

[[nodiscard]] constexpr AuditCivil audit_civil_from_days(std::int64_t z) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return {y + (m <= 2 ? 1 : 0), m, d};
}

[[nodiscard]] constexpr std::int64_t audit_day(std::int64_t t) noexcept {
    return (t >= 0 ? t : t - (kDaySec - 1)) / kDaySec;
}
[[nodiscard]] constexpr std::int64_t audit_minute_of_day(std::int64_t t) noexcept {
    return (t - audit_day(t) * kDaySec) / 60;
}
/// 0 = Monday ... 6 = Sunday.
[[nodiscard]] constexpr int audit_weekday(std::int64_t day) noexcept {
    const std::int64_t w = (day + 3) % 7;   // 1970-01-01 was a Thursday
    return static_cast<int>(w < 0 ? w + 7 : w);
}

/// "YYYY-MM-DD" or "YYYY-MM-DDTHH:MM[:SS][Z|+HH:MM|-HH:MM]" to IST seconds.
/// A time without an offset is taken as IST (how every dataset/ file is written).
[[nodiscard]] inline std::optional<std::int64_t> parse_audit_time(std::string_view s) {
    const auto num = [&s](std::size_t at, std::size_t n, int& out) {
        if (at + n > s.size()) return false;
        out = 0;
        for (std::size_t i = at; i < at + n; ++i) {
            if (s[i] < '0' || s[i] > '9') return false;
            out = out * 10 + (s[i] - '0');
        }
        return true;
    };
    int y = 0, mo = 0, d = 0;
    if (s.size() < 10 || !num(0, 4, y) || s[4] != '-' || !num(5, 2, mo) || s[7] != '-' || !num(8, 2, d))
        return std::nullopt;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return std::nullopt;
    const std::int64_t day = audit_days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
    if (s.size() == 10) return day * kDaySec;
    if (s[10] != 'T' && s[10] != ' ') return std::nullopt;
    int hh = 0, mm = 0, ss = 0;
    if (!num(11, 2, hh) || s.size() < 16 || s[13] != ':' || !num(14, 2, mm)) return std::nullopt;
    std::size_t at = 16;
    if (at < s.size() && s[at] == ':') {
        if (!num(at + 1, 2, ss)) return std::nullopt;
        at += 3;
    }
    if (hh > 23 || mm > 59 || ss > 60) return std::nullopt;
    std::int64_t offset = 19800;   // no suffix: IST
    if (at < s.size()) {
        if (s[at] == 'Z' && at + 1 == s.size()) {
            offset = 0;
        } else if ((s[at] == '+' || s[at] == '-') && at + 6 == s.size() && s[at + 3] == ':') {
            int oh = 0, om = 0;
            if (!num(at + 1, 2, oh) || !num(at + 4, 2, om)) return std::nullopt;
            offset = (s[at] == '-' ? -1 : 1) * (oh * 3600 + om * 60);
        } else {
            return std::nullopt;
        }
    }
    const std::int64_t utc = day * kDaySec + hh * 3600 + mm * 60 + ss - offset;
    return utc + 19800;
}

[[nodiscard]] inline std::string format_audit_time(std::int64_t t, bool daily) {
    const AuditCivil c = audit_civil_from_days(audit_day(t));
    char buf[80];
    if (daily) {
        std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u", static_cast<long long>(c.y), c.m, c.d);
    } else {
        const std::int64_t sec = t - audit_day(t) * kDaySec;
        std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02lld:%02lld:%02lld+05:30",
                      static_cast<long long>(c.y), c.m, c.d, static_cast<long long>(sec / 3600),
                      static_cast<long long>((sec / 60) % 60), static_cast<long long>(sec % 60));
    }
    return buf;
}

[[nodiscard]] inline std::optional<int> audit_tf_from_name(std::string_view name) {
    if (name == "1m") return 1;
    if (name == "5m") return 5;
    if (name == "15m") return 15;
    if (name == "60m") return 60;
    if (name == "1d") return kDailyTf;
    return std::nullopt;
}
[[nodiscard]] inline std::string audit_tf_name(int tf) {
    return tf == kDailyTf ? std::string{"1d"} : std::to_string(tf) + "m";
}

// ---- loading ------------------------------------------------------------------

namespace audit_detail {

inline constexpr std::size_t kMaxNotes = 50;

inline void note(AuditSeries& s, std::string where, std::string what) {
    if (s.notes.size() < kMaxNotes) s.notes.push_back({std::move(where), std::move(what)});
}

[[nodiscard]] inline std::vector<std::string_view> split(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    for (;;) {
        const std::size_t comma = line.find(',', at);
        out.push_back(line.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at));
        if (comma == std::string_view::npos) break;
        at = comma + 1;
    }
    return out;
}

[[nodiscard]] inline bool field(std::string_view text, double& out) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\r')) text.remove_suffix(1);
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    if (text.empty()) return false;
    return altair::dataset::parse_exact_double(text.data(), text.data() + text.size(), out) && std::isfinite(out);
}

[[nodiscard]] inline bool same_bar(const AuditBar& a, const AuditBar& b) {
    const auto eq = [](double x, double y) {
        return (std::isnan(x) && std::isnan(y)) || std::fabs(x - y) <= 1e-9;
    };
    return eq(a.o, b.o) && eq(a.h, b.h) && eq(a.l, b.l) && eq(a.c, b.c) && eq(a.v, b.v);
}

} // namespace audit_detail

/// Read one CSV into `s` (appending). Header names: time, open, high, low,
/// close, volume (optional), oi (optional); order is taken from the header.
inline void load_audit_csv(AuditSeries& s, std::istream& in, const std::string& name) {
    namespace d = audit_detail;
    const auto file = static_cast<std::uint32_t>(s.files.size());
    s.files.push_back(name);
    std::string line;
    if (!std::getline(in, line)) return;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    int ct = -1, co = -1, ch = -1, cl = -1, cc = -1, cv = -1, coi = -1;
    const auto header = d::split(line);
    for (int i = 0; i < static_cast<int>(header.size()); ++i) {
        const std::string_view hname = header[static_cast<std::size_t>(i)];
        if (hname == "time" || hname == "date" || hname == "timestamp") ct = i;
        else if (hname == "open") co = i;
        else if (hname == "high") ch = i;
        else if (hname == "low") cl = i;
        else if (hname == "close") cc = i;
        else if (hname == "volume") cv = i;
        else if (hname == "oi") coi = i;
    }
    if (ct < 0 || co < 0 || ch < 0 || cl < 0 || cc < 0) {
        ++s.parse_errors;
        d::note(s, name + ":1", "no time/open/high/low/close header; file skipped");
        return;
    }
    std::size_t lineno = 1;
    std::int64_t prev = std::numeric_limits<std::int64_t>::min();
    while (std::getline(in, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        ++s.rows_read;
        const auto f = d::split(line);
        const auto at = [&f](int i) { return i >= 0 && static_cast<std::size_t>(i) < f.size()
                                                 ? f[static_cast<std::size_t>(i)] : std::string_view{}; };
        AuditBar b{};
        b.file = file;
        const auto t = parse_audit_time(at(ct));
        if (!t || !d::field(at(co), b.o) || !d::field(at(ch), b.h) || !d::field(at(cl), b.l)
            || !d::field(at(cc), b.c)) {
            ++s.parse_errors;
            d::note(s, name + ":" + std::to_string(lineno), "unparseable row");
            continue;
        }
        b.t = *t;
        // A daily bar is its DATE: brokers stamp it 00:00 or 05:30 IST.
        if (s.tf == kDailyTf) b.t = audit_day(b.t) * kDaySec;
        // Some vendors stamp a minute bar at hh:mm:01. The bar is the minute;
        // it is floored and counted, never silently re-timed.
        if (s.tf != kDailyTf && b.t % 60 != 0) {
            if (s.seconds_floored == 0)
                d::note(s, name + ":" + std::to_string(lineno), "timestamp carries seconds; floored to the minute");
            ++s.seconds_floored;
            b.t -= ((b.t % 60) + 60) % 60;
        }
        double x = 0.0;
        if (cv >= 0 && d::field(at(cv), x)) { b.v = x; s.has_volume = true; }
        if (coi >= 0 && d::field(at(coi), x)) { b.oi = x; s.has_oi = true; }
        if (b.t < prev) ++s.unsorted;   // equal stamps are duplicates, counted later
        prev = b.t;
        s.bars.push_back(b);
    }
}

/// Sort and collapse duplicate timestamps; the first file's bar is kept.
inline void finish_audit_series(AuditSeries& s) {
    std::stable_sort(s.bars.begin(), s.bars.end(),
                     [](const AuditBar& a, const AuditBar& b) { return a.t < b.t; });
    std::vector<AuditBar> out;
    out.reserve(s.bars.size());
    for (const auto& b : s.bars) {
        if (!out.empty() && out.back().t == b.t) {
            if (audit_detail::same_bar(out.back(), b)) {
                ++s.duplicates;
            } else {
                ++s.conflicts;
                audit_detail::note(s, format_audit_time(b.t, s.tf == kDailyTf),
                                   "conflicting bars in " + s.files[out.back().file] + " and " + s.files[b.file]);
            }
            continue;
        }
        out.push_back(b);
    }
    s.bars = std::move(out);
}

/// Every *.csv in `dir` whose name does not start with '.' or '_', in name order.
[[nodiscard]] inline AuditSeries load_audit_dir(const std::filesystem::path& dir, int tf,
                                                std::int64_t session_close = kSessionCloseMin) {
    AuditSeries s;
    s.tf = tf;
    s.session_close = session_close;
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string n = e.path().filename().string();
        if (n.empty() || n[0] == '.' || n[0] == '_' || e.path().extension() != ".csv") continue;
        files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& p : files) {
        std::ifstream in(p, std::ios::binary);
        load_audit_csv(s, in, p.filename().string());
    }
    finish_audit_series(s);
    return s;
}

// ---- integrity ----------------------------------------------------------------

struct AuditIntegrity {
    std::size_t bad_ohlc{};
    std::size_t outside_session{};
    std::size_t off_grid{};
    std::size_t weekend_days{};
    std::size_t trading_days{};
    std::size_t short_days{};          ///< intraday: fewer bars than a full session
    std::size_t missing_bars{};        ///< intraday: total bars short on those days
    std::vector<AuditNote> notes;
};

[[nodiscard]] inline std::int64_t audit_expected_bars(int tf, std::int64_t session_close = kSessionCloseMin) {
    return (session_close - kSessionOpenMin + tf - 1) / tf;
}

[[nodiscard]] inline AuditIntegrity check_audit_series(const AuditSeries& s) {
    AuditIntegrity r;
    const bool daily = s.tf == kDailyTf;
    const auto add = [&r](std::string where, std::string what) {
        if (r.notes.size() < audit_detail::kMaxNotes) r.notes.push_back({std::move(where), std::move(what)});
    };
    std::int64_t day = std::numeric_limits<std::int64_t>::min();
    std::int64_t in_day = 0;
    const auto close_day = [&] {
        if (day == std::numeric_limits<std::int64_t>::min()) return;
        ++r.trading_days;
        const int wd = audit_weekday(day);
        if (wd >= 5) {
            ++r.weekend_days;
            add(format_audit_time(day * kDaySec, true), "weekend session (special session or bad date)");
        }
        const std::int64_t expected = audit_expected_bars(s.tf, s.session_close);
        if (!daily && in_day < expected) {
            ++r.short_days;
            r.missing_bars += static_cast<std::size_t>(expected - in_day);
            add(format_audit_time(day * kDaySec, true),
                std::to_string(in_day) + " of " + std::to_string(expected) + " bars");
        }
    };
    for (const auto& b : s.bars) {
        const bool ok = b.o > 0 && b.h > 0 && b.l > 0 && b.c > 0 && b.l <= b.h + 1e-9
                     && b.l <= std::min(b.o, b.c) + 1e-9 && b.h + 1e-9 >= std::max(b.o, b.c);
        if (!ok) {
            ++r.bad_ohlc;
            add(format_audit_time(b.t, daily), "impossible OHLC");
        }
        const std::int64_t bd = audit_day(b.t);
        if (bd != day) {
            close_day();
            day = bd;
            in_day = 0;
        }
        ++in_day;
        if (!daily) {
            const std::int64_t m = audit_minute_of_day(b.t);
            if (m < kSessionOpenMin || m >= s.session_close) {
                ++r.outside_session;
                add(format_audit_time(b.t, false),
                    "outside the regular session (e.g. a Muhurat evening session)");
            } else if ((m - kSessionOpenMin) % s.tf != 0 || b.t % 60 != 0) {
                ++r.off_grid;
                add(format_audit_time(b.t, false), "not on the " + audit_tf_name(s.tf) + " grid from 09:15");
            }
        }
    }
    close_day();
    return r;
}

// ---- aggregation --------------------------------------------------------------

/// A coarse bar rebuilt from finer bars, and whether every constituent was there.
struct AuditBucket {
    AuditBar bar;
    std::int64_t have{};
    std::int64_t expected{};
    [[nodiscard]] bool complete() const noexcept { return have == expected; }
};

/// Rebuild `coarse_tf` bars from `fine`. Buckets anchor at 09:15; the last
/// intraday bucket of a day is short (15:15-15:30 for 60m, 15:15-15:40 for
/// F&O). Bars outside the session are ignored (check_audit_series reports them).
[[nodiscard]] inline std::vector<AuditBucket> aggregate_audit(const AuditSeries& fine, int coarse_tf) {
    std::vector<AuditBucket> out;
    if (fine.tf >= coarse_tf || fine.tf == kDailyTf) return out;
    for (const auto& b : fine.bars) {
        const std::int64_t day = audit_day(b.t);
        const std::int64_t m = audit_minute_of_day(b.t);
        if (m < kSessionOpenMin || m >= fine.session_close) continue;
        std::int64_t start = 0;
        std::int64_t expected = 0;
        if (coarse_tf == kDailyTf) {
            start = day * kDaySec;
            expected = audit_expected_bars(fine.tf, fine.session_close);
        } else {
            const std::int64_t bm = kSessionOpenMin + (m - kSessionOpenMin) / coarse_tf * coarse_tf;
            start = day * kDaySec + bm * 60;
            const std::int64_t span = std::min<std::int64_t>(coarse_tf, fine.session_close - bm);
            expected = (span + fine.tf - 1) / fine.tf;
        }
        if (out.empty() || out.back().bar.t != start) {
            AuditBucket k;
            k.bar = b;
            k.bar.t = start;
            k.expected = expected;
            k.have = 1;
            out.push_back(k);
            continue;
        }
        AuditBar& a = out.back().bar;
        a.h = std::max(a.h, b.h);
        a.l = std::min(a.l, b.l);
        a.c = b.c;
        a.v = std::isnan(a.v) || std::isnan(b.v) ? std::numeric_limits<double>::quiet_NaN() : a.v + b.v;
        a.oi = b.oi;
        ++out.back().have;
    }
    return out;
}

// ---- comparison -----------------------------------------------------------------

enum class AuditField : std::uint8_t { Open, High, Low, Close };
inline constexpr std::array<const char*, 4> kAuditFieldNames{"open", "high", "low", "close"};

struct AuditDiff {
    std::int64_t t{};
    AuditField field{};
    double mine{}, other{};
    double bp{};
};

struct AuditCompare {
    std::string label;                    ///< e.g. "1m -> 60m", "FYERS"
    std::int64_t overlap_from{}, overlap_to{};
    std::size_t reference_bars{};         ///< in the overlap
    std::size_t other_bars{};             ///< in the overlap
    std::size_t compared{};
    std::size_t not_comparable{};         ///< incomplete rebuilt buckets
    std::size_t only_reference{};
    std::size_t only_other{};
    std::size_t exact_bars{};
    std::size_t rounding_bars{};
    std::size_t mismatch_bars{};
    std::array<std::size_t, 4> field_mismatch{};
    std::array<std::size_t, 4> field_rounding{};
    std::size_t volume_compared{};
    std::size_t volume_equal{};
    double worst_bp{};
    double median_mismatch_bp{};          ///< over the mismatches kept
    std::vector<AuditDiff> diffs;          ///< mismatches only, worst first, capped
    std::vector<std::int64_t> missing_ref; ///< first timestamps only in `other`, capped
    std::vector<std::int64_t> missing_other;
    [[nodiscard]] double match_pct() const noexcept {
        return compared == 0 ? 0.0 : 100.0 * static_cast<double>(exact_bars + rounding_bars)
                                         / static_cast<double>(compared);
    }
};

/// Mismatches kept per comparison (the counts above are always complete).
inline constexpr std::size_t kAuditMaxDiffs = 200'000;

/// Compare `reference` (the stored series) with `other`, bar by bar, over the
/// time range both cover. `complete` (same length as `other`, or empty = all
/// complete) marks rebuilt buckets whose every constituent was present.
[[nodiscard]] inline AuditCompare compare_audit(std::string label, const std::vector<AuditBar>& reference,
                                                const std::vector<AuditBar>& other,
                                                const std::vector<bool>& complete = {}) {
    AuditCompare r;
    r.label = std::move(label);
    if (reference.empty() || other.empty()) return r;
    r.overlap_from = std::max(reference.front().t, other.front().t);
    r.overlap_to = std::min(reference.back().t, other.back().t);
    if (r.overlap_from > r.overlap_to) return r;
    std::size_t i = 0, j = 0;
    while (i < reference.size() && reference[i].t < r.overlap_from) ++i;
    while (j < other.size() && other[j].t < r.overlap_from) ++j;
    while (i < reference.size() || j < other.size()) {
        const bool ri = i < reference.size() && reference[i].t <= r.overlap_to;
        const bool oj = j < other.size() && other[j].t <= r.overlap_to;
        if (!ri && !oj) break;
        if (ri && (!oj || reference[i].t < other[j].t)) {
            ++r.reference_bars;
            ++r.only_reference;
            if (r.missing_other.size() < 200) r.missing_other.push_back(reference[i].t);
            ++i;
            continue;
        }
        if (oj && (!ri || other[j].t < reference[i].t)) {
            ++r.other_bars;
            const bool full = complete.empty() || complete[j];
            if (full) {
                ++r.only_other;
                if (r.missing_ref.size() < 200) r.missing_ref.push_back(other[j].t);
            } else {
                ++r.not_comparable;
            }
            ++j;
            continue;
        }
        ++r.reference_bars;
        ++r.other_bars;
        const AuditBar& a = reference[i];
        const AuditBar& b = other[j];
        const bool full = complete.empty() || complete[j];
        ++i;
        ++j;
        if (!full) { ++r.not_comparable; continue; }
        ++r.compared;
        const std::array<double, 4> av{a.o, a.h, a.l, a.c};
        const std::array<double, 4> bv{b.o, b.h, b.l, b.c};
        bool mismatch = false;
        bool rounding = false;
        for (std::size_t f = 0; f < 4; ++f) {
            const double diff = std::fabs(av[f] - bv[f]);
            if (diff <= kExactAbs) continue;
            const double bp = av[f] != 0.0 ? diff / std::fabs(av[f]) * 1e4 : 1e9;
            r.worst_bp = std::max(r.worst_bp, bp);
            if (bp <= kRoundingBp) {
                rounding = true;
                ++r.field_rounding[f];
            } else {
                mismatch = true;
                ++r.field_mismatch[f];
                if (r.diffs.size() < kAuditMaxDiffs)
                    r.diffs.push_back({a.t, static_cast<AuditField>(f), av[f], bv[f], bp});
            }
        }
        if (mismatch) ++r.mismatch_bars;
        else if (rounding) ++r.rounding_bars;
        else ++r.exact_bars;
        if (!std::isnan(a.v) && !std::isnan(b.v)) {
            ++r.volume_compared;
            if (std::fabs(a.v - b.v) < 0.5) ++r.volume_equal;
        }
    }
    std::stable_sort(r.diffs.begin(), r.diffs.end(),
                     [](const AuditDiff& x, const AuditDiff& y) { return x.bp > y.bp; });
    if (!r.diffs.empty()) r.median_mismatch_bp = r.diffs[r.diffs.size() / 2].bp;
    return r;
}

/// Rebuilt buckets as bars plus their completeness flags, for compare_audit.
inline void split_buckets(const std::vector<AuditBucket>& buckets, std::vector<AuditBar>& bars,
                          std::vector<bool>& complete) {
    bars.clear();
    complete.clear();
    bars.reserve(buckets.size());
    complete.reserve(buckets.size());
    for (const auto& k : buckets) {
        bars.push_back(k.bar);
        complete.push_back(k.complete());
    }
}

/// Timeframe of a file or directory of bars from its spacing: date-only or
/// midnight stamps are daily; otherwise the most common gap between
/// consecutive same-day bars, when it is 1, 5, 15 or 60 minutes.
[[nodiscard]] inline std::optional<int> infer_audit_tf(const AuditSeries& s) {
    if (s.bars.empty()) return std::nullopt;
    bool all_midnight = true;
    std::map<std::int64_t, std::size_t> gaps;
    for (std::size_t i = 0; i < s.bars.size(); ++i) {
        if (audit_minute_of_day(s.bars[i].t) != 0 || s.bars[i].t % 60 != 0) all_midnight = false;
        if (i > 0 && audit_day(s.bars[i].t) == audit_day(s.bars[i - 1].t))
            ++gaps[(s.bars[i].t - s.bars[i - 1].t) / 60];
    }
    if (all_midnight) return kDailyTf;
    std::int64_t best = 0;
    std::size_t count = 0;
    for (const auto& [g, n] : gaps)
        if (n > count) { best = g; count = n; }
    if (best == 1 || best == 5 || best == 15 || best == 60) return static_cast<int>(best);
    return std::nullopt;
}

} // namespace altair::data_audit
