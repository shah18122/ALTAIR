# CX06 N5–N8 verification

2026-09-16. Scope: native desktop layout/navigation only; see LAYOUT_HANDOFF.md.

## Build and tests

- MSVC / Qt 6.8.3, isolated build/codex-cx06. Final incremental build of
  altair_desktop and altair_navigation_shell_test completed with exit 0 (8 steps);
  no compiler warnings observed. Navigation tests were also rebuilt this slice.
- Final CTest selection: `^desktop_(navigation.*|bar_consistency|atlas|terminal|pages)$`.
  All 8 tests passed in 31.99 seconds: navigation_shell, navigation_shell_125,
  navigation_shell_200, bar_consistency, navigation, pages, atlas, terminal.
- Shell tests use offscreen Qt, explicit Windows fonts, isolated temporary INI
  preferences and an empty temporary working directory. Scale variants assert
  actual device scale, rather than only setting an environment variable.
- Geometry covers 1024x640, 1280x720, 1366x768 and 1920x1080 in all three
  navigation modes, all 33 pages at 1280x720, reachable scroll content and
  toolbar control rectangles. Halt navigation reveals its panel after layout.
- Preferences cover schema migration/recovery, bounded widths, all 33 recent
  page identities, valid terminal pane round trips and malformed-state refusal.
- Scoped git diff whitespace check passed; Git reported only LF/CRLF notices.

## Render inspection

Re-ran altair_navigation_shell_test with capture output after the final build:
exit 0, zero failures. Inspected both native renders:

- build/codex-cx06/laptop-workspace.png: Atlas at 1280x720, expanded navigation,
  readable breadcrumb and permanent toolbar controls.
- build/codex-cx06/laptop-workspace.png.terminal.png: Terminal at 1024x640,
  hidden navigation, Halt panel revealed within the scrolling viewport.

Earlier inspection caught an elided breadcrumb and an offscreen Halt panel;
both were corrected and the final tests/renders above include those fixes.
Missing data/transport labels are intentional fixture conditions. No halt request
or clear action was issued; selecting Halt controls only navigates.

## Not certified

No full-project regression, physical-DPI or screen-reader acceptance, Linux,
sanitizer, engine latency benchmark, live feed/broker or release certification.
No changes to trading logic, datasets or Atlas internals by this layout slice.
Backend-dependent CX03/CX07 and broader release gates remain separate.
