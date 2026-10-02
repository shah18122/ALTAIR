// app/bhavcopy.hpp -- NSE F&O bhavcopy: real end-of-day option and futures prices.
//
// TWO FORMATS, ONE LOADER. NSE published the F&O bhavcopy as
//   fo<DD><MON><YYYY>bhav.csv               through 2024-07-05, columns
//   INSTRUMENT,SYMBOL,EXPIRY_DT,STRIKE_PR,OPTION_TYP,OPEN,HIGH,LOW,CLOSE,
//   SETTLE_PR,CONTRACTS,VAL_INLAKH,OPEN_INT,CHG_IN_OI,TIMESTAMP
// and from 2024-07-08 as the UDiFF common bhavcopy
//   BhavCopy_NSE_FO_0_0_0_<YYYYMMDD>_F_0000.csv, with ISO-tag columns
//   TradDt,...,FinInstrmTp,...,TckrSymb,...,XpryDt,...,StrkPric,OptnTp,...,
//   ClsPric,...,SttlmPric,...,OpnIntrst,...,TtlTradgVol,...
// Columns are found by NAME, never by position, so an added or reordered
// column cannot shift a price into the wrong field. A file with neither
// header is counted and skipped.
//
// WHAT IS KEPT. One symbol's index futures and index options. Monthly
// expiries only -- the last listed expiry of each calendar month, as of each
// day -- and option strikes within +-20 % of that expiry's future. An option
// that did not trade that day has NO price here: a settlement price on an
// untraded strike is NSE's model, not a market, and a backtest that sells
// into it is selling to nobody.
//
// The files are broker-independent exchange data, but they are bulky and
// fetched per user: they live in data/bhavcopy/ (git-ignored). Fetch them
// with ops/fetch_bhavcopy.ps1.

#pragma once

#include <app/data_audit.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace altair::bhavcopy {

namespace fs = std::filesystem;

struct Leg {
    double call = std::numeric_limits<double>::quiet_NaN();
    double put = std::numeric_limits<double>::quiet_NaN();
};

struct Day {
    std::map<std::int64_t, double> fut;                      ///< expiry -> future's close
    std::map<std::int64_t, std::map<double, Leg>> opt;       ///< expiry -> strike -> traded closes
};

struct LoadReport {
    std::size_t files = 0, legacy = 0, udiff = 0, unknown = 0, unreadable = 0;
    std::size_t option_rows = 0, untraded = 0, future_rows = 0;
};

namespace detail {

[[nodiscard]] inline std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (const char c : line) {
        if (c == '"') { quoted = !quoted; continue; }
        if (c == ',' && !quoted) { out.push_back(cur); cur.clear(); continue; }
        if (c != '\r') { cur += c; }
    }
    out.push_back(cur);
    for (auto& f : out) {
        const auto b = f.find_first_not_of(' ');
        const auto e = f.find_last_not_of(' ');
        f = b == std::string::npos ? std::string{} : f.substr(b, e - b + 1);
    }
    return out;
}

[[nodiscard]] inline std::optional<double> number(const std::string& s) {
    if (s.empty()) { return std::nullopt; }
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) { return std::nullopt; }
    return v;
}

/// "2024-07-25", "25-Jul-2024" or "25-JUL-2024" -> days since 1970-01-01.
[[nodiscard]] inline std::optional<std::int64_t> date(const std::string& s) {
    const auto to_int = [](std::string_view v) -> std::optional<int> {
        int n = 0;
        if (v.empty()) { return std::nullopt; }
        for (const char c : v) {
            if (c < '0' || c > '9') { return std::nullopt; }
            n = n * 10 + (c - '0');
        }
        return n;
    };
    if (s.size() == 10 && s[4] == '-' && s[7] == '-') {
        const auto y = to_int(std::string_view{s}.substr(0, 4)), m = to_int(std::string_view{s}.substr(5, 2)),
                   d = to_int(std::string_view{s}.substr(8, 2));
        if (!y || !m || !d || *m < 1 || *m > 12 || *d < 1 || *d > 31) { return std::nullopt; }
        return data_audit::audit_days_from_civil(*y, static_cast<unsigned>(*m), static_cast<unsigned>(*d));
    }
    if (s.size() == 11 && s[2] == '-' && s[6] == '-') {
        static constexpr std::string_view kMon[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                                    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
        std::string mon = s.substr(3, 3);
        for (auto& c : mon) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
        unsigned m = 0;
        for (unsigned k = 0; k < 12; ++k) { if (mon == kMon[k]) { m = k + 1; } }
        const auto d = to_int(std::string_view{s}.substr(0, 2)), y = to_int(std::string_view{s}.substr(7, 4));
        if (m == 0 || !d || !y || *d < 1 || *d > 31) { return std::nullopt; }
        return data_audit::audit_days_from_civil(*y, m, static_cast<unsigned>(*d));
    }
    return std::nullopt;
}

struct Raw {
    std::int64_t day = 0, expiry = 0;
    bool future = false, call = false;
    double strike = 0.0, close = 0.0, settle = 0.0, traded = 0.0;
};

} // namespace detail

/// Read one bhavcopy file's rows for `symbol` into `days`. Returns false when
/// the file has neither format's header.
inline bool load_file(const fs::path& file, const std::string& symbol, std::map<std::int64_t, Day>& days,
                      LoadReport& rep) {
    std::ifstream in(file);
    if (!in) { ++rep.unreadable; return false; }
    std::string line;
    if (!std::getline(in, line)) { ++rep.unreadable; return false; }
    const auto head = detail::split(line);
    const auto col = [&](std::string_view name) -> int {
        for (std::size_t k = 0; k < head.size(); ++k) { if (head[k] == name) { return static_cast<int>(k); } }
        return -1;
    };
    const bool udiff = col("TckrSymb") >= 0;
    const bool legacy = !udiff && col("SYMBOL") >= 0 && col("INSTRUMENT") >= 0;
    if (!udiff && !legacy) { ++rep.unknown; return false; }
    ++(udiff ? rep.udiff : rep.legacy);
    const int c_type = udiff ? col("FinInstrmTp") : col("INSTRUMENT");
    const int c_sym = udiff ? col("TckrSymb") : col("SYMBOL");
    const int c_exp = udiff ? col("XpryDt") : col("EXPIRY_DT");
    const int c_strike = udiff ? col("StrkPric") : col("STRIKE_PR");
    const int c_opt = udiff ? col("OptnTp") : col("OPTION_TYP");
    const int c_close = udiff ? col("ClsPric") : col("CLOSE");
    const int c_settle = udiff ? col("SttlmPric") : col("SETTLE_PR");
    const int c_traded = udiff ? col("TtlTradgVol") : col("CONTRACTS");
    const int c_day = udiff ? col("TradDt") : col("TIMESTAMP");
    for (const int c : {c_type, c_sym, c_exp, c_strike, c_opt, c_close, c_settle, c_traded, c_day}) {
        if (c < 0) { ++rep.unknown; return false; }
    }
    const std::string fut_code = udiff ? "IDF" : "FUTIDX";
    const std::string opt_code = udiff ? "IDO" : "OPTIDX";

    std::vector<detail::Raw> raw;
    while (std::getline(in, line)) {
        const auto f = detail::split(line);
        const auto at = [&](int c) -> const std::string& {
            static const std::string empty;
            return c < static_cast<int>(f.size()) ? f[static_cast<std::size_t>(c)] : empty;
        };
        if (at(c_sym) != symbol) { continue; }
        const std::string& type = at(c_type);
        const bool fut = type == fut_code, opt = type == opt_code;
        if (!fut && !opt) { continue; }
        const auto day = detail::date(at(c_day)), expiry = detail::date(at(c_exp));
        const auto close = detail::number(at(c_close)), settle = detail::number(at(c_settle));
        const auto traded = detail::number(at(c_traded));
        if (!day || !expiry || !close || !settle || !traded) { continue; }
        detail::Raw r;
        r.day = *day;
        r.expiry = *expiry;
        r.future = fut;
        r.close = *close;
        r.settle = *settle;
        r.traded = *traded;
        if (opt) {
            const auto k = detail::number(at(c_strike));
            const std::string& t = at(c_opt);
            if (!k || !(*k > 0.0) || (t != "CE" && t != "PE")) { continue; }
            r.strike = *k;
            r.call = t == "CE";
        }
        raw.push_back(r);
    }
    // Monthly expiries as of each day: the last listed expiry of each month.
    std::map<std::int64_t, std::map<std::pair<std::int64_t, unsigned>, std::int64_t>> last_of_month;
    for (const auto& r : raw) {
        const auto c = data_audit::audit_civil_from_days(r.expiry);
        auto& slot = last_of_month[r.day][{c.y, c.m}];
        slot = std::max(slot, r.expiry);
    }
    std::map<std::int64_t, std::set<std::int64_t>> monthly;
    for (const auto& [d, months] : last_of_month) {
        for (const auto& [ym, e] : months) { monthly[d].insert(e); }
    }
    for (const auto& r : raw) {
        if (!monthly[r.day].contains(r.expiry) || !r.future) { continue; }
        days[r.day].fut[r.expiry] = r.traded > 0.0 && r.close > 0.0 ? r.close : r.settle;
        ++rep.future_rows;
    }
    for (const auto& r : raw) {
        if (r.future || !monthly[r.day].contains(r.expiry)) { continue; }
        auto& d = days[r.day];
        const auto fut = d.fut.find(r.expiry);
        if (fut == d.fut.end() || std::fabs(r.strike / fut->second - 1.0) > 0.20) { continue; }
        if (!(r.traded > 0.0) || !(r.close > 0.0)) { ++rep.untraded; continue; }   // no market, no price
        Leg& leg = d.opt[r.expiry][r.strike];
        (r.call ? leg.call : leg.put) = r.close;
        ++rep.option_rows;
    }
    ++rep.files;
    return true;
}

/// Every .csv under `dir`, recursively, for `symbol` (e.g. "NIFTY").
[[nodiscard]] inline std::map<std::int64_t, Day> load_dir(const fs::path& dir, const std::string& symbol, LoadReport& rep) {
    std::map<std::int64_t, Day> days;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) { return days; }
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            std::string ext = it->path().extension().string();
            for (auto& c : ext) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
            if (ext == ".csv") { files.push_back(it->path()); }
        }
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) { (void)load_file(f, symbol, days, rep); }
    return days;
}

} // namespace altair::bhavcopy
