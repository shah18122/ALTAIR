#!/usr/bin/env bash
# ops/tune_host.sh -- tune the Linux server for ALTAIR's latency (or undo it).
#
# What it sets, and where (every file is ALTAIR's own, so --undo removes
# exactly what this added and nothing else):
#
#   /etc/default/grub.d/99-altair.cfg     isolcpus=managed_irq,domain,<cores> nohz_full=<cores>
#                                         rcu_nocbs=<cores> transparent_hugepage=madvise
#                                         (needs a REBOOT; then update-grub has run)
#   /etc/sysctl.d/90-altair.conf          vm.nr_hugepages, net.core.busy_poll/busy_read 50 us,
#                                         vm.swappiness 1, kernel.numa_balancing 0
#   /etc/security/limits.d/altair.conf    memlock unlimited, rtprio 50, nice -10 for --user
#                                         (a new login picks it up; the systemd jobs carry
#                                         the same limits in their units)
#   /etc/default/irqbalance               IRQBALANCE_BANNED_CPULIST=<cores>: device
#                                         interrupts stay off the isolated cores
#   CPU governor                          performance, where the hypervisor exposes one
#
# <cores> are the cores ALTAIR's pinned threads get (config/latency.toml with
# reserve = 2 puts "auto" on 2..). Default: 2 to the last core when the box
# has 8 or more; with fewer, no cores are isolated (the desktop needs them).
#
# The kernel's RT throttle (kernel.sched_rt_runtime_us) is left alone, so a
# SCHED_FIFO thread can never take a whole core from the kernel.
#
#   sudo ops/tune_host.sh                    # dry run: shows every change
#   sudo ops/tune_host.sh --go               # apply (reboot afterwards for the GRUB part)
#   sudo ops/tune_host.sh --go --isolate 4-7 --hugepages 1024
#   sudo ops/tune_host.sh --undo --go        # remove it all (reboot afterwards)
#   build/linux/app/altair_latency_probe     # what is in effect, and the numbers

set -euo pipefail
go=0; undo=0; user="${SUDO_USER:-$(id -un)}"; isolate=""; hugepages=512
while [ $# -gt 0 ]; do
    case "$1" in
        --go) go=1 ;;
        --undo) undo=1 ;;
        --user) user="$2"; shift ;;
        --isolate) isolate="$2"; shift ;;
        --hugepages) hugepages="$2"; shift ;;
        -h|--help) sed -n '2,31p' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done

[ "$go" = 0 ] || [ "$(id -u)" = 0 ] || { echo "run it with sudo" >&2; exit 2; }
grub=/etc/default/grub.d/99-altair.cfg
sysctl_file=/etc/sysctl.d/90-altair.conf
limits=/etc/security/limits.d/altair.conf
irq=/etc/default/irqbalance

if [ "$undo" = 1 ]; then
    for f in "$grub" "$sysctl_file" "$limits"; do
        if [ "$go" = 1 ]; then rm -f "$f"; else echo "would remove $f"; fi
    done
    if [ "$go" = 1 ]; then
        [ -f "$irq" ] && sed -i '/^IRQBALANCE_BANNED_CPULIST=.*# altair$/d' "$irq"
        command -v update-grub >/dev/null && update-grub
        sysctl --system >/dev/null
        systemctl try-restart irqbalance 2>/dev/null || true
        echo "undone. Reboot to drop the kernel command line changes."
    else
        echo "would drop ALTAIR's line from $irq, run update-grub and sysctl --system"
    fi
    exit 0
fi

cores="$(nproc --all)"
if [ -z "$isolate" ] && [ "$cores" -ge 8 ]; then isolate="2-$((cores - 1))"; fi
if [ -n "$isolate" ] && ! [[ "$isolate" =~ ^[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*$ ]]; then
    echo "--isolate takes a CPU list such as 2-7 or 2,3,6-7" >&2; exit 2
fi
[[ "$hugepages" =~ ^[0-9]+$ ]] || { echo "--hugepages takes a number" >&2; exit 2; }
id "$user" >/dev/null 2>&1 || { echo "no such user: $user" >&2; exit 2; }

cmdline="transparent_hugepage=madvise"
[ -n "$isolate" ] && cmdline="isolcpus=managed_irq,domain,$isolate nohz_full=$isolate rcu_nocbs=$isolate $cmdline"

write() {   # write <file> <content>
    if [ "$go" = 1 ]; then
        install -d -m 0755 "$(dirname "$1")"
        printf '%s\n' "$2" > "$1"
        chmod 0644 "$1"
        echo "wrote $1"
    else
        printf 'would write %s:\n%s\n\n' "$1" "$2" | sed 's/^/  /'
    fi
}

echo "== $cores cores; isolated for ALTAIR: ${isolate:-none (fewer than 8 cores)}"

write "$grub" "# ALTAIR (ops/tune_host.sh): cores for the pinned threads, no tick, no RCU callbacks there.
GRUB_CMDLINE_LINUX_DEFAULT=\"\$GRUB_CMDLINE_LINUX_DEFAULT $cmdline\""

write "$sysctl_file" "# ALTAIR (ops/tune_host.sh)
vm.nr_hugepages = $hugepages
net.core.busy_poll = 50
net.core.busy_read = 50
vm.swappiness = 1
kernel.numa_balancing = 0"

write "$limits" "# ALTAIR (ops/tune_host.sh): mlockall and SCHED_FIFO for $user's ALTAIR services.
$user soft memlock unlimited
$user hard memlock unlimited
$user soft rtprio 50
$user hard rtprio 50
$user soft nice -10
$user hard nice -10"

if [ -n "$isolate" ]; then
    if [ "$go" = 1 ]; then
        touch "$irq"
        sed -i '/^IRQBALANCE_BANNED_CPULIST=.*# altair$/d' "$irq"
        echo "IRQBALANCE_BANNED_CPULIST=$isolate  # altair" >> "$irq"
        echo "irqbalance: interrupts kept off $isolate"
    else
        echo "  would add IRQBALANCE_BANNED_CPULIST=$isolate to $irq"
    fi
fi

gov=(/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor)
if [ -e "${gov[0]}" ]; then
    if [ "$go" = 1 ]; then for g in "${gov[@]}"; do echo performance > "$g" 2>/dev/null || true; done; echo "governor: performance (until reboot)"
    else echo "  would set the CPU governor to performance"; fi
else
    echo "  CPU governor: not exposed here (the hypervisor fixes the frequency); nothing to do"
fi

if [ "$go" = 1 ]; then
    command -v update-grub >/dev/null && update-grub
    sysctl --system >/dev/null
    echo never > /sys/kernel/mm/transparent_hugepage/defrag 2>/dev/null || true
    echo madvise > /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || true
    systemctl try-restart irqbalance 2>/dev/null || true
    echo
    echo "Done. Hugepages, busy polling and THP are live now. REBOOT for the isolated cores,"
    echo "and log in again (DCV session) for the memlock/rtprio limits. Then check with:"
    echo "  build/linux/app/altair_latency_probe"
else
    echo
    echo "DRY RUN: nothing changed. Run with --go to apply."
fi
