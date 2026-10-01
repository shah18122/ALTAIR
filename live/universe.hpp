// live/universe.hpp -- what the live terminal streams, and under which token.
//
// The desktop names every instrument by its Kite instrument_token: the strip
// tiles, the option chain, the order ticket and the position table all key on
// it, and data/instruments.csv (the Kite master) is where strikes, lots and
// ticks come from. FYERS names the same contracts by ticker. So the live feed
// subscribes by FYERS ticker and publishes under the Kite token, and this file
// is the one place the two are paired.
//
// THE PAIRING IS BY NAME, AND THE NAMES ARE THE SAME. NSE derivatives carry
// the same trading symbol at both brokers ("NIFTY26OCTFUT",
// "NIFTY26O0623850CE": underlying, YY, then MMM for a monthly or M DD for a
// weekly), and FYERS prefixes the exchange: "NSE:NIFTY26OCTFUT". Equities are
// "NSE:<symbol>-EQ" (config/universe_nifty50.csv carries each one), indices
// are a fixed three. FYERS' symbol-token call reports any ticker it does not
// know, and the feed prints them: a mismatch is visible on the first run, not
// a silent hole in the grid.
//
// WHAT IS STREAMED (all of it under 5000 symbols, FYERS' per-socket limit):
//   * NIFTY 50, NIFTY BANK, INDIA VIX;
//   * the nearest two NIFTY and BANKNIFTY futures;
//   * the nearest NIFTY and BANKNIFTY expiries, ATM +/- `strikes` (CE and PE);
//   * the NIFTY 50 stocks.
// Depth (5 levels) for the futures, the options within +/- `depth_strikes`
// and the stocks; indices have no book.
//
// ATM IS FIXED WHEN THE FEED STARTS, from the spot it is given. The chain
// shown is centred on the live spot inside that band, so +/- 20 subscribed
// leaves room for a 2-3 % day at either index; past that, restart the feed.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace altair::live {

enum class LiveKind : std::uint8_t { Index, Future, Call, Put, Equity };

[[nodiscard]] inline const char* kind_text(LiveKind k) noexcept {
    switch (k) {
    case LiveKind::Index: return "index";
    case LiveKind::Future: return "future";
    case LiveKind::Call: return "call";
    case LiveKind::Put: return "put";
    case LiveKind::Equity: return "equity";
    }
    return "?";
}

[[nodiscard]] inline std::optional<LiveKind> kind_from_text(const std::string& s) noexcept {
    if (s == "index") return LiveKind::Index;
    if (s == "future") return LiveKind::Future;
    if (s == "call") return LiveKind::Call;
    if (s == "put") return LiveKind::Put;
    if (s == "equity") return LiveKind::Equity;
    return std::nullopt;
}

struct LiveInstrument {
    std::uint32_t token = 0;     ///< Kite instrument_token: the identity on the wire
    std::string fyers;           ///< "NSE:NIFTY26OCTFUT"
    std::string symbol;          ///< Kite trading symbol, for display
    std::string underlying;      ///< "NIFTY", "BANKNIFTY", "SBIN"
    LiveKind kind = LiveKind::Index;
    std::int64_t expiry_day = 0; ///< days since 1970-01-01; 0 for none
    double strike = 0.0;         ///< rupees; 0 for none
    std::int64_t lot = 1;
    double tick = 0.05;          ///< rupees
    std::string group;           ///< market-watch group
    bool depth = false;          ///< subscribe the 5-level book
};

/// The three index tokens the desktop has always used.
inline constexpr std::uint32_t kLiveNiftyToken = 256265u;
inline constexpr std::uint32_t kLiveBankNiftyToken = 260105u;
inline constexpr std::uint32_t kLiveVixToken = 264969u;

[[nodiscard]] constexpr std::int64_t days_from_civil(int y, unsigned m, unsigned d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

/// "2026-10-27" to days since epoch; 0 when it is not a date.
[[nodiscard]] inline std::int64_t parse_day(const std::string& s) noexcept {
    if (s.size() < 10 || s[4] != '-' || s[7] != '-') return 0;
    const int y = std::atoi(s.substr(0, 4).c_str());
    const int m = std::atoi(s.substr(5, 2).c_str());
    const int d = std::atoi(s.substr(8, 2).c_str());
    if (y < 1990 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
    return days_from_civil(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
}

[[nodiscard]] inline std::string day_text(std::int64_t z) {
    if (z == 0) return {};
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const auto y = static_cast<int>(static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0));
    char buf[48];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", y, m, d);
    return buf;
}

/// One CSV line into fields, honouring double quotes (Kite quotes `name`).
[[nodiscard]] inline std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (char c : line) {
        if (c == '"') { quoted = !quoted; continue; }
        if (c == ',' && !quoted) { out.push_back(cur); cur.clear(); continue; }
        if (c != '\r') cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

/// One row of the Kite master, the fields this file reads.
struct LiveKiteRow {
    std::uint32_t token = 0;
    std::string symbol, name, type, segment, exchange;
    std::int64_t expiry_day = 0;
    double strike = 0.0, tick = 0.0;
    std::int64_t lot = 0;
};

[[nodiscard]] inline std::vector<LiveKiteRow> read_kite_master(const std::string& path, std::string& error) {
    std::vector<LiveKiteRow> rows;
    std::ifstream in(path);
    if (!in) { error = "cannot open " + path; return rows; }
    std::string line;
    if (!std::getline(in, line)) { error = path + " is empty"; return rows; }
    const auto head = split_csv(line);
    const auto col = [&head](const char* name) -> int {
        for (std::size_t i = 0; i < head.size(); ++i) if (head[i] == name) return static_cast<int>(i);
        return -1;
    };
    const int c_tok = col("instrument_token"), c_sym = col("tradingsymbol"), c_name = col("name"),
              c_exp = col("expiry"), c_str = col("strike"), c_tick = col("tick_size"), c_lot = col("lot_size"),
              c_type = col("instrument_type"), c_seg = col("segment"), c_ex = col("exchange");
    if (c_tok < 0 || c_sym < 0 || c_name < 0 || c_exp < 0 || c_str < 0 || c_tick < 0 || c_lot < 0
        || c_type < 0 || c_seg < 0 || c_ex < 0) {
        error = path + " is not a Kite instrument master (missing columns)";
        return rows;
    }
    const auto at = [](const std::vector<std::string>& f, int i) -> const std::string& {
        static const std::string empty;
        return i >= 0 && static_cast<std::size_t>(i) < f.size() ? f[static_cast<std::size_t>(i)] : empty;
    };
    while (std::getline(in, line)) {
        const auto f = split_csv(line);
        LiveKiteRow r;
        const unsigned long long tok = std::strtoull(at(f, c_tok).c_str(), nullptr, 10);
        if (tok == 0 || tok > 0xFFFFFFFFull) continue;
        r.token = static_cast<std::uint32_t>(tok);
        r.symbol = at(f, c_sym);
        r.name = at(f, c_name);
        r.type = at(f, c_type);
        r.segment = at(f, c_seg);
        r.exchange = at(f, c_ex);
        r.expiry_day = parse_day(at(f, c_exp));
        r.strike = std::atof(at(f, c_str).c_str());
        r.tick = std::atof(at(f, c_tick).c_str());
        r.lot = std::atoll(at(f, c_lot).c_str());
        rows.push_back(std::move(r));
    }
    return rows;
}

/// One stock of config/universe_nifty50.csv: symbol and FYERS ticker.
struct LiveStock { std::string symbol, fyers, sector; };

[[nodiscard]] inline std::vector<LiveStock> read_stock_universe(const std::string& path) {
    std::vector<LiveStock> out;
    std::ifstream in(path);
    std::string line;
    bool header = false;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!header) { header = true; continue; }
        const auto f = split_csv(line);
        if (f.size() >= 3 && !f[0].empty() && !f[1].empty()) out.push_back({f[0], f[1], f[2]});
    }
    return out;
}

struct LiveUniverseOptions {
    std::int64_t today = 0;             ///< days since epoch, IST
    double nifty_spot = 0.0, banknifty_spot = 0.0;
    int strikes = 20;                   ///< each side of ATM, subscribed
    int depth_strikes = 5;              ///< each side of ATM with the 5-level book
    int futures = 2;                    ///< nearest N futures per index
    bool stocks = true;
    bool stock_futures = true;          ///< the stocks' near and next futures (stat-arb trades and rolls them)
};

struct LiveUniverse {
    std::vector<LiveInstrument> instruments;
    std::vector<std::string> notes;     ///< what was skipped, and why
};

namespace detail {

inline void add_chain(const std::vector<LiveKiteRow>& rows, const std::string& under, double spot,
                      const LiveUniverseOptions& o, const std::string& group, LiveUniverse& rep) {
    std::int64_t expiry = 0;
    for (const auto& r : rows) {
        if (r.segment != "NFO-OPT" || r.name != under || r.expiry_day < o.today) continue;
        if (expiry == 0 || r.expiry_day < expiry) expiry = r.expiry_day;
    }
    if (expiry == 0) { rep.notes.push_back(under + ": no option expiry on or after today in the master"); return; }
    if (!(spot > 0.0)) { rep.notes.push_back(under + ": no spot to centre the chain on"); return; }
    std::set<double> strikes;
    for (const auto& r : rows)
        if (r.segment == "NFO-OPT" && r.name == under && r.expiry_day == expiry && r.strike > 0.0) strikes.insert(r.strike);
    const std::vector<double> ks(strikes.begin(), strikes.end());
    if (ks.empty()) return;
    std::size_t atm = 0;
    for (std::size_t i = 1; i < ks.size(); ++i)
        if (std::fabs(ks[i] - spot) < std::fabs(ks[atm] - spot)) atm = i;
    const auto lo = static_cast<std::ptrdiff_t>(atm) - o.strikes;
    const auto hi = static_cast<std::ptrdiff_t>(atm) + o.strikes;
    for (const auto& r : rows) {
        if (r.segment != "NFO-OPT" || r.name != under || r.expiry_day != expiry) continue;
        const auto it = std::lower_bound(ks.begin(), ks.end(), r.strike);
        if (it == ks.end() || *it != r.strike) continue;
        const auto idx = it - ks.begin();
        if (idx < lo || idx > hi) continue;
        LiveInstrument in;
        in.token = r.token;
        in.fyers = "NSE:" + r.symbol;
        in.symbol = r.symbol;
        in.underlying = under;
        in.kind = r.type == "CE" ? LiveKind::Call : LiveKind::Put;
        in.expiry_day = r.expiry_day;
        in.strike = r.strike;
        in.lot = r.lot;
        in.tick = r.tick;
        in.group = group;
        const auto away = idx - static_cast<std::ptrdiff_t>(atm);
        in.depth = away >= -o.depth_strikes && away <= o.depth_strikes;
        rep.instruments.push_back(std::move(in));
    }
}

} // namespace detail

[[nodiscard]] inline LiveUniverse build_universe(const std::vector<LiveKiteRow>& rows,
                                                   const std::vector<LiveStock>& stocks,
                                                   const LiveUniverseOptions& o) {
    LiveUniverse rep;
    const auto index = [&rep](std::uint32_t tok, const char* fy, const char* sym, const char* under) {
        LiveInstrument in;
        in.token = tok; in.fyers = fy; in.symbol = sym; in.underlying = under;
        in.kind = LiveKind::Index; in.group = "Indices"; in.lot = 1;
        rep.instruments.push_back(std::move(in));
    };
    index(kLiveNiftyToken, "NSE:NIFTY50-INDEX", "NIFTY 50", "NIFTY");
    index(kLiveBankNiftyToken, "NSE:NIFTYBANK-INDEX", "NIFTY BANK", "BANKNIFTY");
    index(kLiveVixToken, "NSE:INDIAVIX-INDEX", "INDIA VIX", "INDIAVIX");

    for (const char* under : {"NIFTY", "BANKNIFTY"}) {
        std::vector<const LiveKiteRow*> fut;
        for (const auto& r : rows)
            if (r.segment == "NFO-FUT" && r.name == under && r.expiry_day >= o.today) fut.push_back(&r);
        std::sort(fut.begin(), fut.end(), [](const LiveKiteRow* a, const LiveKiteRow* b) { return a->expiry_day < b->expiry_day; });
        if (fut.empty()) rep.notes.push_back(std::string{under} + ": no future on or after today in the master");
        for (std::size_t i = 0; i < fut.size() && static_cast<int>(i) < o.futures; ++i) {
            LiveInstrument in;
            in.token = fut[i]->token;
            in.fyers = "NSE:" + fut[i]->symbol;
            in.symbol = fut[i]->symbol;
            in.underlying = under;
            in.kind = LiveKind::Future;
            in.expiry_day = fut[i]->expiry_day;
            in.lot = fut[i]->lot;
            in.tick = fut[i]->tick;
            in.group = "Futures";
            in.depth = true;
            rep.instruments.push_back(std::move(in));
        }
    }
    detail::add_chain(rows, "NIFTY", o.nifty_spot, o, "NIFTY options", rep);
    detail::add_chain(rows, "BANKNIFTY", o.banknifty_spot, o, "BANKNIFTY options", rep);

    if (o.stocks) {
        std::map<std::string, const LiveKiteRow*> eq;
        for (const auto& r : rows)
            if (r.segment == "NSE" && r.exchange == "NSE" && r.type == "EQ") eq.emplace(r.symbol, &r);
        for (const auto& s : stocks) {
            const auto it = eq.find(s.symbol);
            if (it == eq.end()) { rep.notes.push_back(s.symbol + ": not an NSE equity in the master"); continue; }
            LiveInstrument in;
            in.token = it->second->token;
            in.fyers = s.fyers;
            in.symbol = s.symbol;
            in.underlying = s.symbol;
            in.kind = LiveKind::Equity;
            in.lot = 1;
            in.tick = it->second->tick > 0.0 ? it->second->tick : 0.05;
            in.group = "NIFTY 50";
            in.depth = true;
            rep.instruments.push_back(std::move(in));
        }
    }
    if (o.stocks && o.stock_futures) {
        std::map<std::string, std::vector<const LiveKiteRow*>> fut;
        for (const auto& r : rows)
            if (r.segment == "NFO-FUT" && r.expiry_day >= o.today) fut[r.name].push_back(&r);
        for (const auto& s : stocks) {
            auto it = fut.find(s.symbol);
            if (it == fut.end()) { rep.notes.push_back(s.symbol + ": no stock future in the master"); continue; }
            auto& v = it->second;
            std::sort(v.begin(), v.end(), [](const LiveKiteRow* a, const LiveKiteRow* b) { return a->expiry_day < b->expiry_day; });
            for (std::size_t k = 0; k < v.size() && k < 2; ++k) {
                LiveInstrument in;
                in.token = v[k]->token;
                in.fyers = "NSE:" + v[k]->symbol;
                in.symbol = v[k]->symbol;
                in.underlying = s.symbol;
                in.kind = LiveKind::Future;
                in.expiry_day = v[k]->expiry_day;
                in.lot = v[k]->lot;
                in.tick = v[k]->tick;
                in.group = "Stock futures";
                rep.instruments.push_back(std::move(in));
            }
        }
    }
    return rep;
}

inline constexpr const char* kLiveUniverseHeader = "token,fyers,symbol,underlying,kind,expiry,strike,lot,tick,group,depth";

[[nodiscard]] inline bool write_universe(const std::string& path, const std::vector<LiveInstrument>& u) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << kLiveUniverseHeader << '\n';
        char strike[32], tick[32];
        for (const auto& i : u) {
            std::snprintf(strike, sizeof strike, "%.2f", i.strike);
            std::snprintf(tick, sizeof tick, "%.2f", i.tick);
            f << i.token << ',' << i.fyers << ',' << i.symbol << ',' << i.underlying << ',' << kind_text(i.kind)
              << ',' << day_text(i.expiry_day) << ',' << strike << ',' << i.lot << ',' << tick << ',' << i.group
              << ',' << (i.depth ? 1 : 0) << '\n';
        }
        if (!f) return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

[[nodiscard]] inline std::vector<LiveInstrument> read_universe(const std::string& path) {
    std::vector<LiveInstrument> out;
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line) || line.rfind("token,", 0) != 0) return out;
    while (std::getline(in, line)) {
        const auto f = split_csv(line);
        if (f.size() < 11) continue;
        const auto k = kind_from_text(f[4]);
        if (!k) continue;
        LiveInstrument i;
        i.token = static_cast<std::uint32_t>(std::strtoul(f[0].c_str(), nullptr, 10));
        i.fyers = f[1]; i.symbol = f[2]; i.underlying = f[3]; i.kind = *k;
        i.expiry_day = parse_day(f[5]);
        i.strike = std::atof(f[6].c_str());
        i.lot = std::atoll(f[7].c_str());
        i.tick = std::atof(f[8].c_str());
        i.group = f[9];
        i.depth = f[10] == "1";
        if (i.token != 0) out.push_back(std::move(i));
    }
    return out;
}

/// The latest close in a dataset partition (dataset/spot/<sym>/1d/*.csv), or
/// 0. LATEST BY DATE, NOT BY FILE NAME: a partition can hold "all.csv" next to
/// "vendor_pre2015.csv", and the name that sorts last is the oldest data.
[[nodiscard]] inline double last_close(const std::string& dir) {
    std::error_code ec;
    std::string best_stamp;
    double best = 0.0;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".csv") continue;
        std::ifstream in(e.path());
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] < '0' || line[0] > '9') continue;
            const auto c = split_csv(line);
            if (c.size() < 5 || c[0] < best_stamp) continue;
            const double v = std::atof(c[4].c_str());
            if (v > 0.0) { best_stamp = c[0]; best = v; }
        }
    }
    return best;
}

/// Today in IST, as days since epoch.
[[nodiscard]] inline std::int64_t ist_today(std::int64_t unix_seconds) noexcept {
    return (unix_seconds + 19800) / 86400;
}

} // namespace altair::live
