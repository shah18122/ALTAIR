#!/usr/bin/env bash
# ops/setup.sh -- install or update everything ALTAIR needs on a Linux server, in one go.
#
# Run it on a fresh Ubuntu 24.04 box to install ALTAIR, and again any time to
# update it: every step checks what is already there and only does what is
# missing or out of date, so a second run changes nothing that is current.
#
#   1. system packages        apt: compilers, CMake, Ninja, Boost, OpenSSL, toml++, libxdp/libbpf
#                             + clang (the AF_XDP path), Qt's runtime libs
#   2. Qt 6.8.3               aqtinstall into /opt/altair/qt (Python venv /opt/altair/venv),
#                             then a check that the xcb screen plugin finds every library it needs
#   3. the screen             with --dcv: a GNOME desktop and Amazon DCV (virtual session,
#                             port 8443) so the terminal is reached from a browser or the DCV client
#   4. the code               git pull --ff-only, only on a clean main checkout
#   5. the build              ops/build.sh (Release, Qt terminal included)
#   6. the tests              ops/build.sh test (with --test)
#   7. scheduled jobs         ops/install.sh --go (systemd timers, IST)
#   8. launchers              "ALTAIR" and "Update ALTAIR" icons on the DCV desktop, and
#                             ALTAIR opening by itself when that desktop starts (every boot)
#   9. latency tuning         ops/tune_host.sh, when present (needs --tune; may need a reboot)
#
# Steps 1-3 and 8 only add packages and files, so they always run. Steps 4-7 and 9
# are skipped (and the script exits 3 at the end) during market hours (Mon-Fri
# 08:45-16:00 IST) or while an ALTAIR job is running, because they replace the
# binaries, config and timers the recorder and the engine are using. --force
# overrides that. apt never restarts running services here (needrestart is told
# to only list them).
#
#   sudo ops/setup.sh                    # install or update
#   sudo ops/setup.sh --test             # ... and run the test suite
#   sudo ops/setup.sh --tune             # ... and apply the host latency tuning
#   sudo ops/setup.sh --dcv              # ... and the desktop + Amazon DCV (first run on EC2)
#   sudo ops/setup.sh --ssh-443          # ... and SSH on port 443 as well as 22 (networks that block 22)
#   sudo ops/setup.sh --user altair      # the user that runs ALTAIR (default: who ran sudo)
#
# Reach the screen through SSH, not an open 8443:
#   ssh -p 443 -L 8443:localhost:8443 ubuntu@<server>   then https://localhost:8443

set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
qt_version=6.8.3
prefix=/opt/altair
qt_root="$prefix/qt/$qt_version/gcc_64"
user="${SUDO_USER:-$(id -un)}"
run_tests=0; tune=0; force=0; dcv=0; ssh443=0

while [ $# -gt 0 ]; do
    case "$1" in
        --test) run_tests=1 ;;
        --tune) tune=1 ;;
        --force) force=1 ;;
        --dcv) dcv=1 ;;
        --user) user="$2"; shift ;;
        --ssh-443) ssh443=1 ;;
        -h|--help) sed -n '2,35p' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n== %s\n' "$*"; }
# A fresh EC2 box runs unattended-upgrades for its first minutes and holds the
# apt/dpkg locks: wait for them (up to 15 minutes) instead of failing.
apt_get() {
    local waited=0
    while fuser /var/lib/dpkg/lock-frontend /var/lib/dpkg/lock /var/lib/apt/lists/lock >/dev/null 2>&1; do
        [ "$waited" = 0 ] && echo "waiting for another apt/dpkg run to finish (unattended-upgrades on a new server)..."
        waited=$((waited + 5))
        [ "$waited" -ge 900 ] && { echo "apt/dpkg still locked after 15 minutes; run setup.sh again later" >&2; return 1; }
        sleep 5
    done
    # NEEDRESTART_*: Ubuntu 24.04's needrestart would otherwise restart services
    # (the recorder among them) after a library upgrade.
    DEBIAN_FRONTEND=noninteractive NEEDRESTART_MODE=l NEEDRESTART_SUSPEND=1 \
        apt-get -o DPkg::Lock::Timeout=600 "$@"
}
# Install the packages that are not installed yet; returns how many it added in $added.
apt_ensure() {
    local missing=() p
    for p in "$@"; do dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p"); done
    added=${#missing[@]}
    [ "$added" = 0 ] && return 0
    apt_get update -q
    apt_get install -y -q "${missing[@]}"
}
as_user() { sudo -u "$user" -H env ALTAIR_QT_ROOT="$qt_root" "$@"; }

[ "$(id -u)" = 0 ] || { echo "run it with sudo: sudo ops/setup.sh" >&2; exit 2; }
id "$user" >/dev/null 2>&1 || { echo "no such user: $user" >&2; exit 2; }
. /etc/os-release
if [ "${ID:-}" != ubuntu ] || [ "${VERSION_ID:-}" != 24.04 ]; then
    echo "warning: tested on Ubuntu 24.04; this is ${PRETTY_NAME:-unknown}" >&2
fi

# Market hours or a running ALTAIR job: install packages and the screen, but do
# not touch the code, binaries, timers or tuning the running jobs depend on.
blocked=""
ist_hm="$(TZ=Asia/Kolkata date +%H%M)"; ist_dow="$(TZ=Asia/Kolkata date +%u)"
if [ "$ist_dow" -le 5 ] && [ "$ist_hm" -ge 0845 ] && [ "$ist_hm" -lt 1600 ]; then
    blocked="market hours (IST $ist_hm; run it again after 16:00)"
fi
busy="$(systemctl list-units 'altair-*.service' --state=active --no-legend --plain 2>/dev/null \
        | awk '{print $1}' | grep -v '^altair-dcv-session' | tr '\n' ' ' || true)"
if [ -z "$blocked" ] && [ -n "$busy" ]; then
    blocked="an ALTAIR job is running ($busy; run it again when it ends)"
fi
[ "$force" = 1 ] && blocked=""
[ -n "$blocked" ] && echo "note: $blocked: packages and the screen are installed, the code, build, timers and tuning are not. --force overrides." >&2

step "1/9 system packages"
# Qt 6.8's xcb screen plugin loads all of the libxcb-* / X11 libraries below at
# start-up; one missing (libxcb-cursor0 most often) aborts altair_desktop.
pkgs=(build-essential cmake ninja-build git curl python3 python3-venv rsync chrony
      libboost-dev libssl-dev libtomlplusplus-dev
      pkg-config clang libbpf-dev libxdp-dev dpdk libdpdk-dev
      libgl1-mesa-dev libegl1 libopengl0 libglib2.0-0t64 libfontconfig1 libfreetype6 libdbus-1-3
      libxkbcommon0 libxkbcommon-x11-0 libx11-6 libx11-xcb1 libsm6 libice6
      libxcb1 libxcb-cursor0 libxcb-glx0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1
      libxcb-randr0 libxcb-render0 libxcb-render-util0 libxcb-shape0 libxcb-shm0
      libxcb-sync1 libxcb-util1 libxcb-xfixes0 libxcb-xinerama0 libxcb-xinput0 libxcb-xkb1)
apt_ensure "${pkgs[@]}"
apt_get install -y -q --only-upgrade "${pkgs[@]}" >/dev/null
echo "packages: ${#pkgs[@]} present ($added newly installed)"

step "2/9 Qt $qt_version"
if [ ! -x "$qt_root/bin/qmake" ] && [ ! -x "$qt_root/bin/qtpaths" ]; then
    python3 -m venv "$prefix/venv"
    "$prefix/venv/bin/pip" install -q --upgrade aqtinstall
    "$prefix/venv/bin/aqt" install-qt linux desktop "$qt_version" linux_gcc_64 -O "$prefix/qt"
fi
echo "Qt: $qt_root"
install -d -m 0755 /etc/altair
touch /etc/altair/altair.env
chmod 0640 /etc/altair/altair.env
chgrp "$user" /etc/altair/altair.env
grep -q '^ALTAIR_QT_ROOT=' /etc/altair/altair.env \
    && sed -i "s#^ALTAIR_QT_ROOT=.*#ALTAIR_QT_ROOT=$qt_root#" /etc/altair/altair.env \
    || echo "ALTAIR_QT_ROOT=$qt_root" >> /etc/altair/altair.env
# The xcb plugin must resolve every library, or the terminal aborts on start.
xcb_plugin="$qt_root/plugins/platforms/libqxcb.so"
if [ -f "$xcb_plugin" ]; then
    unresolved="$(LD_LIBRARY_PATH="$qt_root/lib" ldd "$xcb_plugin" | awk '/not found/ {print $1}' | tr '\n' ' ')"
    if [ -n "$unresolved" ]; then
        echo "the Qt xcb screen plugin is missing: $unresolved" >&2
        echo "install the Ubuntu packages that provide them (apt-file search <name>), then run setup.sh again" >&2
        exit 1
    fi
    echo "Qt xcb plugin: every library found"
fi

if [ "$ssh443" = 1 ]; then
    # Some networks only let 443 out. Keep 22, add 443. Ubuntu 24.04 starts sshd
    # from ssh.socket; older setups run ssh.service on sshd_config's ports.
    if systemctl cat ssh.socket >/dev/null 2>&1 && systemctl is-enabled --quiet ssh.socket 2>/dev/null; then
        install -d -m 0755 /etc/systemd/system/ssh.socket.d
        printf '%s\n' "[Socket]" "ListenStream=" "ListenStream=22" "ListenStream=443" \
            > /etc/systemd/system/ssh.socket.d/port.conf
        systemctl daemon-reload
        systemctl restart ssh.socket
    else
        printf '%s\n' "Port 22" "Port 443" > /etc/ssh/sshd_config.d/altair-ports.conf
        sshd -t && systemctl restart ssh
    fi
    echo "SSH listens on 22 and 443 (the security group must allow TCP 443 from YOUR address)"
fi

step "3/9 the screen (Amazon DCV)"
dcv_installed() { dpkg -s nice-dcv-server >/dev/null 2>&1 && systemctl cat dcvserver.service >/dev/null 2>&1; }
if [ "$dcv" = 1 ]; then
    # A desktop for the DCV session, then DCV itself from Amazon's archive
    # (the archive name carries the Ubuntu version: 24.04 -> ubuntu2404).
    apt_ensure ubuntu-desktop-minimal gnome-terminal gdm3 xserver-xorg-core
    if ! dcv_installed; then
        arch="$(dpkg --print-architecture)"
        if [ "$arch" = amd64 ]; then arch=x86_64; else arch=aarch64; fi
        tgz="nice-dcv-ubuntu${VERSION_ID/./}-$arch.tgz"
        url="https://d1uj6qtbmh3dt5.cloudfront.net/$tgz"
        tmp="$(mktemp -d)"
        if ! curl -fsSL --retry 3 -o "$tmp/$tgz" "$url"; then
            echo "could not download Amazon DCV from $url" >&2
            echo "check the server's internet access, or see https://docs.aws.amazon.com/dcv/latest/adminguide/setting-up-installing-linux-server.html" >&2
            rm -rf "$tmp"; exit 1
        fi
        curl -fsSL --retry 3 -o "$tmp/NICE-GPG-KEY" https://d1uj6qtbmh3dt5.cloudfront.net/NICE-GPG-KEY \
            && gpg --import "$tmp/NICE-GPG-KEY" 2>/dev/null || true
        tar -xzf "$tmp/$tgz" -C "$tmp"
        debs=()
        for name in nice-dcv-server nice-dcv-web-viewer nice-xdcv; do
            deb="$(find "$tmp" -name "${name}_*.deb" | head -1)"
            [ -n "$deb" ] || { echo "the DCV archive has no ${name} package ($(ls "$tmp"))" >&2; rm -rf "$tmp"; exit 1; }
            debs+=("$deb")
        done
        apt_get install -y -q "${debs[@]}"
        rm -rf "$tmp"
        dcv_installed || { echo "DCV packages installed but dcvserver.service is missing" >&2; exit 1; }
    fi
    # GDM on Wayland breaks DCV's X sessions; AWS's guide turns Wayland off.
    if [ -f /etc/gdm3/custom.conf ] && ! grep -q '^WaylandEnable=false' /etc/gdm3/custom.conf; then
        sed -i 's/^#\?WaylandEnable=.*/WaylandEnable=false/' /etc/gdm3/custom.conf
        grep -q '^WaylandEnable=false' /etc/gdm3/custom.conf \
            || sed -i '/^\[daemon\]/a WaylandEnable=false' /etc/gdm3/custom.conf
    fi
    # One virtual session for the ALTAIR user, created at every boot.
    printf '%s\n' \
        "[Unit]" \
        "Description=ALTAIR: the DCV desktop session for $user" \
        "After=dcvserver.service" \
        "Requires=dcvserver.service" \
        "" \
        "[Service]" \
        "Type=oneshot" \
        "RemainAfterExit=yes" \
        "ExecStartPre=/bin/sleep 5" \
        "ExecStart=/usr/bin/dcv create-session --type=virtual --owner $user --name ALTAIR altair" \
        "ExecStop=/usr/bin/dcv close-session altair" \
        "" \
        "[Install]" \
        "WantedBy=multi-user.target" > /etc/systemd/system/altair-dcv-session.service
    systemctl daemon-reload
    systemctl enable --now dcvserver
    systemctl enable altair-dcv-session
    systemctl is-active --quiet altair-dcv-session || systemctl start altair-dcv-session || true
    if systemctl is-active --quiet dcvserver; then
        echo "DCV server running; sessions: $(dcv list-sessions 2>/dev/null | tr '\n' ' ')"
    else
        echo "DCV is installed but dcvserver is not running: journalctl -u dcvserver" >&2
    fi
    echo "DCV: ssh -p 443 -L 8443:localhost:8443 $user@<server>   (or -p 22), then open"
    echo "     https://localhost:8443 in a browser (or point the DCV client at localhost:8443)."
    echo "     $user needs a password for the DCV login (sudo passwd $user). Port 8443 does"
    echo "     not need to be open in the security group when the SSH tunnel is used."
elif dcv_installed; then
    echo "DCV is installed"
else
    echo "skipped (--dcv installs the desktop and Amazon DCV)"
fi

if [ -n "$blocked" ]; then
    step "4-7/9 code, build, tests and timers skipped: $blocked"
else
    step "4/9 the code"
    branch="$(as_user git -C "$root" rev-parse --abbrev-ref HEAD)"
    if [ "$branch" = main ] && [ -z "$(as_user git -C "$root" status --porcelain --untracked-files=no)" ]; then
        as_user git -C "$root" pull --ff-only -q && echo "main at $(as_user git -C "$root" rev-parse --short HEAD)"
    else
        echo "not updating the code: on '$branch' or with local changes (left as they are)"
    fi

    if [ "$run_tests" = 1 ]; then
        step "5-6/9 build and test"
        as_user "$root/ops/build.sh" test
    else
        step "5/9 the build"
        as_user "$root/ops/build.sh"
        step "6/9 tests skipped (--test runs them)"
    fi

    step "7/9 scheduled jobs"
    "$root/ops/install.sh" --go --user "$user"
fi

step "8/9 launchers"
home="$(getent passwd "$user" | cut -d: -f6)"
apps="$home/.local/share/applications"
install -d -o "$user" -g "$user" "$apps"
cat > "$apps/altair.desktop" <<EOD
[Desktop Entry]
Type=Application
Name=ALTAIR
Comment=ALTAIR trading terminal
Exec=env ALTAIR_QT_ROOT=$qt_root $root/build/linux/desktop/altair_desktop
Path=$root
Terminal=false
Categories=Office;Finance;
EOD
cat > "$apps/altair-update.desktop" <<EOD
[Desktop Entry]
Type=Application
Name=Update ALTAIR
Comment=Install or update everything ALTAIR needs (ops/setup.sh)
Exec=x-terminal-emulator -e bash -c 'sudo $root/ops/setup.sh; echo; read -rp "Done. Press Enter to close. "'
Path=$root
Terminal=false
Categories=System;
EOD
chown "$user:$user" "$apps/altair.desktop" "$apps/altair-update.desktop"
if [ -d "$home/Desktop" ]; then
    install -o "$user" -g "$user" -m 0755 "$apps/altair.desktop" "$apps/altair-update.desktop" "$home/Desktop/"
fi
# A machine given over to ALTAIR: the terminal opens on its own in the DCV
# desktop after every boot (altair-dcv-session starts that desktop).
auto="$home/.config/autostart"
install -d -o "$user" -g "$user" "$home/.config" "$auto"
sed -e '/^Categories=/a X-GNOME-Autostart-enabled=true\nX-GNOME-Autostart-Delay=5' "$apps/altair.desktop" > "$auto/altair.desktop"
chown "$user:$user" "$auto/altair.desktop"
echo "launchers in $apps; ALTAIR opens at login ($auto/altair.desktop)"

step "9/9 latency tuning"
if [ "$tune" = 1 ] && [ -n "$blocked" ]; then
    echo "skipped: $blocked"
elif [ "$tune" = 1 ] && [ -x "$root/ops/tune_host.sh" ]; then
    "$root/ops/tune_host.sh" --go --user "$user"
elif [ "$tune" = 1 ]; then
    echo "ops/tune_host.sh is missing: nothing tuned"
else
    echo "skipped (--tune applies it)"
fi

if [ -n "$blocked" ]; then
    printf '\nPackages and the screen are installed. Not rebuilt: %s.\n' "$blocked"
    exit 3
fi
printf '\nALTAIR is installed and up to date in %s.\n' "$root"
