#!/usr/bin/env bash
# ops/linux/session_window.sh -- exit 0 when now (IST) is a weekday between FROM and TO.
#
#   ops/linux/session_window.sh 08:45 15:50     # 0 inside, 1 outside
#
# Used by systemd ExecCondition= and by the other ops scripts, so "market hours"
# means the same thing everywhere. IST is UTC+05:30 all year.
set -euo pipefail
from="${1:-08:45}"; to="${2:-15:50}"
dow="$(TZ=Asia/Kolkata date +%u)"            # 1 Mon .. 7 Sun
now="$(TZ=Asia/Kolkata date +%H%M)"
[ "$dow" -le 5 ] && [ "$now" -ge "${from/:/}" ] && [ "$now" -lt "${to/:/}" ]
