// altair_order_router -- sends the Terminal's orders to FYERS when live
// trading is armed (oms/live_router.hpp says how, and what refuses).
//
//   altair_order_router [--root DIR] [--dry-run] [--once]
//
// Reads data/order_intents.jsonl, data/live_trading.json,
// data/kill_request.json, data/live_orders/cancels.jsonl, data/live/universe.csv
// and the FYERS session (data/fyers_session.json, else ALTAIR_FYERS_CLIENT_ID /
// ALTAIR_FYERS_ACCESS_TOKEN). Writes data/live_orders/orders.json (the rows and
// a heartbeat the Terminal reads) and data/live_orders/journal.jsonl (every
// request, refusal, send, reply and status). The session is never printed.
//
// One router at a time (data/live_orders/router.lock). The Terminal starts it
// when LIVE is switched on; it runs until Ctrl+C or its parent closes it.

#include <app/fyers_env_session.hpp>
#include <broker/fyers_api.hpp>
#include <broker/https_client.hpp>
#include <live/file_lock.hpp>
#include <live/universe.hpp>
#include <oms/live_router.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace altair;

std::atomic<bool> g_stop{false};
extern "C" void on_signal(int) { g_stop.store(true); }

struct RouterSession { std::string client, access; };

/// data/fyers_session.json, then the environment. Never printed.
std::optional<RouterSession> read_session(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (in) {
        const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (const auto j = oms::parse_router_json(all)) {
            RouterSession s{j->str("client_id"), j->str("access_token")};
            if (!s.client.empty() && !s.access.empty() && s.client.size() <= fyers::kClientIdMax
                && s.access.size() <= fyers::kAccessTokenMax)
                return s;
        }
    }
    if (auto env = fyers_env::from_environment()) return RouterSession{env->client, env->access};
    return std::nullopt;
}

std::vector<oms::RouterInstrument> read_instruments(const std::string& path) {
    std::vector<oms::RouterInstrument> out;
    for (const auto& i : live::read_universe(path)) {
        if (i.kind == live::LiveKind::Index) continue;
        oms::RouterInstrument r;
        r.token = i.token;
        r.symbol = i.symbol;
        r.fyers = i.fyers;
        r.exchange = i.kind == live::LiveKind::Equity ? (i.fyers.rfind("BSE:", 0) == 0 ? "BSE" : "NSE") : "NFO";
        r.lot = i.lot;
        r.tick_paise = std::llround(i.tick * 100.0);
        out.push_back(std::move(r));
    }
    return out;
}

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void usage(const char* exe) {
    std::printf(
        "  Sends the Terminal's orders to FYERS while live trading is armed.\n\n"
        "    %s [--root DIR] [--dry-run] [--once]\n\n"
        "    --root     the tree holding data/ (default: the source tree)\n"
        "    --dry-run  check every request and show the exact order, send nothing\n"
        "    --once     one pass, then exit (for a check)\n",
        exe);
}

}  // namespace

int main(int argc, char** argv) {
    std::string root = ALTAIR_SOURCE_DIR;
    bool dry_run = false, once = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--root" && i + 1 < argc) { root = argv[++i]; continue; }
        if (a == "--dry-run") { dry_run = true; continue; }
        if (a == "--once") { once = true; continue; }
        std::printf("unknown argument %s\n", a.c_str());
        usage(argv[0]);
        return 2;
    }
    std::error_code ec;
    fs::create_directories(fs::path(root) / oms::kRouterDir, ec);
    live::LiveFileLock lock((fs::path(root) / oms::kRouterDir / "router.lock").string());
    if (!lock.held()) {
        std::printf("another order router is running (%s)\n", lock.why().c_str());
        return 3;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const std::string session_path = root + "/data/fyers_session.json";
    const std::string universe_path = root + "/data/live/universe.csv";
    std::optional<RouterSession> session = read_session(session_path);
    std::string auth;
    const auto make_auth = [&] {
        auth.clear();
        if (!session) return;
        if (const auto h = fyers::authorization_header(session->client.c_str(), session->access.c_str())) auth = *h;
    };
    make_auth();
    const auto generation = [&]() -> std::uint64_t {
        if (auth.empty()) return 0;
        const std::uint64_t g = oms::detail::fnv1a(auth.data(), auth.size());
        return g == 0 ? 1 : g;
    };

    oms::RouterTransport transport;
    transport.get = [&](const std::string& target) {
        oms::RouterCall c;
        if (auth.empty()) return c;
        const auto r = https_fyers_request("GET", "api-t1.fyers.in", target, {}, auth);
        if (r) { c.transport_ok = true; c.status = static_cast<int>(r->status); c.body = r->body; }
        return c;
    };
    transport.send = [&](const std::string& method, const std::string& target, const std::string& body) {
        oms::RouterCall c;
        if (auth.empty()) return c;
        const auto r = https_fyers_request(method, "api-t1.fyers.in", target, body, auth);
        if (r) { c.transport_ok = true; c.status = static_cast<int>(r->status); c.body = r->body; }
        return c;
    };

    auto instruments = read_instruments(universe_path);
    oms::OrderRouter router(oms::RouterPaths::under(root), instruments, transport, generation(), dry_run, now_ns());
    if (!router.startup_error().empty()) {
        std::printf("order router: %s\n", router.startup_error().c_str());
        return 4;
    }
    std::printf("order router%s: %zu instruments, FYERS session %s; requests from %s\n", dry_run ? " (DRY RUN)" : "",
                instruments.size(), auth.empty() ? "MISSING (log in on the Brokers page)" : "present",
                (root + "/" + oms::kIntentFile).c_str());
    std::fflush(stdout);

    auto universe_time = fs::last_write_time(universe_path, ec);
    auto session_time = fs::last_write_time(session_path, ec);
    std::string last_why;
    std::size_t last_rows = router.rows().size();
    for (;;) {
        // The universe and the session are re-read when their files change: a
        // watchlist addition, a login during the day.
        if (const auto t = fs::last_write_time(universe_path, ec); !ec && t != universe_time) {
            universe_time = t;
            router.set_universe(read_instruments(universe_path));
        }
        if (const auto t = fs::last_write_time(session_path, ec); !ec && t != session_time) {
            session_time = t;
            session = read_session(session_path);
            make_auth();
            router.set_session(generation());
        }
        router.tick(now_ns());
        if (router.status().why != last_why) {
            last_why = router.status().why;
            std::printf("%s\n", last_why.empty() ? "LIVE: requests are sent to FYERS" : ("refusing requests: " + last_why).c_str());
            std::fflush(stdout);
        }
        for (; last_rows < router.rows().size(); ++last_rows) {
            const auto& r = router.rows()[last_rows];
            std::printf("%s %lld %s %s: %s%s%s\n", r.side.c_str(), static_cast<long long>(r.qty), r.symbol.c_str(),
                        oms::router_status_text(r.status), r.message.c_str(), r.fyers_id.empty() ? "" : " id ",
                        r.fyers_id.c_str());
            std::fflush(stdout);
        }
        if (once || g_stop.load()) break;
        // Armed, a request is picked up within 5 ms (an arbitrage's legs go
        // back to back); off, four passes a second is plenty.
        std::this_thread::sleep_for(std::chrono::milliseconds(router.status().armed ? 5 : 250));
    }
    return 0;
}
