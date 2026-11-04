#!/usr/bin/env bash
# ops/build.sh -- build ALTAIR on Linux (the server and the Qt terminal under DCV).
#
#   ops/build.sh            configure (once) and build everything, no tests
#   ops/build.sh test       build, then run the test suite (ctest -LE experiment)
#   ops/build.sh app        build only the terminal, price service, live engine and recorder
#
# Needs: cmake >= 3.28, ninja, g++ 13 (or clang 18), libboost-dev, libssl-dev,
# libtomlplusplus-dev, and Qt 6.8+ for the terminal. Point ALTAIR_QT_ROOT at the
# Qt prefix (for example /opt/qt/6.8.3/gcc_64); without Qt the engine and helpers
# still build and the terminal is skipped.
#
# It stops before building if an ALTAIR process is running from this build
# directory, because the linker would replace a binary under a live process.

set -euo pipefail
cd "$(dirname "$0")/.."

mode="${1:-build}"
preset=linux
dir="build/$preset"

case "$mode" in
    build|test|app) ;;
    *) echo "usage: ops/build.sh [build|test|app]" >&2; exit 2 ;;
esac

if pgrep -f "$PWD/$dir/" >/dev/null 2>&1; then
    echo "an ALTAIR process is running from $dir; stop it first (systemctl stop 'altair-*')" >&2
    pgrep -af "$PWD/$dir/" >&2 || true
    exit 3
fi

# Every time, not only the first: a cache variable the preset gained since
# (ALTAIR_KERNEL_BYPASS) reaches an existing build directory too.
cmake --preset "$preset" >/dev/null

jobs="$(nproc)"
if [ "$mode" = app ]; then
    targets=()
    for t in altair_desktop altair_price_service altair_live_engine altair_feed_recorder; do
        if ninja -C "$dir" -t targets all 2>/dev/null | grep -q "^$t:"; then targets+=("$t"); fi
    done
    cmake --build "$dir" -j "$jobs" --target "${targets[@]}"
else
    cmake --build "$dir" -j "$jobs"
fi

if [ "$mode" = test ]; then
    if [ -d dataset ]; then exclude='experiment'; else exclude='dataset|experiment'; fi
    ctest --test-dir "$dir" -LE "$exclude" -j "$jobs" --timeout 600 --output-on-failure
fi
