// app/tests/test_data_policy.cpp -- the run-time lock of docs/DATA_POLICY.md.

#include <app/data_policy.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

} // namespace

int main() {
    namespace dp = altair::data_policy;
    namespace fs = std::filesystem;
    bool all = true;
    for (const auto n : dp::kExampleBannedNames) all = all && dp::is_banned_file(n);
    check(all, "every banned name format is recognised, in any case");
    check(!dp::is_banned_file("2026-10-23.csv") && !dp::is_banned_file("NSE_FO.csv") && !dp::is_banned_file("history.csv"),
          "FYERS and ALTAIR's own files are not banned");

    const fs::path root = fs::temp_directory_path() / "altair_data_policy_test";
    fs::remove_all(root);
    fs::create_directories(root / "spot/nifty/1d");
    std::ofstream(root / "spot/nifty/1d/2026-10-23.csv") << "time,close\n";
    check(dp::find_banned(root).empty(), "a clean tree passes");
    std::ofstream(root / "spot/nifty/1d" / std::string(dp::kExampleBannedNames[0])) << "x\n";
    check(!dp::find_banned(root).empty(), "a banned file deep in a tree is found");
    check(dp::find_banned(root / "missing").empty() && dp::find_banned({}).empty(), "a missing or empty path is not an error");
    fs::remove_all(root);
    std::printf(g_fail ? "data_policy: %d FAILED\n" : "data_policy: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
