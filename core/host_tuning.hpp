// core/host_tuning.hpp -- what the Linux host is tuned for, read back.
//
// ops/tune_host.sh sets the host up for low latency (isolated cores, no timer
// tick on them, hugepages, busy polling, raised memlock/rtprio limits, IRQs
// kept off the isolated cores). This reads what is ACTUALLY in effect, from
// /proc and /sys, so the latency probe and the terminal's Latency page report
// the machine as it is rather than as the script intended: a GRUB change only
// counts after the reboot, and a limit only for sessions started after it.
//
// The parsers are pure (tested on fixed text); read_host_tuning() does the
// file reads and never throws: a file it cannot read is reported as unknown.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <sys/resource.h>

namespace altair::host {

namespace detail {

[[nodiscard]] inline std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

[[nodiscard]] inline std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

} // namespace detail

/// The value of `key=` on a kernel command line ("" when absent; "on" for a bare flag).
[[nodiscard]] inline std::string cmdline_arg(std::string_view cmdline, std::string_view key) {
    std::size_t pos = 0;
    while (pos < cmdline.size()) {
        while (pos < cmdline.size() && (cmdline[pos] == ' ' || cmdline[pos] == '\n')) ++pos;
        std::size_t end = cmdline.find_first_of(" \n", pos);
        if (end == std::string_view::npos) end = cmdline.size();
        const std::string_view tok = cmdline.substr(pos, end - pos);
        if (tok == key) return "on";
        if (tok.size() > key.size() && tok.substr(0, key.size()) == key && tok[key.size()] == '=')
            return std::string(tok.substr(key.size() + 1));
        pos = end;
    }
    return {};
}

/// A number from /proc/meminfo ("HugePages_Total:     512"); -1 when absent.
[[nodiscard]] inline long long meminfo_value(std::string_view meminfo, std::string_view key) {
    std::size_t pos = 0;
    while (pos < meminfo.size()) {
        std::size_t end = meminfo.find('\n', pos);
        if (end == std::string_view::npos) end = meminfo.size();
        const std::string_view line = meminfo.substr(pos, end - pos);
        if (line.size() > key.size() && line.substr(0, key.size()) == key && line[key.size()] == ':') {
            long long v = 0;
            bool any = false;
            for (const char c : line.substr(key.size() + 1)) {
                if (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); any = true; }
                else if (any) break;
            }
            return any ? v : -1;
        }
        pos = end + 1;
    }
    return -1;
}

/// The selected value of a sysfs choice file ("always [madvise] never" -> "madvise").
[[nodiscard]] inline std::string bracketed(std::string_view s) {
    const auto a = s.find('['), b = s.find(']');
    if (a == std::string_view::npos || b == std::string_view::npos || b < a) return std::string(s);
    return std::string(s.substr(a + 1, b - a - 1));
}

/// How many CPUs a kernel CPU list names ("2-7" -> 6, "2,4,6-7" -> 4, "" -> 0).
[[nodiscard]] inline int cpu_count(std::string_view list) {
    int n = 0;
    std::size_t pos = 0;
    // isolcpus may carry flags first ("managed_irq,domain,2-7"): skip words.
    while (pos < list.size()) {
        std::size_t end = list.find(',', pos);
        if (end == std::string_view::npos) end = list.size();
        const std::string_view part = list.substr(pos, end - pos);
        int a = -1, b = -1, *cur = &a;
        bool ok = !part.empty();
        for (const char c : part) {
            if (c >= '0' && c <= '9') *cur = (*cur < 0 ? 0 : *cur * 10) + (c - '0');
            else if (c == '-' && cur == &a) cur = &b;
            else { ok = false; break; }
        }
        if (ok && a >= 0) n += (b >= a) ? b - a + 1 : 1;
        pos = end + 1;
    }
    return n;
}

struct Item {
    std::string name;
    std::string value;
    bool good = false;      ///< tuned as tune_host.sh sets it
    std::string advice;     ///< what to do when not
};

struct HostTuning {
    std::string isolcpus, nohz_full, rcu_nocbs, governor, thp, clocksource;
    long long hugepages_total = -1, hugepages_free = -1, hugepage_kb = -1;
    long long memlock = -2;   ///< bytes; -1 unlimited; -2 unknown
    long long rtprio = -2;    ///< -1 unlimited; -2 unknown
    long long busy_poll = -1, busy_read = -1;

    [[nodiscard]] std::vector<Item> items() const {
        std::vector<Item> v;
        const std::string tune = "sudo ops/tune_host.sh --go, then reboot";
        v.push_back({"isolated cores (isolcpus)", isolcpus.empty() ? "none" : isolcpus, !isolcpus.empty(),
                     "tune_host.sh --isolate 2-7 (" + tune + ")"});
        v.push_back({"no timer tick (nohz_full)", nohz_full.empty() ? "none" : nohz_full, !nohz_full.empty(), tune});
        v.push_back({"RCU callbacks moved off (rcu_nocbs)", rcu_nocbs.empty() ? "none" : rcu_nocbs, !rcu_nocbs.empty(), tune});
        v.push_back({"hugepages (2 MiB)", hugepages_total < 0 ? "unknown"
                         : std::to_string(hugepages_total) + " (" + std::to_string(hugepages_free) + " free)",
                     hugepages_total > 0, "sudo ops/tune_host.sh --go (vm.nr_hugepages)"});
        v.push_back({"transparent hugepages", thp.empty() ? "unknown" : thp, thp == "madvise" || thp == "never",
                     "madvise: no background compaction stalls (tune_host.sh)"});
        v.push_back({"memlock limit", memlock == -1 ? "unlimited" : memlock < 0 ? "unknown" : std::to_string(memlock / 1024) + " KiB",
                     memlock == -1, "log in again after tune_host.sh (limits.d/altair.conf)"});
        v.push_back({"realtime priority limit", rtprio == -1 ? "unlimited" : rtprio < 0 ? "unknown" : std::to_string(rtprio),
                     rtprio == -1 || rtprio >= 40, "log in again after tune_host.sh (limits.d/altair.conf)"});
        v.push_back({"busy polling (net.core.busy_poll / busy_read)",
                     busy_poll < 0 ? "unknown" : std::to_string(busy_poll) + " / " + std::to_string(busy_read) + " us",
                     busy_poll > 0 && busy_read > 0, "sudo ops/tune_host.sh --go (sysctl)"});
        v.push_back({"CPU frequency governor", governor.empty() ? "not exposed (fixed by the hypervisor)" : governor,
                     governor.empty() || governor == "performance", "performance (tune_host.sh)"});
        v.push_back({"clock source", clocksource.empty() ? "unknown" : clocksource,
                     clocksource == "tsc" || clocksource == "kvm-clock", "tsc"});
        return v;
    }
};

[[nodiscard]] inline HostTuning read_host_tuning() {
    using detail::slurp;
    using detail::trim;
    HostTuning h;
    const std::string cmd = slurp("/proc/cmdline");
    h.isolcpus = cmdline_arg(cmd, "isolcpus");
    h.nohz_full = cmdline_arg(cmd, "nohz_full");
    h.rcu_nocbs = cmdline_arg(cmd, "rcu_nocbs");
    const std::string mi = slurp("/proc/meminfo");
    h.hugepages_total = meminfo_value(mi, "HugePages_Total");
    h.hugepages_free = meminfo_value(mi, "HugePages_Free");
    h.hugepage_kb = meminfo_value(mi, "Hugepagesize");
    h.thp = bracketed(trim(slurp("/sys/kernel/mm/transparent_hugepage/enabled")));
    h.governor = trim(slurp("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"));
    h.clocksource = trim(slurp("/sys/devices/system/clocksource/clocksource0/current_clocksource"));
    const std::string bp = trim(slurp("/proc/sys/net/core/busy_poll")), br = trim(slurp("/proc/sys/net/core/busy_read"));
    if (!bp.empty()) h.busy_poll = std::atoll(bp.c_str());
    if (!br.empty()) h.busy_read = std::atoll(br.c_str());
    rlimit r{};
    if (getrlimit(RLIMIT_MEMLOCK, &r) == 0) h.memlock = r.rlim_cur == RLIM_INFINITY ? -1 : static_cast<long long>(r.rlim_cur);
    if (getrlimit(RLIMIT_RTPRIO, &r) == 0) h.rtprio = r.rlim_cur == RLIM_INFINITY ? -1 : static_cast<long long>(r.rlim_cur);
    return h;
}

} // namespace altair::host
