// app/store_main.cpp -- altair_store: the training store's two nightly steps.
//
//   altair_store decode   --day YYYY-MM-DD [--tapes DIR] [--root DIR] [--ticks]
//   altair_store snapshot --cutoff YYYY-MM-DD --source NAME=DIR [--source NAME=DIR ...]
//                         [--fill NAME=DIR ...] [--out DIR] [--bhavcopy DIR] [--iv DIR] [--root DIR]
//
// What each writes, and the merge rule, are in app/training_store.hpp. Both are
// offline: no network, no credential. Both say what they did in the activity
// log (app/activity_log.hpp). ops/nightly.ps1 runs them in order.
#include <app/activity_log.hpp>
#include <app/training_store.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace st = altair::store;
namespace act = altair::activity;

void usage(const char* exe) {
    std::printf(
        "  The training store: recorded FYERS sessions and dataset/ as one input.\n\n"
        "    %s decode --day YYYY-MM-DD [--tapes DIR] [--root DIR] [--ticks]\n"
        "        the day's tapes (default <root>/data/store/tapes/<day>) into\n"
        "        <root>/data/store/ticks/<day>/ (bars_1m.csv, vol_1m.csv, chain_iv_1m.csv,\n"
        "        _decode.json; with --ticks\n"
        "        also trades, quotes and books) and <root>/data/store/tape_bars/.\n\n"
        "    %s snapshot --cutoff YYYY-MM-DD --source NAME=DIR [--source ...] [--fill NAME=DIR ...]\n"
        "        [--out DIR] [--bhavcopy DIR] [--iv DIR] [--root DIR]\n"
        "        one tree cut at the end of the cutoff day (IST), every source merged:\n"
        "        --source lowest priority first (beyond 1 bp the later one wins and the\n"
        "        pair is listed); --fill only adds bars nobody else has. Default --out\n"
        "        <root>/data/store/snapshots/<cutoff>. A snapshot is never rebuilt over.\n",
        exe, exe);
}

[[nodiscard]] bool split_source(const std::string& v, st::Source& out) {
    const auto eq = v.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 >= v.size()) return false;
    out.name = v.substr(0, eq);
    out.dir = v.substr(eq + 1);
    for (char c : out.name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
    return true;
}

int decode(const fs::path& root, const std::string& day, fs::path tapes, bool ticks) {
    if (!st::parse_day(day)) { std::printf("--day must be YYYY-MM-DD\n"); return 2; }
    if (tapes.empty()) tapes = root / "data" / "store" / "tapes" / day;
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(tapes, ec))
        if (e.path().extension() == ".tape") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::printf("no tapes under %s\n", tapes.string().c_str());
        act::write(root, "store", "WARN", "decode " + day + ": no tapes under " + tapes.string());
        return 1;
    }
    const fs::path out = root / "data" / "store" / "ticks" / day;
    st::TapeDecoder dec(ticks ? out : fs::path{});
    for (const auto& f : files) {
        std::printf("  %s\n", f.filename().string().c_str());
        (void)dec.read(f);
    }
    if (!dec.close_ticks()) {
        act::write(root, "store", "ERROR", "decode " + day + ": a tick file write failed under " + out.string());
        return 3;
    }
    if (!st::write_file(out / "bars_1m.csv", dec.bars_csv()) || !st::write_file(out / "_decode.json", dec.stats_json(day))) {
        act::write(root, "store", "ERROR", "decode " + day + ": cannot write " + out.string());
        return 3;
    }
    // IV, the chain's own VIX, India VIX and RV, a minute; every option's IV.
    const auto vol = dec.vol_views();
    if (!st::write_file(out / "vol_1m.csv", vol.vol) || !st::write_file(out / "chain_iv_1m.csv", vol.chain)) {
        act::write(root, "store", "ERROR", "decode " + day + ": cannot write the vol files under " + out.string());
        return 3;
    }
    std::size_t series = 0;
    for (const auto& [key, csv] : dec.series_csv()) {
        if (!st::write_file(root / "data" / "store" / "tape_bars" / key / "1m" / (day + ".csv"), csv)) {
            act::write(root, "store", "ERROR", "decode " + day + ": cannot write tape bars for " + key);
            return 3;
        }
        ++series;
    }
    const auto& s = dec.stats();
    const std::string summary = "decode " + day + ": " + std::to_string(s.tapes) + " tape(s), " + std::to_string(s.trades)
        + " trades, " + std::to_string(s.quotes) + " quotes, " + std::to_string(s.books) + " books ("
        + std::to_string(s.deep_books) + " over 5 levels), " + std::to_string(dec.bars().size()) + " instruments, "
        + std::to_string(series) + " dataset series, " + std::to_string(vol.vol_rows) + " vol rows, "
        + std::to_string(vol.chain_rows) + " option IVs; missed by sequence: " + std::to_string(s.missed[altair::kTopicTrades])
        + " trades, " + std::to_string(s.missed[altair::kTopicBook]) + " books; SIM/replay dropped "
        + std::to_string(s.simulated + s.replayed);
    std::printf("  %s\n", summary.c_str());
    act::write(root, "store", s.tapes == 0 ? "ERROR" : (s.missed[altair::kTopicTrades] > 0 ? "WARN" : "INFO"), summary);
    return s.tapes == 0 ? 1 : 0;
}

int snapshot(const fs::path& root, int argc, char** argv) {
    st::SnapshotOptions o;
    std::string cutoff;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if ((a == "--source" || a == "--fill") && has) {
            st::Source s;
            if (!split_source(argv[++i], s)) { std::printf("%s takes NAME=DIR (NAME: letters, digits, _ or -)\n", a.c_str()); return 2; }
            s.fill_only = a == "--fill";
            o.sources.push_back(std::move(s));
            continue;
        }
        if (a == "--cutoff" && has) { cutoff = argv[++i]; continue; }
        if (a == "--out" && has) { o.out = argv[++i]; continue; }
        if (a == "--bhavcopy" && has) { o.bhavcopy = argv[++i]; continue; }
        if (a == "--iv" && has) { o.iv = argv[++i]; continue; }
        if (a == "--root" && has) { ++i; continue; }   // read by main
        usage(argv[0]);
        return 2;
    }
    const auto day = st::parse_day(cutoff);
    if (!day) { std::printf("--cutoff must be YYYY-MM-DD\n"); return 2; }
    if (o.sources.empty()) { std::printf("at least one --source\n"); return 2; }
    o.cutoff_day = *day;
    if (o.out.empty()) o.out = root / "data" / "store" / "snapshots" / cutoff;
    const auto rep = st::build_snapshot(o);
    if (!rep.error.empty()) {
        std::printf("  snapshot failed: %s\n", rep.error.c_str());
        act::write(root, "store", "ERROR", "snapshot " + cutoff + " failed: " + rep.error);
        return 3;
    }
    std::size_t rows = 0;
    std::string behind;
    for (const auto& r : rep.series) {
        rows += r.rows;
        std::printf("  %-26s %9zu rows  %s .. %s  conflicts %zu\n", r.key.c_str(), r.rows,
                    r.empty ? "-" : altair::data_audit::format_audit_time(r.first, r.key.ends_with("/1d")).substr(0, 10).c_str(),
                    r.empty ? "-" : altair::data_audit::format_audit_time(r.last, r.key.ends_with("/1d")).substr(0, 10).c_str(),
                    r.conflicts);
        if (!r.empty && altair::data_audit::audit_day(r.last) < *day) behind += " " + r.key;
    }
    const std::string summary = "snapshot " + cutoff + " id " + rep.id + ": " + std::to_string(rep.series.size()) + " series, "
        + std::to_string(rows) + " rows, " + std::to_string(rep.conflicts) + " conflict(s), " + std::to_string(rep.bhavcopy_files)
        + " bhavcopy + " + std::to_string(rep.iv_files) + " IV file(s)" + (behind.empty() ? "" : "; ends before the cutoff:" + behind);
    std::printf("  %s\n  -> %s\n", summary.c_str(), o.out.string().c_str());
    act::write(root, "store", behind.empty() ? "INFO" : "WARN", summary);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
#ifdef ALTAIR_SOURCE_DIR
    fs::path root = ALTAIR_SOURCE_DIR;
#else
    fs::path root = ".";
#endif
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--root") == 0) root = argv[i + 1];
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        usage(argv[0]);
        return argc < 2 ? 2 : 0;
    }
    const std::string cmd = argv[1];
    if (cmd == "decode") {
        std::string day;
        fs::path tapes;
        bool ticks = false;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            const bool has = i + 1 < argc;
            if (a == "--day" && has) { day = argv[++i]; continue; }
            if (a == "--tapes" && has) { tapes = argv[++i]; continue; }
            if (a == "--root" && has) { ++i; continue; }
            if (a == "--ticks") { ticks = true; continue; }
            usage(argv[0]);
            return 2;
        }
        return decode(root, day, tapes, ticks);
    }
    if (cmd == "snapshot") return snapshot(root, argc, argv);
    usage(argv[0]);
    return 2;
}
