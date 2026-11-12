#!/usr/bin/env bash
# ops/linux/core_qos.sh -- keep the isolated cores out of deep C-states (runtime, no reboot).
#
# A sleeping isolated core drops into C6 (~190 us exit on the c7i); every tick
# that wakes a thread there pays it (probe: p99 wake-up 221 us). Writing a
# resume-latency limit per core lets the idle governor use only POLL/C1 there;
# the housekeeping cores keep their C-states. Undone with --undo (0 = no limit).
#
#   sudo ops/linux/core_qos.sh [CORES] [--us 2]     # default cores: /sys/.../isolated, else 2-7
#   sudo ops/linux/core_qos.sh --undo
set -euo pipefail
cores=""; us=2; undo=0
while [ $# -gt 0 ]; do
    case "$1" in --us) us="$2"; shift ;; --undo) undo=1 ;; *) cores="$1" ;; esac; shift
done
[ -n "$cores" ] || cores="$(cat /sys/devices/system/cpu/isolated 2>/dev/null)"
[ -n "$cores" ] || cores="2-$(( $(nproc --all) - 1 ))"
expand() { local IFS=,; for r in $1; do if [[ $r == *-* ]]; then seq "${r%-*}" "${r#*-}"; else echo "$r"; fi; done; }
v="$us"; [ "$undo" = 1 ] && v=0      # 0 means "no constraint" for this file
for c in $(expand "$cores"); do
    f=/sys/devices/system/cpu/cpu$c/power/pm_qos_resume_latency_us
    [ -w "$f" ] && echo "$v" > "$f"
done
echo "cores $cores: resume latency limit $( [ "$undo" = 1 ] && echo none || echo "${us} us")"
