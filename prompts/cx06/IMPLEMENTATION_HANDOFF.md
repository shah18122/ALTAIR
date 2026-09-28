# CX06 native navigation and OHLCV checker — implementation handoff

2026-09-16. Scope: N1–N4 navigation and D1–D2 cold-path reconciliation.
This is not completion of the entire terminal rebuild or live-trading approval.

## Implemented

- Native Qt sidebar with seven functional groups and expandable arrow trees.
- All 33 legacy page indices and labels preserved; stable IDs additionally work
  with existing named routes and `--page`. Model Atlas remains unchanged.
- Native Workspaces menu/submenus, compact group-popup rail, expanded/compact/
  hidden modes, permanent hide/show control, and Ctrl+Shift+B.
- Ctrl+K workspace search, keyboard selection, saved-page favourites, location
  breadcrumb, and per-user presentation preferences.
- Invalid preferences visibly recover to expanded Terminal and preserve the
  malformed values in a recovery group. No credentials, orders or halt state
  are persisted. Preference writes are UI interactions, never tick callbacks.
- F11 remains fullscreen. Removed the conflicting window Escape binding;
  Terminal's existing Escape and F1–F5 bindings are not changed.
- Permanent Halt-controls action only reveals the existing tab. It cannot
  request, clear or acknowledge a halt.
- Standalone `altair_bar_consistency`: strict integer-paise CSV reader and
  session-aligned OHLCV comparison, gap/duplicate/range/overflow detection,
  bounded diagnostics, explicit unknown volume and partial-session treatment.
  No existing loader, aggregator, writer or dataset was edited.

## Ownership

Codex files: `desktop/navigation_registry.hpp`, `navigation_state.hpp`,
`navigation_search.hpp`, `workspace_navigation.hpp`; navigation-only edits in
`desktop/main_window.hpp`; additive CX06 target registration in
`desktop/CMakeLists.txt`; `desktop/data/bar_consistency{,_io}.hpp`,
`desktop/data/bar_consistency.cpp`; three new tests
`test_navigation.cpp`, `test_navigation_shell.cpp`, `test_bar_consistency.cpp`;
this directory's evidence and appended root change log.

Claude's existing changes, including the kill-switch test registration in shared
desktop CMake, were preserved. No OMS, broker, order ticket, price transport,
watchlist, quant/model, Atlas or historical-data source was edited by this work.
No commits, broker login, fetching, streaming, orders or live halt actions.

## Verification

Isolated build: `build/codex-cx06`, MSVC 19.51, Qt 6.8.3, RelWithDebInfo.
Targets: desktop, navigation unit test, real-window test, checker/test, existing
Atlas, terminal and pages tests. Final clean-build/test outcome is recorded in
`VERIFICATION.md`.

Native tests cover route identity across tree/menus/compact/search; group arrows
never routing to page zero; invalid index/ID handling; favourites and corrupt
preference recovery; hide/show restoration; menu-object lifetime; keyboard
search; real page-stack integration; persistent controls/status visibility; and
opening Halt controls without creating a request file.

The shell test uses a temporary working directory, empty tape and temporary INI
settings, not real account or dataset paths. An early harness run exposed that
the QSettings organisation/application constructor ignores the default format;
the shell now explicitly honours it. The single test-user registry key from
that early run was removed and absence checked. No real user's preferences were
removed.

Headless Qt on Windows requires the platform plugin path and font directory.
An initial fontless render produced missing glyphs and a misleading terminal
width-test failure. With Windows fonts supplied, the existing terminal test
passes. New tests register these environment requirements in CMake.

## OHLCV findings

Raw reports, UTC capture times and before/after SHA256 hashes:
`OHLCV_RESULTS.json`. All four input pairs were unchanged during their recorded
audit. Files may be updated later by the existing data workflow.

| Sample | Result under regular 09:15–15:30 interval-open policy |
| --- | --- |
| NIFTY spot Aug 2026, 1m → 5m | All 1,575 windows match OHLC; volume unknown in all windows, so NOT fully OHLCV-verified. |
| NIFTY spot Sep 2026, 1m → 5m | 757 available windows match. Latest sampled session is partial: 339 missing base bars and 68 missing targets. This is incomplete coverage, not a price mismatch. |
| NIFTY futures Aug 2026, 1m → 5m | All 1,575 regular-session windows match OHLCV; 252 input rows fall outside the configured session. Overall result refuses a pass. |
| NIFTY futures Aug 2026, 5m → 60m | 126 of 147 windows match; 42 out-of-session rows, 41 price-field mismatches, 21 volume mismatches. |

Do not automatically delete or fold the futures post-15:30 rows into regular
hours. Confirm provider timestamp/session, contract/roll and adjustment semantics
before proposing a writer or historical-data migration. Existing
`aggregate_bars` remains epoch-aligned; this checker deliberately uses a
09:15 session anchor and exposes the distinction without changing Claude's code.

The checker compares all dates present on either side of its two inputs.
It cannot detect a whole session absent from both inputs without a calendar.
Daily bars, special sessions, auctions and other session policies are unsupported.
These four samples are not a full-history or all-instrument certification.
Exit 0 means consistency of available fields; check `ohlcv_fully_verified`
before claiming volume verification. Optional rounding is opt-in and counted.

## Run

After building, from the project directory:

```powershell
.\build\codex-cx06\desktop\altair_bar_consistency.exe --base dataset/spot/nifty/1m/2026-08.csv --target dataset/spot/nifty/5m/2026-08.csv --base-minutes 1 --target-minutes 5
```

Both inputs may also be directories (nonrecursive CSV discovery). Output is
JSON on stdout; exit 0 consistent, 1 invalid/incomplete/mismatched, 2 usage error.
No output-file or repair option exists.

## Remaining gates / integration

- The wider CX06 terminal redesign, additional native workflows and optional
  design-contract fields (recent pages, width/layout persistence) are not claimed.
- Existing Options and Terminal chain destinations remain distinct; no live
  option-chain feed, FYERS integration or new readiness claim was added.
- No engine latency benchmark, sanitizer, Linux, live broker, manual high-DPI/
  accessibility or complete full-project regression was run for this slice.
- Existing page minimum widths are not redesigned here. The captured full shell
  with Segoe UI is 1,623 logical pixels wide because existing pages constrain it;
  smaller-screen layout remains a broader terminal task.
- Claude CX02/CX03 retains ownership of concurrency, buffer reuse, order ranges,
  feed pipeline and data writers. This UI/checker work adds no hot-path code.
- Do not replace the user's existing launcher/default build blindly: this build
  is deliberately isolated. Rebuild the selected shared preset after coordination.
