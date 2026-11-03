// core/affinity.cpp -- the OS calls behind core/affinity.hpp.

#include <core/affinity.hpp>

#include <fstream>
#include <sstream>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace altair::latency {

unsigned hardware_cores() {
    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1u : n;
}

Plan load_plan(const std::string& path, std::string& note) {
    const unsigned cores = hardware_cores();
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        note = "latency: " + path + " not found; defaults (" + std::to_string(cores) + " cores)";
        return default_plan(cores);
    }
    std::stringstream s;
    s << f.rdbuf();
    std::string err;
    if (auto p = parse_plan(s.str(), cores, err)) {
        note = "latency: " + path + " (" + std::to_string(cores) + " cores)";
        return *p;
    }
    note = "latency: " + err + "; defaults used";
    return default_plan(cores);
}

std::string apply_process(const Plan& p) {
    if (!p.enabled) return "latency: off (config/latency.toml)";
    if (!p.high_priority) return "latency: process priority left as it is";
#if defined(__linux__)
    const bool raised = setpriority(PRIO_PROCESS, 0, -5) == 0;
    return raised ? "process: nice -5" : "process: priority unchanged (raising it needs privileges)";
#else
    return "process: priority not set on this OS";
#endif
}

std::string apply_thread(const Plan& p, std::string_view role) {
    const std::string name(role);
    if (!p.enabled) return name + ": not pinned (latency off)";
    const auto it = p.roles.find(name);
    if (it == p.roles.end()) return name + ": no such role";
    const RolePlan& r = it->second;
    std::string out = name + ":";
#if defined(__linux__)
    if (r.core >= 0) {
        cpu_set_t allowed;
        CPU_ZERO(&allowed);
        const bool known = sched_getaffinity(0, sizeof allowed, &allowed) == 0;
        if (known && r.core < CPU_SETSIZE && CPU_ISSET(r.core, &allowed)) {
            cpu_set_t one;
            CPU_ZERO(&one);
            CPU_SET(r.core, &one);
            out += pthread_setaffinity_np(pthread_self(), sizeof one, &one) == 0 ? " core " + std::to_string(r.core)
                                                                                 : " core " + std::to_string(r.core) + " refused";
        } else {
            out += " core " + std::to_string(r.core) + " not available to this process";
        }
    } else {
        out += " not pinned";
    }
    if (r.prio != Prio::Normal) {
        const int nice = r.prio == Prio::AboveNormal ? -2 : r.prio == Prio::Highest ? -5 : -10;
        const auto tid = static_cast<id_t>(syscall(SYS_gettid));
        out += setpriority(PRIO_PROCESS, tid, nice) == 0 ? std::string(", ") + prio_text(r.prio)
                                                         : std::string(", priority unchanged (needs privileges)");
    }
#else
    (void)r;
    out += " pinning and priorities not supported on this OS";
#endif
    return out;
}

} // namespace altair::latency
