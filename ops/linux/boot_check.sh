#!/usr/bin/env bash
# ops/linux/boot_check.sh -- what came up after a boot, written to data/logs/boot-<time>.txt
# (altair-boot-check.service runs it 3 minutes after every boot; also safe by hand).
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="$root/data/logs/boot-$(TZ=Asia/Kolkata date +%F-%H%M).txt"
mkdir -p "$root/data/logs"
{
    echo "boot $(uptime -s) UTC, checked $(TZ=Asia/Kolkata date '+%F %T IST')"
    echo "kernel: $(uname -r)"; echo "cmdline: $(cat /proc/cmdline)"
    echo "clock: $(chronyc -n tracking 2>/dev/null | awk -F': ' '/Reference ID|System time/{printf "%s; ", $2}')"
    echo; echo "== ALTAIR units"
    systemctl list-units 'altair-*' --all --no-legend --no-pager | awk '{printf "%-40s %s %s\n", $1, $3, $4}'
    echo; echo "== failed units"; systemctl --failed --no-legend --no-pager
    echo; echo "== timers"; systemctl list-timers 'altair-*' --no-pager | head -20
    echo; echo "== cpusets"; "$here/cpuset.sh" show
    echo; echo "== hugepages $(cat /proc/sys/vm/nr_hugepages), THP $(cat /sys/kernel/mm/transparent_hugepage/enabled)"
    echo "== isolated: $(cat /sys/devices/system/cpu/isolated) nohz_full: $(cat /sys/devices/system/cpu/nohz_full 2>/dev/null)"
    echo "== memory"; free -h
} > "$out" 2>&1
grep -q "failed" <(systemctl list-units 'altair-*' --failed --no-legend) && echo "boot check: FAILED units, see $out" && exit 1
echo "boot check: ok ($out)"
