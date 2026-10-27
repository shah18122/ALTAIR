// app/fyers_source.hpp -- is this folder FYERS data?
//
// Smit's standing rule: research and paper trading run on FYERS data, never
// NSE bhavcopy, and a tool that cannot tell where its bars came from must not
// guess. A folder counts as FYERS when it carries a SOURCE.txt naming FYERS
// (written by the fetch scripts), or when it sits under a folder named
// `fyers` -- the layout of ops/broker_audit.ps1
// (data\broker_audit\fyers\<segment>\<instrument>\<tf>) and of the recorder
// store (data\store\fyers\...).

#pragma once

#include <algorithm>
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

} // namespace altair::app
