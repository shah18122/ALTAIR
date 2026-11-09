#!/usr/bin/env bash
# ops/linux/dcv_session.sh -- create the "altair" DCV virtual session at boot, and
# make sure it really exists.
#
# On 2026-11-09 the boot-time `dcv create-session` gave up after DCV's 30 s start
# wait while Xdcv came up 90 s late: the session vanished from `dcv list-sessions`
# but its desktop kept running on a screen nobody could join, and the oneshot unit
# still said "active". This waits for the DCV server, tries up to five times, checks
# the session is listed each time, and exits non-zero (the unit shows FAILED) if it
# never appears.
#
#   usage: dcv_session.sh <owner>      (run by altair-dcv-session.service as root)
set -u
owner="${1:?owner}"
listed() { /usr/bin/dcv list-sessions 2>/dev/null | grep -q "'altair'"; }

for _ in $(seq 1 60); do /usr/bin/dcv list-sessions >/dev/null 2>&1 && break; sleep 2; done
for try in 1 2 3 4 5; do
    listed && { echo "DCV session altair is up"; exit 0; }
    echo "creating DCV session altair for $owner (try $try)"
    /usr/bin/dcv create-session --type=virtual --owner "$owner" --name ALTAIR altair || true
    for _ in $(seq 1 30); do listed && { echo "DCV session altair is up"; exit 0; }; sleep 3; done
    # A failed try can leave its X server behind on the session's display: close what DCV knows of it.
    /usr/bin/dcv close-session altair >/dev/null 2>&1 || true
    sleep 5
done
echo "DCV session altair did not come up after 5 tries: see /var/log/dcv/server.log" >&2
exit 1
