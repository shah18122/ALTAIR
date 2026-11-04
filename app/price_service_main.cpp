// app/price_service_main.cpp -- one process owns the feed.
//
// P37-03. Every live thing in this tree so far is a subprocess that runs,
// writes a JSON file and exits: the tick feed samples for N seconds, the quote
// refresh fetches once, the account panel fetches once, the dataset updater
// tops up and stops. Four programs, four reads of the credential, and not one
// of them is a stream -- so the "Live Grid" has always been a replay and the
// depth ladder has been handed a null book since the day it was written.
//
// This is the stream. It holds ONE broker connection (FYERS) and republishes
// every tick
// on a local port as protocol frames. Anything that wants prices connects and
// reads; nothing else needs the credential.
//
// TWO SOURCES, ONE PATH -- HARD RULE 6.
//
//   --live --go       live from FYERS
//   --replay SYM IV   bars from dataset/, at their own timestamps
//
// Both go through the same encoder, the same bus and the same frames. That is
// the rule that says a backtest which does not share the live code path is a
// lie, and it is also what makes this testable at all: the FYERS token is a
// daily credential, and a service that could only be exercised with a live one
// would be exercised approximately never.
//
// A REPLAY FRAME SAYS SO. kPriceReplay is set, and engine_time carries the
// bar's ORIGINAL timestamp while server_time carries now -- so a subscriber
// that plots engine time sees 2015 and one that measures latency sees
// something absurd, which is exactly right. A replay must never be mistakable
// for a quiet afternoon.

#include <broker/fyers_data_socket.hpp>   // first: it settles the Windows header order
#include <server/price_bus.hpp>

#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <app/fyers_env_session.hpp>
#include <app/fyers_masters_fetch.hpp>
#include <app/fyers_price_frames.hpp>
#include <app/live_feed_sources.hpp>
#include <broker/fyers_api.hpp>
#include <broker/https_client.hpp>
#include <core/affinity.hpp>
#include <feed/fyers_hsm.hpp>
#include <feed/fyers_tbt.hpp>
#include <instruments/contract_spec.hpp>
#include <live/obi_underlyings.hpp>
#include <live/instrument_master.hpp>
#include <live/universe.hpp>

#include <atomic>
#include <csignal>
#include <optional>

namespace {

void usage(const char* exe) {
    std::printf(
        "Altair price service -- one process owns the feed.\n\n"
        "  %s --replay <symbol> <interval> [--port N] [--seconds N]\n"
        "  %s --go [same options as --live]\n"
        "  %s --fyers [--go] [--until HH:MM] [--strikes N] [--depth-strikes N]\n"
        "      [--no-stocks] [--atm-nifty X] [--atm-banknifty X] [--depth50 N|off]\n"
        "      [--expiries N] [--monthly]\n"
        "  %s --live [--go] [same options as --fyers]\n"
        "  %s --sim [--date YYYY-MM-DD] [--speed N] [--from HH:MM] [--seed N]\n\n"
        "  --fyers             the LIVE TERMINAL feed from FYERS: NIFTY, BANKNIFTY,\n"
        "                      INDIA VIX, the near futures, both option chains\n"
        "                      (ATM +/- --strikes, default 20; the nearest --expiries\n"
        "                      of each, default 1, up to 4; --monthly adds the\n"
        "                      nearest monthly when it is not among them) and the\n"
        "                      NIFTY 50, with\n"
        "                      quotes and depth: the FYERS 50-level book for the\n"
        "                      scrip on screen (data/live/depth_request.csv, written\n"
        "                      by the desktop), else the near futures: --depth50 N\n"
        "                      slots, default 1 (FYERS Standard; Prime allows 5 a\n"
        "                      connection on up to three connections: --depth50 15\n"
        "                      fills the BANKNIFTY and NYKAA futures and their strikes\n"
        "                      nearest the money); off: the 5-level book only. NSE equity\n"
        "                      and F&O only. Needs data/fyers_session.json\n"
        "                      (altair_fyers_login) or ALTAIR_FYERS_CLIENT_ID and\n"
        "                      ALTAIR_FYERS_ACCESS_TOKEN. Without --go: prints the\n"
        "                      universe and exits. Runs until --until (default 15:35\n"
        "                      IST), reconnecting after a drop.\n"
        "  --live              FYERS, when its session is good today. The desktop's\n"
        "                      \"Start live feed\" runs this.\n"
        "  --sim               the same universe, SIMULATED and flagged SIM on every\n"
        "                      frame: the terminal and the live models with the\n"
        "                      market shut. --speed: simulated seconds per second\n"
        "                      (default 1); --from: the session time to start at\n"
        "                      (default 09:15). --date: simulate that day -- the\n"
        "                      previous closes are the session before it, and when\n"
        "                      dataset/ holds that day's 1-minute bars, NIFTY,\n"
        "                      BANKNIFTY and INDIA VIX follow them.\n"
        "  --replay nifty 1m   serve bars from dataset/spot/<symbol>/<iv>/\n"
        "  --go                serve LIVE (alone, the same as --live --go)\n"
        "  --port N            loopback port to publish on (default 7421)\n"
        "  --seconds N         stop after N seconds (default: run until\n"
        "                      interrupted; a replay always stops at the end)\n"
        "  --rate N            replay bars per second (default 200)\n"
        "  --live-dir DIR      where universe.csv, watchlist.csv and feed_status.json\n"
        "                      live (default data/live in the source tree; tests\n"
        "                      point it elsewhere)\n"
        "  --master FILE       the instrument master (default data/instruments.csv in\n"
        "                      the source tree; tests pass a fixture)\n"
        "  --tail N            replay only the most recent N bars. A\n"
        "                      replay otherwise starts at the first bar on\n"
        "                      disk -- 2015 for NIFTY 1-minute -- and would\n"
        "                      price a 2026 chain around a 2015 spot.\n\n"
        "WITHOUT --go OR --replay THIS PRINTS THIS TEXT AND EXITS. The same\n"
        "dry-run guard every other network binary here has: a program that\n"
        "opens a credentialed socket because it was run with no arguments is\n"
        "a program that will one day do it by accident.\n\n"
        "It binds 127.0.0.1 only. This carries live market data derived from\n"
        "a trading credential and protocol.hpp's session layer does not exist\n"
        "yet, so there is nothing to authenticate a remote reader with.\n",
        exe, exe, exe, exe, exe);
}

/// Bars from a dataset partition, oldest first.
struct Bar {
    std::int64_t ts_ns = 0;
    double close = 0.0;
    double volume = 0.0;
    bool has_volume = false;
};

/// Parse "2026-09-09T15:29:00+05:30" or "2026-09-09" to ns since epoch.
///
/// Only the fields that exist are read: a daily stamp has no time, and
/// inventing 00:00 for it would be inventing a bar that opened at midnight.
/// The offset is IST throughout the dataset, so it is subtracted rather than
/// parsed -- the day the tree holds a non-IST series this must be revisited,
/// and it will be visible because the stamps will be wrong by hours.
[[nodiscard]] std::int64_t parse_stamp_ns(const std::string& s) {
    if (s.size() < 10) { return 0; }
    std::tm tm{};
    tm.tm_year = std::atoi(s.substr(0, 4).c_str()) - 1900;
    tm.tm_mon = std::atoi(s.substr(5, 2).c_str()) - 1;
    tm.tm_mday = std::atoi(s.substr(8, 2).c_str());
    if (s.size() >= 19 && s[10] == 'T') {
        tm.tm_hour = std::atoi(s.substr(11, 2).c_str());
        tm.tm_min = std::atoi(s.substr(14, 2).c_str());
        tm.tm_sec = std::atoi(s.substr(17, 2).c_str());
    }
    // Days since epoch by civil-from-days (Howard Hinnant's algorithm), so
    // there is no dependence on the host time zone -- std::mktime would apply
    // whatever the machine is set to, which is how a dataset becomes a
    // function of the laptop that read it.
    const int y = tm.tm_year + 1900;
    const int m = tm.tm_mon + 1;
    const int d = tm.tm_mday;
    const int yy = y - (m <= 2 ? 1 : 0);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(yy - era * 400);
    const unsigned doy =
        static_cast<unsigned>((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days =
        static_cast<std::int64_t>(era) * 146097
        + static_cast<std::int64_t>(doe) - 719468;
    const std::int64_t secs = days * 86400 + tm.tm_hour * 3600
                              + tm.tm_min * 60 + tm.tm_sec
                              - (5 * 3600 + 30 * 60);   // IST -> UTC
    return secs * 1'000'000'000LL;
}

[[nodiscard]] std::vector<Bar> load_bars(const std::string& dir) {
    std::vector<Bar> out;
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".csv") { files.push_back(e.path().string()); }
    }
    std::sort(files.begin(), files.end());
    for (const std::string& f : files) {
        std::ifstream in(f);
        if (!in) { continue; }
        std::string line;
        std::getline(in, line);                       // header
        while (std::getline(in, line)) {
            std::istringstream ss(line);
            std::string cell;
            int col = 0;
            Bar b;
            std::string stamp;
            while (std::getline(ss, cell, ',')) {
                if (col == 0) { stamp = cell; }
                if (col == 4 && !cell.empty()) { b.close = std::atof(cell.c_str()); }
                if (col == 5 && !cell.empty()) {
                    // ABSENT IS NOT ZERO. An index reports no volume at all
                    // and the ingested files leave the field EMPTY for
                    // exactly that reason -- a zero here would be a measured
                    // zero and the wire would carry it as one.
                    b.volume = std::atof(cell.c_str());
                    b.has_volume = true;
                }
                ++col;
            }
            b.ts_ns = parse_stamp_ns(stamp);
            if (b.close > 0.0 && b.ts_ns > 0) { out.push_back(b); }
        }
    }
    return out;
}

// ---- the live terminal's sources ----------------------------------------

std::atomic<bool> g_stop{false};
extern "C" void on_stop_signal(int) { g_stop.store(true); }
/// How many instruments hold the FYERS 50-level book at once (--depth50 N;
/// 0 = off). FYERS allows 1 on a Standard plan and 5 a connection, three
/// connections, on Prime: up to 15, one socket and thread a connection.
std::size_t g_depth50 = altair::fyers_tbt::kTbtStandardSlots;
/// The OBI trader's underlyings (BANKNIFTY and config/obi_underlyings.csv)
/// and the level each chain was centred on: their near futures, then their
/// strikes nearest the money, are the 50-level defaults.
std::vector<std::string> g_obi_unders;
std::unordered_map<std::string, double> g_obi_spot;
/// Where the desktop says which scrip is on screen (data/live/depth_request.csv).
std::string g_depth_request;
/// The order-book imbalance trader's watch list (data/live/obi_watch.csv):
/// asked for the 50-level slots after the scrip on screen.
std::string g_obi_watch;
/// Cores and priorities for this process's threads (config/latency.toml).
altair::latency::Plan g_latency;

/// "HH:MM" to minutes after midnight; -1 when it is not a time.
[[nodiscard]] int parse_hhmm(const std::string& s) {
    if (s.size() != 5 || s[2] != ':') { return -1; }
    const int h = std::atoi(s.substr(0, 2).c_str());
    const int m = std::atoi(s.substr(3, 2).c_str());
    if (h < 0 || h > 23 || m < 0 || m > 59) { return -1; }
    return h * 60 + m;
}

[[nodiscard]] std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/// Unix seconds of `minute` IST on IST day `day`.
[[nodiscard]] std::int64_t ist_unix(std::int64_t day, int minute) {
    return day * 86400 + static_cast<std::int64_t>(minute) * 60 - 19800;
}

struct FySession { std::string client, access; };

/// data/fyers_session.json, read by the same eight-line scan as the other
/// FYERS helpers; then the environment. Never printed.
[[nodiscard]] std::optional<FySession> fyers_session(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (in) {
        const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto field = [&all](const char* key) -> std::string {
            const std::string k = std::string("\"") + key + "\"";
            const std::size_t at = all.find(k);
            if (at == std::string::npos) { return {}; }
            const std::size_t q1 = all.find('"', all.find(':', at) + 1);
            if (q1 == std::string::npos) { return {}; }
            const std::size_t q2 = all.find('"', q1 + 1);
            return q2 == std::string::npos ? std::string{} : all.substr(q1 + 1, q2 - q1 - 1);
        };
        FySession s{field("client_id"), field("access_token")};
        if (!s.client.empty() && !s.access.empty() && s.client.size() <= altair::fyers::kClientIdMax
            && s.access.size() <= altair::fyers::kAccessTokenMax) {
            return s;
        }
    }
    if (auto env = altair::fyers_env::from_environment()) { return FySession{env->client, env->access}; }
    return std::nullopt;
}

/// The two index values right now, from FYERS /data/quotes, to centre the
/// option chains on. 0 for either one that did not come back.
void fyers_spots(const FySession& s, double& nifty, double& banknifty) {
    nifty = 0.0;
    banknifty = 0.0;
    const auto auth = altair::fyers::authorization_header(s.client.c_str(), s.access.c_str());
    const auto target = altair::fyers::quotes_target(
        std::vector<std::string>{"NSE:NIFTY50-INDEX", "NSE:NIFTYBANK-INDEX"});
    if (!auth || !target) { return; }
    const auto r = altair::https_get_auth("api-t1.fyers.in", *target, *auth, std::chrono::seconds{20});
    if (!r || r->status != 200) { return; }
    const auto lp_after = [&r](const char* name) {
        const std::size_t at = r->body.find(name);
        if (at == std::string::npos) { return 0.0; }
        const std::size_t lp = r->body.find("\"lp\":", at);
        return lp == std::string::npos ? 0.0 : std::atof(r->body.c_str() + lp + 5);
    };
    nifty = lp_after("\"NSE:NIFTY50-INDEX\"");
    banknifty = lp_after("\"NSE:NIFTYBANK-INDEX\"");
}

/// The last price of each FYERS symbol of `syms` (0 where FYERS gave none).
std::vector<double> fyers_last_prices(const FySession& s, const std::vector<std::string>& syms) {
    std::vector<double> out(syms.size(), 0.0);
    if (syms.empty()) { return out; }
    const auto auth = altair::fyers::authorization_header(s.client.c_str(), s.access.c_str());
    const auto target = altair::fyers::quotes_target(syms);
    if (!auth || !target) { return out; }
    const auto r = altair::https_get_auth("api-t1.fyers.in", *target, *auth, std::chrono::seconds{20});
    if (!r || r->status != 200) { return out; }
    for (std::size_t i = 0; i < syms.size(); ++i) {
        const std::size_t at = r->body.find("\"" + syms[i] + "\"");
        if (at == std::string::npos) { continue; }
        const std::size_t lp = r->body.find("\"lp\":", at);
        if (lp != std::string::npos) { out[i] = std::atof(r->body.c_str() + lp + 5); }
    }
    return out;
}

/// The FYERS 50-level book (feed/fyers_tbt.hpp), on its own socket and
/// thread, for the few instruments FYERS allows at once -- the scrip on
/// screen first (data/live/depth_request.csv, written by the desktop), then
/// the near futures. Its books replace the HSM's five levels on the bus:
/// while one is fresh, the HSM book for that instrument is not published, so
/// the two never alternate on screen.
struct TbtFeed {
    static constexpr std::size_t kSlots = altair::fyers_tbt::kTbtSlotsPerConnection;
    TbtFeed() = default;
    ~TbtFeed() {
        stop.store(true);
        if (th.joinable()) th.join();
    }
    /// Whether `token`'s 50-level book was published in the last 5 seconds.
    [[nodiscard]] bool fresh(std::uint32_t token) const {
        for (std::size_t k = 0; k < kSlots; ++k) {
            if (tok[k].load(std::memory_order_relaxed) != token || token == 0) continue;
            const std::int64_t t = last_ns[k].load(std::memory_order_relaxed);
            if (t > 0 && steady_ns() - t < 5'000'000'000LL) return true;
        }
        return false;
    }
    /// A slot handed to another symbol: its old token's book is not fresh.
    void release(std::size_t slot) {
        if (slot < kSlots) last_ns[slot].store(0, std::memory_order_relaxed);
    }
    void mark(std::size_t slot, std::uint32_t token) {
        if (slot >= kSlots) return;
        tok[slot].store(token, std::memory_order_relaxed);
        last_ns[slot].store(steady_ns(), std::memory_order_relaxed);
    }
    [[nodiscard]] static std::int64_t steady_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::array<std::atomic<std::uint32_t>, kSlots> tok{};
    std::array<std::atomic<std::int64_t>, kSlots> last_ns{};   ///< per slot: the last 50-level publish (steady)
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> books{0};
    std::thread th;
};

/// One TbtFeed a FYERS connection (three on Prime).
struct TbtFeeds {
    std::array<TbtFeed, altair::fyers_tbt::kTbtPrimeConnections> feeds;
    [[nodiscard]] bool fresh(std::uint32_t token) const {
        return std::any_of(feeds.begin(), feeds.end(), [token](const TbtFeed& f) { return f.fresh(token); });
    }
};

/// data/live/depth_request.csv: one "<fyers symbol>,<token>" a line, the scrip
/// on screen first. Missing or unreadable: no request.
[[nodiscard]] std::vector<std::pair<std::string, std::uint32_t>> read_depth_request(const std::string& path) {
    std::vector<std::pair<std::string, std::uint32_t>> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line) && out.size() < 64) {
        const auto c = line.find(',');
        if (c == std::string::npos || c == 0) continue;
        const unsigned long t = std::strtoul(line.c_str() + c + 1, nullptr, 10);
        if (t > 0 && t <= 0xFFFFFFFFul) out.push_back({line.substr(0, c), static_cast<std::uint32_t>(t)});
    }
    return out;
}

void run_fyers_tbt(const FySession& session, const std::vector<altair::live::LiveInstrument>& u,
                   altair::live_sources::SharedBus& bus, std::int64_t deadline_unix, TbtFeed& feed, std::size_t conn) {
    namespace tbt = altair::fyers_tbt;
    using altair::live::LiveKind;
    const auto auth = altair::fyers::authorization_header(session.client.c_str(), session.access.c_str());
    if (!auth) return;
    // The defaults -- what the slots hold when the desktop asks for nothing:
    // the OBI underlyings' near futures, then their strikes nearest the money
    // (calls and puts), then the other futures, the other options, equities.
    std::vector<std::size_t> idx;
    for (std::size_t i = 0; i < u.size(); ++i)
        if (u[i].depth && u[i].kind != LiveKind::Index) idx.push_back(i);
    std::unordered_map<std::string, std::int64_t> near_expiry;   // each OBI underlying's nearest future
    for (const std::size_t i : idx)
        if (u[i].kind == LiveKind::Future && g_obi_spot.contains(u[i].underlying)) {
            auto [it, fresh] = near_expiry.emplace(u[i].underlying, u[i].expiry_day);
            if (!fresh && u[i].expiry_day < it->second) it->second = u[i].expiry_day;
        }
    const auto rank = [&](std::size_t i) -> std::pair<int, double> {
        const auto& in = u[i];
        const auto spot = g_obi_spot.find(in.underlying);
        const bool obi = spot != g_obi_spot.end();
        // Near futures in the OBI list's order, BANKNIFTY first: with FYERS
        // Standard's single slot the 50-level book is always BANKNIFTY's.
        if (in.kind == LiveKind::Future) {
            const auto at = std::find(g_obi_unders.begin(), g_obi_unders.end(), in.underlying);
            return {obi && near_expiry[in.underlying] == in.expiry_day ? 0 : 2,
                    static_cast<double>(at - g_obi_unders.begin())};
        }
        if (in.kind == LiveKind::Call || in.kind == LiveKind::Put) return {obi ? 1 : 3, obi ? std::fabs(in.strike - spot->second) : 0.0};
        return {4, 0.0};
    };
    std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return rank(a) < rank(b); });
    std::vector<std::string> defaults;
    std::unordered_map<std::string, std::uint32_t> token_of;   // every symbol the feed knows
    std::vector<std::uint32_t> currency;                       // "NSE:" names that are not NSE F&O
    for (const auto& in : u) {
        if (in.group == altair::live::kCurrencyGroup) { currency.push_back(in.token); continue; }
        if (in.kind != LiveKind::Index) token_of.emplace(in.fyers, in.token);
    }
    for (const std::size_t i : idx) defaults.push_back(u[i].fyers);
    // RULE 11: refused, not truncated silently -- five a connection, three connections.
    const std::size_t total = std::min(g_depth50, TbtFeed::kSlots * altair::fyers_tbt::kTbtPrimeConnections);
    if (conn == 0 && g_depth50 > total)
        std::printf("  50-level book: --depth50 %zu asks for more than three FYERS connections hold; %zu slots\n", g_depth50, total);
    // This connection's share of the plan: slots [5 conn, 5 conn + 5).
    const std::size_t first = conn * TbtFeed::kSlots;
    const std::size_t slots = first < total ? std::min(TbtFeed::kSlots, total - first) : 0;
    if (slots == 0) return;
    // What is asked for: the desktop's scrip first, then the OBI watch list.
    using Stamp = std::pair<std::filesystem::file_time_type, std::filesystem::file_time_type>;
    const auto stamp = [] {
        std::error_code a, b;
        const auto t1 = std::filesystem::last_write_time(g_depth_request, a);
        const auto t2 = std::filesystem::last_write_time(g_obi_watch, b);
        return Stamp{a ? std::filesystem::file_time_type{} : t1, b ? std::filesystem::file_time_type{} : t2};
    };
    const auto read_requests = [&] {
        auto r = read_depth_request(g_depth_request);
        for (const auto& sym : altair::live::read_obi_watch(g_obi_watch))
            if (const auto it = token_of.find(sym); it != token_of.end()) r.emplace_back(sym, it->second);
        return r;
    };
    std::vector<std::pair<std::string, std::uint32_t>> request = read_requests();
    Stamp request_time = stamp();
    const auto plan = [&]() {
        std::vector<std::string> asked;
        for (const auto& [sym, tok] : request) {
            if (std::find(currency.begin(), currency.end(), tok) != currency.end()) continue;   // no 50 levels for currency
            asked.push_back(sym);
            token_of.emplace(sym, tok);
        }
        // Every connection plans the whole list from the same files and keeps
        // its own slice, so the connections never hold the same symbol twice
        // (for a second at most, while one has read a changed file and
        // another has not).
        const auto all = tbt::tbt_plan_slots(asked, defaults, total, [&](const std::string& x) { return token_of.contains(x); });
        std::vector<std::string> mine;
        for (std::size_t k = first; k < all.size() && k < first + slots; ++k) mine.push_back(all[k]);
        return mine;
    };
    std::vector<std::string> held = plan();
    // Nothing that can hold the book yet (no depth instruments, no request):
    // wait for the desktop to put an NSE scrip on screen.
    while (held.empty()) {
        for (int k = 0; k < 2; ++k) {
            if (feed.stop.load() || g_stop.load() || unix_now() >= deadline_unix) return;
            std::this_thread::sleep_for(std::chrono::seconds{1});
        }
        const Stamp t = stamp();
        if (t == request_time) continue;
        request_time = t;
        request = read_requests();
        held = plan();
    }

    std::string host = tbt::kTbtDefaultHost, path = tbt::kTbtDefaultPath;
    if (const auto r = altair::https_get_auth(tbt::kTbtUrlHost, tbt::kTbtUrlPath, *auth, std::chrono::seconds{15});
        r && r->status == 200) {
        if (const auto url = tbt::tbt_socket_url(r->body)) { host = url->first; path = url->second; }
    }
    const auto bytes = [](const std::string& t) { return altair::FyersFrame(t.begin(), t.end()); };
    const auto say_held = [&] {
        std::string names;
        for (const auto& h : held) names += (names.empty() ? "" : ", ") + h;
        std::printf("  50-level book, connection %zu: %zu slot(s) on %s (FYERS: NSE equity and F&O only; 1 symbol on Standard,"
                    " 5 a connection, three connections, on Prime; --depth50)\n", conn + 1, slots, names.c_str());
        std::fflush(stdout);
    };
    say_held();

    tbt::TbtBooks books;
    int server_errors = 0;
    const auto on_frame = [&](const std::uint8_t* p, std::size_t n, std::vector<altair::FyersFrame>&) -> bool {
        tbt::TbtMessage m;
        if (books.on_message(p, n, m)) {
            if (m.error) {
                // RULE 11: the first few reasons are printed; the rest are counted.
                if (++server_errors <= 5) std::printf("  50-level book: FYERS says: %s\n", m.text.c_str());
                std::fflush(stdout);
            }
            const std::int64_t recv = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            for (const auto& t : m.updated) {
                const auto slot = static_cast<std::size_t>(std::find(held.begin(), held.end(), t) - held.begin());
                const auto it = token_of.find(t);
                const tbt::TbtBook* b = books.book(t);
                if (slot >= held.size() || it == token_of.end() || b == nullptr) continue;   // a book from before a switch
                const std::size_t levels = std::min(b->depth(), altair::kMaxDepthLevels);
                if (levels == 0) continue;
                altair::PricePayload pp;
                pp.token = it->second;
                pp.flags |= altair::kPriceHasBook;
                pp.depth_levels = static_cast<std::uint16_t>(levels);
                pp.exchange_ts_ns = recv;
                altair::PriceLevel bids[altair::kMaxDepthLevels]{};
                altair::PriceLevel asks[altair::kMaxDepthLevels]{};
                for (std::size_t k = 0; k < levels; ++k) {
                    bids[k] = {b->bid_px[k], b->bid_qty[k], b->bid_orders[k], 0};
                    asks[k] = {b->ask_px[k], b->ask_qty[k], b->ask_orders[k], 0};
                }
                bus.second_book(pp, bids, asks, recv);   // this thread is the bus's second producer
                feed.mark(slot, it->second);
                feed.books.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return !feed.stop.load() && !g_stop.load() && unix_now() < deadline_unix;
    };
    altair::FyersSocketOptions opt;
    opt.host = host;
    opt.path = path;
    opt.authorization = *auth;
    opt.text = true;
    opt.stop = [&feed] { return feed.stop.load() || g_stop.load(); };
    // Every second: has the desktop put another scrip on screen? Then the
    // slots move to it -- unsubscribe what left, subscribe what came.
    opt.poll = [&](std::vector<altair::FyersFrame>& out) {
        const Stamp t = stamp();
        if (t == request_time) return;
        request_time = t;
        request = read_requests();
        const auto next = plan();
        if (next.empty() || next == held) return;
        for (const auto& f : tbt::tbt_switch_texts(held, next, 1)) out.push_back(bytes(f));
        for (std::size_t k = 0; k < TbtFeed::kSlots; ++k)
            if (k >= next.size() || k >= held.size() || next[k] != held[k]) feed.release(k);
        held = next;
        say_held();
    };
    const std::string ping = "ping";
    for (long attempt = 0; !feed.stop.load() && !g_stop.load(); ++attempt) {
        const std::int64_t left_s = deadline_unix - unix_now();
        if (left_s < 1) break;
        books.reset();
        const std::vector<altair::FyersFrame> opening{bytes(tbt::tbt_subscribe_text(held, 1)), bytes(tbt::tbt_resume_text(1))};
        // The book can be quiet for minutes (a closed market): never "idle".
        const auto run = altair::fyers_data_socket_run(opening, on_frame, bytes(ping), std::chrono::seconds{tbt::kTbtPingSeconds},
                                                       std::chrono::seconds{left_s}, std::chrono::seconds{left_s}, opt);
        if (feed.stop.load() || g_stop.load()) break;
        if (!run && (attempt < 3 || attempt % 20 == 0)) {
            std::printf("  50-level book: %s; the 5-level book stays (retrying)\n", altair::fyers_socket_error_text(run.error()));
            std::fflush(stdout);
        }
        // RULE 11: safe-side clamp -- back-off grows to 30 s and stops there.
        const long backoff = attempt < 4 ? (2L << attempt) : 30L;
        for (long s2 = 0; s2 < backoff && !feed.stop.load() && !g_stop.load(); ++s2) std::this_thread::sleep_for(std::chrono::seconds{1});
    }
}

/// FYERS's cash, currency and MCX symbol masters, cached under `live_dir`/fyers_masters and
/// fetched again when older than 20 hours (live/fyers_names.hpp says why the
/// names are looked up, not built). Public files: no login, no credential. A
/// download that does not work keeps the copy on disk; with none, the names
/// are guessed and the feed names any FYERS refuses.
[[nodiscard]] altair::live::FyersCashNames fyers_cash_names(const std::string& live_dir, bool fetch) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(live_dir) / "fyers_masters";
    if (fetch) { altair::fyers_masters::download(dir); }
    altair::live::FyersCashNames names;
    const std::size_t n = names.load(dir.string());
    std::printf("FYERS names (cash, currency, MCX): %zu scrip(s) from %s\n", n, dir.string().c_str());
    return names;
}

/// Stream the universe from FYERS until `deadline_unix` or Ctrl+C.
/// Returned by run_fyers when the market watch added scrips: the
/// caller rebuilds the universe and reconnects with them.
inline constexpr int kRestartForWatchlist = 10;

/// FYERS symbol tokens for `symbols` -> (symbol, HSM token) pairs, in chunks
/// the symbol-token call accepts. Unknown symbols are counted in `invalid`.
/// Empty with `why` set when the call itself failed.
[[nodiscard]] inline std::vector<std::pair<std::string, std::string>>
fyers_symbol_tokens(const FySession& session, const std::vector<std::string>& symbols, std::string& why,
                    std::vector<std::string>& invalid) {
    namespace hsm = altair::fyers_hsm;
    std::vector<std::pair<std::string, std::string>> out;
    const auto bare = std::string{hsm::bare_token(session.access)};
    const auto rest_auth = altair::fyers::authorization_header(session.client.c_str(), session.access.c_str());
    constexpr std::size_t kChunk = 50;
    for (std::size_t at = 0; at < symbols.size(); at += kChunk) {
        std::string body = "{\"symbols\":[";
        for (std::size_t i = at; i < symbols.size() && i < at + kChunk; ++i) {
            if (i > at) { body.push_back(','); }
            body += "\"" + symbols[i] + "\"";
        }
        body += "]}";
        std::optional<hsm::SymbolTokens> tokens;
        for (int attempt = 0; attempt < 2 && !tokens; ++attempt) {
            const std::string auth = attempt == 0 ? bare : (rest_auth ? *rest_auth : std::string{});
            if (auth.empty()) { break; }
            const auto response = altair::https_post_json(hsm::kSymbolTokenHost, hsm::kSymbolTokenPath, body,
                                                          std::chrono::seconds{20}, auth);
            if (!response) { why = "symbol-token request: transport failed"; continue; }
            auto parsed = hsm::parse_symbol_tokens(response->body);
            if (parsed) { tokens = std::move(*parsed); break; }
            why = "symbol-token request: HTTP " + std::to_string(response->status) + ", "
                + hsm::error_text(parsed.error());
        }
        if (!tokens) { return {}; }
        for (const auto& bad : tokens->invalid) { invalid.push_back(bad); }
        for (const auto& [symbol, fytoken] : tokens->valid) { out.emplace_back(symbol, fytoken); }
    }
    why.clear();
    return out;
}

/// Stream the universe from FYERS until `deadline_unix` or Ctrl+C.
///
/// SCRIPS ADDED WHILE STREAMING ARE SUBSCRIBED ON THE SAME SOCKET. `take_added`
/// is asked every couple of seconds for instruments the market watch or an
/// opened option chain asked for; their FYERS symbol tokens are looked up on a
/// helper thread (an HTTPS call must not stall the socket), and the socket
/// thread maps and subscribes them on its next frame. Nothing reconnects, so
/// nothing else misses a tick. They are re-subscribed with the rest after any
/// reconnect. (The 50-level TBT book keeps the universe it started with.)
int run_fyers(const FySession& session, const std::vector<altair::live::LiveInstrument>& u,
              altair::live_sources::SharedBus& bus, std::int64_t deadline_unix, const std::string& status_path,
              const std::function<std::vector<altair::live::LiveInstrument>()>& take_added = {}) {
    namespace hsm = altair::fyers_hsm;
    using altair::live_sources::FeedStatus;
    FeedStatus st;
    st.source = "fyers";
    st.state = "connecting";
    st.instruments = u.size();
    altair::live_sources::write_status(status_path, st);

    const auto hsm_key = hsm::hsm_key_from_token(session.access, unix_now());
    if (!hsm_key) {
        st.state = "refused";
        st.error = hsm::error_text(hsm_key.error());
        altair::live_sources::write_status(status_path, st);
        std::printf("  %s\n", st.error.c_str());
        return 2;
    }

    // symbols -> HSM topics, in chunks the symbol-token call accepts.
    std::unordered_map<std::string, std::size_t> index_of;
    for (std::size_t i = 0; i < u.size(); ++i) { index_of.emplace(u[i].fyers, i); }
    hsm::HsmSession decoder{false};
    // Symbol (sf/if) and depth (dp) topics are subscribed in SEPARATE frames,
    // symbols first and depth a second later -- as the official SDK does (one
    // data type per subscribe call, half a second apart). Mixing a scrip's sf
    // and dp topics in one frame is something the SDK never does, and with it
    // futures and stocks showed a book but no price on a live FYERS feed; the
    // 30-second report below names any instrument that still has no price.
    std::vector<std::string> sym_topics, depth_topics;
    std::vector<std::uint8_t> subscribed(u.size(), 0);   // cookie -> 1 when it has a topic
    {
        std::vector<std::string> symbols;
        symbols.reserve(u.size());
        for (const auto& in : u) symbols.push_back(in.fyers);
        std::string why;
        std::vector<std::string> invalid;
        const auto valid = fyers_symbol_tokens(session, symbols, why, invalid);
        if (!why.empty()) {
            st.state = "refused";
            st.error = why;
            altair::live_sources::write_status(status_path, st);
            std::printf("  %s\n", why.c_str());
            return 3;
        }
        for (const auto& bad : invalid) {
            ++st.unknown_symbols;
            st.unknown.push_back(bad);
            std::printf("    FYERS does not know %s -- not streamed\n", bad.c_str());
        }
        for (const auto& [symbol, fytoken] : valid) {
            const auto it = index_of.find(symbol);
            if (it == index_of.end()) { continue; }
            const auto cookie = static_cast<std::uint32_t>(it->second);
            for (int pass = 0; pass < (u[it->second].depth ? 2 : 1); ++pass) {
                const auto type = pass == 0 ? hsm::DataType::SymbolUpdate : hsm::DataType::DepthUpdate;
                auto topic = hsm::topic_for(symbol, fytoken, type);
                if (topic.empty()) { continue; }
                decoder.map_topic(topic, symbol, cookie);
                (pass == 0 ? sym_topics : depth_topics).push_back(std::move(topic));
                subscribed[it->second] = 1;
            }
        }
    }
    if (sym_topics.empty() && depth_topics.empty()) {
        st.state = "refused";
        st.error = "nothing subscribable";
        altair::live_sources::write_status(status_path, st);
        std::printf("  nothing subscribable\n");
        return 3;
    }
    const std::string source{hsm::kDefaultSource};
    // RULE 11: chunking, not truncation -- every topic is sent, kSubscribeChunk at a time.
    const auto make_frames = [&](const std::vector<std::string>& t, std::vector<hsm::Bytes>& out) -> bool {
        for (std::size_t i = 0; i < t.size(); i += hsm::kSubscribeChunk) {
            const std::size_t n = t.size() - i < hsm::kSubscribeChunk ? t.size() - i : hsm::kSubscribeChunk;
            const auto frame = hsm::topics_frame(true, std::span<const std::string>{t.data() + i, n},
                                                 hsm::kDefaultChannel, session.client.size() + 1 + session.access.size(),
                                                 source.size());
            if (!frame) { std::printf("  %s\n", hsm::error_text(frame.error())); return false; }
            out.push_back(*frame);
        }
        return true;
    };
    std::vector<hsm::Bytes> subscribe, subscribe_depth;   // all of them again after a reconnect
    if (!make_frames(sym_topics, subscribe) || !make_frames(depth_topics, subscribe_depth)) return 3;
    std::vector<hsm::Bytes> depth_queue;                  // depth frames waiting for their second
    auto depth_due = std::chrono::steady_clock::now();
    std::printf("  subscribing %zu symbol and %zu depth topic(s) for %zu instrument(s)\n", sym_topics.size(),
                depth_topics.size(), u.size());
    std::fflush(stdout);

    TbtFeeds tbt;
    for (std::size_t c = 0; c < tbt.feeds.size(); ++c)
        if (g_depth50 > c * TbtFeed::kSlots)
            tbt.feeds[c].th = std::thread([&, c] {
                std::printf("  %s\n", altair::latency::apply_thread(g_latency, "tbt").c_str());
                run_fyers_tbt(session, u, bus, deadline_unix, tbt.feeds[c], c);
            });
    std::vector<altair::fyers_frames::LastTrade> last(u.size());
    // Instruments added while streaming: cookies u.size() + k. Deques, so a
    // reference handed to the decoder's callback never moves.
    std::deque<altair::live::LiveInstrument> extra;
    std::deque<altair::fyers_frames::LastTrade> extra_last;
    // What each instrument has had: bit 0 a price (trade or quote), bit 1 a book.
    std::vector<std::uint8_t> seen(u.size(), 0);
    std::deque<std::uint8_t> extra_seen;
    std::uint64_t n_sym = 0, n_depth = 0;   // HSM updates by kind
    auto streaming_since = std::chrono::steady_clock::time_point{};
    auto last_report = std::chrono::steady_clock::now();
    // Symbol-token lookups for them, done off the socket thread.
    struct Looked { altair::live::LiveInstrument in; std::string fytoken; };
    std::mutex looked_mu;
    std::vector<Looked> looked;
    std::vector<std::string> refused;   // names FYERS did not know, for the status file
    std::vector<std::thread> lookups;
    altair::fyers_frames::Frames f;
    bool auth_rejected = false;
    const auto emit = [&](const hsm::HsmUpdate& up) {
        const std::size_t n = u.size() + extra.size();
        if (up.cookie >= n) { return; }
        const bool base = up.cookie < u.size();
        const altair::live::LiveInstrument& inst = base ? u[up.cookie] : extra[up.cookie - u.size()];
        const std::uint32_t token = inst.token;
        auto& lt = base ? last[up.cookie] : extra_last[up.cookie - u.size()];
        const std::int64_t recv = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        // Currency is published per 100 units of the base (live/contract_units.hpp).
        if (!altair::fyers_frames::to_frames(up, token, lt, recv, f, altair::live::quote_per(inst))) { return; }
        auto& sn = base ? seen[up.cookie] : extra_seen[up.cookie - u.size()];
        if (f.quote || f.trade) sn |= 1u;
        if (f.book) sn |= 2u;
        ++(up.topic == hsm::HsmTopic::Depth ? n_depth : n_sym);
        if (f.quote) { bus.quote(f.quote_p, f.quote_ns); }
        if (f.trade) { bus.trade(f.price, f.trade_ns); st.engine_ns = f.trade_ns; }
        // The 50-level book wins while it is fresh: no flicker back to five levels.
        if (f.book && !tbt.fresh(token)) { bus.book(f.price, f.bids, f.asks, f.trade_ns); }
    };
    // Map and subscribe what the helper threads looked up (socket thread only).
    const auto subscribe_looked = [&](std::vector<altair::FyersFrame>& replies) {
        std::vector<Looked> ready;
        {
            const std::lock_guard<std::mutex> lock(looked_mu);
            ready.swap(looked);
            for (auto& bad : refused) { ++st.unknown_symbols; st.unknown.push_back(std::move(bad)); }
            refused.clear();
        }
        if (ready.empty()) { return; }
        std::vector<std::string> fresh_sym, fresh_depth;
        for (auto& l : ready) {
            const auto cookie = static_cast<std::uint32_t>(u.size() + extra.size());
            for (int pass = 0; pass < (l.in.depth ? 2 : 1); ++pass) {
                const auto type = pass == 0 ? hsm::DataType::SymbolUpdate : hsm::DataType::DepthUpdate;
                auto topic = hsm::topic_for(l.in.fyers, l.fytoken, type);
                if (topic.empty()) { continue; }
                decoder.map_topic(topic, l.in.fyers, cookie);
                (pass == 0 ? fresh_sym : fresh_depth).push_back(std::move(topic));
            }
            std::printf("  streaming %s (added while running)\n", l.in.symbol.c_str());
            extra.push_back(std::move(l.in));
            extra_last.emplace_back();
            extra_seen.push_back(0);
        }
        // Symbols now, depth a second later (see sym_topics); both again after any reconnect.
        std::vector<hsm::Bytes> fs, fd;
        if (make_frames(fresh_sym, fs))
            for (auto& fr : fs) { replies.push_back(fr); subscribe.push_back(std::move(fr)); }
        if (make_frames(fresh_depth, fd)) {
            for (auto& fr : fd) { depth_queue.push_back(fr); subscribe_depth.push_back(std::move(fr)); }
            depth_due = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
        st.instruments = u.size() + extra.size();
        std::fflush(stdout);
    };
    const auto look_up = [&](std::vector<altair::live::LiveInstrument> added) {
        lookups.emplace_back([&, added = std::move(added)]() mutable {
            std::vector<std::string> symbols;
            for (const auto& in : added) symbols.push_back(in.fyers);
            std::string why;
            std::vector<std::string> invalid;
            const auto valid = fyers_symbol_tokens(session, symbols, why, invalid);
            if (!why.empty()) { std::printf("  could not add scrips: %s\n", why.c_str()); return; }
            for (const auto& bad : invalid) std::printf("    FYERS does not know %s -- not streamed\n", bad.c_str());
            const std::lock_guard<std::mutex> lock(looked_mu);
            refused.insert(refused.end(), invalid.begin(), invalid.end());
            for (const auto& [symbol, fytoken] : valid)
                for (auto& in : added)
                    if (in.fyers == symbol) { looked.push_back({in, fytoken}); break; }
        });
    };
    auto last_status = std::chrono::steady_clock::now();
    const auto on_frame = [&](const std::uint8_t* p, std::size_t n, std::vector<altair::FyersFrame>& replies) -> bool {
        const auto r = decoder.on_frame(p, n, emit);
        if (r.ack) { replies.push_back(r.ack_bytes); }
        if (r.event == hsm::HsmEvent::AuthOk) {
            std::printf("  authenticated; subscribing symbols, then depth a second later\n");
            std::fflush(stdout);
            for (const auto& fr : subscribe) { replies.push_back(fr); }
            depth_queue = subscribe_depth;
            depth_due = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            streaming_since = std::chrono::steady_clock::now();
            st.state = "streaming";
        } else if (r.event == hsm::HsmEvent::AuthFailed) {
            auth_rejected = true;
            return false;
        }
        if (st.state == "streaming") { subscribe_looked(replies); }
        const auto now = std::chrono::steady_clock::now();
        if (!depth_queue.empty() && now >= depth_due) {
            for (auto& fr : depth_queue) replies.push_back(std::move(fr));
            depth_queue.clear();
        }
        // Every 30 s: which instruments have prices and books, and which have
        // none yet (named, so a blank row has a reason in the log and status).
        if (st.state == "streaming" && now - last_report > std::chrono::seconds(30)
            && now - streaming_since > std::chrono::seconds(20)) {
            last_report = now;
            std::size_t subs = 0, priced = 0, want_book = 0, booked = 0, fresh50 = 0;
            st.no_price.clear();
            const auto look = [&](const altair::live::LiveInstrument& in, std::uint8_t sn, bool sub, bool tbt_fresh) {
                if (!sub) return;
                ++subs;
                if (sn & 1u) ++priced;
                else if (st.no_price.size() < 20) st.no_price.push_back(in.fyers);
                if (in.depth) { ++want_book; if (sn & 2u) ++booked; }
                if (tbt_fresh) ++fresh50;
            };
            for (std::size_t i = 0; i < u.size(); ++i) look(u[i], seen[i], subscribed[i] != 0, tbt.fresh(u[i].token));
            for (std::size_t i = 0; i < extra.size(); ++i) look(extra[i], extra_seen[i], true, tbt.fresh(extra[i].token));
            st.subscribed = subs; st.priced = priced; st.booked = booked; st.depth50 = fresh50;
            std::printf("  prices for %zu of %zu instrument(s); books %zu of %zu (50-level fresh: %zu); "
                        "updates: symbol %llu, depth %llu\n", priced, subs, booked, want_book, fresh50,
                        static_cast<unsigned long long>(n_sym), static_cast<unsigned long long>(n_depth));
            if (!st.no_price.empty()) {
                std::string names;
                for (std::size_t k = 0; k < st.no_price.size() && k < 8; ++k) names += (k ? ", " : "") + st.no_price[k];
                std::printf("    no price yet: %s%s\n", names.c_str(),
                            subs - priced > 8 ? (" (+" + std::to_string(subs - priced - 8) + ")").c_str() : "");
            }
            std::fflush(stdout);
        }
        if (now - last_status > std::chrono::seconds(2)) {
            last_status = now;
            st.clients = bus.clients();
            st.trades = bus.trades(); st.quotes = bus.quotes(); st.books = bus.books();
            altair::live_sources::write_status(status_path, st);
            if (take_added) {
                auto added = take_added();
                if (!added.empty()) { look_up(std::move(added)); }
            }
        }
        return !g_stop.load() && unix_now() < deadline_unix;
    };

    std::optional<altair::FyersSocketError> last_error;
    for (long attempt = 0; !g_stop.load(); ++attempt) {
        const std::int64_t left = deadline_unix - unix_now();
        if (left < 1) { break; }
        decoder.reset();
        if (attempt > 0) {
            ++st.reconnects;
            st.state = "reconnecting";
            altair::live_sources::write_status(status_path, st);
        }
        const auto run = altair::fyers_data_socket_run(
            {hsm::auth_frame(*hsm_key, source), hsm::mode_frame(false, hsm::kDefaultChannel)}, on_frame,
            hsm::ping_frame(), std::chrono::seconds{hsm::kPingSeconds}, std::chrono::seconds{left});
        if (run) {
            last_error.reset();
            if (run->interrupted) { break; }
        } else {
            last_error = run.error();
            st.error = altair::fyers_socket_error_text(run.error());
            std::printf("  feed error: %s\n", st.error.c_str());
            std::fflush(stdout);
        }
        if (auth_rejected) {
            st.state = "refused";
            st.error = "FYERS refused the socket token; link FYERS again";
            break;
        }
        // RULE 11: safe-side clamp -- back-off grows to 8 s and stops there; a
        // feed that keeps dropping keeps retrying until the deadline.
        const long backoff = attempt < 3 ? (1L << attempt) : 8L;
        std::this_thread::sleep_for(std::chrono::seconds{backoff});
    }
    for (auto& t : lookups) { if (t.joinable()) t.join(); }
    if (st.state != "refused") { st.state = "stopped"; }
    st.clients = bus.clients();
    st.trades = bus.trades(); st.quotes = bus.quotes(); st.books = bus.books();
    altair::live_sources::write_status(status_path, st);
    std::printf("  stopped: %llu trade(s), %llu quote(s), %llu book(s) published, %llu reconnect(s)\n",
                static_cast<unsigned long long>(st.trades), static_cast<unsigned long long>(st.quotes),
                static_cast<unsigned long long>(st.books), static_cast<unsigned long long>(st.reconnects));
    if (auth_rejected) { return 4; }
    return last_error && st.trades == 0 ? 4 : 0;
}

/// FYERS is usable today: a session exists and its token has not expired.
[[nodiscard]] bool fyers_usable(const std::optional<FySession>& s, std::string& why) {
    if (!s) { why = "no FYERS session (log in to FYERS)"; return false; }
    const auto key = altair::fyers_hsm::hsm_key_from_token(s->access, unix_now());
    if (!key) { why = std::string("FYERS token: ") + altair::fyers_hsm::error_text(key.error()); return false; }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode, sym, iv;
    unsigned short port = 7421;
    int seconds = 0;
    int rate = 200;
    std::size_t tail = 0;
    bool fyers = false, sim = false, live_auto = false, stocks = true;
    int until_min = 15 * 60 + 35, from_min = 9 * 60 + 15, strikes = 20, depth_strikes = 5, expiries = 1;
    bool monthly = false;
    double speed = 1.0, atm_nifty = 0.0, atm_banknifty = 0.0;
    std::uint64_t seed = 20261001;
    std::string sim_date;   // YYYY-MM-DD: simulate that day instead of today
    std::string live_dir_arg;   // --live-dir: universe.csv and friends (default <source>/data/live)
    std::string master_arg;     // --master: the instrument master (default <source>/data/instruments.csv)

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--replay" && i + 2 < argc) {
            mode = "replay";
            sym = argv[i + 1];
            iv = argv[i + 2];
            i += 2;
        } else if (a == "--go") {
            mode = "live";
        } else if (a == "--port" && i + 1 < argc) {
            port = static_cast<unsigned short>(std::atoi(argv[++i]));
        } else if (a == "--seconds" && i + 1 < argc) {
            seconds = std::atoi(argv[++i]);
        } else if (a == "--tail" && i + 1 < argc) {
            const long long t = std::atoll(argv[++i]);
            tail = t > 0 ? static_cast<std::size_t>(t) : 0;
        } else if (a == "--rate" && i + 1 < argc) {
            rate = std::max(1, std::atoi(argv[++i]));
        } else if (a == "--fyers") {
            fyers = true;
        } else if (a == "--sim") {
            sim = true;
        } else if (a == "--date" && i + 1 < argc) {
            sim_date = argv[++i];
        } else if (a == "--live") {
            live_auto = true;
        } else if (a == "--until" && i + 1 < argc) {
            until_min = parse_hhmm(argv[++i]);
        } else if (a == "--from" && i + 1 < argc) {
            from_min = parse_hhmm(argv[++i]);
        } else if (a == "--speed" && i + 1 < argc) {
            speed = std::atof(argv[++i]);
        } else if (a == "--seed" && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--strikes" && i + 1 < argc) {
            strikes = std::atoi(argv[++i]);
        } else if (a == "--depth-strikes" && i + 1 < argc) {
            depth_strikes = std::atoi(argv[++i]);
        } else if (a == "--monthly") {
            monthly = true;
        } else if (a == "--expiries" && i + 1 < argc) {
            expiries = std::atoi(argv[++i]);
        } else if (a == "--live-dir" && i + 1 < argc) {
            live_dir_arg = argv[++i];
        } else if (a == "--master" && i + 1 < argc) {
            master_arg = argv[++i];
        } else if (a == "--no-stocks") {
            stocks = false;
        } else if (a == "--depth50" && i + 1 < argc) {
            const std::string v = argv[++i];
            g_depth50 = v == "off" ? 0 : static_cast<std::size_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (a == "--atm-nifty" && i + 1 < argc) {
            atm_nifty = std::atof(argv[++i]);
        } else if (a == "--atm-banknifty" && i + 1 < argc) {
            atm_banknifty = std::atof(argv[++i]);
        } else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            return 0;
        }
    }
    if (fyers) { mode = mode == "live" ? "fyers" : "fyers-dry"; }
    else if (live_auto || mode == "live") { mode = mode == "live" ? "auto" : "auto-dry"; }
    if (sim) { mode = "sim"; }
    if (!sim_date.empty() && (altair::live::parse_day(sim_date) == 0 || mode != "sim")) {
        std::printf("--date YYYY-MM-DD goes with --sim\n");
        return 2;
    }
    if (until_min < 0 || from_min < 0 || !(speed > 0.0 && speed <= 1000.0) || strikes < 0 || strikes > 60
        || depth_strikes < 0 || depth_strikes > strikes || expiries < 1 || expiries > 4) {
        std::printf("--until/--from must be HH:MM, --speed 0..1000, --strikes 0..60, "
                    "--depth-strikes 0..--strikes, --expiries 1..4\n");
        return 2;
    }

    if (mode.empty()) {
        usage(argv[0]);
        return 0;
    }

    // ---- the live terminal: FYERS or simulated, over the whole universe ----
    if (mode == "fyers" || mode == "fyers-dry" || mode == "sim" || mode == "auto" || mode == "auto-dry") {
        const std::string src = ALTAIR_SOURCE_DIR;
        const std::string ds = ALTAIR_DATASET_DIR;
        const std::int64_t today = sim_date.empty() ? altair::live::ist_today(unix_now())
                                                    : altair::live::parse_day(sim_date);
        std::optional<FySession> session;
        double nifty = atm_nifty, bnf = atm_banknifty;
        if (mode == "auto" || mode == "auto-dry") {
            std::string fy_why;
            session = fyers_session(src + "/data/fyers_session.json");
            if (!fyers_usable(session, fy_why)) {
                std::printf("no live source today.\n  FYERS: %s\n  (log in to FYERS on Brokers, or run "
                            "broker/tools/fyers_auto_login.py)\n", fy_why.c_str());
                return 2;
            }
            std::printf("live source: FYERS\n");
            mode = mode == "auto" ? "fyers" : "fyers-dry";
        }
        if (mode == "fyers") {
            if (!session) session = fyers_session(src + "/data/fyers_session.json");
            if (!session) {
                std::printf("no valid FYERS session: run altair_fyers_login first, or set\n"
                            "ALTAIR_FYERS_CLIENT_ID and ALTAIR_FYERS_ACCESS_TOKEN.\n");
                return 2;
            }
            if (!(nifty > 0.0) || !(bnf > 0.0)) {
                double n = 0.0, b = 0.0;
                fyers_spots(*session, n, b);
                if (!(nifty > 0.0) && n > 0.0) { nifty = n; std::printf("NIFTY now %.2f (FYERS quote)\n", n); }
                if (!(bnf > 0.0) && b > 0.0) { bnf = b; std::printf("BANKNIFTY now %.2f (FYERS quote)\n", b); }
            }
        }
        if (!(nifty > 0.0)) { nifty = altair::live_sources::sim_close(ds + "/spot/nifty/1d", sim_date); std::printf("NIFTY ATM from the last close in dataset/: %.2f\n", nifty); }
        if (!(bnf > 0.0)) { bnf = altair::live_sources::sim_close(ds + "/spot/banknifty/1d", sim_date); std::printf("BANKNIFTY ATM from the last close in dataset/: %.2f\n", bnf); }

        const std::string live_dir = live_dir_arg.empty() ? src + "/data/live" : live_dir_arg;
        const std::string master = master_arg.empty() ? src + "/data/instruments.csv" : master_arg;
        // FYERS's own names for cash scrips, and its F&O master (fetched for a
        // FYERS feed, at most once in 20 hours; a SIM only reads the copy on disk).
        const auto cash_names = fyers_cash_names(live_dir, mode == "fyers" || mode == "fyers-dry");
        // data/instruments.csv is built from the FYERS masters
        // (live/instrument_master.hpp) whenever a master is newer than it.
        if (altair::live::instrument_master_stale(live_dir + "/fyers_masters", master)) {
            altair::live::MasterBuildReport rep;
            std::string why;
            if (altair::live::build_instrument_master(live_dir + "/fyers_masters", master, rep, why)) {
                std::printf("data/instruments.csv rebuilt from the FYERS masters: %zu row(s)\n", rep.rows);
            } else {
                std::printf("data/instruments.csv not rebuilt: %s\n", why.c_str());
            }
        }
        std::string err;
        auto rows = altair::live::read_instrument_master(master, err);
        if (!err.empty()) { std::printf("%s\n", err.c_str()); return 2; }
        // The index contracts listed since data/instruments.csv was written
        // (each new weekly), from FYERS's master.
        // The OBI trader's stocks (config/obi_underlyings.csv) come whole: the
        // stock, its near futures and its chain centred on FYERS's last price.
        const auto obi_stocks = altair::live::read_obi_underlyings(src + "/config/obi_underlyings.csv");
        std::set<std::string> fo_unders{"NIFTY", "BANKNIFTY"};
        for (const auto& u : obi_stocks) { fo_unders.insert(u.symbol); }
        if (std::ifstream fo(live_dir + "/fyers_masters/NSE_FO.csv", std::ios::binary); fo) {
            const std::size_t n = altair::live::add_fyers_fo(rows, fo, fo_unders);
            std::printf("FYERS F&O master: %zu index/OBI-stock contract(s) not in data/instruments.csv added\n", n);
        }
        altair::live::LiveUniverseOptions uo;
        uo.today = today;
        uo.nifty_spot = nifty;
        uo.banknifty_spot = bnf;
        uo.strikes = strikes;
        uo.depth_strikes = depth_strikes;
        uo.expiries = expiries;
        uo.monthly = monthly;
        uo.stocks = stocks;
        for (const auto& u : obi_stocks) { uo.fo_stocks.push_back(u.symbol); }
        if (!uo.fo_stocks.empty()) {
            std::vector<std::string> eq;
            for (const auto& name : uo.fo_stocks) { eq.push_back("NSE:" + name + "-EQ"); }
            if (mode == "fyers" && session) { uo.fo_spots = fyers_last_prices(*session, eq); }
            uo.fo_spots.resize(uo.fo_stocks.size(), 0.0);
            for (std::size_t k = 0; k < uo.fo_stocks.size(); ++k) {
                if (uo.fo_spots[k] > 0.0) { std::printf("OBI stock %s now %.2f (FYERS quote)\n", uo.fo_stocks[k].c_str(), uo.fo_spots[k]); }
                else { std::printf("OBI stock %s: no FYERS quote, so its futures stream without a chain\n", uo.fo_stocks[k].c_str()); }
            }
        }
        g_obi_unders = {"BANKNIFTY"};
        g_obi_spot["BANKNIFTY"] = bnf;
        for (std::size_t k = 0; k < uo.fo_stocks.size(); ++k) {
            g_obi_unders.push_back(uo.fo_stocks[k]);
            if (k < uo.fo_spots.size() && uo.fo_spots[k] > 0.0) g_obi_spot[uo.fo_stocks[k]] = uo.fo_spots[k];
        }
        auto uni = altair::live::build_universe(
            rows, altair::live::read_stock_universe(src + "/config/universe_nifty50.csv"), uo);
        g_depth_request = live_dir + "/depth_request.csv";
        g_obi_watch = live_dir + "/obi_watch.csv";
        const std::string watch_path = live_dir + "/watchlist.csv";
        // The market watch's added scrips (data/live/watchlist.csv).
        std::vector<std::uint32_t> watched = altair::live::read_watchlist(watch_path);
        altair::live::add_watchlist(uni.instruments, rows, watched, uni.notes, &cash_names);
        for (const auto& n : uni.notes) { std::printf("  note: %s\n", n.c_str()); }
        std::size_t depth_n = 0;
        for (const auto& i : uni.instruments) { depth_n += i.depth ? 1 : 0; }
        std::printf("universe: %zu instrument(s), %zu with depth, expiries on or after %s\n",
                    uni.instruments.size(), depth_n, altair::live::day_text(today).c_str());
        std::error_code ec;
        std::filesystem::create_directories(live_dir, ec);
        if (!altair::live::write_universe(live_dir + "/universe.csv", uni.instruments)) {
            std::printf("could not write %s/universe.csv\n", live_dir.c_str());
            return 1;
        }
        std::printf("wrote %s/universe.csv\n", live_dir.c_str());
        if (mode == "fyers-dry") {
            for (std::size_t i = 0; i < uni.instruments.size() && i < 12; ++i) {
                std::printf("  %-10u %s\n", uni.instruments[i].token, uni.instruments[i].fyers.c_str());
            }
            std::printf("\nDRY RUN. No socket was opened. Add --go to stream from FYERS.\n");
            return 0;
        }

        boost::asio::io_context io;
        altair::PriceBus bus(io, port);
        if (!bus.ok()) {
            std::printf("could not bind 127.0.0.1:%u -- %s\n", static_cast<unsigned>(port),
                        bus.error().message().c_str());
            return 1;
        }
        std::printf("altair price service on 127.0.0.1:%u  [%s]\n", static_cast<unsigned>(bus.port()),
                    mode == "sim" ? "SIMULATED" : "FYERS LIVE");
        std::fflush(stdout);
        std::signal(SIGINT, on_stop_signal);
        std::signal(SIGTERM, on_stop_signal);
        // Cores and priorities (config/latency.toml): the process, this
        // thread (the feed's socket and decoder), the bus's owner thread.
        {
            std::string note;
            g_latency = altair::latency::load_plan(std::string(ALTAIR_SOURCE_DIR) + "/config/latency.toml", note);
            std::printf("%s\n  %s\n  %s\n", note.c_str(), altair::latency::apply_process(g_latency).c_str(),
                        altair::latency::apply_thread(g_latency, "feed").c_str());
        }
        altair::live_sources::SharedBus shared(bus, [] {
            std::printf("  %s\n", altair::latency::apply_thread(g_latency, "bus").c_str());
            std::fflush(stdout);
        });
        const std::string status = live_dir + "/feed_status.json";
        const auto started = std::chrono::steady_clock::now();
        // The watchlist file and the option chains the Terminal opened
        // (chain_request.csv), checked every couple of seconds: new tokens are
        // added to what streams (and universe.csv, which the desktop reads).
        // `known` grows; uni.instruments does not change under a running feed
        // (the 50-level thread reads it), and is caught up between runs.
        std::vector<altair::live::LiveInstrument> known = uni.instruments;
        const std::string chain_path = live_dir + "/chain_request.csv";
        std::error_code wec;
        auto watch_stamp = std::filesystem::last_write_time(watch_path, wec);
        auto chain_stamp = std::filesystem::last_write_time(chain_path, wec);
        const auto take_new_watch = [&]() -> std::vector<altair::live::LiveInstrument> {
            std::error_code e1, e2;
            const auto ws = std::filesystem::last_write_time(watch_path, e1);
            const auto cs = std::filesystem::last_write_time(chain_path, e2);
            const bool watch_new = !e1 && ws != watch_stamp, chain_new = !e2 && cs != chain_stamp;
            if (!watch_new && !chain_new) return {};
            if (!e1) watch_stamp = ws;
            if (!e2) chain_stamp = cs;
            auto wanted = altair::live::read_watchlist(watch_path);
            for (const auto t : altair::live::read_watchlist(chain_path))
                if (std::find(wanted.begin(), wanted.end(), t) == wanted.end()) wanted.push_back(t);
            const std::size_t before = known.size();
            std::vector<std::string> notes;
            altair::live::add_watchlist(known, rows, wanted, notes, &cash_names);
            for (const auto& n : notes) { std::printf("  note: %s\n", n.c_str()); }
            std::vector<altair::live::LiveInstrument> added(known.begin() + static_cast<std::ptrdiff_t>(before), known.end());
            if (!added.empty()) {
                (void)altair::live::write_universe(live_dir + "/universe.csv", known);
                for (const auto& in : added) std::printf("  added %s\n", in.symbol.c_str());
                std::fflush(stdout);
            }
            return added;
        };
        if (mode == "sim") {
            const std::int64_t start_ns = ist_unix(today, from_min) * 1'000'000'000LL;
            if (!sim_date.empty()) std::printf("simulating %s from %02d:%02d\n", sim_date.c_str(), from_min / 60, from_min % 60);
            altair::live_sources::run_sim(
                shared, uni.instruments, altair::live_sources::sim_seeds(src, ds, uni.instruments, sim_date), seed,
                start_ns, speed, status,
                [&] {
                    return g_stop.load() || (seconds > 0 && std::chrono::steady_clock::now() - started
                                                                >= std::chrono::seconds(seconds));
                },
                [&](altair::live::LiveSim& sim) {
                    if (sim_date.empty()) return;
                    // A past day: the indices follow its real minute closes.
                    auto n = altair::live::day_minutes(ds + "/spot/nifty/1m", sim_date);
                    auto b = altair::live::day_minutes(ds + "/spot/banknifty/1m", sim_date);
                    auto v = altair::live::day_minutes(ds + "/spot/indiavix/1m", sim_date);
                    std::printf("  %s on disk: %zu NIFTY, %zu BANKNIFTY, %zu VIX minute(s)%s\n", sim_date.c_str(),
                                n.size(), b.size(), v.size(),
                                n.empty() ? " -- no real path; a random walk from the previous close" : "");
                    sim.set_anchors(std::move(n), std::move(b), std::move(v));
                },
                [&] {
                    std::vector<std::pair<altair::live::LiveInstrument, double>> out;
                    for (auto& in : take_new_watch()) {
                        std::string dir = in.symbol;
                        for (auto& c : dir) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
                        const double close = in.kind == altair::live::LiveKind::Equity
                            ? altair::live_sources::sim_close(src + "/data/pairs/" + dir + "/1d", sim_date) : 0.0;
                        out.emplace_back(std::move(in), close);
                    }
                    return out;
                });
            std::printf("sim stopped: %llu trade(s), %llu quote(s), %llu book(s)\n",
                        static_cast<unsigned long long>(shared.trades()),
                        static_cast<unsigned long long>(shared.quotes()),
                        static_cast<unsigned long long>(shared.books()));
            return 0;
        }
        // The later of --until and the close of every segment that streams:
        // an MCX scrip keeps the feed up to 23:59, currency to 17:05
        // (live/contract_units.hpp). Worked out again on every reconnect, as
        // the watchlist grows.
        const std::int64_t seconds_deadline = seconds > 0 ? unix_now() + seconds : std::numeric_limits<std::int64_t>::max();
        const auto deadline_of = [&] {
            const int end = altair::live::session_end_minute(
                known, [](const altair::live::LiveInstrument& i) -> std::string_view { return i.group; }, until_min);
            if (end > until_min) std::printf("  streaming until %02d:%02d IST (MCX or currency in the watchlist)\n", end / 60, end % 60);
            return std::min(ist_unix(today, end), seconds_deadline);
        };
        std::int64_t deadline = deadline_of();
        for (;;) {
            uni.instruments = known;   // everything added so far streams from the start of this run
            deadline = deadline_of();
            const int rc = run_fyers(*session, uni.instruments, shared, deadline, status, take_new_watch);
            // New scrips: reconnect with them (the board cache keeps the rest).
            if (rc == kRestartForWatchlist && !g_stop.load() && unix_now() < deadline) continue;
            return rc == kRestartForWatchlist ? 0 : rc;
        }
    }

    boost::asio::io_context io;
    altair::PriceBus bus(io, port);
    if (!bus.ok()) {
        std::printf("could not bind 127.0.0.1:%u -- %s\n",
                    static_cast<unsigned>(port),
                    bus.error().message().c_str());
        return 1;
    }
    std::printf("altair price service on 127.0.0.1:%u  [%s]\n",
                static_cast<unsigned>(bus.port()), mode.c_str());
    std::fflush(stdout);

    const auto started = std::chrono::steady_clock::now();
    const auto expired = [&] {
        if (seconds <= 0) { return false; }
        return std::chrono::steady_clock::now() - started
               >= std::chrono::seconds(seconds);
    };

    if (mode == "replay") {
        const std::string dir =
            std::string(ALTAIR_DATASET_DIR) + "/spot/" + sym + "/" + iv;
        std::vector<Bar> bars = load_bars(dir);
        if (tail > 0 && bars.size() > tail) {
            bars.erase(bars.begin(),
                       bars.end() - static_cast<std::ptrdiff_t>(tail));
        }
        if (bars.empty()) {
            std::printf("no bars under %s\n", dir.c_str());
            return 1;
        }
        std::printf("replaying %zu bars from %s at %d/s\n", bars.size(),
                    dir.c_str(), rate);
        std::fflush(stdout);

        // The token is looked up nowhere: a replay of a spot series is
        // published under the series' own canonical token so a subscriber
        // sees the same instrument it would see live.
        const std::uint32_t token =
            sym == "nifty" ? 256265u
            : sym == "banknifty" ? 260105u
            : sym == "indiavix" ? 264969u : 0u;

        const auto per = std::chrono::microseconds(1'000'000 / rate);
        auto next = std::chrono::steady_clock::now();
        std::size_t sent = 0;
        for (const Bar& b : bars) {
            if (expired()) { break; }
            altair::PricePayload p;
            p.token = token;
            p.last_paise = static_cast<std::int64_t>(b.close * 100.0 + 0.5);
            p.exchange_ts_ns = b.ts_ns;
            p.flags = altair::kPriceReplay;
            if (b.has_volume) {
                p.flags |= altair::kPriceHasVolume;
                p.volume = static_cast<std::int64_t>(b.volume);
            }
            bus.publish(altair::kTopicTrades, p, nullptr, nullptr, b.ts_ns);
            ++sent;
            next += per;
            std::this_thread::sleep_until(next);
            bus.poll();
        }
        std::printf("replay done: %zu frames published, %llu delivered, "
                    "%llu coalesced, %zu subscriber(s)\n",
                    sent,
                    static_cast<unsigned long long>(bus.frames_sent()),
                    static_cast<unsigned long long>(bus.coalesced()),
                    bus.clients());
        return 0;
    }

    return 0;
}
