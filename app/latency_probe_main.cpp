// altair_latency_probe -- what latency this server can actually give ALTAIR.
//
//   altair_latency_probe [--n 20] [--hosts api-t1.fyers.in,socket.fyers.in]
//                        [--core N] [--wakeups 5000] [--no-network] [--out data/verified/latency]
//
// Three measurements, and the host's tuning as it is in effect now:
//   1. NETWORK: TCP connect time to each FYERS host on 443, --n times (one
//      SYN/SYN-ACK round trip; nothing is sent, no login, no TLS). This is the
//      floor under every tick and every order: the number kernel bypass
//      would have to beat to matter.
//   2. TIMER WAKE-UP: a 100 us clock_nanosleep, --wakeups times, on --core
//      (default: the "feed" core in config/latency.toml), with the plan's
//      priority. The overshoot is the scheduling jitter a blocked thread pays
//      when its data arrives: what isolcpus/nohz_full/SCHED_FIFO shrink.
//   3. MEMORY: whether mlockall succeeds under this session's limits, and
//      whether a 2 MiB hugepage can be mapped.
// Tick-to-decision inside the engine is measured by altair_live_engine itself
// (its "frames that ran the models" histogram, printed at the end of a run).
//
// Writes the report to <out>/probe-<IST day>.txt and .json. Read-only for the
// host: it changes no setting (ops/tune_host.sh does that).

#include <app/activity_log.hpp>
#include <core/affinity.hpp>
#include <core/host_tuning.hpp>
#include <live/latency.hpp>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace {

using altair::live::LatencyHistogram;

std::int64_t mono_ns() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000LL + t.tv_nsec;
}

/// One TCP connect to `ai`, in ns; -1 on failure or a 2 s timeout.
std::int64_t connect_ns(const addrinfo* ai) {
    const int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol);
    if (fd < 0) return -1;
    const std::int64_t t0 = mono_ns();
    std::int64_t out = -1;
    int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc == 0) {
        out = mono_ns() - t0;
    } else if (errno == EINPROGRESS) {
        pollfd p{fd, POLLOUT, 0};
        if (::poll(&p, 1, 2000) == 1) {
            int err = 0;
            socklen_t len = sizeof err;
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) out = mono_ns() - t0;
        }
    }
    ::close(fd);
    return out;
}

struct HostResult {
    std::string host, address, error;
    LatencyHistogram rtt;
    int failed = 0;
};

HostResult probe_host(const std::string& host, int n) {
    HostResult r;
    r.host = host;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::int64_t d0 = mono_ns();
    if (const int g = ::getaddrinfo(host.c_str(), "443", &hints, &res); g != 0 || res == nullptr) {
        r.error = std::string("DNS: ") + gai_strerror(g);
        return r;
    }
    const std::int64_t dns = mono_ns() - d0;
    char buf[NI_MAXHOST] = {0};
    ::getnameinfo(res->ai_addr, res->ai_addrlen, buf, sizeof buf, nullptr, 0, NI_NUMERICHOST);
    r.address = buf;
    r.error = "dns " + std::to_string(dns / 1000) + " us";
    for (int i = 0; i < n; ++i) {
        const std::int64_t ns = connect_ns(res);
        if (ns < 0) ++r.failed; else r.rtt.record(ns);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::freeaddrinfo(res);
    return r;
}

LatencyHistogram wakeups(int n) {
    LatencyHistogram h;
    for (int i = 0; i < n; ++i) {
        timespec t{};
        clock_gettime(CLOCK_MONOTONIC, &t);
        const std::int64_t want = static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000LL + t.tv_nsec + 100'000;
        timespec at{static_cast<time_t>(want / 1'000'000'000LL), static_cast<long>(want % 1'000'000'000LL)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &at, nullptr) == EINTR) {}
        h.record(mono_ns() - want);
    }
    return h;
}

std::string hugepage_try() {
    void* p = ::mmap(nullptr, 2u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p == MAP_FAILED) return std::string("no (") + std::strerror(errno) + ")";
    static_cast<volatile char*>(p)[0] = 1;
    ::munmap(p, 2u << 20);
    return "yes";
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (const char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) o += c;
    }
    return o;
}

} // namespace

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    int n = 20, nwake = 5000, core = -2;
    bool network = true;
    std::vector<std::string> hosts = {"api-t1.fyers.in", "socket.fyers.in"};
    fs::path out = fs::path(ALTAIR_SOURCE_DIR) / "data" / "verified" / "latency";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--n" && has) n = std::max(1, std::atoi(argv[++i]));
        else if (a == "--wakeups" && has) nwake = std::max(1, std::atoi(argv[++i]));
        else if (a == "--core" && has) core = std::atoi(argv[++i]);
        else if (a == "--out" && has) out = argv[++i];
        else if (a == "--no-network") network = false;
        else if (a == "--hosts" && has) {
            hosts.clear();
            std::string s = argv[++i], cur;
            for (const char c : s) { if (c == ',') { if (!cur.empty()) hosts.push_back(cur); cur.clear(); } else cur += c; }
            if (!cur.empty()) hosts.push_back(cur);
        } else {
            std::printf("usage: altair_latency_probe [--n 20] [--hosts a,b] [--core N] [--wakeups 5000] [--no-network] [--out DIR]\n");
            return a == "-h" || a == "--help" ? 0 : 2;
        }
    }

    std::string text, json = "{";
    const auto line = [&](const std::string& s) { std::printf("%s\n", s.c_str()); text += s + "\n"; };
    const auto ms_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string day = altair::activity::ist_day(ms_now);
    line("ALTAIR latency probe, " + altair::activity::ist_stamp(ms_now));

    line("\nhost tuning (in effect now):");
    const auto tuning = altair::host::read_host_tuning();
    json += "\"tuning\": [";
    bool first = true;
    for (const auto& it : tuning.items()) {
        line(std::string("  ") + (it.good ? "ok    " : "todo  ") + it.name + ": " + it.value + (it.good ? "" : "  -> " + it.advice));
        json += std::string(first ? "" : ", ") + "{\"name\": \"" + json_escape(it.name) + "\", \"value\": \""
              + json_escape(it.value) + "\", \"good\": " + (it.good ? "true" : "false") + "}";
        first = false;
    }
    json += "]";

    std::string note;
    altair::latency::Plan plan = altair::latency::load_plan(std::string(ALTAIR_SOURCE_DIR) + "/config/latency.toml", note);
    line("\nmemory:");
    line("  " + altair::latency::lock_memory(plan));
    const std::string huge = hugepage_try();
    line("  a 2 MiB hugepage maps: " + huge);
    json += ", \"mlock\": \"" + json_escape(altair::latency::lock_memory(plan)) + "\", \"hugepage\": \"" + json_escape(huge) + "\"";

    if (network) {
        line("\nnetwork: TCP connect to FYERS on 443 (one round trip; nothing sent)");
        json += ", \"network\": [";
        first = true;
        for (const auto& h : hosts) {
            const HostResult r = probe_host(h, n);
            if (r.address.empty()) {
                line("  " + h + ": " + r.error);
            } else {
                line("  " + h + " (" + r.address + ", " + r.error + "): " + r.rtt.text(1e3, "us")
                     + (r.failed ? ", " + std::to_string(r.failed) + " failed" : ""));
            }
            json += std::string(first ? "" : ", ") + "{\"host\": \"" + json_escape(h) + "\", \"address\": \"" + json_escape(r.address)
                  + "\", \"failed\": " + std::to_string(r.failed) + ", \"rtt_us\": " + r.rtt.json(1e3) + "}";
            first = false;
        }
        json += "]";
    }

    // The wake-up test runs where the feed thread would: its core and priority.
    if (core == -2) {
        const auto it = plan.roles.find("feed");
        core = it != plan.roles.end() ? it->second.core : -1;
    }
    if (core >= 0) plan.roles["feed"].core = core;
    std::string where;
    LatencyHistogram wake;
    std::thread th([&] { where = altair::latency::apply_thread(plan, "feed"); wake = wakeups(nwake); });
    th.join();
    line("\ntimer wake-up overshoot, 100 us sleeps (" + where + "): " + wake.text(1e3, "us"));
    json += ", \"wakeup_where\": \"" + json_escape(where) + "\", \"wakeup_us\": " + wake.json(1e3) + "}";

    line("\nWhat the numbers mean: the network round trip is the floor under every tick and order;"
         "\nkernel bypass could only cut the host's share, which is the wake-up overshoot plus the"
         "\nkernel's socket path (tens of us). Tick-to-decision: altair_live_engine's own histogram.");

    std::error_code ec;
    fs::create_directories(out, ec);
    std::ofstream(out / ("probe-" + day + ".txt"), std::ios::binary | std::ios::trunc) << text;
    std::ofstream(out / ("probe-" + day + ".json"), std::ios::binary | std::ios::trunc) << json << "\n";
    std::printf("\nwritten: %s\n", (out / ("probe-" + day + ".txt")).string().c_str());
    return 0;
}
