#!/usr/bin/env bash
# ops/linux/host_trim.sh -- remove what a trading server does not need, and the
# one-time Ubuntu configuration. Dry run by default; --go applies; --undo puts
# the configuration back (purged packages stay purged; reinstall with apt).
#
# REMOVED (apt purge, each only if installed, and only if apt would remove
# nothing else -- a package whose removal drags in anything outside this list,
# e.g. a desktop metapackage that DCV's GNOME needs, is skipped and reported):
#   apport whoopsie kerneloops popularity-contest ubuntu-report   crash reporters / telemetry
#   modemmanager multipath-tools open-iscsi                       no modems, SAN or iSCSI on EC2
#   fwupd fwupd-signed packagekit packagekit-tools                 firmware/GUI updaters (no firmware on Nitro)
#   landscape-common update-notifier-common                         login banners that run apt
#   snap: lxd                                                       container manager, unused
#
# STOPPED + MASKED (kept installed: the GNOME desktop that DCV draws needs the packages):
#   cups cups-browsed avahi-daemon bluetooth ModemManager whoopsie kerneloops
#   tracker-miner-fs-3 (user)  -- desktop file indexer
#
# KEPT on purpose: snapd (amazon-ssm-agent is a snap: the console's recovery
# path), cloud-init (ENI/netplan), openssh, nice-dcv-*, gdm/gnome-shell,
# chrony, irqbalance, git, python3, the AWS CLI, cloudflared, everything ALTAIR builds with.
#
# ONE-TIME CONFIGURATION (each file ALTAIR's own, removed by --undo):
#   /etc/apt/apt.conf.d/99-altair            security updates still install, but only Saturday
#   /etc/systemd/system/apt-daily*.timer.d/  apt-daily 09:30 Sat, apt-daily-upgrade 10:00 Sat IST
#   /etc/needrestart/conf.d/altair.conf      apt never restarts a service by itself (list only)
#   /etc/systemd/journald.conf.d/altair.conf journal capped at 2 GB, no rate-limit drops of ALTAIR lines
#   /etc/chrony/sources.d/altair-aws.sources Amazon Time Sync (169.254.169.123), polled every 16 s
#   chrony-wait.service enabled               time-sync.target means the clock IS synced at boot
#   snap refresh window                       Saturday 02:00-04:00 only
#   motd-news, apt news, ESM ads              off
#   networkd-wait-online --any                boot no longer waits 2 min for the DPDK ENI
#
# Refused in market hours (08:45-15:50 IST weekdays) unless --force: a purge can
# trigger triggers and services restarts.
#
#   sudo ops/linux/host_trim.sh            # dry run: exactly what would change
#   sudo ops/linux/host_trim.sh --go
#   sudo ops/linux/host_trim.sh --undo --go

set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
go=0; undo=0; force=0
for a in "$@"; do
    case "$a" in
        --go) go=1 ;; --undo) undo=1 ;; --force) force=1 ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done
[ "$go" = 0 ] || [ "$(id -u)" = 0 ] || { echo "run it with sudo" >&2; exit 2; }
if [ "$go" = 1 ] && [ "$force" = 0 ] && "$here/session_window.sh" 08:45 15:50; then
    echo "refused: market hours (08:45-15:50 IST). Run after 15:50, or --force." >&2; exit 3
fi

purge="apport whoopsie kerneloops popularity-contest ubuntu-report modemmanager multipath-tools open-iscsi fwupd fwupd-signed packagekit packagekit-tools landscape-common update-notifier-common"
mask="cups.service cups.socket cups.path cups-browsed.service avahi-daemon.service avahi-daemon.socket bluetooth.service ModemManager.service whoopsie.service kerneloops.service"
masked_list=/etc/altair/masked-services
f_apt=/etc/apt/apt.conf.d/99-altair
d_daily=/etc/systemd/system/apt-daily.timer.d
d_upg=/etc/systemd/system/apt-daily-upgrade.timer.d
f_nr=/etc/needrestart/conf.d/altair.conf
f_jd=/etc/systemd/journald.conf.d/altair.conf
f_chrony=/etc/chrony/sources.d/altair-aws.sources
f_wo=/etc/systemd/system/systemd-networkd-wait-online.service.d/altair.conf

say() { echo "$*"; }
run() { if [ "$go" = 1 ]; then "$@"; else echo "  would run: $*"; fi; }
write() {
    if [ "$go" = 1 ]; then install -d -m 0755 "$(dirname "$1")"; printf '%s\n' "$2" > "$1"; chmod 0644 "$1"; echo "wrote $1"
    else printf '  would write %s:\n%s\n' "$1" "$(printf '%s\n' "$2" | sed 's/^/    /')"; fi
}

if [ "$undo" = 1 ]; then
    for f in "$f_apt" "$d_daily/altair.conf" "$d_upg/altair.conf" "$f_nr" "$f_jd" "$f_chrony" "$f_wo"; do run rm -f "$f"; done
    if [ -f "$masked_list" ]; then
        # shellcheck disable=SC2046
        run systemctl unmask $(cat "$masked_list")
        run rm -f "$masked_list"
    fi
    run systemctl daemon-reload
    run systemctl try-restart systemd-journald chrony
    command -v snap >/dev/null && run snap unset system refresh.timer
    say "configuration undone (purged packages stay purged)."
    exit 0
fi

say "== packages"
present=""
for p in $purge; do dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "install ok installed" && present="$present $p"; done
safe=""
for p in $present; do
    # What apt would remove with it; skip it if that reaches outside our list.
    extra="$(apt-get -s purge "$p" 2>/dev/null | awk '/^Purg |^Remv /{print $2}' | while read -r q; do
        case " $purge " in *" $q "*) ;; *) echo "$q" ;; esac; done | tr '\n' ' ')"
    if [ -n "$extra" ]; then say "  skip $p: would also remove $extra"; else safe="$safe $p"; fi
done
if [ -n "$safe" ]; then
    # shellcheck disable=SC2086
    run env DEBIAN_FRONTEND=noninteractive NEEDRESTART_MODE=l apt-get purge -y $safe
else say "  nothing to purge"; fi
if command -v snap >/dev/null && snap list lxd >/dev/null 2>&1; then run snap remove --purge lxd; fi

say "== services masked (packages kept)"
here_units=""
for u in $mask; do systemctl list-unit-files "$u" --no-legend 2>/dev/null | grep -q . && here_units="$here_units $u"; done
if [ -n "$here_units" ]; then
    if [ "$go" = 1 ]; then
        install -d -m 0755 /etc/altair
        { [ -f "$masked_list" ] && cat "$masked_list"; printf '%s\n' $here_units; } | sort -u > "$masked_list.new"
        mv "$masked_list.new" "$masked_list"
    fi
    # shellcheck disable=SC2086
    run systemctl disable --now $here_units
    # shellcheck disable=SC2086
    run systemctl mask $here_units
else say "  none present"; fi
# The desktop indexer runs per user (GNOME); mask it for every user.
[ -e /usr/lib/systemd/user/tracker-miner-fs-3.service ] && run systemctl --global mask tracker-miner-fs-3.service

say "== one-time configuration"
write "$f_apt" '// ALTAIR (ops/linux/host_trim.sh): updates are fetched and installed on Saturday only
// (the apt-daily timers are moved there); nothing ever restarts a service by itself.
APT::Periodic::Update-Package-Lists "1";
APT::Periodic::Unattended-Upgrade "1";
Unattended-Upgrade::Automatic-Reboot "false";
APT::Periodic::Download-Upgradeable-Packages "0";'
write "$d_daily/altair.conf" '# ALTAIR: never on a trading day
[Timer]
OnCalendar=
OnCalendar=Sat 09:30 Asia/Kolkata
RandomizedDelaySec=0'
write "$d_upg/altair.conf" '# ALTAIR: never on a trading day
[Timer]
OnCalendar=
OnCalendar=Sat 10:00 Asia/Kolkata
RandomizedDelaySec=0'
write "$f_nr" "# ALTAIR: list services that need a restart, never restart them
\$nrconf{restart} = 'l';
\$nrconf{kernelhints} = 0;"
write "$f_jd" '# ALTAIR (ops/linux/host_trim.sh)
[Journal]
SystemMaxUse=2G
RateLimitIntervalSec=0
RateLimitBurst=0'
write "$f_chrony" '# ALTAIR: the Amazon Time Sync Service (link-local, every EC2 instance), preferred.
server 169.254.169.123 prefer iburst minpoll 4 maxpoll 4'
write "$f_wo" '# ALTAIR: the spare ENI belongs to DPDK and never comes up; one interface online is enough
[Service]
ExecStart=
ExecStart=/usr/lib/systemd/systemd-networkd-wait-online --any --timeout=30'
run systemctl daemon-reload
run systemctl enable chrony-wait.service
run systemctl restart systemd-journald
run systemctl try-restart chrony
command -v snap >/dev/null && run snap set system refresh.timer=sat,02:00-04:00
[ -f /etc/default/motd-news ] && run sed -i 's/^ENABLED=1/ENABLED=0/' /etc/default/motd-news
command -v pro >/dev/null && run pro config set apt_news=false
[ -x /etc/update-motd.d/91-contract-ua-esm-status ] && run chmod -x /etc/update-motd.d/91-contract-ua-esm-status

if [ "$go" = 1 ]; then
    echo; echo "done. chronyc sources should now list 169.254.169.123 with '*'."
else
    echo; echo "DRY RUN: nothing changed. Run with --go to apply."
fi
