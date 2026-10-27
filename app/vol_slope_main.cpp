// app/vol_slope_main.cpp -- altair_vol_slope.
//
// Nations' volatility slope (Options Math for Traders, Ch. 9) on our own data:
// how far INDIA VIX moves, in vol points, for each 1% bucket of the day's
// index move, and the two through-origin betas (analytics/option_math.hpp,
// vol_slope) that the stress tests can use instead of a guessed number.
//
//   altair_vol_slope --spot FILE --vix FILE [--from YYYY-MM-DD]
//
// Both files are daily OHLC CSVs with a header and the date first
// (time,open,high,low,close,...), as the FYERS history download writes them.
// Use FYERS files: this tool does not care where the numbers came from, the
// project does. Days present in only one file are dropped.

#include <analytics/option_math.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::map<std::string, double> load_closes(const std::string& path) {
    std::map<std::string, double> out;
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);   // header
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string cell, date;
        std::vector<std::string> cols;
        while (std::getline(ss, cell, ',')) { cols.push_back(cell); }
        if (cols.size() < 5 || cols[0].size() < 10) { continue; }
        try { out[cols[0].substr(0, 10)] = std::stod(cols[4]); } catch (...) { continue; }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::string spot_path, vix_path, from;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--spot" && has) { spot_path = argv[++i]; continue; }
        if (a == "--vix" && has) { vix_path = argv[++i]; continue; }
        if (a == "--from" && has) { from = argv[++i]; continue; }
        std::printf("usage: altair_vol_slope --spot FILE --vix FILE [--from YYYY-MM-DD]\n");
        return a == "--help" ? 0 : 2;
    }
    if (spot_path.empty() || vix_path.empty()) {
        std::printf("usage: altair_vol_slope --spot FILE --vix FILE [--from YYYY-MM-DD]\n");
        return 2;
    }
    const auto spot = load_closes(spot_path);
    const auto vix = load_closes(vix_path);
    std::vector<double> ret, dv;
    double prev_s = NAN, prev_v = NAN;
    for (const auto& [day, s] : spot) {
        if (!from.empty() && day < from) { continue; }
        const auto it = vix.find(day);
        if (it == vix.end() || !(s > 0.0)) { continue; }
        if (prev_s > 0.0 && std::isfinite(prev_v)) {
            ret.push_back(100.0 * (s / prev_s - 1.0));
            dv.push_back(it->second - prev_v);
        }
        prev_s = s;
        prev_v = it->second;
    }
    if (ret.size() < 40) { std::printf("only %zu joined days -- not enough\n", ret.size()); return 1; }

    // Table 9.1, in vol points rather than percent of VIX.
    std::map<int, std::pair<double, int>> bucket;
    for (std::size_t i = 0; i < ret.size(); ++i) {
        const int b = static_cast<int>(std::floor(ret[i]));
        bucket[b].first += dv[i];
        ++bucket[b].second;
    }
    std::printf("%zu days\n  index move      days   avg VIX change (vol pts)\n", ret.size());
    for (const auto& [b, v] : bucket) {
        std::printf("  %+3d%% to %+3d%%  %6d   %+6.2f\n", b, b + 1, v.second, v.first / v.second);
    }
    const auto s = altair::vol_slope(ret, dv);
    std::printf("vol slope: +%.2f vol pts per 1%% fall (%zu days), -%.2f per 1%% rise (%zu days)\n", s.per_pct_fall,
                s.n_fall, s.per_pct_rise, s.n_rise);
    return 0;
}
