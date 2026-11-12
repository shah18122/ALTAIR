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
#include <broker/https_keepalive.hpp>
#if defined(ALTAIR_BYPASS_HTTPS)
#include <net/bypass_https.hpp>
#include <net/links.hpp>
#endif
#include <memory>
#include <core/affinity.hpp>
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

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

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
        // MCX and currency are prices and paper only: not in the router's
        // universe, so a request for one is refused as unknown.
        if (i.group == live::kCommodityGroup || i.group == live::kCurrencyGroup) continue;
        oms::RouterInstrument r;
        r.token = i.token;
        r.symbol = i.symbol;
        r.fyers = i.fyers;
        r.exchange = i.kind == live::LiveKind::Equity ? (i.fyers.rfind("BSE:", 0) == 0 ? "BSE" : "NSE") : "NFO";
        r.lot = i.lot;
        r.tick_paise = std::llround(i.tick * 100.0);
        r.option = i.kind == live::LiveKind::Call || i.kind == live::LiveKind::Put;
        out.push_back(std::move(r));
    }
    return out;
}

/// Wakes the router the moment the Terminal or a strategy writes a request,
/// arms, kills or cancels, instead of after a 5 ms sleep (2.5 ms on average).
/// inotify on the two folders, filtered by file name: the router's own writes
/// (orders.json, journal.jsonl, the cursor) never wake it. Without inotify it
/// falls back to the plain sleep.
class RequestWake {
public:
    RequestWake(const std::string& data_dir, const std::string& router_dir) {
        fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd_ < 0) return;
        const std::uint32_t mask = IN_CLOSE_WRITE | IN_MODIFY | IN_MOVED_TO | IN_CREATE | IN_DELETE;
        data_wd_ = ::inotify_add_watch(fd_, data_dir.c_str(), mask);
        router_wd_ = ::inotify_add_watch(fd_, router_dir.c_str(), mask);
    }
    ~RequestWake() { if (fd_ >= 0) ::close(fd_); }
    RequestWake(const RequestWake&) = delete;
    RequestWake& operator=(const RequestWake&) = delete;
    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0 && data_wd_ >= 0; }
    /// A request file changed since the last look (never blocks).
    [[nodiscard]] bool ready_now() {
        if (!ok()) return false;
        pollfd p{fd_, POLLIN, 0};
        return ::poll(&p, 1, 0) > 0 && relevant();
    }
    /// FYERS's webhook wrote an order update (data/fyers_webhook_events.jsonl)
    /// since the last call.
    [[nodiscard]] bool take_order_update() noexcept { const bool u = order_update_; order_update_ = false; return u; }

    /// Returns when a request file changed, or after `timeout_ms`.
    void wait(int timeout_ms) {
        if (!ok()) { std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms)); return; }
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
            if (left <= 0) return;
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, static_cast<int>(left)) <= 0) return;
            if (relevant()) return;
        }
    }

private:
    bool relevant() {
        alignas(inotify_event) char buf[4096];
        bool hit = false;
        for (;;) {
            const ssize_t n = ::read(fd_, buf, sizeof buf);
            if (n <= 0) break;
            for (ssize_t at = 0; at < n;) {
                const auto* e = reinterpret_cast<const inotify_event*>(buf + at);
                if ((e->mask & IN_Q_OVERFLOW) != 0) hit = true;
                if (e->len > 0) {
                    const std::string name = e->name;
                    if (e->wd == data_wd_ && name == "fyers_webhook_events.jsonl") { order_update_ = true; hit = true; }
                    if ((e->wd == data_wd_ && (name == "order_intents.jsonl" || name == "live_trading.json" || name == "kill_request.json"))
                        || (e->wd == router_wd_ && name == "cancels.jsonl"))
                        hit = true;
                }
                at += static_cast<ssize_t>(sizeof(inotify_event) + e->len);
            }
        }
        return hit;
    }
    int fd_ = -1, data_wd_ = -1, router_wd_ = -1;
    bool order_update_ = false;
};

/// Atomic write of the connection numbers the Terminal and the latency report read.
void write_latency(const std::string& path, const std::string& body) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return;
        f << body << '\n';
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
}

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void usage(const char* exe) {
    std::printf(
        "  Sends the Terminal's orders to FYERS while live trading is armed.\n\n"
        "    %s [--root DIR] [--dry-run] [--once]\n"
        "        [--dpdk-port PCI --dpdk-ip ADDR --dpdk-gw ADDR [--dpdk-eal ARGS]]\n\n"
        "    --root     the tree holding data/ (default: the source tree)\n"
        "    --dry-run  check every request and show the exact order, send nothing\n"
        "    --once     one pass, then exit (for a check)\n"
        "    --dpdk-port  send orders and cancels over ALTAIR's own TCP/TLS on this DPDK port\n"
        "               (the spare ENI on vfio-pci; it needs a public IP, an Elastic IP). The\n"
        "               kernel line stays as the fallback for anything that could not be sent.\n"
        "    --dpdk-ip  the port's private address; --dpdk-gw the subnet's gateway\n"
        "    --dpdk-eal DPDK's arguments (default: -a <PCI> --in-memory)\n",
        exe);
}

}  // namespace

int main(int argc, char** argv) {
    std::string root = ALTAIR_SOURCE_DIR;
    bool dry_run = false, once = false;
    std::string dpdk_port, dpdk_ip, dpdk_gw, dpdk_eal;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--root" && i + 1 < argc) { root = argv[++i]; continue; }
        if (a == "--dry-run") { dry_run = true; continue; }
        if (a == "--once") { once = true; continue; }
        if (a == "--dpdk-port" && i + 1 < argc) { dpdk_port = argv[++i]; continue; }
        if (a == "--dpdk-ip" && i + 1 < argc) { dpdk_ip = argv[++i]; continue; }
        if (a == "--dpdk-gw" && i + 1 < argc) { dpdk_gw = argv[++i]; continue; }
        if (a == "--dpdk-eal" && i + 1 < argc) { dpdk_eal = argv[++i]; continue; }
        std::printf("unknown argument %s\n", a.c_str());
        usage(argv[0]);
        return 2;
    }
    // The Terminal starts the router without flags: the server's DPDK order
    // line, when it has one, is named in /etc/altair/order_line.conf
    // (DPDK_PORT=0000:28:00.0, DPDK_IP=<the card's private IP>, DPDK_GW=<gateway>).
    if (dpdk_port.empty()) {
        std::ifstream conf("/etc/altair/order_line.conf");
        for (std::string line; std::getline(conf, line);) {
            const auto eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "DPDK_PORT") dpdk_port = v;
            else if (k == "DPDK_IP") dpdk_ip = v;
            else if (k == "DPDK_GW") dpdk_gw = v;
            else if (k == "DPDK_EAL") dpdk_eal = v;
        }
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

    // Two kept connections (broker/https_keepalive.hpp): one carries only the
    // orders and cancels, so an order never queues behind the 2-second order
    // book poll or a quote; the other carries every read. A new connection per
    // call cost ~8 ms (connect + TLS handshake) before each order's first byte.
    oms::RouterTransport transport;
    HttpsKeepAlive order_conn("api-t1.fyers.in"), query_conn("api-t1.fyers.in");
    constexpr const char* kProfilePath = "/api/v3/profile";   // a cheap authorised read that keeps the order line warm
    transport.get = [&](const std::string& target) {
        oms::RouterCall c;
        if (auth.empty()) return c;
        const auto r = query_conn.request("GET", target, {}, auth);
        if (r) { c.transport_ok = true; c.status = static_cast<int>(r->status); c.body = r->body; }
        return c;
    };
    // The bypass order line (--dpdk-port): orders and cancels on ALTAIR's own
    // TCP/TLS over the spare ENI, the kernel line behind it. Only a request
    // that never left (no ARP, no SYN-ACK, no TLS) goes to the kernel line;
    // one that was written and got no reply is uncertain and is reported so.
#if defined(ALTAIR_BYPASS_HTTPS) && defined(ALTAIR_DPDK)
    std::unique_ptr<net::DpdkPort> dpdk;
    std::unique_ptr<net::DpdkLink> dpdk_link;
    std::unique_ptr<net::BypassHttps> bypass;
    if (!dpdk_port.empty()) {
        std::string err;
        dpdk = net::DpdkPort::open(dpdk_eal.empty() ? "-a " + dpdk_port + " --in-memory --log-level=lib.*:error" : dpdk_eal,
                                   dpdk_port, err);
        if (!dpdk) {
            std::printf("DPDK order line: %s; orders go over the kernel line\n", err.c_str());
        } else {
            dpdk_link = std::make_unique<net::DpdkLink>(*dpdk);
            net::BypassHttpsConfig bc;
            bc.host = "api-t1.fyers.in";
            bc.local_ip = dpdk_ip;
            bc.next_hop_ip = dpdk_gw;
            bypass = std::make_unique<net::BypassHttps>(*dpdk_link, bc);
            std::printf("DPDK order line on %s (%s via %s)\n", dpdk_port.c_str(), dpdk_ip.c_str(), dpdk_gw.c_str());
        }
    }
#else
    struct NoBypass {
        void service() {}
    };
    std::unique_ptr<NoBypass> bypass;
    if (!dpdk_port.empty()) std::printf("--dpdk-port: this build has no DPDK (build with ALTAIR_DPDK); orders go over the kernel line\n");
#endif
    std::string bypass_note;   // the last reason the bypass line could not send, printed when it changes
    transport.send = [&](const std::string& method, const std::string& target, const std::string& body) {
        oms::RouterCall c;
        if (auth.empty()) return c;
#if defined(ALTAIR_BYPASS_HTTPS) && defined(ALTAIR_DPDK)
        if (bypass) {
            const auto b = bypass->request(method, target, body, auth);
            if (b.ok) {
                c.transport_ok = true; c.status = static_cast<int>(b.response.status); c.body = b.response.body;
                return c;
            }
            if (b.error == net::BypassHttpsError::Transport) return c;   // written, no reply: uncertain, never resent
            if (b.why != bypass_note) {
                bypass_note = b.why;
                std::printf("DPDK order line could not send (%s): this one goes over the kernel line\n", b.why.c_str());
                std::fflush(stdout);
            }
        }
#endif
        const auto r = order_conn.request(method, target, body, auth);
        if (r) { c.transport_ok = true; c.status = static_cast<int>(r->status); c.body = r->body; }
        return c;
    };
    // Warm the order line: connect, then one read, which also takes in the TLS session tickets.
    const auto warm_orders = [&] {
        if (auth.empty() || dry_run) return;
        if (order_conn.warm()) (void)order_conn.request("GET", kProfilePath, {}, auth);
#if defined(ALTAIR_BYPASS_HTTPS) && defined(ALTAIR_DPDK)
        if (bypass && bypass->warm()) (void)bypass->request("GET", kProfilePath, {}, auth);
#endif
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

    // Cores and priorities (config/latency.toml): this thread sends the orders.
    {
        std::string note;
        const auto plan = altair::latency::load_plan(root + "/config/latency.toml", note);
        std::printf("%s\n  %s\n  %s\n", note.c_str(), altair::latency::apply_process(plan).c_str(),
                    altair::latency::apply_thread(plan, "router").c_str());
        std::fflush(stdout);
    }
    warm_orders();
    RequestWake wake(root + "/data", (fs::path(root) / oms::kRouterDir).string());
    std::printf("request wake-up: %s\n", wake.ok() ? "inotify (a request is picked up as it is written)" : "polling every 5 ms");
    const std::string latency_path = (fs::path(root) / oms::kRouterDir / "latency.json").string();
    std::uint64_t latency_written = ~0ull;
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
            order_conn.close();   // a new login: new connections under the new token
            query_conn.close();
            warm_orders();
        }
        if (wake.take_order_update()) router.poke_book(now_ns());   // FYERS's webhook: read the book now
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
        // Armed, the order line is kept warm: a read when it has been idle 20 s
        // (inside every idle limit on the way, well under FYERS' rate limits).
        bool cold = !order_conn.connected() || order_conn.idle() > std::chrono::seconds(20);
#if defined(ALTAIR_BYPASS_HTTPS) && defined(ALTAIR_DPDK)
        if (bypass) cold = cold || !bypass->connected() || bypass->idle() > std::chrono::seconds(20);
#endif
        if (router.status().armed && !dry_run && !auth.empty() && cold) warm_orders();
        if (const std::uint64_t n = order_conn.stats().requests + query_conn.stats().requests + order_conn.stats().connects
                                    + query_conn.stats().connects;
            n != latency_written) {
            latency_written = n;
            std::string j = "{\"orders\": " + order_conn.json() + ", \"reads\": " + query_conn.json();
#if defined(ALTAIR_BYPASS_HTTPS) && defined(ALTAIR_DPDK)
            if (bypass) j += ", \"orders_dpdk\": " + bypass->json();
#endif
            write_latency(latency_path, j + "}");
        }
        if (once || g_stop.load()) break;
        // Armed, a request wakes the router as it is written (inotify), with a
        // 5 ms pass as the floor; off, four passes a second is plenty.
        if (bypass) {
            // The DPDK port has nobody else to answer its ARP or take in its
            // ACKs: it is serviced while the router waits, spinning (the
            // router's core is its own), a request still waking it at once.
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(router.status().armed ? 5 : 250);
            while (std::chrono::steady_clock::now() < until && !wake.ready_now()) bypass->service();
        } else {
            wake.wait(router.status().armed ? 5 : 250);
        }
    }
    return 0;
}
