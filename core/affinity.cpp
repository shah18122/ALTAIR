// core/affinity.cpp -- the OS calls behind core/affinity.hpp.

#include <core/affinity.hpp>

#include <fstream>
#include <sstream>
#include <thread>

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

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
    std::string out = setpriority(PRIO_PROCESS, 0, -5) == 0 ? "process: nice -5"
                                                            : "process: priority unchanged (raising it needs privileges)";
    out += "; " + lock_memory(p);
    return out;
}

std::string lock_memory(const Plan& p) {
    if (!p.enabled || !p.lock_memory) return "memory not locked (lock_memory = false)";
    rlimit lim{};
    if (getrlimit(RLIMIT_MEMLOCK, &lim) != 0) return "memory not locked (RLIMIT_MEMLOCK unreadable)";
    if (lim.rlim_cur != RLIM_INFINITY)
        return "memory not locked (RLIMIT_MEMLOCK " + std::to_string(lim.rlim_cur / 1024)
             + " KiB, not unlimited: run sudo ops/tune_host.sh --go)";
#if defined(MCL_ONFAULT)
    const int flags = MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT;
#else
    const int flags = MCL_CURRENT | MCL_FUTURE;
#endif
    return mlockall(flags) == 0 ? "memory locked (mlockall)" : "memory not locked (mlockall refused)";
}

std::string apply_thread(const Plan& p, std::string_view role) {
    const std::string name(role);
    if (!p.enabled) return name + ": not pinned (latency off)";
    const auto it = p.roles.find(name);
    if (it == p.roles.end()) return name + ": no such role";
    const RolePlan& r = it->second;
    std::string out = name + ":";
    bool pinned = false;
    if (r.core >= 0) {
        cpu_set_t allowed;
        CPU_ZERO(&allowed);
        const bool known = sched_getaffinity(0, sizeof allowed, &allowed) == 0;
        if (known && r.core < CPU_SETSIZE && CPU_ISSET(static_cast<std::size_t>(r.core), &allowed)) {
            cpu_set_t one;
            CPU_ZERO(&one);
            CPU_SET(static_cast<std::size_t>(r.core), &one);
            pinned = pthread_setaffinity_np(pthread_self(), sizeof one, &one) == 0;
            out += " core " + std::to_string(r.core) + (pinned ? "" : " refused");
        } else {
            out += " core " + std::to_string(r.core) + " not available to this process";
        }
    } else {
        out += " not pinned";
    }
    if (p.realtime && pinned && (r.prio == Prio::Highest || r.prio == Prio::TimeCritical)) {
        sched_param sp{};
        sp.sched_priority = r.prio == Prio::TimeCritical ? 40 : 20;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) {
            out += std::string(", SCHED_FIFO ") + std::to_string(sp.sched_priority);
            return out;
        }
        out += ", SCHED_FIFO refused (RLIMIT_RTPRIO: run sudo ops/tune_host.sh --go)";
    }
    if (r.prio != Prio::Normal) {
        const int nice = r.prio == Prio::AboveNormal ? -2 : r.prio == Prio::Highest ? -5 : -10;
        const auto tid = static_cast<id_t>(syscall(SYS_gettid));
        out += setpriority(PRIO_PROCESS, tid, nice) == 0 ? std::string(", ") + prio_text(r.prio)
                                                         : std::string(", priority unchanged (needs privileges)");
    }
    return out;
}

} // namespace altair::latency
