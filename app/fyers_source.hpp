// app/fyers_source.hpp -- is this folder FYERS data?
//
// Smit's standing rule: research and paper trading run on FYERS data only
// (docs/DATA_POLICY.md), and a tool that cannot tell where its bars came from must not
// guess. A folder counts as FYERS when it carries a SOURCE.txt naming FYERS
// (written by the fetch scripts), or when it sits under a folder named
// `fyers` -- the layout of ops/linux/fetch.py audit
// (data\broker_audit\fyers\<segment>\<instrument>\<tf>) and of the recorder
// store (data\store\fyers\...).

#pragma once

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace altair::app {

[[nodiscard]] inline bool is_fyers_folder(const std::filesystem::path& dir) {
    std::ifstream in(dir / "SOURCE.txt");
    if (in) {
        std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        // "NOT FYERS" (e.g. drive-ticks) is an explicit no, never a yes.
        if (s.find("NOT FYERS") != std::string::npos) { return false; }
        if (s.find("FYERS") != std::string::npos) { return true; }
    }
    std::error_code ec;
    const auto abs = std::filesystem::weakly_canonical(dir, ec);
    for (const auto& part : ec ? dir : abs) {
        std::string n = part.string();
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (n == "fyers") { return true; }
    }
    return false;
}

/// The default history for a research tool when none is named: the newest
/// FYERS snapshot (data/store/snapshots/<YYYY-MM-DD>/ with a _snapshot.json,
/// the nightly's FYERS-only merge), else data/store/fyers/, else an empty
/// path -- never dataset/ (docs/DATA_POLICY.md: no fallback to another
/// source). `base` is the folder holding data/.
[[nodiscard]] inline std::filesystem::path default_history(const std::filesystem::path& base = ".") {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string best;
    for (const auto& e : fs::directory_iterator(base / "data/store/snapshots", ec)) {
        const std::string n = e.path().filename().string();
        if (n.size() == 10 && n[4] == '-' && n[7] == '-' && fs::exists(e.path() / "_snapshot.json", ec) && n > best) best = n;
    }
    if (!best.empty()) return base / "data/store/snapshots" / best;
    if (fs::is_directory(base / "data/store/fyers/spot", ec)) return base / "data/store/fyers";
    return {};
}

/// A tool's history folder: `asked` when the caller named one, else
/// default_history(). Prints which, and whether it is FYERS. Empty (and an
/// explanation printed) when there is none: the tool must stop.
[[nodiscard]] inline std::filesystem::path pick_history(const std::filesystem::path& asked, const char* tool) {
    std::error_code ec;
    if (!asked.empty()) {
        const bool fy = is_fyers_folder(asked) || std::filesystem::exists(asked / "_snapshot.json", ec);
        std::printf("%s: history %s%s\n", tool, asked.string().c_str(),
                    fy ? " (FYERS)" : " (NOT FYERS: named by the caller; not evidence, docs/DATA_POLICY.md)");
        return asked;
    }
    auto d = default_history();
    if (d.empty()) {
        std::printf("%s: no FYERS history (no data/store/snapshots/<day>, no data/store/fyers). Run the nightly or\n"
                    "  ops/linux/fetch.py history, or name a folder with --dataset. dataset/ is never used on its own.\n", tool);
        return {};
    }
    std::printf("%s: history %s (FYERS)\n", tool, d.string().c_str());
    return d;
}

} // namespace altair::app
