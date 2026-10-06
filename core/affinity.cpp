// core/affinity.cpp -- the OS calls behind core/affinity.hpp.

#include <core/affinity.hpp>

#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#if defined(_MSC_VER)
#pragma comment(lib, "winmm.lib")
#endif
#elif defined(__linux__)
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
#if defined(_WIN32)
    std::string out = "process:";
    out += SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) ? " HIGH priority class" : " priority class refused";
    if (p.timer_ms > 0)
        out += timeBeginPeriod(static_cast<UINT>(p.timer_ms)) == TIMERR_NOERROR
                   ? ", " + std::to_string(p.timer_ms) + " ms timer"
                   : ", timer refused";
#if defined(PROCESS_POWER_THROTTLING_CURRENT_VERSION) && defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0602
    PROCESS_POWER_THROTTLING_STATE st{};
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask = 0;   // controlled, and off: never an efficiency core
    out += SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st) ? ", power throttling off"
                                                                                              : ", power throttling unchanged";
#endif
    return out;
#elif defined(__linux__)
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
#if defined(_WIN32)
    if (r.core >= 0 && r.core < 64) {
        const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << r.core;
        out += SetThreadAffinityMask(GetCurrentThread(), mask) != 0 ? " core " + std::to_string(r.core)
                                                                     : " core " + std::to_string(r.core) + " refused";
    } else {
        out += " not pinned";
    }
    int wp = THREAD_PRIORITY_NORMAL;
    switch (r.prio) {
    case Prio::Normal: wp = THREAD_PRIORITY_NORMAL; break;
    case Prio::AboveNormal: wp = THREAD_PRIORITY_ABOVE_NORMAL; break;
    case Prio::Highest: wp = THREAD_PRIORITY_HIGHEST; break;
    case Prio::TimeCritical: wp = THREAD_PRIORITY_TIME_CRITICAL; break;
    }
    out += SetThreadPriority(GetCurrentThread(), wp) ? std::string(", ") + prio_text(r.prio)
                                                     : std::string(", priority refused");
#elif defined(__linux__)
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
