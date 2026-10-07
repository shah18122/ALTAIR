// app/price_service_main.cpp -- one process owns the feed.
//
// P37-03. Every live thing in this tree so far is a subprocess that runs,
// writes a JSON file and exits: the tick feed samples for N seconds, the quote
// refresh fetches once, the account panel fetches once, the dataset updater
// tops up and stops. Four programs, four reads of the credential, and not one
// of them is a stream -- so the "Live Grid" has always been a replay and the
// depth ladder has been handed a null book since the day it was written.
//
// This is the stream. It holds ONE broker connection (FYERS; Kite only when
// asked for by name, as legacy) and republishes every tick
// on a local port as protocol frames. Anything that wants prices connects and
// reads; nothing else needs the credential.
//
// TWO SOURCES, ONE PATH -- HARD RULE 6.
//
//   --go              live from Kite
//   --replay SYM IV   bars from dataset/, at their own timestamps
//
// Both go through the same encoder, the same bus and the same frames. That is
// the rule that says a backtest which does not share the live code path is a
// lie, and it is also what makes this testable at all: the Kite token is a
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
#include <deque>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <app/fyers_env_session.hpp>
#include <app/fyers_price_frames.hpp>
#include <app/kite_price_frames.hpp>
#include <app/live_feed_sources.hpp>
#include <broker/fyers_api.hpp>
#include <broker/credential_store.hpp>
#include <broker/https_client.hpp>
#include <broker/kite_ticker.hpp>
#include <core/affinity.hpp>
#include <feed/fyers_hsm.hpp>
#include <feed/fyers_tbt.hpp>
#include <feed/kite_decoder.hpp>
#include <instruments/contract_spec.hpp>
#include <live/universe.hpp>

#include <atomic>
#include <csignal>
#include <optional>

// FILE SCOPE, NOT THE STACK. SpecStore is 1.63 MB against a 1 MB default
// thread stack on MSVC, and a stack-allocated one crashes with 0xC00000FD --
// a code that names neither the object nor the file. P32-07 paid for this
// once already.
altair::SpecStore g_specs;

namespace {

void usage(const char* exe) {
    std::printf(
        "Altair price service -- one process owns the feed.\n\n"
        "  %s --replay <symbol> <interval> [--port N] [--seconds N]\n"
        "  %s --go [--port N] [--seconds N] [--tokens a,b,c]\n"
        "  %s --fyers [--go] [--until HH:MM] [--strikes N] [--depth-strikes N]\n"
        "      [--no-stocks] [--atm-nifty X] [--atm-banknifty X] [--depth50 N|off]\n"
        "  %s --kite [--go] [same options as --fyers]\n"
        "  %s --live [--go] [same options as --fyers]\n"
        "  %s --sim [--date YYYY-MM-DD] [--speed N] [--from HH:MM] [--seed N]\n\n"
        "  --fyers             the LIVE TERMINAL feed from FYERS: NIFTY, BANKNIFTY,\n"
        "                      INDIA VIX, the near futures, both option chains\n"
        "                      (ATM +/- --strikes, default 20) and the NIFTY 50, with\n"
        "                      quotes and depth: the FYERS 50-level book for the\n"
        "                      futures, the options near the money and the stocks\n"
        "                      (--depth50 N: at most N of them, default 250; off: the\n"
        "                      5-level book only). Needs data/fyers_session.json\n"
        "                      (altair_fyers_login) or ALTAIR_FYERS_CLIENT_ID and\n"
        "                      ALTAIR_FYERS_ACCESS_TOKEN. Without --go: prints the\n"
        "                      universe and exits. Runs until --until (default 15:35\n"
        "                      IST), reconnecting after a drop.\n"
        "  --kite              LEGACY, by hand only: the same live universe from the\n"
        "                      Kite ticker (full mode: trades, quotes and 5-level\n"
        "                      depth). Needs the Kite API key (OS vault or\n"
        "                      ALTAIR_KITE_API_KEY) and today's data/kite_session.json.\n"
        "  --live              FYERS only, when its session is good today; never a\n"
        "                      fall-back to Kite. The desktop's \"Start live feed\"\n"
        "                      runs this.\n"
        "  --sim               the same universe, SIMULATED and flagged SIM on every\n"
        "                      frame: the terminal and the live models with the\n"
        "                      market shut. --speed: simulated seconds per second\n"
        "                      (default 1); --from: the session time to start at\n"
        "                      (default 09:15). --date: simulate that day -- the\n"
        "                      previous closes are the session before it, and when\n"
        "                      dataset/ holds that day's 1-minute bars, NIFTY,\n"
        "                      BANKNIFTY and INDIA VIX follow them.\n"
        "  --replay nifty 1m   serve bars from dataset/spot/<symbol>/<iv>/\n"
        "  --go                serve LIVE from Kite. Needs\n"
        "                      ALTAIR_KITE_API_KEY and data/kite_session.json\n"
        "  --port N            loopback port to publish on (default 7421)\n"
        "  --seconds N         stop after N seconds (default: run until\n"
        "                      interrupted; a replay always stops at the end)\n"
        "  --rate N            replay bars per second (default 200)\n"
        "  --live-dir DIR      where universe.csv, watchlist.csv and feed_status.json\n"
        "                      live (default data/live in the source tree; tests\n"
        "                      point it elsewhere)\n"
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
        exe, exe, exe, exe, exe, exe);
}

/// Environment variable, or empty. _dupenv_s on MSVC because getenv is
/// deprecated there and /W4 with warnings-as-errors will not have it.
[[nodiscard]] std::string env_or_empty(const char* name) {
#if defined(_WIN32)
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) { return {}; }
    std::string v(buf);
    std::free(buf);
    return v;
#else
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
#endif
}

/// Read `access_token` out of data/kite_session.json.
///
/// A hand-rolled eight-line scan rather than a JSON library, deliberately: the
/// surface that touches a live trading credential should be as small as it can
/// be, and every other binary here does the same. The value is never printed.
[[nodiscard]] bool read_access_token(const char* path, std::string& out) {
    std::ifstream in(path);
    if (!in) { return false; }
    const std::string all((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    const std::string key = "\"access_token\"";
    const std::size_t at = all.find(key);
    if (at == std::string::npos) { return false; }
    const std::size_t q1 = all.find('"', all.find(':', at) + 1);
    if (q1 == std::string::npos) { return false; }
    const std::size_t q2 = all.find('"', q1 + 1);
    if (q2 == std::string::npos) { return false; }
    out = all.substr(q1 + 1, q2 - q1 - 1);
    return !out.empty();
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
/// Most instruments given the FYERS 50-level book (--depth50 N; 0 = off).
std::size_t g_depth50 = 250;
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
    const auto r = altair::https_get_auth("api-t1.fyers.in", *target, *auth, "", std::chrono::seconds{20});
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

/// The FYERS 50-level book (feed/fyers_tbt.hpp) for the instruments that
/// carry depth, on its own socket and thread. Its books replace the HSM's five
/// levels on the bus: while one is fresh, the HSM book for that instrument is
/// not published, so the two never alternate on screen.
struct TbtFeed {
    explicit TbtFeed(std::size_t n) : last_ns(n) {}
    ~TbtFeed() {
        stop.store(true);
        if (th.joinable()) th.join();
    }
    [[nodiscard]] bool fresh(std::size_t i) const {
        if (i >= last_ns.size()) return false;
        const std::int64_t t = last_ns[i].load(std::memory_order_relaxed);
        return t > 0 && steady_ns() - t < 5'000'000'000LL;
    }
    [[nodiscard]] static std::int64_t steady_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::vector<std::atomic<std::int64_t>> last_ns;   ///< per universe index: the last 50-level publish (steady)
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> books{0};
    std::thread th;
};

void run_fyers_tbt(const FySession& session, const std::vector<altair::live::LiveInstrument>& u,
                   altair::live_sources::SharedBus& bus, std::int64_t deadline_unix, TbtFeed& feed) {
    namespace tbt = altair::fyers_tbt;
    using altair::live::LiveKind;
    const auto auth = altair::fyers::authorization_header(session.client.c_str(), session.access.c_str());
    if (!auth) return;
    // Futures first, then options, then equities: if FYERS caps the count,
    // the instruments the 50-level book matters most for are the ones it keeps.
    std::vector<std::size_t> idx;
    for (std::size_t i = 0; i < u.size(); ++i)
        if (u[i].depth && u[i].kind != LiveKind::Index) idx.push_back(i);
    const auto rank = [&u](std::size_t i) { return u[i].kind == LiveKind::Future ? 0 : u[i].kind == LiveKind::Equity ? 2 : 1; };
    std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return rank(a) < rank(b); });
    if (idx.size() > g_depth50) {
        // RULE 11: said, not silent -- the rest keep the five-level book.
        std::printf("  50-level book: %zu instrument(s) beyond --depth50 %zu keep the 5-level book\n", idx.size() - g_depth50, g_depth50);
        idx.resize(g_depth50);
    }
    if (idx.empty()) return;
    std::unordered_map<std::string, std::size_t> by_symbol;
    std::vector<std::string> symbols;
    for (const std::size_t i : idx) { by_symbol.emplace(u[i].fyers, i); symbols.push_back(u[i].fyers); }
    std::vector<std::string> left;
    const auto channels = tbt::tbt_channels(symbols, &left);
    if (!left.empty()) std::printf("  50-level book: %zu instrument(s) past 50 channels keep the 5-level book\n", left.size());

    std::string host = tbt::kTbtDefaultHost, path = tbt::kTbtDefaultPath;
    if (const auto r = altair::https_get_auth(tbt::kTbtUrlHost, tbt::kTbtUrlPath, *auth, "", std::chrono::seconds{15});
        r && r->status == 200) {
        if (const auto url = tbt::tbt_socket_url(r->body)) { host = url->first; path = url->second; }
    }
    std::vector<altair::FyersFrame> opening;
    const auto bytes = [](const std::string& t) { return altair::FyersFrame(t.begin(), t.end()); };
    for (std::size_t c = 0; c < channels.size(); ++c) opening.push_back(bytes(tbt::tbt_subscribe_text(channels[c], static_cast<int>(c) + 1)));
    opening.push_back(bytes(tbt::tbt_resume_text(static_cast<int>(channels.size()))));
    std::printf("  50-level book: %zu instrument(s) on %zu channel(s) at %s\n", symbols.size(), channels.size(), host.c_str());
    std::fflush(stdout);

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
                const auto it = by_symbol.find(t);
                const tbt::TbtBook* b = books.book(t);
                if (it == by_symbol.end() || b == nullptr) continue;
                const std::size_t levels = std::min(b->depth(), altair::kMaxDepthLevels);
                if (levels == 0) continue;
                altair::PricePayload pp;
                pp.token = u[it->second].token;
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
                feed.last_ns[it->second].store(TbtFeed::steady_ns(), std::memory_order_relaxed);
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
    const std::string ping = "ping";
    for (long attempt = 0; !feed.stop.load() && !g_stop.load(); ++attempt) {
        const std::int64_t left_s = deadline_unix - unix_now();
        if (left_s < 1) break;
        books.reset();
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

/// FYERS's cash symbol masters, cached under `live_dir`/fyers_masters and
/// fetched again when older than 20 hours (live/fyers_names.hpp says why the
/// names are looked up, not built). Public files: no login, no credential. A
/// download that does not work keeps the copy on disk; with none, the names
/// are guessed and the feed names any FYERS refuses.
[[nodiscard]] altair::live::FyersCashNames fyers_cash_names(const std::string& live_dir, bool fetch) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(live_dir) / "fyers_masters";
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (const char* seg : {"NSE_CM", "BSE_CM"}) {
        if (!fetch) break;
        const fs::path path = dir / (std::string(seg) + ".csv");
        const auto stamp = fs::last_write_time(path, ec);
        if (!ec && fs::file_time_type::clock::now() - stamp < std::chrono::hours(20)) continue;
        const auto r = altair::https_get_auth("public.fyers.in", std::string("/sym_details/") + seg + ".csv", "", "",
                                              std::chrono::seconds{30});
        if (!r || r->status != 200 || r->body.size() < 1000) {
            std::printf("  FYERS %s master: download did not work (%s); %s\n", seg,
                        r ? ("HTTP " + std::to_string(r->status)).c_str() : "transport",
                        fs::exists(path, ec) ? "using the copy on disk" : "names are guessed");
            continue;
        }
        const fs::path tmp = path.string() + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary);
            f.write(r->body.data(), static_cast<std::streamsize>(r->body.size()));
            if (!f) continue;
        }
        fs::rename(tmp, path, ec);
    }
    altair::live::FyersCashNames names;
    const std::size_t n = names.load(dir.string());
    std::printf("FYERS cash names: %zu scrip(s) from %s\n", n, dir.string().c_str());
    return names;
}

/// Stream the universe from FYERS until `deadline_unix` or Ctrl+C.
/// Returned by run_fyers/run_kite when the market watch added scrips: the
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
    std::vector<std::string> topics;
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
                topics.push_back(std::move(topic));
            }
        }
    }
    if (topics.empty()) {
        st.state = "refused";
        st.error = "nothing subscribable";
        altair::live_sources::write_status(status_path, st);
        std::printf("  nothing subscribable\n");
        return 3;
    }
    const std::string source{hsm::kDefaultSource};
    std::vector<hsm::Bytes> subscribe;
    for (std::size_t i = 0; i < topics.size(); i += hsm::kSubscribeChunk) {
        // RULE 11: chunking, not truncation -- every topic is sent, kSubscribeChunk at a time.
        const std::size_t n = topics.size() - i < hsm::kSubscribeChunk ? topics.size() - i : hsm::kSubscribeChunk;
        const auto frame = hsm::topics_frame(true, std::span<const std::string>{topics.data() + i, n},
                                             hsm::kDefaultChannel, session.client.size() + 1 + session.access.size(),
                                             source.size());
        if (!frame) { std::printf("  %s\n", hsm::error_text(frame.error())); return 3; }
        subscribe.push_back(*frame);
    }
    std::printf("  subscribing %zu topic(s) for %zu instrument(s)\n", topics.size(), u.size());
    std::fflush(stdout);

    TbtFeed tbt(u.size());
    if (g_depth50 > 0)
        tbt.th = std::thread([&] {
            std::printf("  %s\n", altair::latency::apply_thread(g_latency, "tbt").c_str());
            run_fyers_tbt(session, u, bus, deadline_unix, tbt);
        });
    std::vector<altair::fyers_frames::LastTrade> last(u.size());
    // Instruments added while streaming: cookies u.size() + k. Deques, so a
    // reference handed to the decoder's callback never moves.
    std::deque<altair::live::LiveInstrument> extra;
    std::deque<altair::fyers_frames::LastTrade> extra_last;
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
        const std::uint32_t token = base ? u[up.cookie].token : extra[up.cookie - u.size()].token;
        auto& lt = base ? last[up.cookie] : extra_last[up.cookie - u.size()];
        const std::int64_t recv = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (!altair::fyers_frames::to_frames(up, token, lt, recv, f)) { return; }
        if (f.quote) { bus.quote(f.quote_p, f.quote_ns); }
        if (f.trade) { bus.trade(f.price, f.trade_ns); st.engine_ns = f.trade_ns; }
        // The 50-level book wins while it is fresh: no flicker back to five levels.
        if (f.book && !(base && tbt.fresh(up.cookie))) { bus.book(f.price, f.bids, f.asks, f.trade_ns); }
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
        std::vector<std::string> fresh;
        for (auto& l : ready) {
            const auto cookie = static_cast<std::uint32_t>(u.size() + extra.size());
            for (int pass = 0; pass < (l.in.depth ? 2 : 1); ++pass) {
                const auto type = pass == 0 ? hsm::DataType::SymbolUpdate : hsm::DataType::DepthUpdate;
                auto topic = hsm::topic_for(l.in.fyers, l.fytoken, type);
                if (topic.empty()) { continue; }
                decoder.map_topic(topic, l.in.fyers, cookie);
                fresh.push_back(std::move(topic));
            }
            std::printf("  streaming %s (added while running)\n", l.in.symbol.c_str());
            extra.push_back(std::move(l.in));
            extra_last.emplace_back();
        }
        for (std::size_t i = 0; i < fresh.size(); i += hsm::kSubscribeChunk) {
            const std::size_t n = fresh.size() - i < hsm::kSubscribeChunk ? fresh.size() - i : hsm::kSubscribeChunk;
            const auto frame = hsm::topics_frame(true, std::span<const std::string>{fresh.data() + i, n},
                                                 hsm::kDefaultChannel, session.client.size() + 1 + session.access.size(),
                                                 hsm::kDefaultSource.size());
            if (!frame) { continue; }
            replies.push_back(*frame);
            subscribe.push_back(*frame);   // and again after any reconnect
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
            std::printf("  authenticated; subscribing\n");
            std::fflush(stdout);
            for (const auto& fr : subscribe) { replies.push_back(fr); }
            st.state = "streaming";
        } else if (r.event == hsm::HsmEvent::AuthFailed) {
            auth_rejected = true;
            return false;
        }
        if (st.state == "streaming") { subscribe_looked(replies); }
        const auto now = std::chrono::steady_clock::now();
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

struct KiteCreds { std::string api_key, access; };

/// Kite ticker credentials: the API key from the OS vault (where Brokers >
/// Log in · Kite saves it) or ALTAIR_KITE_API_KEY, and TODAY's access token
/// from data/kite_session.json. Never printed. `why` says what is missing.
[[nodiscard]] std::optional<KiteCreds> kite_creds(const std::string& session_path, std::int64_t today,
                                                 std::string& why) {
    KiteCreds c;
    const auto stored = altair::broker::load_credential({"altair.kite", "api-key"});
    c.api_key = stored ? *stored : env_or_empty("ALTAIR_KITE_API_KEY");
    if (c.api_key.empty()) {
        why = "no Kite API key (save it in Brokers > Log in - Kite, or set ALTAIR_KITE_API_KEY)";
        return std::nullopt;
    }
    if (!read_access_token(session_path.c_str(), c.access)) {
        why = "no Kite session (log in to Kite)";
        return std::nullopt;
    }
    // A Kite token lives for one IST day: login_time must be today.
    std::ifstream in(session_path, std::ios::binary);
    const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t at = all.find("\"login_time\"");
    const std::size_t q1 = at == std::string::npos ? at : all.find('"', all.find(':', at) + 1);
    const std::string day = q1 == std::string::npos ? std::string{} : all.substr(q1 + 1, 10);
    if (day != altair::live::day_text(today)) {
        why = "the Kite session is from " + (day.empty() ? std::string("an unknown day") : day)
              + "; log in to Kite again";
        return std::nullopt;
    }
    return c;
}

/// FYERS is usable today: a session exists and its token has not expired.
[[nodiscard]] bool fyers_usable(const std::optional<FySession>& s, std::string& why) {
    if (!s) { why = "no FYERS session (log in to FYERS)"; return false; }
    const auto key = altair::fyers_hsm::hsm_key_from_token(s->access, unix_now());
    if (!key) { why = std::string("FYERS token: ") + altair::fyers_hsm::error_text(key.error()); return false; }
    return true;
}

/// Stream the universe from the Kite ticker, full mode, until `deadline_unix`.
int run_kite(const KiteCreds& creds, const std::vector<altair::live::LiveInstrument>& u,
             altair::live_sources::SharedBus& bus, std::int64_t deadline_unix, const std::string& status_path,
             const std::function<bool()>& watch_changed = {}) {
    using altair::live_sources::FeedStatus;
    FeedStatus st;
    st.source = "kite";
    st.state = "connecting";
    st.instruments = u.size();
    altair::live_sources::write_status(status_path, st);

    // The decoder maps (Kite, token) to a canonical id; token_of[id] maps it
    // back. Registered once per token: a reconnect after a watchlist change
    // finds the earlier ones already there.
    std::vector<std::uint32_t> tokens, token_of;
    tokens.reserve(u.size());
    for (const auto& i : u) {
        if (!g_specs.id_of(altair::FeedSource::Kite, i.token)) {
            altair::ContractSpec cs{};
            cs.token[static_cast<std::size_t>(altair::FeedSource::Kite)] = i.token;
            std::snprintf(cs.symbol, sizeof(cs.symbol), "TOKEN%u", i.token);
            cs.lot_size = altair::LotSize{1};
            cs.tick_size = altair::Price{1};
            cs.price_scale = 100;
            if (!g_specs.add(cs)) { continue; }
        }
        const auto id = g_specs.id_of(altair::FeedSource::Kite, i.token);
        if (!id) { continue; }
        const auto at = static_cast<std::size_t>(*id);
        if (token_of.size() <= at) token_of.resize(at + 1, 0);
        token_of[at] = i.token;
        tokens.push_back(i.token);
    }
    if (tokens.empty()) {
        st.state = "refused";
        st.error = "nothing subscribable";
        altair::live_sources::write_status(status_path, st);
        return 3;
    }
    constexpr std::size_t kMax = 4096;
    std::vector<altair::Tick> ticks(kMax);
    std::vector<altair::DepthUpdate> depths(kMax);
    std::uint32_t seq = 0;
    bool restart = false;
    auto last_status = std::chrono::steady_clock::now();
    const auto on_frame = [&](const unsigned char* raw, std::size_t len) {
        const std::int64_t recv_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (st.state != "streaming") { st.state = "streaming"; }
        // Quotes first, so a row has its previous close before its price.
        altair::kite_frames::quotes_from_frame(raw, len, recv_ns,
            [&bus](const altair::QuotePayload& q, std::int64_t ns) { bus.quote(q, ns); });
        const auto r = altair::decode_kite_frame(raw, len, g_specs, altair::Timestamp{recv_ns}, seq,
                                                 ticks.data(), kMax, depths.data(), kMax);
        if (!r) { return; }
        for (std::size_t i = 0; i < r->ticks; ++i) {
            const altair::Tick& t = ticks[i];
            const auto idx = static_cast<std::size_t>(t.id);
            if (idx >= token_of.size() || token_of[idx] == 0) { continue; }
            altair::PricePayload p;
            p.token = token_of[idx];
            p.last_paise = t.last.raw();
            p.last_qty = t.last_qty.raw();
            p.exchange_ts_ns = t.exchange_ts.ns_since_epoch();
            if (t.volume.raw() > 0) { p.flags |= altair::kPriceHasVolume; p.volume = t.volume.raw(); }
            if (t.oi > 0) { p.flags |= altair::kPriceHasOi; p.oi = t.oi; }
            if ((t.flags & static_cast<std::uint16_t>(altair::TickFlag::NoExchangeTs)) != 0) {
                p.flags |= altair::kPriceNoExchTs;
            }
            const std::int64_t ns = p.exchange_ts_ns != 0 ? p.exchange_ts_ns : recv_ns;
            bus.trade(p, ns);
            st.engine_ns = ns;
        }
        for (std::size_t i = 0; i < r->depths; ++i) {
            const altair::DepthUpdate& d = depths[i];
            const auto idx = static_cast<std::size_t>(d.id);
            if (idx >= token_of.size() || token_of[idx] == 0) { continue; }
            const auto levels = static_cast<std::uint16_t>(
                std::min<std::size_t>(altair::kMaxDepthLevels, std::max(d.bid_levels, d.ask_levels)));
            if (levels == 0) { continue; }
            altair::PricePayload p;
            p.token = token_of[idx];
            p.exchange_ts_ns = d.exchange_ts.ns_since_epoch();
            p.flags |= altair::kPriceHasBook;
            p.depth_levels = levels;
            altair::PriceLevel bids[altair::kMaxDepthLevels]{};
            altair::PriceLevel asks[altair::kMaxDepthLevels]{};
            for (std::uint16_t k = 0; k < levels; ++k) {
                bids[k] = {d.bid[k].px.raw(), d.bid[k].qty.raw(), d.bid[k].orders, 0};
                asks[k] = {d.ask[k].px.raw(), d.ask[k].qty.raw(), d.ask[k].orders, 0};
            }
            bus.book(p, bids, asks, p.exchange_ts_ns != 0 ? p.exchange_ts_ns : recv_ns);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_status > std::chrono::seconds(2)) {
            last_status = now;
            st.clients = bus.clients();
            st.trades = bus.trades(); st.quotes = bus.quotes(); st.books = bus.books();
            altair::live_sources::write_status(status_path, st);
            if (watch_changed && watch_changed()) restart = true;
        }
    };
    const auto done = [deadline_unix, &restart] { return restart || g_stop.load() || unix_now() >= deadline_unix; };
    std::printf("  subscribing %zu token(s) on the Kite ticker, full mode\n", tokens.size());
    std::fflush(stdout);
    int rc = 0;
    for (long attempt = 0; !done(); ++attempt) {
        if (attempt > 0) {
            ++st.reconnects;
            st.state = "reconnecting";
            altair::live_sources::write_status(status_path, st);
        }
        // The whole session, not the ticker's 60-second default: a run that
        // ends early (the server closed, or the connection went silent) is
        // reconnected below, and done() stops it within a fraction of a second.
        const auto left = std::chrono::seconds{std::max<std::int64_t>(1, deadline_unix - unix_now())};
        const auto stats = altair::kite_ticker_run(creds.api_key, creds.access, tokens,
                                                   altair::TickerMode::Full, on_frame, done, left);
        if (restart) {
            std::printf("  watchlist changed: reconnecting with the new scrips\n");
            std::fflush(stdout);
            return kRestartForWatchlist;
        }
        if (stats && done()) { break; }
        if (stats) {
            std::printf("  kite feed ended early; reconnecting\n");
            std::fflush(stdout);
            attempt = 0;   // it was streaming: not a refusal
            std::this_thread::sleep_for(std::chrono::seconds{1});   // and never a hot loop against a closing server
            continue;
        }
        st.error = altair::ticker_error_text(stats.error());
        std::printf("  kite feed error: %s\n", st.error.c_str());
        std::fflush(stdout);
        if (bus.trades() == 0 && attempt >= 2) { st.state = "refused"; rc = 4; break; }
        const long backoff = attempt < 3 ? (1L << attempt) : 8L;
        std::this_thread::sleep_for(std::chrono::seconds{backoff});
    }
    if (st.state != "refused") { st.state = "stopped"; }
    st.clients = bus.clients();
    st.trades = bus.trades(); st.quotes = bus.quotes(); st.books = bus.books();
    altair::live_sources::write_status(status_path, st);
    std::printf("  stopped: %llu trade(s), %llu quote(s), %llu book(s) published\n",
                static_cast<unsigned long long>(st.trades), static_cast<unsigned long long>(st.quotes),
                static_cast<unsigned long long>(st.books));
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode, sym, iv;
    unsigned short port = 7421;
    int seconds = 0;
    int rate = 200;
    std::size_t tail = 0;
    // NIFTY 50, NIFTY BANK, INDIA VIX -- the three the ticker has always
    // defaulted to. Overridable, because P38 needs an option chain here.
    std::vector<std::uint32_t> sub_tokens{256265u, 260105u, 264969u};
    bool fyers = false, sim = false, kite = false, live_auto = false, stocks = true;
    int until_min = 15 * 60 + 35, from_min = 9 * 60 + 15, strikes = 20, depth_strikes = 5;
    double speed = 1.0, atm_nifty = 0.0, atm_banknifty = 0.0;
    std::uint64_t seed = 20261001;
    std::string sim_date;   // YYYY-MM-DD: simulate that day instead of today
    std::string live_dir_arg;   // --live-dir: universe.csv and friends (default <source>/data/live)

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
        } else if (a == "--tokens" && i + 1 < argc) {
            sub_tokens.clear();
            std::istringstream ts(argv[++i]);
            std::string one;
            while (std::getline(ts, one, ',')) {
                if (!one.empty()) {
                    sub_tokens.push_back(
                        static_cast<std::uint32_t>(std::atoll(one.c_str())));
                }
            }
        } else if (a == "--tail" && i + 1 < argc) {
            const long long t = std::atoll(argv[++i]);
            tail = t > 0 ? static_cast<std::size_t>(t) : 0;
        } else if (a == "--rate" && i + 1 < argc) {
            rate = std::max(1, std::atoi(argv[++i]));
        } else if (a == "--fyers") {
            fyers = true;
        } else if (a == "--sim") {
            sim = true;
        } else if (a == "--kite") {
            kite = true;
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
        } else if (a == "--live-dir" && i + 1 < argc) {
            live_dir_arg = argv[++i];
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
    else if (kite) { mode = mode == "live" ? "kite" : "kite-dry"; }
    else if (live_auto) { mode = mode == "live" ? "auto" : "auto-dry"; }
    if (sim) { mode = "sim"; }
    if (!sim_date.empty() && (altair::live::parse_day(sim_date) == 0 || mode != "sim")) {
        std::printf("--date YYYY-MM-DD goes with --sim\n");
        return 2;
    }
    if (until_min < 0 || from_min < 0 || !(speed > 0.0 && speed <= 1000.0) || strikes < 0 || strikes > 60
        || depth_strikes < 0 || depth_strikes > strikes) {
        std::printf("--until/--from must be HH:MM, --speed 0..1000, --strikes 0..60, "
                    "--depth-strikes 0..--strikes\n");
        return 2;
    }

    if (mode.empty()) {
        usage(argv[0]);
        return 0;
    }

    // ---- the live terminal: FYERS or simulated, over the whole universe ----
    if (mode == "fyers" || mode == "fyers-dry" || mode == "sim" || mode == "kite" || mode == "kite-dry"
        || mode == "auto" || mode == "auto-dry") {
        const std::string src = ALTAIR_SOURCE_DIR;
        const std::string ds = ALTAIR_DATASET_DIR;
        const std::int64_t today = sim_date.empty() ? altair::live::ist_today(unix_now())
                                                    : altair::live::parse_day(sim_date);
        std::optional<FySession> session;
        std::optional<KiteCreds> kcreds;
        double nifty = atm_nifty, bnf = atm_banknifty;
        // FYERS only. Kite is legacy: it streams only when asked for by name
        // (--kite), never as a fall-back.
        if (mode == "auto" || mode == "auto-dry") {
            std::string fy_why;
            session = fyers_session(src + "/data/fyers_session.json");
            if (!fyers_usable(session, fy_why)) {
                std::printf("no live source today.\n  FYERS: %s\n  (log in to FYERS on Brokers; Kite is legacy and is not "
                            "used in its place)\n", fy_why.c_str());
                return 2;
            }
            std::printf("live source: FYERS\n");
            mode = mode == "auto" ? "fyers" : "fyers-dry";
        } else if (mode == "kite" || mode == "kite-dry") {
            std::string why;
            kcreds = kite_creds(src + "/data/kite_session.json", today, why);
            if (!kcreds) { std::printf("Kite: %s\n", why.c_str()); return 2; }
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

        std::string err;
        const auto rows = altair::live::read_kite_master(src + "/data/instruments.csv", err);
        if (!err.empty()) { std::printf("%s\n", err.c_str()); return 2; }
        altair::live::LiveUniverseOptions uo;
        uo.today = today;
        uo.nifty_spot = nifty;
        uo.banknifty_spot = bnf;
        uo.strikes = strikes;
        uo.depth_strikes = depth_strikes;
        uo.stocks = stocks;
        auto uni = altair::live::build_universe(
            rows, altair::live::read_stock_universe(src + "/config/universe_nifty50.csv"), uo);
        const std::string live_dir = live_dir_arg.empty() ? src + "/data/live" : live_dir_arg;
        const std::string watch_path = live_dir + "/watchlist.csv";
        // FYERS's own names for cash scrips (fetched for a FYERS feed; a SIM
        // only reads the copy on disk, if any).
        const auto cash_names = fyers_cash_names(live_dir, mode == "fyers" || mode == "fyers-dry");
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
        if (mode == "fyers-dry" || mode == "kite-dry") {
            for (std::size_t i = 0; i < uni.instruments.size() && i < 12; ++i) {
                std::printf("  %-10u %s\n", uni.instruments[i].token, uni.instruments[i].fyers.c_str());
            }
            std::printf("\nDRY RUN. No socket was opened. Add --go to stream from %s.\n",
                        mode == "kite-dry" ? "Kite" : "FYERS");
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
                    mode == "sim" ? "SIMULATED" : mode == "kite" ? "KITE LIVE" : "FYERS LIVE");
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
        std::int64_t deadline = ist_unix(today, until_min);
        if (seconds > 0 && unix_now() + seconds < deadline) { deadline = unix_now() + seconds; }
        const std::function<bool()> watch_changed = [&] { return !take_new_watch().empty(); };
        bool on_kite = mode == "kite";
        for (;;) {
            uni.instruments = known;   // everything added so far streams from the start of this run
            const int rc = on_kite ? run_kite(*kcreds, uni.instruments, shared, deadline, status, watch_changed)
                                   : run_fyers(*session, uni.instruments, shared, deadline, status, take_new_watch);
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

    // ---- live ------------------------------------------------------------
    const std::string api_key = env_or_empty("ALTAIR_KITE_API_KEY");
    std::string access;
    if (!read_access_token("data/kite_session.json", access)) {
        std::printf("no usable data/kite_session.json -- link a token first\n");
        return 2;
    }
    if (api_key.empty()) {
        std::printf("ALTAIR_KITE_API_KEY is not set\n");
        return 2;
    }

    // The decoder refuses an unknown token rather than inventing a price
    // scale, so every subscription is registered first. Scale 100 is what
    // equity and F&O ARE; lot and tick are deliberately 1 and unused, because
    // rule 1 says those come from the point-in-time spec store and this
    // program does not load one -- a lot size of 1 is obviously wrong in a way
    // that a plausible 75 typed here would not be.
    for (std::uint32_t t : sub_tokens) {
        altair::ContractSpec cs{};
        cs.token[static_cast<std::size_t>(altair::FeedSource::Kite)] = t;
        std::snprintf(cs.symbol, sizeof(cs.symbol), "TOKEN%u", t);
        cs.lot_size = altair::LotSize{1};
        cs.tick_size = altair::Price{1};
        cs.price_scale = 100;
        if (!g_specs.add(cs)) {
            std::printf("could not register token %u\n", t);
            return 2;
        }
    }

    constexpr std::size_t kMaxTicks = 512;
    constexpr std::size_t kMaxDepth = 512;
    std::vector<altair::Tick> ticks(kMaxTicks);
    std::vector<altair::DepthUpdate> depths(kMaxDepth);
    std::uint32_t seq = 0;
    std::size_t frames = 0, published = 0, unknown = 0, bad = 0;

    const auto on_frame = [&](const unsigned char* raw, std::size_t len) {
        ++frames;
        // THE CLOCK IS READ HERE AND PASSED IN, never inside the decoder. A
        // decoder that reads a wall clock cannot be replayed.
        const auto now = std::chrono::system_clock::now();
        const altair::Timestamp recv{
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()).count()};
        const auto r = altair::decode_kite_frame(
            raw, len, g_specs, recv, seq, ticks.data(), kMaxTicks,
            depths.data(), kMaxDepth);
        if (!r) { ++bad; return; }
        unknown += r->unknown_token;

        for (std::size_t i = 0; i < r->ticks; ++i) {
            const altair::Tick& t = ticks[i];
            // A TICK CARRIES CANONICAL IDENTITY, NOT A BROKER TOKEN. The
            // token is recovered HERE, at the one boundary that legitimately
            // knows both: SpecStore::add assigns ids densely from zero in
            // insertion order, so id i is sub_tokens[i]. Same boundary
            // kite_ticker_main.cpp uses, for the same reason.
            const auto idx = static_cast<std::size_t>(t.id);
            if (idx >= sub_tokens.size()) { continue; }

            altair::PricePayload p;
            p.token = sub_tokens[idx];
            p.last_paise = t.last.raw();
            p.last_qty = t.last_qty.raw();
            p.exchange_ts_ns = t.exchange_ts.ns_since_epoch();
            // ABSENCE IS NOT ZERO. An index reports no volume and no open
            // interest at all, and the decoder leaves them zero; publishing
            // them with the presence bit set would turn "this instrument has
            // no such field" into "it measured zero".
            if (t.volume.raw() > 0) {
                p.flags |= altair::kPriceHasVolume;
                p.volume = t.volume.raw();
            }
            if (t.oi > 0) {
                p.flags |= altair::kPriceHasOi;
                p.oi = t.oi;
            }
            if ((t.flags & static_cast<std::uint16_t>(
                     altair::TickFlag::NoExchangeTs)) != 0) {
                p.flags |= altair::kPriceNoExchTs;
            }
            bus.publish(altair::kTopicTrades, p, nullptr, nullptr,
                        p.exchange_ts_ns != 0 ? p.exchange_ts_ns
                                              : recv.ns_since_epoch());
            ++published;
        }

        for (std::size_t i = 0; i < r->depths; ++i) {
            const altair::DepthUpdate& d = depths[i];
            const auto idx = static_cast<std::size_t>(d.id);
            if (idx >= sub_tokens.size()) { continue; }
            altair::PricePayload p;
            p.token = sub_tokens[idx];
            p.exchange_ts_ns = d.exchange_ts.ns_since_epoch();
            const std::uint16_t levels = static_cast<std::uint16_t>(
                std::min<std::size_t>(altair::kMaxDepthLevels,
                                      std::max(d.bid_levels, d.ask_levels)));
            if (levels == 0) { continue; }
            p.flags |= altair::kPriceHasBook;
            p.depth_levels = levels;
            altair::PriceLevel bids[altair::kMaxDepthLevels]{};
            altair::PriceLevel asks[altair::kMaxDepthLevels]{};
            for (std::uint16_t k = 0; k < levels; ++k) {
                bids[k].price_paise = d.bid[k].px.raw();
                bids[k].qty = d.bid[k].qty.raw();
                bids[k].orders = d.bid[k].orders;
                asks[k].price_paise = d.ask[k].px.raw();
                asks[k].qty = d.ask[k].qty.raw();
                asks[k].orders = d.ask[k].orders;
            }
            bus.publish(altair::kTopicBook, p, bids, asks,
                        p.exchange_ts_ns != 0 ? p.exchange_ts_ns
                                              : recv.ns_since_epoch());
            ++published;
        }
        bus.poll();
    };

    std::printf("subscribing %zu token(s), full mode\n",
                sub_tokens.size());
    std::fflush(stdout);

    // Bounded by --seconds when given, else by the session (Ctrl-C): never
    // by the ticker's 60-second default.
    const auto stats = altair::kite_ticker_run(
        api_key, access, sub_tokens, altair::TickerMode::Full, on_frame,
        [&] { return expired() || g_stop.load(); },
        seconds > 0 ? std::chrono::seconds{seconds} : std::chrono::seconds{24 * 3600});

    std::printf("frames %zu, ticks+books published %zu, unknown token %zu, "
                "undecodable %zu\n",
                frames, published, unknown, bad);
    if (!stats) {
        std::printf("ticker ended: %s\n",
                    altair::ticker_error_text(stats.error()));
        return 1;
    }
    std::printf("delivered %llu frame(s) to subscribers, %llu coalesced\n",
                static_cast<unsigned long long>(bus.frames_sent()),
                static_cast<unsigned long long>(bus.coalesced()));
    return 0;
}
