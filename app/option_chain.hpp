// app/option_chain.hpp -- one trading day of an index option chain, and the
// small CSV helpers the FYERS chain loader (app/fyers_chain.hpp) uses.
//
// Source policy (docs/DATA_POLICY.md): option prices come from FYERS only,
// the saved FYERS chains or the live FYERS feed. Exchange end-of-day files
// are banned and tools/check_data_policy.sh fails the build on any loader.

#pragma once

#include <app/data_audit.hpp>

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace altair::chain {

struct Leg {
    double call = std::numeric_limits<double>::quiet_NaN();
    double put = std::numeric_limits<double>::quiet_NaN();
};

struct Day {
    std::map<std::int64_t, double> fut;                      ///< expiry -> future's close
    std::map<std::int64_t, std::map<double, Leg>> opt;       ///< expiry -> strike -> traded closes
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

} // namespace detail

} // namespace altair::chain
