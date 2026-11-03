// live/instrument_master.hpp -- data/instruments.csv, built from FYERS's own
// public symbol masters.
//
// Every page that resolves a scrip (the market watch, the option chains, the
// terminal, the order ticket, the lot lookups of the paper strategies) reads
// ONE instrument master: data/instruments.csv. This file writes it from the
// FYERS masters the price service caches under data/live/fyers_masters/
// (https://public.fyers.in/sym_details/<SEG>.csv: public, no login, current
// every trading day). Nothing in it comes from any other broker.
//
// THE COLUMNS (header first, one contract a line):
//   instrument_token, exchange_token, tradingsymbol, name, last_price, expiry,
//   strike, tick_size, lot_size, instrument_type, segment, exchange
//
// THE INSTRUMENT TOKEN is ALTAIR's identity on the price wire. It is the
// exchange token times 256 plus a segment code:
//     NSE 1, NFO 2, CDS 3, BSE 4, BFO 5, BCD 6, MCX 7, INDICES 9
// so it is stable across days and machines, and every saved watchlist, layout
// and paper book keeps resolving. The indices the desktop has always used keep
// their fixed numbers (kIndexTokens: NIFTY 50 = 256265, NIFTY BANK = 260105,
// INDIA VIX = 264969, ...); any other FYERS index is its FYERS exchange token
// times 256 plus 9.
//
// NAMES. Derivatives keep FYERS's trading symbol without the exchange prefix
// ("NSE:NIFTY26NOVFUT" -> "NIFTY26NOVFUT"), which is also the exchange's own.
// An NSE cash scrip drops "-EQ" ("NSE:SBIN-EQ" -> "SBIN"; other series keep
// theirs, "NSE:XYZ-BE" -> "XYZ-BE"); a BSE cash scrip drops its group
// ("BSE:SBIN-A" -> "SBIN"). live/fyers_names.hpp maps them back to FYERS's
// ticker by exchange token, so nothing is guessed on the way out.
//
// LOTS. NSE/BSE equity and F&O carry FYERS's lot (units). MCX and currency
// rows carry 1: an order there is in lots, and live/contract_units.hpp says
// what one lot is.
//
// A row this file does not understand (an unknown exchange or segment, a
// token past 2^24, a field missing) is skipped and counted, never guessed.

#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <istream>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace altair::live {

/// The FYERS master files read, in this order.
inline constexpr std::array<std::string_view, 6> kFyersMasterFiles{
    "NSE_CM", "NSE_FO", "NSE_CD", "BSE_CM", "BSE_FO", "MCX_COM"};

inline constexpr std::string_view kInstrumentMasterHeader =
    "instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,strike,"
    "tick_size,lot_size,instrument_type,segment,exchange";

/// The indices whose token is fixed (the desktop and every test key on them).
struct FixedIndex { std::string_view fyers, symbol, exchange; std::uint32_t token, exchange_token; };
inline constexpr std::array<FixedIndex, 6> kIndexTokens{{
    {"NSE:NIFTY50-INDEX", "NIFTY 50", "NSE", 256265u, 1001u},
    {"NSE:NIFTYBANK-INDEX", "NIFTY BANK", "NSE", 260105u, 1016u},
    {"NSE:INDIAVIX-INDEX", "INDIA VIX", "NSE", 264969u, 1035u},
    {"NSE:FINNIFTY-INDEX", "NIFTY FIN SERVICE", "NSE", 257801u, 1007u},
    {"NSE:NIFTYNXT50-INDEX", "NIFTY NEXT 50", "NSE", 270857u, 1058u},
    {"BSE:SENSEX-INDEX", "SENSEX", "BSE", 265u, 1u},
}};

struct MasterBuildReport {
    std::size_t rows = 0;       ///< contracts written
    std::size_t skipped = 0;    ///< lines not understood
    std::size_t duplicates = 0; ///< a token already written (first wins)
    std::size_t files = 0;      ///< FYERS master files read
    std::vector<std::string> missing;   ///< master files not on disk
};

namespace master_detail {

/// "2026-11-25" for an epoch-seconds expiry, the IST date (FYERS stamps the close).
[[nodiscard]] inline std::string ist_date(long long epoch_s) {
    long long z = (epoch_s + 19'800) / 86'400 + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const long long y = static_cast<long long>(yoe) + era * 400 + (m <= 2 ? 1 : 0);
    char buf[24];
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02u", y, m, d);
    return buf;
}

/// A number as the master writes it: no trailing zeros, no exponent.
[[nodiscard]] inline std::string num(std::string_view text) {
    const double v = std::atof(std::string(text).c_str());
    if (!(v > 0.0)) return "0";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

[[nodiscard]] inline std::string csv_quoted(std::string_view s) {
    std::string out = "\"";
    for (char c : s) if (c != '"' && c != '\r' && c != '\n') out.push_back(c);
    out.push_back('"');
    return out;
}

} // namespace master_detail

/// One FYERS master line as one instrument-master line (no newline), or an
/// empty string when the row is not understood. `token` receives the ALTAIR
/// instrument token.
[[nodiscard]] inline std::string master_line_from_fyers(std::string_view line, std::uint32_t& token) {
    using namespace master_detail;
    std::vector<std::string_view> f;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            f.push_back(line.substr(start, i - start));
            start = i + 1;
        }
    }
    if (f.size() != 21) return {};   // FYERS does not quote: a comma inside a field is refused
    const std::string_view ticker = f[9];
    const std::size_t colon = ticker.find(':');
    if (colon == std::string_view::npos) return {};
    const std::string_view ex = f[10], seg = f[11], opt = f[16];
    const unsigned long long xtok = std::strtoull(std::string(f[12]).c_str(), nullptr, 10);
    std::string_view body = ticker.substr(colon + 1);

    // Indices: FYERS lists them in the cash masters with a "-INDEX" ticker.
    if (body.size() > 6 && body.substr(body.size() - 6) == "-INDEX") {
        for (const auto& fi : kIndexTokens) {
            if (fi.fyers == ticker) {
                token = fi.token;
                return std::to_string(fi.token) + "," + std::to_string(fi.exchange_token) + "," + std::string(fi.symbol)
                       + "," + csv_quoted(fi.symbol) + ",0,,0,0,0,EQ,INDICES," + std::string(fi.exchange);
            }
        }
        if (xtok == 0 || xtok >= (1ull << 24)) return {};
        token = static_cast<std::uint32_t>(xtok * 256 + 9);
        const std::string sym(body.substr(0, body.size() - 6));
        return std::to_string(token) + "," + std::to_string(xtok) + "," + sym + "," + csv_quoted(sym) + ",0,,0,0,0,EQ,INDICES,"
               + std::string(ticker.substr(0, colon));
    }
    if (xtok == 0 || xtok >= (1ull << 24)) return {};

    std::string exchange, segment, type = "EQ", symbol(body), name;
    unsigned code = 0;
    bool lots_are_contracts = false;   // MCX and currency: lot_size 1
    const bool fo = seg == "11", cd = seg == "12", com = seg == "20", cash = seg == "10";
    const bool is_opt = opt == "CE" || opt == "PE";
    if (!cash && !is_opt && opt != "XX") return {};
    if (ex == "10" && cash) {
        exchange = "NSE"; segment = "NSE"; code = 1;
        if (symbol.size() > 3 && symbol.substr(symbol.size() - 3) == "-EQ") symbol.resize(symbol.size() - 3);
        name = std::string(f[1]);
    } else if (ex == "12" && cash) {
        exchange = "BSE"; segment = "BSE"; code = 4;
        const std::size_t dash = symbol.rfind('-');
        if (dash != std::string::npos) symbol.resize(dash);
        name = std::string(f[1]);
    } else if (ex == "10" && fo) {
        exchange = "NFO"; segment = is_opt ? "NFO-OPT" : "NFO-FUT"; code = 2;
    } else if (ex == "12" && fo) {
        exchange = "BFO"; segment = is_opt ? "BFO-OPT" : "BFO-FUT"; code = 5;
    } else if (ex == "10" && cd) {
        exchange = "CDS"; segment = is_opt ? "CDS-OPT" : "CDS-FUT"; code = 3; lots_are_contracts = true;
    } else if (ex == "12" && cd) {
        exchange = "BCD"; segment = is_opt ? "BCD-OPT" : "BCD-FUT"; code = 6; lots_are_contracts = true;
    } else if (ex == "11" && com) {
        exchange = "MCX"; segment = is_opt ? "MCX-OPT" : "MCX-FUT"; code = 7; lots_are_contracts = true;
    } else {
        return {};
    }
    if (!cash) {
        type = is_opt ? std::string(opt) : "FUT";
        name = std::string(f[13]);
    }
    if (symbol.empty() || name.empty()) return {};
    std::string expiry;
    if (!cash) {
        const long long e = std::atoll(std::string(f[8]).c_str());
        if (e <= 0) return {};
        expiry = ist_date(e);
    }
    token = static_cast<std::uint32_t>(xtok * 256 + code);
    const std::string lot = lots_are_contracts ? std::string("1") : num(f[3]);
    return std::to_string(token) + "," + std::to_string(xtok) + "," + symbol + "," + csv_quoted(name) + ",0," + expiry + ","
           + (is_opt ? num(f[15]) : std::string("0")) + "," + num(f[4]) + "," + lot + "," + type + "," + segment + ","
           + exchange;
}

/// Append every understood row of one FYERS master to `out`; a token already
/// in `seen` is skipped (the first master wins).
inline void append_fyers_master(std::istream& in, std::ostream& out, std::set<std::uint32_t>& seen,
                                MasterBuildReport& rep) {
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::uint32_t token = 0;
        const std::string row = master_line_from_fyers(line, token);
        if (row.empty()) { ++rep.skipped; continue; }
        if (!seen.insert(token).second) { ++rep.duplicates; continue; }
        out << row << '\n';
        ++rep.rows;
    }
}

/// Build `out_path` from the FYERS masters in `masters_dir`. Written to a
/// temporary file and renamed, so a reader never sees half a master. Fails
/// (returns false, `error` says why) when no master is on disk or nothing
/// was understood; the previous file is then left as it was.
[[nodiscard]] inline bool build_instrument_master(const std::filesystem::path& masters_dir,
                                                  const std::filesystem::path& out_path, MasterBuildReport& rep,
                                                  std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!out_path.parent_path().empty()) fs::create_directories(out_path.parent_path(), ec);
    const fs::path tmp = out_path.string() + ".building";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { error = "cannot write " + tmp.string(); return false; }
        out << kInstrumentMasterHeader << '\n';
        std::set<std::uint32_t> seen;
        for (const auto name : kFyersMasterFiles) {
            const fs::path p = masters_dir / (std::string(name) + ".csv");
            std::ifstream in(p, std::ios::binary);
            if (!in) { rep.missing.emplace_back(name); continue; }
            ++rep.files;
            append_fyers_master(in, out, seen, rep);
        }
        out.flush();
        if (!out) { error = "write failed: " + tmp.string(); fs::remove(tmp, ec); return false; }
    }
    if (rep.rows == 0) {
        fs::remove(tmp, ec);
        error = rep.files == 0 ? "no FYERS master in " + masters_dir.string() : "no FYERS master row was understood";
        return false;
    }
    fs::rename(tmp, out_path, ec);
    if (ec) {   // Windows: rename over a file another process holds open
        fs::remove(out_path, ec);
        ec.clear();
        fs::rename(tmp, out_path, ec);
        if (ec) { error = "cannot replace " + out_path.string() + ": " + ec.message(); return false; }
    }
    return true;
}

/// True when `out_path` is missing or older than any FYERS master in `masters_dir`.
[[nodiscard]] inline bool instrument_master_stale(const std::filesystem::path& masters_dir,
                                                  const std::filesystem::path& out_path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto have = fs::last_write_time(out_path, ec);
    if (ec) return true;
    for (const auto name : kFyersMasterFiles) {
        std::error_code e2;
        const auto t = fs::last_write_time(masters_dir / (std::string(name) + ".csv"), e2);
        if (!e2 && t > have) return true;
    }
    return false;
}

} // namespace altair::live
