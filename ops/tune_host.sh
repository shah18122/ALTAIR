#!/usr/bin/env bash
# ops/tune_host.sh -- tune the Linux server for ALTAIR's latency (or undo it).
#
# What it sets, and where (every file is ALTAIR's own, so --undo removes
# exactly what this added and nothing else):
#
#   /etc/default/grub.d/99-altair.cfg     isolcpus=managed_irq,domain,<cores> nohz_full=<cores>
#                                         rcu_nocbs=<cores> transparent_hugepage=madvise
#                                         (needs a REBOOT; then update-grub has run)
#                                         (default: the scheduler never balances onto them, not even
#                                         after hours. --dynamic-isolation drops "domain": the cores stay
#                                         schedulable and
#                                         ops/linux/cpuset.sh keeps every non-ALTAIR service off
#                                         them 08:45-15:50, so the nightly retrain gets every core.
#                                         NOT yet safe: the DCV desktop (user.slice) can still roam
#                                         onto 2-7 in the session.)
#   /etc/sysctl.d/90-altair.conf          vm.nr_hugepages, net.core.busy_poll/busy_read 50 us,
#                                         vm.swappiness 1, kernel.numa_balancing 0, no KSM-style
#                                         reclaim, TCP: no autocorking, no slow start after idle
#                                         (the order line), small notsent queue, bigger socket
#                                         buffers and backlog for the feed
#   /etc/security/limits.d/altair.conf    memlock unlimited, rtprio 50, nice -10 for --user
#                                         (a new login picks it up; the systemd jobs carry
#                                         the same limits in their units)
#   /etc/default/irqbalance               IRQBALANCE_BANNED_CPULIST=<cores>: device
#                                         interrupts stay off the isolated cores
#   CPU governor                          performance, where the hypervisor exposes one
#
# --deep (opt-in, on top of the above; every file again ALTAIR's own):
#   /etc/default/grub.d/98-altair-deep.cfg   irqaffinity=<housekeeping> kthread_cpus=<housekeeping>
#                                         idle=poll (no HLT exit and wake-up on a vCPU: tens of us
#                                         off every wake; every core then shows 100 % busy, which
#                                         EC2 does not bill for) processor.max_cstate=1
#                                         intel_idle.max_cstate=0 nosoftlockup nowatchdog
#                                         nmi_watchdog=0 tsc=reliable skew_tick=1 rcu_nocb_poll
#                                         preempt=full audit=0    (REBOOT)
#   /etc/sysctl.d/91-altair-deep.conf     kernel.timer_migration 0, vm.stat_interval 10,
#                                         kernel.watchdog 0, vm.dirty_* small (no write-back storms)
#   --mitigations-off                     adds mitigations=off: 5-30 % faster syscalls, but turns
#                                         off the CPU's protection between processes ON THIS VM.
#                                         Only on a box that runs nothing but ALTAIR. Never default.
#   --trim                                stops and disables services a trading box does not
#                                         need (snapd, unattended-upgrades, apport, ModemManager,
#                                         multipathd, fwupd, packagekit; the list is kept in
#                                         /etc/altair/trimmed-services and --undo restarts exactly those)
#   --headless                            default boot target multi-user (no display manager).
#                                         Refused when a DCV CONSOLE session exists (that needs X);
#                                         DCV virtual sessions are unaffected. The old target is
#                                         kept in /etc/altair/default-target for --undo.
#
# BEFORE A --deep REBOOT: take an EBS snapshot of the root volume (EC2 console,
# Volumes, Create snapshot) and make sure the EC2 serial console is enabled
# (EC2, Settings, EC2 Serial Console). If the box does not come back, the serial
# console reaches GRUB: pick "Advanced options", edit the entry, delete the
# ALTAIR words from the linux line, boot, then `sudo ops/tune_host.sh --undo --go`.
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
#   sudo ops/tune_host.sh --go --deep --trim # the deep tier as well (after market hours)
#   sudo ops/tune_host.sh --undo --go        # remove it all, deep tier included (reboot afterwards)
#   build/linux/app/altair_latency_probe     # what is in effect, and the numbers

set -euo pipefail
go=0; undo=0; user="${SUDO_USER:-$(id -un)}"; isolate=""; hugepages=512
deep=0; mitig=0; trim=0; headless=0; static_iso=1
while [ $# -gt 0 ]; do
    case "$1" in
        --go) go=1 ;;
        --undo) undo=1 ;;
        --user) user="$2"; shift ;;
        --isolate) isolate="$2"; shift ;;
        --hugepages) hugepages="$2"; shift ;;
        --deep) deep=1 ;;
        --mitigations-off) mitig=1 ;;
        --trim) trim=1 ;;
        --headless) headless=1 ;;
        --static-isolation) static_iso=1 ;;
        --dynamic-isolation) static_iso=0 ;;
        -h|--help) sed -n '2,68p' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done

[ "$go" = 0 ] || [ "$(id -u)" = 0 ] || { echo "run it with sudo" >&2; exit 2; }
grub=/etc/default/grub.d/99-altair.cfg
sysctl_file=/etc/sysctl.d/90-altair.conf
limits=/etc/security/limits.d/altair.conf
irq=/etc/default/irqbalance
grub_deep=/etc/default/grub.d/98-altair-deep.cfg
sysctl_deep=/etc/sysctl.d/91-altair-deep.conf
trimmed=/etc/altair/trimmed-services
target_file=/etc/altair/default-target
trim_list="snapd.service snapd.socket unattended-upgrades.service apport.service ModemManager.service multipathd.service multipathd.socket fwupd.service packagekit.service apt-daily.timer apt-daily-upgrade.timer motd-news.timer"

if [ "$undo" = 1 ]; then
    for f in "$grub" "$sysctl_file" "$limits" "$grub_deep" "$sysctl_deep"; do
        if [ "$go" = 1 ]; then rm -f "$f"; else echo "would remove $f"; fi
    done
    if [ -f "$trimmed" ]; then
        if [ "$go" = 1 ]; then
            back="$(tr '\n' ' ' < "$trimmed")"
            # shellcheck disable=SC2086
            [ -n "$back" ] && systemctl enable --now $back >/dev/null 2>&1 || true
            rm -f "$trimmed"; echo "services re-enabled: ${back:-none}"
        else echo "would re-enable: $(cat "$trimmed" | tr '\n' ' ')"; fi
    fi
    if [ -f "$target_file" ]; then
        if [ "$go" = 1 ]; then systemctl set-default "$(cat "$target_file")" && rm -f "$target_file"
        else echo "would set the default boot target back to $(cat "$target_file")"; fi
    fi
    if [ "$go" = 1 ]; then
        [ -f "$irq" ] && sed -i -e '/^IRQBALANCE_BANNED_CPULIST=.*# altair$/d' -e '/^# altair: next line$/{N;d}' "$irq"
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
if [ -z "$isolate" ] && [ "$cores" -ge 8 ]; then
    # Housekeeping = cpu0's whole physical core (0 and its hyperthread twin);
    # ALTAIR gets every other physical core whole, so no system noise shares
    # an L1/L2 with a pinned thread. c7i.2xlarge: housekeeping 0,4; isolated 1-3,5-7.
    twin="$(cut -d, -f2 /sys/devices/system/cpu/cpu0/topology/thread_siblings_list 2>/dev/null | cut -d- -f2)"
    if [[ "$twin" =~ ^[0-9]+$ ]] && [ "$twin" -gt 1 ] && [ "$twin" -lt $((cores - 1)) ]; then
        isolate="1-$((twin - 1)),$((twin + 1))-$((cores - 1))"
    else
        isolate="2-$((cores - 1))"
    fi
fi
if [ -n "$isolate" ] && ! [[ "$isolate" =~ ^[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*$ ]]; then
    echo "--isolate takes a CPU list such as 2-7 or 2,3,6-7" >&2; exit 2
fi
[[ "$hugepages" =~ ^[0-9]+$ ]] || { echo "--hugepages takes a number" >&2; exit 2; }
id "$user" >/dev/null 2>&1 || { echo "no such user: $user" >&2; exit 2; }

cmdline="transparent_hugepage=madvise"
iso_flags="managed_irq"; [ "$static_iso" = 1 ] && iso_flags="managed_irq,domain"
[ -n "$isolate" ] && cmdline="isolcpus=$iso_flags,$isolate nohz_full=$isolate rcu_nocbs=$isolate $cmdline"

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
kernel.numa_balancing = 0
vm.zone_reclaim_mode = 0
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216
net.core.netdev_max_backlog = 16384
net.core.netdev_budget = 600
net.ipv4.tcp_rmem = 4096 262144 16777216
net.ipv4.tcp_wmem = 4096 65536 16777216
net.ipv4.tcp_autocorking = 0
net.ipv4.tcp_slow_start_after_idle = 0
net.ipv4.tcp_notsent_lowat = 16384
net.ipv4.tcp_mtu_probing = 1
net.ipv4.tcp_fastopen = 3"

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
        sed -i -e '/^IRQBALANCE_BANNED_CPULIST=.*# altair$/d' -e '/^# altair: next line$/{N;d}' "$irq"
        # The comment on its own line: systemd EnvironmentFile keeps an inline "# ..." as part of the value.
        printf '# altair: next line\nIRQBALANCE_BANNED_CPULIST=%s\n' "$isolate" >> "$irq"
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

if [ "$deep" = 1 ]; then
    housekeeping="0-1"
    if [ -n "$isolate" ]; then   # the complement of $isolate
        hk=""; for c in $(seq 0 $((cores - 1))); do
            in=0; IFS=, read -ra parts <<< "$isolate"
            for r in "${parts[@]}"; do lo="${r%-*}"; hi="${r#*-}"; [ "$c" -ge "$lo" ] && [ "$c" -le "$hi" ] && in=1; done
            [ "$in" = 0 ] && hk="$hk,$c"; done
        [ -n "$hk" ] && housekeeping="${hk#,}"
    fi
    dcmd="irqaffinity=$housekeeping kthread_cpus=$housekeeping idle=poll processor.max_cstate=1 intel_idle.max_cstate=0"
    dcmd="$dcmd nosoftlockup nowatchdog nmi_watchdog=0 tsc=reliable skew_tick=1 rcu_nocb_poll preempt=full audit=0"
    [ "$mitig" = 1 ] && dcmd="$dcmd mitigations=off"
    write "$grub_deep" "# ALTAIR (ops/tune_host.sh --deep): housekeeping on $housekeeping, no idle exits, no watchdogs.
GRUB_CMDLINE_LINUX_DEFAULT=\"\$GRUB_CMDLINE_LINUX_DEFAULT $dcmd\""
    write "$sysctl_deep" "# ALTAIR (ops/tune_host.sh --deep)
kernel.timer_migration = 0
vm.stat_interval = 10
kernel.watchdog = 0
vm.dirty_background_bytes = 16777216
vm.dirty_bytes = 67108864"
fi
if [ "$trim" = 1 ]; then
    present=""
    for u in $trim_list; do
        systemctl list-unit-files "$u" --no-legend 2>/dev/null | grep -q . && present="$present $u"
    done
    if [ "$go" = 1 ]; then
        install -d -m 0755 /etc/altair
        # Only what was enabled before is put back on --undo.
        : > "$trimmed.new"
        for u in $present; do systemctl is-enabled "$u" >/dev/null 2>&1 && echo "$u" >> "$trimmed.new"; done
        [ -f "$trimmed" ] && sort -u "$trimmed" "$trimmed.new" -o "$trimmed.new"
        mv "$trimmed.new" "$trimmed"
        # shellcheck disable=SC2086
        [ -n "$present" ] && systemctl disable --now $present >/dev/null 2>&1 || true
        echo "services stopped and disabled:$present (kept in $trimmed for --undo)"
    else
        echo "  would stop and disable:$present"
    fi
fi
if [ "$headless" = 1 ]; then
    if command -v dcv >/dev/null && dcv list-sessions 2>/dev/null | grep -qi "console"; then
        echo "  --headless refused: a DCV CONSOLE session exists and needs the display manager (use a virtual session)" >&2
    elif [ "$go" = 1 ]; then
        install -d -m 0755 /etc/altair
        [ -f "$target_file" ] || systemctl get-default > "$target_file"
        systemctl set-default multi-user.target
        echo "default boot target: multi-user (was $(cat "$target_file"))"
    else
        echo "  would set the default boot target to multi-user.target (now $(systemctl get-default 2>/dev/null || echo unknown))"
    fi
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
