#!/usr/bin/env bash
# ops/install.sh -- register ALTAIR's scheduled jobs as systemd timers (or remove them).
#
#   altair-fyers-login.timer   Mon-Fri 08:30 IST  broker/tools/fyers_auto_login.py (TOTP + PIN, no browser)
#   altair-recorder.timer      Mon-Fri 08:50 IST  ops/linux/record_session.py --go (records to 15:45)
#   altair-chain-open.timer    Mon-Fri 09:20 IST  ops/linux/chain_snapshot.py --go
#   altair-chain-close.timer   Mon-Fri 15:27 IST  ops/linux/chain_snapshot.py --go
#   altair-fyers-chain.timer   Mon-Fri 15:45 IST  ops/linux/fyers_chain.py --go
#   altair-push.timer          Mon-Fri 15:47 IST  ops/linux/nightly.py --go --no-retrain (the data, pushed by 16:00)
#   altair-nightly.timer       Mon-Fri 20:00 IST  ops/linux/nightly.py --go (store, retrain, push)
#
# Times are IST whatever the server's clock zone is (OnCalendar ... Asia/Kolkata).
# Persistent=true runs a job missed while the server was off as soon as it boots.
# systemd never starts a second copy of a unit that is still running.
#
# The jobs run as --user (default: the user running sudo), the same user that
# logs in to the DCV desktop and runs the terminal, so they share data/ and the
# day's FYERS session. Optional environment for them: /etc/altair/altair.env.
#
#   sudo ops/install.sh                  # dry run: what it would install
#   sudo ops/install.sh --go
#   sudo ops/install.sh --remove --go
#   systemctl list-timers 'altair-*'     # next and last runs
#   journalctl -u altair-nightly         # a job's output

set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
go=0; remove=0; user="${SUDO_USER:-$(id -un)}"
while [ $# -gt 0 ]; do
    case "$1" in
        --go) go=1 ;;
        --remove) remove=1 ;;
        --user) user="$2"; shift ;;
        *) echo "usage: ops/install.sh [--go] [--remove] [--user NAME]" >&2; exit 2 ;;
    esac
    shift
done

dest=/etc/systemd/system
timers=(altair-fyers-login altair-recorder altair-chain-open altair-chain-close altair-fyers-chain altair-push altair-nightly)
units=("$here"/systemd/*.service "$here"/systemd/*.timer)

if [ "$remove" = 1 ]; then
    for t in "${timers[@]}"; do
        if [ "$go" = 1 ]; then systemctl disable --now "$t.timer" 2>/dev/null || true; else echo "would disable $t.timer"; fi
    done
    for u in "${units[@]}"; do
        n="$(basename "$u")"
        if [ "$go" = 1 ]; then rm -f "$dest/$n"; else echo "would remove $dest/$n"; fi
    done
    [ "$go" = 1 ] && systemctl daemon-reload
    exit 0
fi

id "$user" >/dev/null 2>&1 || { echo "no such user: $user" >&2; exit 2; }
for u in "${units[@]}"; do
    n="$(basename "$u")"
    if [ "$go" = 1 ]; then
        sed -e "s#@ROOT@#$root#g" -e "s#@USER@#$user#g" "$u" > "$dest/$n"
        chmod 0644 "$dest/$n"
    else
        echo "would install $dest/$n (root $root, user $user)"
    fi
done
if [ "$go" = 1 ]; then
    systemctl daemon-reload
    for t in "${timers[@]}"; do systemctl enable --now "$t.timer"; done
    # The DPDK binding at boot, once ops/linux/dpdk_bind.sh --at-boot chose an ENI.
    [ -f /etc/altair/dpdk.conf ] && systemctl enable altair-dpdk-bind.service
    systemctl list-timers 'altair-*' --no-pager
else
    echo "would enable: ${timers[*]/%/.timer}"
fi
