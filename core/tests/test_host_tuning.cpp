// core/host_tuning.hpp: the kernel command line, /proc/meminfo and sysfs
// choice files read correctly, and the live read never throws.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <core/host_tuning.hpp>

#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "BAD ", what);
    if (!ok) ++failures;
}
} // namespace

int main() {
    using namespace altair::host;
    std::printf("host tuning\n");
    const char* cmd = "BOOT_IMAGE=/vmlinuz root=UUID=x ro isolcpus=managed_irq,domain,2-7 nohz_full=2-7 rcu_nocbs=2-7 quiet\n";
    check(cmdline_arg(cmd, "isolcpus") == "managed_irq,domain,2-7", "isolcpus with its flags");
    check(cmdline_arg(cmd, "nohz_full") == "2-7" && cmdline_arg(cmd, "quiet") == "on" && cmdline_arg(cmd, "nosmt").empty(),
          "a value, a bare flag, and an absent key");
    check(cmdline_arg("nohz_fullx=1 nohz_full=3", "nohz_full") == "3", "a longer key with the same prefix is not it");
    const char* mi = "MemTotal:       16000000 kB\nHugePages_Total:     512\nHugePages_Free:      500\nHugepagesize:       2048 kB\n";
    check(meminfo_value(mi, "HugePages_Total") == 512 && meminfo_value(mi, "HugePages_Free") == 500
              && meminfo_value(mi, "Hugepagesize") == 2048 && meminfo_value(mi, "Nope") == -1,
          "meminfo numbers, and -1 for a missing key");
    check(bracketed("always [madvise] never") == "madvise" && bracketed("x") == "x", "the selected sysfs choice");
    check(cpu_count("2-7") == 6 && cpu_count("2,4,6-7") == 4 && cpu_count("managed_irq,domain,2-7") == 6 && cpu_count("") == 0,
          "CPU lists count, flags skipped");
    const HostTuning h = read_host_tuning();
    const auto items = h.items();
    for (const auto& it : items) std::printf("    %s %s: %s\n", it.good ? "ok  " : "todo", it.name.c_str(), it.value.c_str());
    check(items.size() == 10 && h.memlock != -2, "this host reads back, every item named");
    std::printf(failures == 0 ? "all host tuning checks passed\n" : "%d host tuning checks did not pass\n", failures);
    return failures == 0 ? 0 : 1;
}
