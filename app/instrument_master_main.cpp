// app/instrument_master_main.cpp -- data/instruments.csv from FYERS.
//
// Downloads FYERS's public symbol masters into data/live/fyers_masters
// (app/fyers_masters_fetch.hpp) and builds data/instruments.csv from them
// (live/instrument_master.hpp). Public files only: no login, no credential.
// altair_price_service does the same on its own when the file is stale; this
// is for a machine that needs the master before the feed has ever run.
//
//   altair_instrument_master            dry run: says what it would do
//   altair_instrument_master --go       download (if older than 20 h) and build
//   altair_instrument_master --go --force   download every master again
//   altair_instrument_master --offline  build from the masters already on disk

#include <app/fyers_masters_fetch.hpp>
#include <live/instrument_master.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

[[nodiscard]] bool has_flag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) return true;
    }
    return false;
}

[[nodiscard]] const char* arg_value(int argc, char** argv, const char* flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const std::string root = arg_value(argc, argv, "--root") ? arg_value(argc, argv, "--root") : ".";
    const fs::path masters = fs::path(root) / "data" / "live" / "fyers_masters";
    const fs::path out = arg_value(argc, argv, "--out") ? fs::path(arg_value(argc, argv, "--out"))
                                                        : fs::path(root) / "data" / "instruments.csv";
    const bool go = has_flag(argc, argv, "--go");
    const bool offline = has_flag(argc, argv, "--offline");
    if (has_flag(argc, argv, "--help") || (!go && !offline)) {
        std::printf("altair_instrument_master [--root DIR] [--out FILE] (--go [--force] | --offline)\n\n"
                    "  Builds %s from the FYERS public symbol masters in %s.\n"
                    "  --go       download the masters older than 20 hours, then build\n"
                    "  --force    with --go: download every master again\n"
                    "  --offline  build from the masters already on disk\n\n"
                    "  DRY RUN: nothing downloaded and nothing written.\n",
                    out.string().c_str(), masters.string().c_str());
        return 0;
    }
    if (go) {
        const std::size_t n = altair::fyers_masters::download(masters, has_flag(argc, argv, "--force"));
        std::printf("FYERS masters on disk: %zu of %zu\n", n, std::size(altair::fyers_masters::kSegments));
    }
    altair::live::MasterBuildReport rep;
    std::string err;
    if (!altair::live::build_instrument_master(masters, out, rep, err)) {
        std::printf("not built: %s\n", err.c_str());
        return 3;
    }
    std::printf("wrote %s: %zu contract(s) from %zu master file(s); %zu line(s) skipped, %zu duplicate token(s)\n",
                out.string().c_str(), rep.rows, rep.files, rep.skipped, rep.duplicates);
    for (const auto& m : rep.missing) std::printf("  missing master: %s.csv\n", m.c_str());
    return 0;
}
