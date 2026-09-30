// app/forecast_tracks.hpp -- dataset/ into clean forecast-curriculum tracks.
//
// A track is one instrument at one horizon (models/curriculum.hpp). This
// file decides what the models are allowed to see, and it is where the
// data audit's findings (ops/data-audit.md) turn into rules:
//
//   * Stamps with seconds are floored and repeated stamps keep the first bar
//     (app/data_audit.hpp's loader); both are counted.
//   * A bar whose open or close lies outside its own high-low (INDIA VIX,
//     Dec 2018-2019) has its range widened to include them and is counted.
//     Its close -- what every target uses -- is kept as printed.
//   * A non-positive or non-finite price drops the bar, counted.
//   * Hourly: only days with the full 09:15-15:15 grid of seven bars are
//     forecast. Muhurat and special sessions and the partial last day
//     (2026-09-25, ends 12:47) are excluded, counted.
//   * NIFTY futures: daily only. The intraday files hold the next-month
//     contract for July-August 2026 (the audit's finding), so an intraday
//     futures track would be learning the wrong instrument. A daily outcome
//     that crosses an expiry is a roll, not a move -- the continuous series
//     switches contract -- and is excluded. The expiry calendar (last
//     Thursday of the month, last Tuesday from September 2025, the previous
//     trading day when that is a holiday) is checked against the data on
//     every run: the basis should be ~0 on expiry and jump the next day.
//     Futures features come from the spot index plus the basis, so the roll
//     jump never enters a feature.
//   * INDIA VIX enters every index track as a feature, at the same bar. A row
//     with no VIX bar at its time is dropped, counted -- not filled.
//   * History before INDIA VIX begins (NIFTY daily goes back to 1990) is not
//     used: those rows could not carry the VIX features.
//
// Every feature of row i is computed from bars at or before row i's
// decision time; the outcome is the next close. models/curriculum.hpp checks
// the timing again and refuses a track that breaks it.

#pragma once

#include <app/data_audit.hpp>
#include <models/curriculum.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace altair::forecast_tracks {

namespace da = altair::data_audit;

inline constexpr std::int64_t kDaySec = 86'400;
/// 15:30 IST: the cash close, and when a daily decision is taken.
inline constexpr std::int64_t kCloseSec = 15 * 3600 + 30 * 60;
/// Futures STT on the sell side rose from 0.02 % to 0.05 % on this day
/// (config/charges.toml).
inline constexpr std::int64_t kSttRiseDay = da::audit_days_from_civil(2026, 4, 1);
/// NIFTY monthly expiry moved from Thursday to Tuesday with the September 2025 series.
inline constexpr std::int64_t kTuesdayExpiryFrom = da::audit_days_from_civil(2025, 9, 1);

/// What cleaning did to one track, for the report.
struct TrackInfo {
    std::string track;
    std::string source;
    std::size_t files{}, rows_read{}, parse_errors{}, duplicates{}, conflicts{}, seconds_floored{};
    std::size_t bars{};             ///< after cleaning
    std::size_t bad_price{};        ///< dropped
    std::size_t ohlc_repaired{};    ///< range widened to include open and close
    std::size_t short_days{};       ///< hourly: days without the full grid, not forecast
    std::size_t before_vix{};       ///< bars before INDIA VIX begins, not used
    std::size_t no_vix{};           ///< rows dropped: no VIX bar at the same time
    std::size_t no_spot{};          ///< futures rows dropped: no spot close for the basis
    std::size_t roll_excluded{};    ///< futures outcomes that cross an expiry
    std::size_t expiries{};
    double basis_on_expiry_bp = std::numeric_limits<double>::quiet_NaN();
    double basis_jump_after_bp = std::numeric_limits<double>::quiet_NaN();
    std::size_t warmup{};           ///< bars used only as history for the first features
    std::size_t rows{};
    std::int32_t days{};
    std::string first, last;        ///< first and last decision
    std::string cost_note;
};

/// Round trip on NIFTY futures, bp: STT (sell side, dated) plus everything else.
[[nodiscard]] inline double futures_cost_bp(std::int64_t day, double other_bp) noexcept {
    return (day < kSttRiseDay ? 2.0 : 5.0) + other_bp;
}

/// Load one dataset folder and copy the loader's counts into `info`.
[[nodiscard]] inline std::vector<da::AuditBar>
load_bars(const std::filesystem::path& dir, int tf, TrackInfo& info) {
    const auto s = da::load_audit_dir(dir, tf);
    info.source = dir.generic_string();
    info.files += s.files.size();
    info.rows_read += s.rows_read;
    info.parse_errors += s.parse_errors;
    info.duplicates += s.duplicates;
    info.conflicts += s.conflicts;
    info.seconds_floored += s.seconds_floored;
    return s.bars;
}

/// Drop impossible prices; widen an OHLC whose open or close is outside it.
inline void clean_bars(std::vector<da::AuditBar>& bars, TrackInfo& info) {
    std::vector<da::AuditBar> out;
    out.reserve(bars.size());
    for (da::AuditBar b : bars) {
        const bool finite = std::isfinite(b.o) && std::isfinite(b.h) && std::isfinite(b.l) && std::isfinite(b.c);
        if (!finite || !(b.o > 0.0) || !(b.h > 0.0) || !(b.l > 0.0) || !(b.c > 0.0)) {
            ++info.bad_price;
            continue;
        }
        const double hi = std::max(std::max(b.o, b.h), std::max(b.l, b.c));
        const double lo = std::min(std::min(b.o, b.h), std::min(b.l, b.c));
        if (hi != b.h || lo != b.l) {
            // RULE 11: counted -- TrackInfo::ohlc_repaired.
            b.h = hi;
            b.l = lo;
            ++info.ohlc_repaired;
        }
        out.push_back(b);
    }
    bars.swap(out);
    info.bars = bars.size();
}

[[nodiscard]] inline std::int64_t bar_day(const da::AuditBar& b) noexcept { return da::audit_day(b.t); }

/// NIFTY monthly expiries on the trading days present in `calendar`.
[[nodiscard]] inline std::set<std::int64_t> nifty_expiries(const std::vector<da::AuditBar>& calendar) {
    std::set<std::int64_t> days;
    for (const auto& b : calendar) { days.insert(bar_day(b)); }
    std::set<std::int64_t> out;
    if (days.empty()) { return out; }
    const auto first = da::audit_civil_from_days(*days.begin());
    const auto last = da::audit_civil_from_days(*days.rbegin());
    for (std::int64_t y = first.y; y <= last.y; ++y) {
        for (unsigned m = 1; m <= 12; ++m) {
            const std::int64_t next = m == 12 ? da::audit_days_from_civil(y + 1, 1, 1)
                                              : da::audit_days_from_civil(y, m + 1, 1);
            std::int64_t d = next - 1;   // last day of the month
            const std::int64_t month_start = da::audit_days_from_civil(y, m, 1);
            const int want = month_start >= kTuesdayExpiryFrom ? 1 : 3;   // Tue : Thu (Mon = 0)
            while (da::audit_weekday(d) != want) { --d; }
            if (d > *days.rbegin()) { continue; }   // the data ends before this month's expiry
            while (d >= month_start && !days.contains(d)) { --d; }   // holiday: the day before
            if (d >= month_start) { out.insert(d); }
        }
    }
    return out;
}

namespace detail {

inline double mean_log_close(const std::vector<da::AuditBar>& v, std::size_t end_inclusive, std::size_t n) {
    double s = 0.0;
    for (std::size_t k = end_inclusive + 1 - n; k <= end_inclusive; ++k) { s += std::log(v[k].c); }
    return s / static_cast<double>(n);
}

inline double clv(const da::AuditBar& b) noexcept {
    return b.h > b.l ? (b.c - b.l) / (b.h - b.l) - 0.5 : 0.0;
}

inline std::map<std::int64_t, std::size_t> index_by_day(const std::vector<da::AuditBar>& v) {
    std::map<std::int64_t, std::size_t> m;
    for (std::size_t i = 0; i < v.size(); ++i) { m.emplace(bar_day(v[i]), i); }
    return m;
}

inline void push_row(CurriculumTrack& tr, const std::vector<double>& f, std::int64_t t, std::int64_t t_out,
                     std::int32_t day, double anchor, double actual, double cost) {
    tr.x.insert(tr.x.end(), f.begin(), f.end());
    tr.t.push_back(t);
    tr.t_out.push_back(t_out);
    tr.day.push_back(day);
    tr.anchor.push_back(anchor);
    tr.actual.push_back(actual);
    if (tr.tradable) { tr.cost_bp.push_back(cost); }
}

inline void finish_info(const CurriculumTrack& tr, TrackInfo& info) {
    info.track = tr.name;
    info.rows = tr.rows();
    info.days = tr.days();
    if (!tr.t.empty()) {
        info.first = da::format_audit_time(tr.t.front(), false);
        info.last = da::format_audit_time(tr.t.back(), false);
    }
}

} // namespace detail

/// What a daily track is built from. Bars are cleaned and daily (t = date).
struct DailyInputs {
    std::string name, instrument;
    const std::vector<da::AuditBar>* own = nullptr;    ///< the instrument (futures: the contract)
    const std::vector<da::AuditBar>* vix = nullptr;    ///< INDIA VIX daily; null for VIX itself
    const std::vector<da::AuditBar>* spot = nullptr;   ///< NIFTY spot, for a futures track
    bool tradable = true;
    double other_cost_bp = 1.3;
};

/// Next-day direction. Features at the close of day i; outcome: day i+1's close.
[[nodiscard]] inline CurriculumTrack build_daily(const DailyInputs& in, TrackInfo& info) {
    CurriculumTrack tr;
    tr.name = in.name;
    tr.instrument = in.instrument;
    tr.horizon = "next day";
    tr.tradable = in.tradable;
    const bool futures = in.spot != nullptr;
    const bool is_vix = in.vix == nullptr;
    // Price features come from the spot index for a futures track (no roll jumps).
    const std::vector<da::AuditBar>& f = futures ? *in.spot : *in.own;
    const std::vector<da::AuditBar>& own = *in.own;
    tr.feature_names = {"ret 1d", "ret 1d lag 1", "ret 1d lag 2", "ret 5d", "ret 20d", "vol 10d",
                        "range", "close in range", "gap", "weekday"};
    if (is_vix) {
        tr.feature_names.insert(tr.feature_names.end(), {"log level", "level vs 20d"});
    } else {
        tr.feature_names.insert(tr.feature_names.end(), {"VIX", "VIX change", "VIX vs 20d"});
    }
    if (futures) { tr.feature_names.insert(tr.feature_names.end(), {"basis", "basis change"}); }
    tr.p = tr.feature_names.size();
    tr.seq_cols = {0, 6, 8, is_vix ? std::size_t{7} : std::size_t{11}};

    const auto fday = detail::index_by_day(f);
    const auto vday = is_vix ? std::map<std::int64_t, std::size_t>{} : detail::index_by_day(*in.vix);
    const auto sday = futures ? detail::index_by_day(*in.spot) : std::map<std::int64_t, std::size_t>{};
    const std::int64_t vix_start = is_vix ? bar_day(own.front()) : bar_day(in.vix->front());
    std::set<std::int64_t> expiry;
    if (futures) {
        expiry = nifty_expiries(*in.spot);
        const std::int64_t first = bar_day(own.front()), last = bar_day(own.back());
        for (const std::int64_t e : expiry) { info.expiries += e >= first && e <= last ? 1 : 0; }
        double on = 0.0, jump = 0.0;
        std::size_t n_on = 0, n_jump = 0;
        for (std::size_t i = 0; i + 1 < own.size(); ++i) {
            const auto a = sday.find(bar_day(own[i]));
            const auto b = sday.find(bar_day(own[i + 1]));
            if (!expiry.contains(bar_day(own[i])) || a == sday.end() || b == sday.end()) { continue; }
            const double b0 = std::log(own[i].c / (*in.spot)[a->second].c);
            const double b1 = std::log(own[i + 1].c / (*in.spot)[b->second].c);
            on += b0; ++n_on;
            jump += b1 - b0; ++n_jump;
        }
        if (n_on > 0) { info.basis_on_expiry_bp = 1e4 * on / static_cast<double>(n_on); }
        if (n_jump > 0) { info.basis_jump_after_bp = 1e4 * jump / static_cast<double>(n_jump); }
    }

    std::int32_t day_ordinal = -1;
    std::vector<double> row(tr.p);
    for (std::size_t i = 0; i + 1 < own.size(); ++i) {
        const std::int64_t d = bar_day(own[i]);
        if (d < vix_start) { ++info.before_vix; continue; }
        const auto fk = fday.find(d);
        if (fk == fday.end()) { ++info.no_spot; continue; }
        const std::size_t k = fk->second;
        if (k < 20) { ++info.warmup; continue; }
        if (futures && expiry.contains(d)) { ++info.roll_excluded; continue; }

        std::size_t c = 0;
        const auto lr = [&f](std::size_t a, std::size_t b) { return std::log(f[a].c / f[b].c); };
        row[c++] = lr(k, k - 1);
        row[c++] = lr(k - 1, k - 2);
        row[c++] = lr(k - 2, k - 3);
        row[c++] = lr(k, k - 5);
        row[c++] = lr(k, k - 20);
        double s = 0.0, ss = 0.0;
        for (std::size_t m = k - 9; m <= k; ++m) { const double r = lr(m, m - 1); s += r; ss += r * r; }
        row[c++] = std::sqrt(std::max(0.0, (ss - s * s / 10.0) / 9.0));
        row[c++] = std::log(f[k].h / f[k].l);
        row[c++] = detail::clv(f[k]);
        row[c++] = std::log(f[k].o / f[k - 1].c);
        row[c++] = static_cast<double>(da::audit_weekday(d));
        if (is_vix) {
            row[c++] = std::log(f[k].c);
            row[c++] = std::log(f[k].c) - detail::mean_log_close(f, k, 20);
        } else {
            const auto vk = vday.find(d);
            if (vk == vday.end() || vk->second < 20) { ++info.no_vix; continue; }
            const auto& v = *in.vix;
            const std::size_t j = vk->second;
            row[c++] = std::log(v[j].c);
            row[c++] = std::log(v[j].c / v[j - 1].c);
            row[c++] = std::log(v[j].c) - detail::mean_log_close(v, j, 20);
        }
        if (futures) {
            if (i == 0) { ++info.warmup; continue; }
            const auto prev = sday.find(bar_day(own[i - 1]));
            const double basis = std::log(own[i].c / f[k].c);
            row[c++] = basis;
            if (prev == sday.end()) { ++info.no_spot; continue; }
            // The first day of a new contract: yesterday's basis was the old one's.
            row[c++] = expiry.contains(bar_day(own[i - 1]))
                           ? 0.0 : basis - std::log(own[i - 1].c / f[prev->second].c);
        }
        const std::int64_t next = bar_day(own[i + 1]);
        detail::push_row(tr, row, d * kDaySec + kCloseSec, next * kDaySec + kCloseSec, ++day_ordinal,
                         own[i].c, own[i + 1].c, futures_cost_bp(d, in.other_cost_bp));
    }
    detail::finish_info(tr, info);
    return tr;
}

/// When an hourly bar closes. The session's last bar starts at 15:15 and is
/// only fifteen minutes long: it closes with the session at 15:30.
[[nodiscard]] constexpr std::int64_t hourly_close(std::int64_t stamp) noexcept {
    const std::int64_t close = da::audit_day(stamp) * kDaySec + kCloseSec;
    return stamp + 3600 > close ? close : stamp + 3600;   // RULE 11: proven -- the 15:15 bar is the only one past 15:30
}

/// What an hourly track is built from. Bars are cleaned 60-minute bars.
struct HourlyInputs {
    std::string name, instrument;
    const std::vector<da::AuditBar>* own = nullptr;
    const std::vector<da::AuditBar>* vix = nullptr;   ///< INDIA VIX 60m; null for VIX itself
    bool tradable = true;
    double other_cost_bp = 1.3;
};

/// Next-hour direction. A decision at the close of each of a full day's
/// first six hourly bars (10:15 ... 15:15); outcome: the next bar's close.
/// The 15:15 bar's close (15:30) is only ever an outcome: forecasting
/// across the night is a different horizon.
[[nodiscard]] inline CurriculumTrack build_hourly(const HourlyInputs& in, TrackInfo& info) {
    CurriculumTrack tr;
    tr.name = in.name;
    tr.instrument = in.instrument;
    tr.horizon = "next hour";
    tr.tradable = in.tradable;
    const bool is_vix = in.vix == nullptr;
    tr.feature_names = {"ret bar", "ret prev bar", "ret since open", "gap", "range", "close in range",
                        "hour", "prev day ret", "prev day range"};
    if (is_vix) {
        tr.feature_names.insert(tr.feature_names.end(), {"log level", "ret since prev close"});
    } else {
        tr.feature_names.insert(tr.feature_names.end(), {"VIX", "VIX ret bar", "VIX since prev close"});
    }
    tr.p = tr.feature_names.size();
    tr.seq_cols = {0, 4, 5, is_vix ? std::size_t{3} : std::size_t{10}};

    const auto& bars = *in.own;
    // Days, in order: [begin, end) into bars.
    struct DaySpan { std::int64_t day; std::size_t b, e; };
    std::vector<DaySpan> days;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const std::int64_t d = bar_day(bars[i]);
        if (days.empty() || days.back().day != d) { days.push_back({d, i, i}); }
        days.back().e = i + 1;
    }
    const auto full = [&bars](const DaySpan& s) {
        if (s.e - s.b != 7) { return false; }
        for (std::size_t k = 0; k < 7; ++k) {
            if (da::audit_minute_of_day(bars[s.b + k].t) != 555 + 60 * static_cast<std::int64_t>(k)) { return false; }
        }
        return true;
    };
    // VIX by stamp, and VIX's last close by day.
    std::map<std::int64_t, double> vix_at;
    std::map<std::int64_t, double> vix_close;
    std::int64_t vix_start = 0;
    if (!is_vix) {
        for (const auto& b : *in.vix) { vix_at[b.t] = b.c; vix_close[bar_day(b)] = b.c; }
        vix_start = in.vix->empty() ? 0 : bar_day(in.vix->front());
    }

    std::int32_t day_ordinal = -1;
    std::vector<double> row(tr.p);
    for (std::size_t q = 0; q < days.size(); ++q) {
        const DaySpan& s = days[q];
        if (!is_vix && s.day < vix_start) { info.before_vix += s.e - s.b; continue; }
        if (!full(s)) { ++info.short_days; continue; }
        if (q < 2) { info.warmup += s.e - s.b; continue; }
        const DaySpan& p1 = days[q - 1];
        const DaySpan& p2 = days[q - 2];
        // Yesterday's last bar and the bar before it. After a one-bar session
        // (Muhurat) that earlier bar belongs to the day before -- still the
        // previous bar in time.
        if (p1.e < 2) { info.warmup += s.e - s.b; continue; }
        const double pc = bars[p1.e - 1].c;
        const double ppc = bars[p2.e - 1].c;
        double ph = 0.0, pl = std::numeric_limits<double>::infinity();
        for (std::size_t k = p1.b; k < p1.e; ++k) { ph = std::max(ph, bars[k].h); pl = std::min(pl, bars[k].l); }
        const double open = bars[s.b].o;
        const auto vix_prev_close = is_vix ? vix_close.end() : vix_close.find(p1.day);
        bool counted_day = false;
        for (std::size_t j = 0; j < 6; ++j) {
            const da::AuditBar& b = bars[s.b + j];
            // The closes before this bar: today's, then yesterday's last two.
            const double prev_c = j > 0 ? bars[s.b + j - 1].c : pc;
            const double prev2_c = j > 1 ? bars[s.b + j - 2].c : (j == 1 ? pc : bars[p1.e - 2].c);
            std::size_t c = 0;
            row[c++] = std::log(b.c / prev_c);
            row[c++] = std::log(prev_c / prev2_c);
            row[c++] = std::log(b.c / open);
            row[c++] = std::log(open / pc);
            row[c++] = std::log(b.h / b.l);
            row[c++] = detail::clv(b);
            row[c++] = static_cast<double>(j);
            row[c++] = std::log(pc / ppc);
            row[c++] = std::log(ph / pl);
            if (is_vix) {
                row[c++] = std::log(b.c);
                row[c++] = std::log(b.c / pc);
            } else {
                const auto v = vix_at.find(b.t);
                const auto vp = j > 0 ? vix_at.find(bars[s.b + j - 1].t) : vix_at.end();
                const double vprev = j > 0 ? (vp != vix_at.end() ? vp->second : 0.0)
                                           : (vix_prev_close != vix_close.end() ? vix_prev_close->second : 0.0);
                if (v == vix_at.end() || !(vprev > 0.0) || vix_prev_close == vix_close.end()) {
                    ++info.no_vix;
                    continue;
                }
                row[c++] = std::log(v->second);
                row[c++] = std::log(v->second / vprev);
                row[c++] = std::log(v->second / vix_prev_close->second);
            }
            if (!counted_day) { ++day_ordinal; counted_day = true; }
            const std::int64_t close_j = hourly_close(b.t);
            const std::int64_t close_next = hourly_close(bars[s.b + j + 1].t);
            detail::push_row(tr, row, close_j, close_next, day_ordinal, b.c, bars[s.b + j + 1].c,
                             futures_cost_bp(s.day, in.other_cost_bp));
        }
    }
    detail::finish_info(tr, info);
    return tr;
}

} // namespace altair::forecast_tracks
