#!/usr/bin/env bash
# ops/linux/cpuset.sh -- which cores the rest of the box may use, by time of day.
#
#   hours   08:45-15:50 IST weekdays: system.slice and init.scope (every daemon,
#           the S3 sync, the push, cron, journald, apt) are held on the
#           housekeeping cores; altair.slice (recorder, price service, engine,
#           webhook) keeps every core and pins its threads itself
#           (config/latency.toml).
#   open    after the session: every slice gets every core, so the nightly
#           retrain and train_all use all of them.
#   auto    whichever of the two applies now (run at boot).
#
# Runtime only (systemctl set-property --runtime): a reboot starts open, and
# altair-cpuset-boot.service applies "auto" right after. The user's desktop
# (user.slice: DCV, the Qt terminal) is left alone: processes it starts pin
# themselves to their cores, which a cpuset on user.slice would forbid.
#
#   sudo ops/linux/cpuset.sh hours|open|auto|show [--housekeeping 0-1]

set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
mode="${1:-show}"; hk="$("$here/housekeeping.sh")"
[ "${2:-}" = "--housekeeping" ] && hk="${3:?}"
all="0-$(( $(nproc --all) - 1 ))"
[ "$mode" = auto ] && { if "$here/session_window.sh" 08:45 15:50; then mode=hours; else mode=open; fi; }

set_cpus() {  # set_cpus UNIT CPUS
    systemctl set-property --runtime "$1" AllowedCPUs="$2" 2>/dev/null \
        || echo "warn: could not set $1 to $2" >&2
}
case "$mode" in
    hours) set_cpus system.slice "$hk"; set_cpus init.scope "$hk"; set_cpus altair.slice "$all" ;;
    open)  set_cpus system.slice "$all"; set_cpus init.scope "$all"; set_cpus altair.slice "$all" ;;
    show)  ;;
    *) echo "usage: cpuset.sh hours|open|auto|show" >&2; exit 2 ;;
esac
for u in system.slice init.scope altair.slice user.slice; do
    printf '%-14s %s\n' "$u" "$(systemctl show -p AllowedCPUs --value "$u" 2>/dev/null || true)"
done | sed 's/ $/ (all)/'
[ "$mode" = show ] || echo "cpuset: $mode"
