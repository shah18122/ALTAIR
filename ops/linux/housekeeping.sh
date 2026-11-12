#!/usr/bin/env bash
# ops/linux/housekeeping.sh -- print the housekeeping cores: every core NOT in
# /sys/devices/system/cpu/isolated (e.g. "0,4" on the c7i with 1-3,5-7 isolated).
# With nothing isolated: cpu0 and its hyperthread twin.
set -euo pipefail
expand() { local IFS=,; for r in $1; do [ -z "$r" ] && continue; if [[ $r == *-* ]]; then seq "${r%-*}" "${r#*-}"; else echo "$r"; fi; done; }
iso=" $(expand "$(cat /sys/devices/system/cpu/isolated 2>/dev/null)" | tr '\n' ' ') "
if [ "$iso" = "  " ]; then cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list 2>/dev/null || echo 0; exit 0; fi
out=""
for c in $(seq 0 $(( $(nproc --all) - 1 ))); do case "$iso" in *" $c "*) ;; *) out="$out,$c" ;; esac; done
echo "${out#,}"
