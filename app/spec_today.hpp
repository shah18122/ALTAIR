// app/spec_today.hpp -- today's contract specs, read from the repo's own files.
//
// The research CLIs size one lot by TODAY's contract and step strikes by
// TODAY's gap, and say so: earlier lot sizes are not transcribed, and a
// literal would be a guess.

#pragma once

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace altair::spec_today {

namespace fs = std::filesystem;

namespace detail {
inline bool parse_double(std::string_view s, double& out) {
    const std::string v{s};
    char* end = nullptr;
    out = std::strtod(v.c_str(), &end);
    return end != v.c_str() && *end == '\0' && std::isfinite(out);
}
} // namespace detail

/// Today's verified lot size from config/lot_size_history.csv: the OPEN row
/// whose `verified` column carries a verification date ("NO" otherwise).
/// History is not transcribed there, and is not guessed here.
[[nodiscard]] inline std::optional<double> lot_size(const fs::path& file, const std::string& symbol) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') { continue; }
        std::vector<std::string> f;
        std::stringstream ss(line);
        for (std::string c; std::getline(ss, c, ',');) { f.push_back(c); }
        if (f.size() >= 7 && f[0] == symbol && f[2] == "OPEN" && f[6].size() == 10 && f[6][4] == '-') {
            double v = 0.0;
            if (detail::parse_double(f[3], v) && v > 0.0) { return v; }
        }
    }
    return std::nullopt;
}

/// Today's strike step from the instrument master: the commonest gap between
/// adjacent strikes of the nearest expiry's options on `name`.
[[nodiscard]] inline std::optional<double> strike_step(const fs::path& master, const std::string& name) {
    std::ifstream in(master);
    std::string line;
    std::map<std::string, std::set<double>> by_expiry;
    std::getline(in, line);   // header
    while (std::getline(in, line)) {
        // instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,strike,tick_size,lot_size,instrument_type,segment,exchange
        std::vector<std::string> f;
        std::string cur;
        bool quoted = false;
        for (const char c : line) {
            if (c == '"') { quoted = !quoted; continue; }
            if (c == ',' && !quoted) { f.push_back(cur); cur.clear(); continue; }
            cur += c;
        }
        f.push_back(cur);
        if (f.size() < 12 || f[3] != name || f[10] != "NFO-OPT") { continue; }
        double k = 0.0;
        if (detail::parse_double(f[6], k) && k > 0.0) { by_expiry[f[5]].insert(k); }
    }
    if (by_expiry.empty()) { return std::nullopt; }
    const auto& strikes = by_expiry.begin()->second;   // ISO dates sort: the nearest first
    std::map<double, int> gaps;
    double prev = -1.0;
    for (const double k : strikes) {
        if (prev > 0.0) { ++gaps[std::round((k - prev) * 100.0) / 100.0]; }
        prev = k;
    }
    double best = 0.0;
    int count = 0;
    for (const auto& [g, c] : gaps) { if (c > count) { best = g; count = c; } }
    return best > 0.0 ? std::optional<double>{best} : std::nullopt;
}

} // namespace altair::spec_today
