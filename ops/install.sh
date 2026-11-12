#!/usr/bin/env bash
# ops/install.sh -- register ALTAIR's scheduled jobs as systemd timers (or remove them).
#
#   altair-fyers-login.timer   Mon-Fri 08:30 IST  broker/tools/fyers_auto_login.py (TOTP + PIN, no browser),
#                              again at 08:40 as a backstop (a no-op once today's token works)
#   altair-recorder.timer      Mon-Fri 08:50 IST  ops/linux/record_session.py --go (records to 15:45)
#   altair-chain-open.timer    Mon-Fri 09:20 IST  ops/linux/chain_snapshot.py --go
#   altair-chain-close.timer   Mon-Fri 15:27 IST  ops/linux/chain_snapshot.py --go
#   altair-fyers-chain.timer   Mon-Fri 15:45 IST  ops/linux/fyers_chain.py --go
#   altair-push.timer          Mon-Fri 15:47 IST  ops/linux/nightly.py --go --quick (the data, pushed by 16:00)
#   altair-nightly.timer       Mon-Fri 20:00 IST  ops/linux/nightly.py --go (store, retrain, push)
#   altair-fyers-callback      always             broker/tools/fyers_callback_server.py (FYERS redirect URL, 127.0.0.1:80)
#   altair-engine.timer        Mon-Fri 09:05 IST  altair_live_engine in altair.slice (only with /etc/altair/engine.conf)
#   altair-s3-sync.timer       every 5 min        ops/linux/s3_sync.sh (only with ALTAIR_S3_BUCKET in /etc/altair/s3.env)
#   altair-cpuset-*.timer      08:45 / 15:50 IST  ops/linux/cpuset.sh hours / open (and at boot)
#   altair-nic-tune            at boot            ops/linux/nic_tune.sh, before the recorder
#   altair-boot-check.timer    3 min after boot   ops/linux/boot_check.sh -> data/logs/boot-*.txt
#
# BOOT ORDER: network-online -> chrony-wait (time-sync.target) -> nic-tune ->
# dpdk-bind -> callback -> webhook; then the timers: login (catch-up at boot),
# recorder (boot + 90 s, after login), engine (boot + 2 min, after recorder),
# cpuset (boot), boot check (boot + 3 min), S3 (boot + 5 min).
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
timers=(altair-fyers-login altair-recorder altair-chain-open altair-chain-close altair-fyers-chain altair-push altair-nightly
        altair-engine altair-s3-sync altair-cpuset-hours altair-cpuset-open altair-boot-check)
boot_units=(altair-nic-tune altair-cpuset-boot)
units=("$here"/systemd/*.service "$here"/systemd/*.timer "$here"/systemd/*.slice)

if [ "$remove" = 1 ]; then
    for t in "${timers[@]}"; do
        if [ "$go" = 1 ]; then systemctl disable --now "$t.timer" 2>/dev/null || true; else echo "would disable $t.timer"; fi
    done
    if [ "$go" = 1 ]; then systemctl disable --now altair-fyers-callback.service altair-fyers-webhook.service "${boot_units[@]/%/.service}" 2>/dev/null || true; fi
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
    # Boot-time oneshots: enabled for the next boot; the cpuset is applied now too.
    for b in "${boot_units[@]}"; do systemctl enable "$b.service"; done
    systemctl restart altair-cpuset-boot.service || true
    # time-sync.target only means "synced" when something waits for the clock.
    systemctl enable chrony-wait.service 2>/dev/null || true
    # The FYERS redirect URL on 127.0.0.1:80 (a browser login ends there; the
    # tunnel from ops/linux/fyers_tunnel.sh brings https to it).
    systemctl enable altair-fyers-callback.service
    systemctl restart altair-fyers-callback.service || echo "altair-fyers-callback did not start: journalctl -u altair-fyers-callback"
    # FYERS's order webhook on 127.0.0.1:8787 (behind the callback above). Its
    # secret is made once, here, and never printed: register the URL with
    # ops/linux/fyers_webhook_url.sh.
    sec="$root/data/fyers_webhook_secret.txt"
    if [ ! -s "$sec" ]; then
        install -d -o "$user" -g "$user" "$root/data"
        ( umask 077; openssl rand -hex 24 > "$sec" ); chown "$user:$user" "$sec"
    fi
    systemctl enable altair-fyers-webhook.service
    systemctl restart altair-fyers-webhook.service || echo "altair-fyers-webhook did not start: journalctl -u altair-fyers-webhook"
    # The DPDK binding at boot, once ops/linux/dpdk_bind.sh --at-boot chose an ENI.
    [ -f /etc/altair/dpdk.conf ] && systemctl enable altair-dpdk-bind.service
    systemctl list-timers 'altair-*' --no-pager
else
    echo "would enable: ${timers[*]/%/.timer}"
fi
