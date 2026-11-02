// app/obi_select_main.cpp -- altair_obi_select: score yesterday's (or a given
// day's) F&O stocks off the recorded FYERS session tapes and, with --write,
// put the best N in config/obi_underlyings.csv (app/obi_select.hpp).
//
//   altair_obi_select [--root DIR] [--day YYYY-MM-DD] [--top N] [--keep SYM,SYM]
//                     [--min-samples N] [--write]
//
// --top counts the --keep stocks. The ranking always goes to data/live/obi_selection.csv. Without --write
// nothing in config/ changes.

#include <app/obi_select.hpp>
#include <live/obi_underlyings.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace os = altair::obi_select;

void usage(const char* exe) {
    std::printf(
        "altair_obi_select -- pick the stocks the OBI trader watches, from a recorded FYERS session\n\n"
        "    %s [--root DIR] [--day YYYY-MM-DD] [--top N] [--keep SYM,SYM] [--min-samples N] [--write]\n\n"
        "    --root         the tree holding data/live/tapes, data/store/tapes and config/ (default: the source tree)\n"
        "    --day          the session to score (default: the latest recorded)\n"
        "    --top          how many stocks to keep (default 3)\n"
        "    --keep         stocks kept whatever their score, with their current row (e.g. NYKAA)\n"
        "    --min-samples  book samples (one a second at most) a stock needs to be scored (default 300)\n"
        "    --write        rewrite config/obi_underlyings.csv (otherwise only data/live/obi_selection.csv)\n\n"
        "  Score = 0.5 x value-traded rank + 0.3 x touch-depth rank + 0.2 x tight-spread rank.\n"
        "  Restart the price service and the live engine after --write so both pick the new stocks up.\n",
        exe);
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    for (const char c : s + ",") {
        if (c == ',') { if (!cur.empty()) v.push_back(cur); cur.clear(); }
        else if (c != ' ') cur.push_back(c);
    }
    return v;
}

} // namespace

int main(int argc, char** argv) {
    fs::path root = ALTAIR_SOURCE_DIR;
    std::string day;
    std::size_t top = 3, min_samples = 300;
    std::vector<std::string> keep;
    bool write = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--root" && has) root = argv[++i];
        else if (a == "--day" && has) day = argv[++i];
        else if (a == "--top" && has) top = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--keep" && has) keep = split(argv[++i]);
        else if (a == "--min-samples" && has) min_samples = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--write") write = true;
        else { usage(argv[0]); return a == "--help" || a == "-h" ? 0 : 2; }
    }

    // The day's tapes (a tape's name starts with its session's date).
    const auto all = altair::tune::find_tapes(root);
    if (day.empty())
        for (const auto& t : all) day = std::max(day, t.filename().string().substr(0, 10));
    std::vector<fs::path> tapes;
    for (const auto& t : all) if (t.filename().string().rfind(day, 0) == 0) tapes.push_back(t);
    if (day.empty() || tapes.empty()) {
        std::fprintf(stderr, "no recorded session tape%s%s under %s/data/live/tapes or data/store/tapes\n",
                     day.empty() ? "" : " for ", day.c_str(), root.string().c_str());
        return 1;
    }

    std::map<std::string, os::StockDay> days;
    std::string err;
    if (!os::collect(tapes, days, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    const auto ranked = os::score(days, min_samples);
    std::printf("%s: %zu tape(s), %zu stock(s) seen, %zu scored (>= %zu book samples)\n", day.c_str(), tapes.size(), days.size(),
                ranked.size(), min_samples);
    if (ranked.empty()) {
        std::fprintf(stderr, "nothing to score: the tapes held no stock books in session hours\n");
        return 1;
    }

    // The ranking, for people and for the record.
    fs::create_directories(root / "data/live");
    {
        std::ofstream f(root / "data/live/obi_selection.csv", std::ios::trunc);
        f << "day,rank,symbol,score,value_cr,median_spread_ticks,median_touch_rs,price,updates,samples,target,stop,safety\n";
        char b[256];
        for (std::size_t i = 0; i < ranked.size(); ++i) {
            const auto& s = ranked[i];
            std::snprintf(b, sizeof b, "%s,%zu,%s,%.4f,%.2f,%.2f,%.0f,%.2f,%zu,%zu,%.2f,%.2f,%.2f\n", day.c_str(), i + 1,
                          s.symbol.c_str(), s.score, s.value / 1e7, s.spread_ticks, s.depth_rs, s.price, s.updates, s.samples,
                          s.target, s.stop, s.safety);
            f << b;
        }
    }
    std::printf("  rank  symbol         score   traded Rs cr  spread ticks  touch Rs     target  stop\n");
    for (std::size_t i = 0; i < ranked.size() && i < 20; ++i) {
        const auto& s = ranked[i];
        std::printf("  %4zu  %-12s  %6.3f  %12.2f  %12.2f  %9.0f  %8.2f  %5.2f\n", i + 1, s.symbol.c_str(), s.score, s.value / 1e7,
                    s.spread_ticks, s.depth_rs, s.target, s.stop);
    }

    // The new list: the kept stocks (their current row), then the best.
    const fs::path cfg = root / "config/obi_underlyings.csv";
    const auto current = altair::live::read_obi_underlyings(cfg.string());
    std::vector<os::Score> chosen;
    std::set<std::string> in;
    for (const auto& k : keep) {
        os::Score s;
        s.symbol = k;
        bool found = false;
        for (const auto& c : current)
            if (c.symbol == k) { s.target = c.target_pts; s.stop = c.stop_pts; s.safety = c.safety_pts; found = true; }
        if (!found)
            for (const auto& r : ranked) if (r.symbol == k) { s = r; found = true; }
        if (!found) { std::fprintf(stderr, "--keep %s: not in %s and not scored today; left out\n", k.c_str(), cfg.string().c_str()); continue; }
        if (in.insert(k).second) chosen.push_back(s);
    }
    const std::size_t want = std::max(top, chosen.size());   // --top counts the kept stocks too
    for (const auto& r : ranked) {
        if (chosen.size() >= want) break;
        if (in.insert(r.symbol).second) chosen.push_back(r);
    }
    std::printf("chosen:");
    for (const auto& s : chosen) std::printf(" %s", s.symbol.c_str());
    std::printf("\n");
    if (!write) {
        std::printf("(--write not given: %s unchanged; the ranking is in data/live/obi_selection.csv)\n", cfg.string().c_str());
        return 0;
    }
    if (!os::write_underlyings(cfg, chosen, day)) {
        std::fprintf(stderr, "could not write %s\n", cfg.string().c_str());
        return 1;
    }
    std::printf("wrote %s: restart the price service and the live engine to trade the new list\n", cfg.string().c_str());
    return 0;
}
