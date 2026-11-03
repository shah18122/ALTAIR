#!/usr/bin/env bash
# ops/setup.sh -- install or update everything ALTAIR needs on a Linux server, in one go.
#
# Run it on a fresh Ubuntu 24.04 box to install ALTAIR, and again any time to
# update it: every step checks what is already there and only does what is
# missing or out of date, so a second run changes nothing that is current.
#
#   1. system packages        apt: compilers, CMake, Ninja, Boost, OpenSSL, toml++, Qt's runtime libs
#   2. Qt 6.8.3               aqtinstall into /opt/altair/qt (Python venv /opt/altair/venv)
#   3. the code               git pull --ff-only, only on a clean main checkout
#   4. the build              ops/build.sh (Release, Qt terminal included)
#   5. the tests              ops/build.sh test (with --test)
#   6. scheduled jobs         ops/install.sh --go (systemd timers, IST)
#   7. launchers              "ALTAIR" and "Update ALTAIR" icons on the DCV desktop
#   8. latency tuning         ops/tune_host.sh, when present (needs --tune; may need a reboot)
#
# It will not rebuild during market hours (Mon-Fri 08:45-16:00 IST), because a
# rebuild replaces the binaries the recorder and the engine are running.
# --force overrides that.
#
#   sudo ops/setup.sh                    # install or update
#   sudo ops/setup.sh --test             # ... and run the test suite
#   sudo ops/setup.sh --tune             # ... and apply the host latency tuning
#   sudo ops/setup.sh --user altair      # the user that runs ALTAIR (default: who ran sudo)

set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
qt_version=6.8.3
prefix=/opt/altair
qt_root="$prefix/qt/$qt_version/gcc_64"
user="${SUDO_USER:-$(id -un)}"
run_tests=0; tune=0; force=0

while [ $# -gt 0 ]; do
    case "$1" in
        --test) run_tests=1 ;;
        --tune) tune=1 ;;
        --force) force=1 ;;
        --user) user="$2"; shift ;;
        -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n== %s\n' "$*"; }
as_user() { sudo -u "$user" -H env ALTAIR_QT_ROOT="$qt_root" "$@"; }

[ "$(id -u)" = 0 ] || { echo "run it with sudo: sudo ops/setup.sh" >&2; exit 2; }
id "$user" >/dev/null 2>&1 || { echo "no such user: $user" >&2; exit 2; }
. /etc/os-release
if [ "${ID:-}" != ubuntu ] || [ "${VERSION_ID:-}" != 24.04 ]; then
    echo "warning: tested on Ubuntu 24.04; this is ${PRETTY_NAME:-unknown}" >&2
fi

ist_hm="$(TZ=Asia/Kolkata date +%H%M)"; ist_dow="$(TZ=Asia/Kolkata date +%u)"
if [ "$force" = 0 ] && [ "$ist_dow" -le 5 ] && [ "$ist_hm" -ge 0845 ] && [ "$ist_hm" -lt 1600 ]; then
    echo "market hours (IST $ist_hm): not rebuilding under the running recorder and engine. Run it after 16:00, or pass --force." >&2
    exit 3
fi

step "1/8 system packages"
pkgs=(build-essential cmake ninja-build git python3 python3-venv rsync chrony
      libboost-dev libssl-dev libtomlplusplus-dev
      libgl1-mesa-dev libegl1 libxkbcommon0 libxkbcommon-x11-0 libxcb-cursor0
      libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-render-util0 libxcb-shape0
      libfontconfig1 libdbus-1-3)
missing=()
for p in "${pkgs[@]}"; do dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p"); done
if [ "${#missing[@]}" -gt 0 ]; then
    apt-get update -q
    DEBIAN_FRONTEND=noninteractive apt-get install -y -q "${missing[@]}"
fi
DEBIAN_FRONTEND=noninteractive apt-get install -y -q --only-upgrade "${pkgs[@]}" >/dev/null
echo "packages: ${#pkgs[@]} present (${#missing[@]} newly installed)"

step "2/8 Qt $qt_version"
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

step "3/8 the code"
branch="$(as_user git -C "$root" rev-parse --abbrev-ref HEAD)"
if [ "$branch" = main ] && [ -z "$(as_user git -C "$root" status --porcelain --untracked-files=no)" ]; then
    as_user git -C "$root" pull --ff-only -q && echo "main at $(as_user git -C "$root" rev-parse --short HEAD)"
else
    echo "not updating the code: on '$branch' or with local changes (left as they are)"
fi

step "4/8 the build"
busy="$(systemctl list-units 'altair-*.service' --state=active --no-legend --plain 2>/dev/null | awk '{print $1}' | tr '\n' ' ')"
if [ -n "$busy" ] && [ "$force" = 0 ]; then
    echo "an ALTAIR job is running ($busy): not rebuilding under it. Try again when it ends, or pass --force." >&2
    exit 3
fi
if [ "$run_tests" = 1 ]; then
    step "5/8 build and test"
    as_user "$root/ops/build.sh" test
else
    as_user "$root/ops/build.sh"
    step "5/8 tests skipped (--test runs them)"
fi

step "6/8 scheduled jobs"
"$root/ops/install.sh" --go --user "$user"

step "7/8 launchers"
home="$(getent passwd "$user" | cut -d: -f6)"
apps="$home/.local/share/applications"
install -d -o "$user" -g "$user" "$apps"
cat > "$apps/altair.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=ALTAIR
Comment=ALTAIR trading terminal
Exec=env ALTAIR_QT_ROOT=$qt_root $root/build/linux/desktop/altair_desktop
Path=$root
Terminal=false
Categories=Office;Finance;
EOF
cat > "$apps/altair-update.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Update ALTAIR
Comment=Install or update everything ALTAIR needs (ops/setup.sh)
Exec=x-terminal-emulator -e bash -c 'sudo $root/ops/setup.sh; echo; read -rp "Done. Press Enter to close. "'
Path=$root
Terminal=false
Categories=System;
EOF
chown "$user:$user" "$apps/altair.desktop" "$apps/altair-update.desktop"
if [ -d "$home/Desktop" ]; then
    install -o "$user" -g "$user" -m 0755 "$apps/altair.desktop" "$apps/altair-update.desktop" "$home/Desktop/"
fi
echo "launchers in $apps"

step "8/8 latency tuning"
if [ "$tune" = 1 ] && [ -x "$root/ops/tune_host.sh" ]; then
    "$root/ops/tune_host.sh" --go
elif [ "$tune" = 1 ]; then
    echo "ops/tune_host.sh is not in this version yet: nothing tuned"
else
    echo "skipped (--tune applies it)"
fi

printf '\nALTAIR is installed and up to date in %s.\n' "$root"
