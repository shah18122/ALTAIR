# CX06 verification — 2026-09-16

## Final build and tests

- Clean rebuild of the seven requested targets plus checker dependency: exit 0,
  all 38 build steps completed, no compiler warnings observed.
- Toolchain: MSVC 19.51.36256.0, Qt 6.8.3, C++23, RelWithDebInfo.
- Build directory exclusively `build/codex-cx06`.
- CTest: **6/6 passed**, 22.48 seconds.
  - `desktop_navigation_shell`: 0.78 s.
  - `desktop_bar_consistency`: 0.41 s.
  - `desktop_navigation`: 0.26 s.
  - `desktop_pages`: 19.88 s.
  - `desktop_atlas`: 0.15 s.
  - `desktop_terminal`: 0.93 s.
- Separate real-window capture run: exit 0, zero shell-test failures.
- Native screenshot inspected with readable Segoe UI, seven expandable groups,
  selected Atlas route, permanent recovery/Halt controls and visible status.
  Capture: `build/codex-cx06/native-shell.png`.
- `git diff --check -- desktop/main_window.hpp desktop/CMakeLists.txt`: exit 0.
- Atlas source and existing bar loader have no diff.
- Early test-user registry preference key was removed; its absence was checked
  after the corrected temporary-INI harness ran.

Commands, from a Visual Studio developer shell with bundled CMake/Ninja on PATH:

```powershell
cmake -S . -B build/codex-cx06 -G Ninja -DALTAIR_BUILD_TESTS=ON -DALTAIR_BUILD_BENCH=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/codex-cx06 --clean-first --target altair_navigation_test altair_navigation_shell_test altair_desktop test_bar_consistency altair_atlas_test altair_terminal_test altair_pages_test -j 2
$env:QT_QPA_PLATFORM='offscreen'
$env:QT_QPA_PLATFORM_PLUGIN_PATH='D:\Qt\6.8.3\msvc2022_64\plugins\platforms'
$env:QT_QPA_FONTDIR='C:\Windows\Fonts'
$env:PATH='D:\Qt\6.8.3\msvc2022_64\bin;' + $env:PATH
ctest --test-dir build/codex-cx06 -R '^desktop_(navigation|navigation_shell|bar_consistency|atlas|terminal|pages)$' --output-on-failure --timeout 60
```

The first configure also supplied the absolute bundled Ninja path through
`CMAKE_MAKE_PROGRAM`. The shared default/net/asan directories were not used.

## Data evidence

`OHLCV_RESULTS.json` contains four final read-only sample audits, hashes before
and after, explicit nonzero audit exit codes, and bounded diagnostics.
All sample input hashes remained unchanged during their respective run.
See `IMPLEMENTATION_HANDOFF.md` for interpretation and remaining source/session
questions. Neither passing unit tests nor matching sample bars certify all data.

## Gates not claimed

No full-project regression, ASan/TSan/UBSan, Linux, manual desktop/high-DPI or
screen-reader validation, engine latency suite, live broker, FYERS, feed-pipeline
or order-safety certification. Missing network/toml dependencies were named by
configure and not installed. Existing page-width constraints remain.

Claude's concurrent edits were preserved; this evidence is for this bounded
navigation/data-checker slice, not a review approval of all working-tree changes.
