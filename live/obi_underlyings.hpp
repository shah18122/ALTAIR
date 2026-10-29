// live/obi_underlyings.hpp -- the stocks the order-book imbalance trader
// (live/obi_trader.hpp) runs on besides NIFTY and BANKNIFTY, read from
// config/obi_underlyings.csv: one NSE F&O stock a line with its target, stop
// and safety margin in rupees a share.
//
// The price service streams each one's stock, its two nearest futures (with
// the 5-level book) and its nearest monthly chain around the money; the live
// engine gives each the same two models as the indices (the near future long
// or short, and the signal as a SOLD put or call). Lot and tick come from the
// instrument master. Adding a line needs a restart of the price service and
// the engine, not a rebuild.

#pragma once

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace altair::live {

struct ObiUnderlying {
    std::string symbol;        ///< the NSE symbol: "NYKAA"
    double target_pts = 0.0;   ///< rupees a share
    double stop_pts = 0.0;
    double safety_pts = 0.0;
};

/// The stocks of `path` ("symbol,target,stop,safety" after a header; '#'
/// comments). A line without a positive target and stop is skipped, as is a
/// repeated symbol; a missing file is no stocks.
[[nodiscard]] inline std::vector<ObiUnderlying> read_obi_underlyings(const std::string& path) {
    std::vector<ObiUnderlying> out;
    std::ifstream in(path);
    std::string line;
    bool header = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (!header) { header = true; continue; }
        std::vector<std::string> f;
        std::string cur;
        for (const char ch : line) {
            if (ch == ',') { f.push_back(cur); cur.clear(); }
            else if (ch != ' ' && ch != '\t') cur += ch;
        }
        f.push_back(cur);
        if (f.size() < 3 || f[0].empty()) continue;
        ObiUnderlying u;
        u.symbol = f[0];
        u.target_pts = std::atof(f[1].c_str());
        u.stop_pts = std::atof(f[2].c_str());
        u.safety_pts = f.size() > 3 ? std::atof(f[3].c_str()) : 0.0;
        if (!(u.target_pts > 0.0) || !(u.stop_pts > 0.0) || !(u.safety_pts >= 0.0)) continue;
        bool dup = false;
        for (const auto& o : out) dup = dup || o.symbol == u.symbol;
        if (dup || u.symbol == "NIFTY" || u.symbol == "BANKNIFTY") continue;
        out.push_back(std::move(u));
    }
    return out;
}

} // namespace altair::live
