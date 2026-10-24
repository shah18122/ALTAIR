// app/ivrv_feed.hpp -- implied and realised volatility as forecast inputs.
//
// WHAT. For each index, three things a volatility desk watches:
//   * REALISED volatility: each session's realised variance from its
//     5-minute bars (squared 5-minute returns plus the overnight gap,
//     analytics/har_rv.hpp), its 5- and 22-session averages, the HAR forecast
//     of the next session's, and how far today has run so far;
//   * IMPLIED volatility: INDIA VIX for NIFTY (NSE's 30-day IV of NIFTY
//     options); the 30-day at-the-money IV of the index's monthly options from
//     the NSE F&O bhavcopy (NIFTY and BANKNIFTY, app/bhavcopy.hpp); and the
//     at-the-money IV the terminal records from the FYERS chain, minute by
//     minute (altair_iv_record);
//   * the SPREAD between them, implied over realised (the variance risk
//     premium, Carr & Wu 2009): what option sellers are paid, which widens
//     with fear and narrows when the market is calm.
// All are logs of annualised vols, so a spread is a log ratio and 0 means
// implied equals realised.
//
// WHEN EACH VALUE IS KNOWN -- THE WHOLE LOOK-AHEAD ARGUMENT. Each value is
// stamped with the first moment it could be read, and the curriculum joins it
// as of each decision (models/curriculum_feeds.hpp), so nothing stamped after
// a decision reaches it:
//   * a session's realised variance and its HAR forecast: the 15:30 close;
//   * realised variance so far today: the close of each 5-minute bar;
//   * INDIA VIX's daily close: 15:30;
//   * bhavcopy IV: NSE publishes the file in the evening, so it is stamped
//     18:00 -- a decision at 15:30 reads the previous session's;
//   * FYERS IV: the end of each recorded minute.
// The leak check (app/forecast_leak_check.hpp) rebuilds these inputs from only
// what was known at its cut, and from a different future after it, and needs
// every row decided before the cut to come out the same.

#pragma once

#include <analytics/har_rv.hpp>
#include <analytics/iv.hpp>
#include <app/bhavcopy.hpp>
#include <app/data_audit.hpp>
#include <models/curriculum_feeds.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace altair::ivrv {

namespace da = altair::data_audit;
namespace fs = std::filesystem;
using Bars = std::vector<da::AuditBar>;

inline constexpr std::int64_t kDaySec = 86'400;
inline constexpr std::int64_t kCloseSec = 15 * 3600 + 30 * 60;
/// NSE puts the day's F&O bhavcopy out in the evening.
inline constexpr std::int64_t kBhavKnownSec = 18 * 3600;
/// Five-minute bars in a full session, 09:15 to 15:30.
inline constexpr double kSessionBars = 75.0;
inline constexpr double kTradingDays = 252.0;

[[nodiscard]] inline double nan() noexcept { return std::numeric_limits<double>::quiet_NaN(); }

/// log of the annualised vol of a per-session variance.
[[nodiscard]] inline double log_vol(double variance) noexcept {
    return variance > 0.0 ? 0.5 * std::log(variance * kTradingDays) : nan();
}

/// When a five-minute bar's close is known: its end, the session's last at 15:30.
[[nodiscard]] inline std::int64_t bar_end(const da::AuditBar& b) noexcept {
    const std::int64_t close = da::audit_day(b.t) * kDaySec + kCloseSec;
    const std::int64_t end = b.t + 300;
    return b.t < close && end > close ? close : end;   // RULE 11: proven -- only the session's last bar runs past 15:30
}

/// One implied-vol reading, stamped when it could first be read.
struct IvPoint {
    std::int64_t t = 0;      ///< known at
    double iv = nan();       ///< annualised, e.g. 0.14
    double iv_next = nan();  ///< the next expiry's, for the term slope (bhavcopy only)
};
using IvSeries = std::vector<IvPoint>;

// ---- realised ----------------------------------------------------------------------

/// Each session's realised variance from five-minute bars: the squared
/// five-minute returns plus the overnight gap. A session with no movement at
/// all (a data glitch) is left out: its log would be minus infinity.
struct SessionRv {
    std::vector<std::int64_t> day;
    std::vector<double> rv;
};

[[nodiscard]] inline SessionRv session_rv(const Bars& five) {
    SessionRv out;
    std::size_t i = 0;
    double prev_close = 0.0;
    std::vector<double> closes;
    while (i < five.size()) {
        const std::int64_t d = da::audit_day(five[i].t);
        const double first_open = five[i].o;
        closes.clear();
        std::size_t j = i;
        for (; j < five.size() && da::audit_day(five[j].t) == d; ++j) { closes.push_back(five[j].c); }
        const double v = altair::session_rv(closes, first_open, prev_close);
        if (v > 0.0 && std::isfinite(v)) {
            out.day.push_back(d);
            out.rv.push_back(v);
        }
        prev_close = closes.back();
        i = j;
    }
    return out;
}

/// Mean of the last `n` sessions' variance ending at `k`, or NaN before `n`.
[[nodiscard]] inline double mean_rv(const SessionRv& s, std::size_t k, std::size_t n) noexcept {
    if (k + 1 < n) { return nan(); }
    double sum = 0.0;
    for (std::size_t j = k + 1 - n; j <= k; ++j) { sum += s.rv[j]; }
    return sum / static_cast<double>(n);
}

/// Daily realised-vol inputs, stamped at each session's 15:30 close: the
/// session's realised vol, its 5- and 22-session averages and the HAR
/// forecast of the next session's; with INDIA VIX's daily bars, also VIX over
/// the 22-session realised vol and over the HAR forecast.
[[nodiscard]] inline ModelFeed rv_daily_feed(const std::string& source, const SessionRv& s, const Bars* vix_daily) {
    ModelFeed f;
    f.source = source;
    f.names = {"RV session", "RV 5d", "RV 22d", "HAR next session"};
    if (vix_daily != nullptr) { f.names.insert(f.names.end(), {"VIX vs RV 22d", "VIX vs HAR"}); }
    f.intraday = false;
    f.required = true;
    std::map<std::int64_t, double> vix;
    if (vix_daily != nullptr) {
        for (const auto& b : *vix_daily) {
            if (b.c > 0.0) { vix[da::audit_day(b.t)] = std::log(b.c / 100.0); }
        }
    }
    const std::span<const double> rv(s.rv);
    for (std::size_t k = 0; k < s.day.size(); ++k) {
        const double r22 = mean_rv(s, k, 22);
        if (!std::isfinite(r22)) { continue; }
        const auto har = altair::har_forecast(rv, k, 1);
        if (!har) { continue; }
        const double l22 = log_vol(r22), lhar = log_vol(har->variance);
        std::vector<double> row{log_vol(s.rv[k]), log_vol(mean_rv(s, k, 5)), l22, lhar};
        if (vix_daily != nullptr) {
            const auto v = vix.find(s.day[k]);
            if (v == vix.end()) { continue; }
            row.push_back(v->second - l22);
            row.push_back(v->second - lhar);
        }
        f.t.push_back(s.day[k] * kDaySec + kCloseSec);
        for (const double x : row) { f.v.push_back(static_cast<float>(x)); }
    }
    return f;
}

/// Intraday realised-vol inputs, stamped at each five-minute bar's close:
/// today's realised vol so far (gap included), scaled to a whole session, and
/// the same against the last 22 sessions'. Only the same day's value is used.
[[nodiscard]] inline ModelFeed rv_intraday_feed(const std::string& source, const Bars& five, const SessionRv& s) {
    ModelFeed f;
    f.source = source;
    f.names = {"RV today so far", "RV today vs 22d"};
    f.intraday = true;
    f.required = true;
    std::map<std::int64_t, std::size_t> at;   // day -> index in s
    for (std::size_t k = 0; k < s.day.size(); ++k) { at[s.day[k]] = k; }
    double prev_close = 0.0, sum = 0.0, last = 0.0;
    std::int64_t day = std::numeric_limits<std::int64_t>::min();
    std::size_t bars = 0;
    for (const auto& b : five) {
        const std::int64_t d = da::audit_day(b.t);
        if (d != day) {
            prev_close = last;
            day = d;
            sum = 0.0;
            bars = 0;
            if (prev_close > 0.0 && b.o > 0.0) { const double g = std::log(b.o / prev_close); sum += g * g; }
            last = b.o;
        }
        if (last > 0.0 && b.c > 0.0) { const double r = std::log(b.c / last); sum += r * r; }
        last = b.c;
        ++bars;
        // The 22 sessions before today: all closed before this bar.
        const auto it = at.lower_bound(d);
        if (it == at.begin()) { continue; }
        const std::size_t before = std::prev(it)->second;
        const double r22 = mean_rv(s, before, 22);
        const double so_far = sum * kSessionBars / static_cast<double>(bars);
        if (!std::isfinite(r22) || !(so_far > 0.0)) { continue; }
        const double lv = log_vol(so_far);
        f.t.push_back(bar_end(b));
        f.v.push_back(static_cast<float>(lv));
        f.v.push_back(static_cast<float>(lv - log_vol(r22)));
    }
    return f;
}

// ---- implied: NSE bhavcopy ------------------------------------------------------

/// One expiry's at-the-money implied vol on a bhavcopy day: the strike nearest
/// the expiry's own future with both a call and a put traded, Black-76 on that
/// future, the two IVs averaged (NaN if neither solves).
[[nodiscard]] inline double atm_iv(const bhavcopy::Day& d, std::int64_t day, std::int64_t expiry, double rate) {
    const auto fut = d.fut.find(expiry);
    const auto opt = d.opt.find(expiry);
    if (fut == d.fut.end() || opt == d.opt.end() || !(fut->second > 0.0) || expiry <= day) { return nan(); }
    const double F = fut->second;
    double best = std::numeric_limits<double>::infinity(), K = 0.0;
    bhavcopy::Leg leg;
    for (const auto& [k, l] : opt->second) {
        if (!(l.call > 0.0) || !(l.put > 0.0)) { continue; }
        if (std::fabs(k - F) < best) { best = std::fabs(k - F); K = k; leg = l; }
    }
    if (!(K > 0.0)) { return nan(); }
    const double T = static_cast<double>(expiry - day) / 365.0;
    double sum = 0.0;
    int n = 0;
    for (const bool call : {true, false}) {
        const auto v = altair::implied_vol_black76(call ? altair::OptionRight::Call : altair::OptionRight::Put,
                                                   altair::Price{std::llround((call ? leg.call : leg.put) * 100.0)},
                                                   altair::Price{std::llround(F * 100.0)}, altair::Price{std::llround(K * 100.0)},
                                                   altair::Years{T}, rate);
        if (v && std::isfinite(v->vol.raw()) && v->vol.raw() > 0.0) { sum += v->vol.raw(); ++n; }
    }
    return n > 0 ? sum / n : nan();
}

/// The 30-day at-the-money implied vol of each bhavcopy day, from the two
/// monthly expiries either side of 30 days (total variance interpolated in
/// time; the nearest one alone when 30 days is outside them). Expiries within
/// 5 days are skipped: an expiring option's IV is mostly noise. Stamped 18:00.
[[nodiscard]] inline IvSeries iv_from_bhavcopy(const std::map<std::int64_t, bhavcopy::Day>& days, double rate = 0.065) {
    IvSeries out;
    constexpr double kTarget = 30.0;
    for (const auto& [day, d] : days) {
        std::vector<std::pair<double, double>> curve;   // (days to expiry, iv)
        for (const auto& [e, unused] : d.opt) {
            (void)unused;
            const double dte = static_cast<double>(e - day);
            if (dte < 5.0) { continue; }
            const double v = atm_iv(d, day, e, rate);
            if (std::isfinite(v)) { curve.emplace_back(dte, v); }
        }
        if (curve.empty()) { continue; }
        std::sort(curve.begin(), curve.end());
        double iv = curve.front().second;
        for (std::size_t k = 0; k + 1 < curve.size(); ++k) {
            const auto [t1, v1] = curve[k];
            const auto [t2, v2] = curve[k + 1];
            if (t1 <= kTarget && kTarget <= t2) {
                const double w = (kTarget - t1) / (t2 - t1);
                const double var = (1.0 - w) * v1 * v1 * t1 + w * v2 * v2 * t2;
                iv = std::sqrt(var / kTarget);
                break;
            }
            if (kTarget > t2) { iv = v2; }
        }
        IvPoint p;
        p.t = day * kDaySec + kBhavKnownSec;
        p.iv = iv;
        p.iv_next = curve.size() > 1 ? curve[1].second : nan();
        out.push_back(p);
    }
    return out;
}

/// Daily implied-vol inputs from a stamped series: the IV, its change since
/// the previous reading, implied over realised (22 sessions and HAR), and the
/// term slope (the next expiry's IV over the near one's).
[[nodiscard]] inline ModelFeed iv_daily_feed(const std::string& source, const IvSeries& iv, const SessionRv& s) {
    ModelFeed f;
    f.source = source;
    f.names = {"IV 30d", "IV change", "IV vs RV 22d", "IV vs HAR", "IV term slope"};
    f.intraday = false;
    f.required = true;
    const std::span<const double> rv(s.rv);
    double prev = nan();
    for (const auto& p : iv) {
        if (!(p.iv > 0.0)) { continue; }
        const double l = std::log(p.iv);
        // Realised up to the last session closed by p.t.
        const auto it = std::upper_bound(s.day.begin(), s.day.end(), (p.t - kCloseSec) / kDaySec);
        if (it == s.day.begin()) { prev = l; continue; }
        const auto k = static_cast<std::size_t>(it - s.day.begin()) - 1;
        const double r22 = mean_rv(s, k, 22);
        const auto har = altair::har_forecast(rv, k, 1);
        if (!std::isfinite(r22) || !har) { prev = l; continue; }
        const double slope = p.iv_next > 0.0 ? std::log(p.iv_next / p.iv) : 0.0;
        f.t.push_back(p.t);
        for (const double x : {l, std::isfinite(prev) ? l - prev : 0.0, l - log_vol(r22), l - log_vol(har->variance), slope}) {
            f.v.push_back(static_cast<float>(x));
        }
        prev = l;
    }
    return f;
}

// ---- implied: the FYERS chain, recorded -----------------------------------------

/// Every `<dir>/<SYMBOL>/*.csv` (time,iv -- written by altair_iv_record from
/// the terminal's tapes), in time order. Missing: an empty series.
[[nodiscard]] inline IvSeries load_recorded_iv(const fs::path& dir, const std::string& symbol) {
    IvSeries out;
    std::error_code ec;
    const fs::path base = dir / symbol;
    if (!fs::is_directory(base, ec)) { return out; }
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(base, ec)) {
        if (e.path().extension() == ".csv") { files.push_back(e.path()); }
    }
    std::sort(files.begin(), files.end());
    for (const auto& p : files) {
        std::ifstream in(p);
        std::string line;
        std::getline(in, line);   // header
        while (std::getline(in, line)) {
            const auto comma = line.find(',');
            if (comma == std::string::npos) { continue; }
            const auto t = da::parse_audit_time(line.substr(0, comma));
            const double v = std::atof(line.c_str() + comma + 1);
            if (t && v > 0.0 && v < 5.0) { out.push_back({*t, v, nan()}); }
        }
    }
    std::sort(out.begin(), out.end(), [](const IvPoint& a, const IvPoint& b) { return a.t < b.t; });
    return out;
}

/// Intraday implied-vol inputs from recorded minutes: the IV now and its
/// spread over the 22 sessions' realised vol. Only the same day's value is used.
[[nodiscard]] inline ModelFeed iv_intraday_feed(const std::string& source, const IvSeries& iv, const SessionRv& s) {
    ModelFeed f;
    f.source = source;
    f.names = {"IV now", "IV now vs RV 22d"};
    f.intraday = true;
    f.required = false;
    for (const auto& p : iv) {
        const std::int64_t day = p.t / kDaySec;
        const auto it = std::lower_bound(s.day.begin(), s.day.end(), day);   // sessions before today
        if (it == s.day.begin() || !(p.iv > 0.0)) { continue; }
        const double r22 = mean_rv(s, static_cast<std::size_t>(it - s.day.begin()) - 1, 22);
        if (!std::isfinite(r22)) { continue; }
        if (!f.t.empty() && p.t <= f.t.back()) { continue; }   // strictly ascending stamps
        f.t.push_back(p.t);
        f.v.push_back(static_cast<float>(std::log(p.iv)));
        f.v.push_back(static_cast<float>(std::log(p.iv) - log_vol(r22)));
    }
    return f;
}

/// How many distinct sessions a recorded series covers.
[[nodiscard]] inline std::size_t sessions_covered(const IvSeries& iv) {
    std::size_t n = 0;
    std::int64_t last = std::numeric_limits<std::int64_t>::min();
    for (const auto& p : iv) {
        const std::int64_t d = p.t / kDaySec;
        if (d != last) { ++n; last = d; }
    }
    return n;
}

} // namespace altair::ivrv
