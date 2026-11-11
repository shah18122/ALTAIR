// app/data_policy.hpp -- the one place that names the banned data source.
//
// Smit's standing rule (2026-10-26, made a hard rule 2026-11-11): ALTAIR never
// reads the NSE bhavcopy, in any format (the old fo<DD><MON><YYYY>bhav.csv or
// the UDiFF BhavCopy_NSE_*_0_0_0_<YYYYMMDD>_F_0000.csv). Prices come from
// FYERS; if FYERS lacks something, the tool says so and stops -- it never falls
// back. See docs/DATA_POLICY.md.
//
// Two locks:
//   * build time: tools/check_data_policy.sh (CI and ctest `data_policy`) fails
//     on any mention of the source outside this file and the policy doc;
//   * run time: every input tree a snapshot reads is walked first, and a file
//     whose name matches is_banned_file() aborts the snapshot.

#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>

namespace altair::data_policy {

/// Names tests use to prove the locks hold (kept here so no other file names the source).
inline constexpr std::string_view kExampleBannedNames[] = {
    "fo05JUL2024bhav.csv", "BhavCopy_NSE_FO_0_0_0_20240708_F_0000.csv", "FO_UDIFF.ZIP", "x_cm_0_0_0_y.csv"};

/// True for a file name from the banned source, whatever its case.
[[nodiscard]] inline bool is_banned_file(std::string_view name) {
    std::string low(name);
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return low.find("bhav") != std::string::npos           // fo05JUL2024bhav.csv, BhavCopy_*.csv
        || low.find("_fo_0_0_0_") != std::string::npos      // UDiFF, renamed
        || low.find("_cm_0_0_0_") != std::string::npos
        || low.find("udiff") != std::string::npos;          // fo_udiff.zip, cm_udiff.zip
}

/// The first banned file under `dir` (recursively), or an empty path.
[[nodiscard]] inline std::filesystem::path find_banned(const std::filesystem::path& dir) {
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return {};
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (is_banned_file(it->path().filename().string())) return it->path();
    }
    return {};
}

} // namespace altair::data_policy
