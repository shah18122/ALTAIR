// core/affinity.hpp -- thread pinning and priorities for the latency-critical
// services (altair_price_service, altair_live_engine, altair_order_router).
//
// THE PLAN. config/latency.toml names, for each role, the core its thread is
// pinned to and its priority:
//   bus     the price bus's owner thread (publishes every tick to the clients)
//   feed    the FYERS socket and decoder thread
//   tbt     the FYERS 50-level book thread
//   engine  the models engine
//   router  the order router
// "auto" leaves the first `reserve` cores (default 2) to the system and the
// DCV desktop and gives the roles the rest, most critical first. With fewer
// than four cores nothing is pinned; the priorities still apply.
//
// THE PROCESS (Linux). nice -5; and, with lock_memory, mlockall(MCL_CURRENT |
// MCL_FUTURE | MCL_ONFAULT) so no page the service has touched is ever paged
// out -- but ONLY when RLIMIT_MEMLOCK is unlimited (ops/tune_host.sh sets it):
// under a finite limit MCL_FUTURE would turn a later allocation past the limit
// into an out-of-memory failure, and a service must never die of its tuning.
//
// THE THREADS. Pinned by pthread_setaffinity_np. A priority is a nice value;
// with realtime = true a PINNED thread at highest/time_critical runs
// SCHED_FIFO instead (priority 20 / 40), which needs RLIMIT_RTPRIO (again
// tune_host.sh) and is kept to pinned threads so it can only ever compete on
// its own core. The kernel's RT throttle (sched_rt_runtime_us) still leaves
// 5 % to everything else. Nothing here ever stops a service: a call that is
// refused says so and the service runs as it was.
//
// The plan and its parsing are here (pure, tested); the OS calls are in
// core/affinity.cpp.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace altair::latency {

enum class Prio : std::uint8_t { Normal, AboveNormal, Highest, TimeCritical };

[[nodiscard]] inline const char* prio_text(Prio p) noexcept {
    switch (p) {
    case Prio::Normal: return "normal";
    case Prio::AboveNormal: return "above_normal";
    case Prio::Highest: return "highest";
    case Prio::TimeCritical: return "time_critical";
    }
    return "normal";
}

[[nodiscard]] inline std::optional<Prio> prio_from(std::string_view t) noexcept {
    if (t == "normal") return Prio::Normal;
    if (t == "above_normal") return Prio::AboveNormal;
    if (t == "highest") return Prio::Highest;
    if (t == "time_critical") return Prio::TimeCritical;
    return std::nullopt;
}

/// The roles, most latency-critical first: "auto" hands out cores in this order.
inline constexpr const char* kRoles[] = {"bus", "feed", "engine", "router", "tbt", "reader"};

struct RolePlan {
    int core = -1;            ///< -1: not pinned
    Prio prio = Prio::Highest;
};

struct Plan {
    bool enabled = true;
    bool high_priority = true;   ///< the process: nice -5
    bool lock_memory = true;     ///< mlockall, when RLIMIT_MEMLOCK is unlimited
    bool realtime = false;       ///< SCHED_FIFO for pinned highest/time_critical threads
    bool spin = false;           ///< hot-path threads busy-poll instead of sleeping (needs isolated cores)
    int reserve = 2;             ///< cores left to the OS and the desktop when "auto"
    std::map<std::string, RolePlan> roles;
};

/// "auto" for every role: cores reserve.. in kRoles order, wrapping; none
/// pinned with fewer than four cores (or no core left after the reserve).
[[nodiscard]] inline Plan default_plan(unsigned cores, int reserve = 2) {
    Plan p;
    p.reserve = reserve < 0 ? 0 : reserve;
    const int n = static_cast<int>(cores);
    const int free = n - p.reserve;
    int k = 0;
    for (const char* r : kRoles) {
        RolePlan rp;
        rp.prio = std::string_view(r) == "tbt" ? Prio::AboveNormal : Prio::Highest;
        rp.core = (n >= 4 && free > 0) ? p.reserve + (k++ % free) : -1;
        p.roles[r] = rp;
    }
    return p;
}

namespace detail {
[[nodiscard]] inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
[[nodiscard]] inline std::string_view unquote(std::string_view s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}
[[nodiscard]] inline std::optional<int> to_int(std::string_view s) {
    if (s.empty() || s.size() > 6) return std::nullopt;
    int v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + (c - '0');
    }
    return v;
}
} // namespace detail

/// Parse config/latency.toml (the small subset it uses: sections, `key =
/// value`, # comments). A key it does not know, or a value it cannot read, is
/// an error naming the line: a typo must not silently run unpinned.
[[nodiscard]] inline std::optional<Plan> parse_plan(std::string_view text, unsigned cores, std::string& err) {
    // First pass for `reserve`, which "auto" depends on.
    int reserve = 2;
    {
        std::string_view rest = text, section;
        while (!rest.empty()) {
            const auto nl = rest.find('\n');
            std::string_view line = detail::trim(rest.substr(0, nl));
            rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
            if (const auto h = line.find('#'); h != std::string_view::npos) line = detail::trim(line.substr(0, h));
            if (!line.empty() && line.front() == '[') { section = line; continue; }
            const auto eq = line.find('=');
            if (eq == std::string_view::npos || !section.empty()) continue;
            if (detail::trim(line.substr(0, eq)) == "reserve")
                if (const auto v = detail::to_int(detail::trim(line.substr(eq + 1)))) reserve = *v;
        }
    }
    Plan p = default_plan(cores, reserve);
    std::string_view rest = text, section;
    int lineno = 0;
    while (!rest.empty()) {
        ++lineno;
        const auto nl = rest.find('\n');
        std::string_view line = detail::trim(rest.substr(0, nl));
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        if (const auto h = line.find('#'); h != std::string_view::npos) line = detail::trim(line.substr(0, h));
        if (line.empty()) continue;
        const auto bad = [&](const std::string& why) {
            err = "config/latency.toml line " + std::to_string(lineno) + ": " + why;
            return std::nullopt;
        };
        if (line.front() == '[') {
            section = line;
            if (section != "[cores]" && section != "[priority]") return bad("unknown section " + std::string(section));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) return bad("expected key = value");
        const std::string_view key = detail::trim(line.substr(0, eq)), raw = detail::trim(line.substr(eq + 1));
        const std::string_view val = detail::unquote(raw);
        if (section.empty()) {
            if (key == "enabled" || key == "high_priority" || key == "lock_memory" || key == "realtime"
                || key == "spin") {
                if (val != "true" && val != "false") return bad(std::string(key) + " must be true or false");
                bool& b = key == "enabled" ? p.enabled : key == "high_priority" ? p.high_priority
                        : key == "lock_memory" ? p.lock_memory : key == "realtime" ? p.realtime : p.spin;
                b = val == "true";
            } else if (key == "reserve") {
                if (!detail::to_int(val)) return bad("reserve must be a whole number");
            } else {
                return bad("unknown key " + std::string(key));
            }
            continue;
        }
        const auto it = p.roles.find(std::string(key));
        if (it == p.roles.end()) return bad("unknown role " + std::string(key));
        if (section == "[cores]") {
            if (val == "auto") continue;
            if (val == "off") { it->second.core = -1; continue; }
            const auto v = detail::to_int(val);
            if (!v) return bad("a core is \"auto\", \"off\" or a number");
            if (*v >= static_cast<int>(cores)) return bad("core " + std::to_string(*v) + " does not exist here ("
                                                          + std::to_string(cores) + " cores)");
            it->second.core = *v;
        } else {
            const auto pr = prio_from(val);
            if (!pr) return bad("a priority is normal, above_normal, highest or time_critical");
            it->second.prio = *pr;
        }
    }
    return p;
}

/// The plan for this machine: config/latency.toml when present (a bad one is
/// reported in `note` and the defaults are used), else the defaults.
[[nodiscard]] Plan load_plan(const std::string& path, std::string& note);

/// Hardware threads (at least 1).
[[nodiscard]] unsigned hardware_cores();

/// The process: nice, memory locking. Returns what was done.
std::string apply_process(const Plan& p);

/// mlockall when the plan asks and RLIMIT_MEMLOCK is unlimited. Returns what was done.
std::string lock_memory(const Plan& p);

/// The calling thread, as `role`: pin and priority. Returns what was done.
/// `spinning`: the thread busy-polls (Plan::spin). It is then never put on
/// SCHED_FIFO: the kernel's RT throttle would stop a FIFO thread that never
/// sleeps for 50 ms of every second. On an isolated core it needs no FIFO.
std::string apply_thread(const Plan& p, std::string_view role, bool spinning = false);

} // namespace altair::latency
