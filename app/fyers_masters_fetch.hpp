// app/fyers_masters_fetch.hpp -- download FYERS's public symbol masters.
//
// public.fyers.in/sym_details/<SEGMENT>.csv: public files, no login and no
// credential. Each is cached under `dir` and fetched again when older than
// `max_age`. A download that does not work keeps the copy on disk. The file is
// written to a .tmp beside it and renamed, so a reader never sees half a file.
//
// Used by altair_price_service (cash names, the F&O master and
// data/instruments.csv) and altair_instrument_master.

#pragma once

#include <broker/https_client.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace altair::fyers_masters {

/// Every segment ALTAIR reads: the cash, currency and MCX names
/// (live/fyers_names.hpp), the F&O contracts, and data/instruments.csv
/// (live/instrument_master.hpp).
inline constexpr const char* kSegments[] = {"NSE_CM", "NSE_FO", "NSE_CD", "BSE_CM",
                                            "BSE_FO", "BSE_CD", "MCX_COM"};

/// Fetch every master older than `max_age` (all of them with `force`).
/// Returns how many files are on disk afterwards; prints one line per failure.
inline std::size_t download(const std::filesystem::path& dir, bool force = false,
                            std::chrono::hours max_age = std::chrono::hours(20)) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::size_t have = 0;
    for (const char* seg : kSegments) {
        const fs::path path = dir / (std::string(seg) + ".csv");
        ec.clear();
        const auto stamp = fs::last_write_time(path, ec);
        if (!force && !ec && fs::file_time_type::clock::now() - stamp < max_age) {
            ++have;
            continue;
        }
        const auto r = altair::https_get_auth("public.fyers.in", std::string("/sym_details/") + seg + ".csv", "",
                                              std::chrono::seconds{30});
        if (!r || r->status != 200 || r->body.size() < 1000) {
            const bool kept = fs::exists(path, ec);
            std::printf("  FYERS %s master: download did not work (%s); %s\n", seg,
                        r ? ("HTTP " + std::to_string(r->status)).c_str() : "transport",
                        kept ? "using the copy on disk" : "no copy on disk");
            if (kept) ++have;
            continue;
        }
        const fs::path tmp = path.string() + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary);
            f.write(r->body.data(), static_cast<std::streamsize>(r->body.size()));
            if (!f) continue;
        }
        fs::rename(tmp, path, ec);
        if (!ec) ++have;
    }
    return have;
}

}  // namespace altair::fyers_masters
