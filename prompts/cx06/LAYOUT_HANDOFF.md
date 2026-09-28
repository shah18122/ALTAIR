# CX06 N5–N8 — native layout implementation

2026-09-16. Continues the earlier N1–N4 navigation and D1–D2 checker delivery.
This document supersedes the earlier handoff's deferred laptop-layout, recent
pages and sidebar-width items. It does not supersede backend/release blockers.

## Delivered

- Active-page-only layout constraints with an outer scrolling workspace.
  Inactive pages no longer force the desktop to1,623 logical pixels. Oversize
  active panels retain their contents and scroll; they are not silently clipped
  or squeezed into unusable controls.
- Existing33 routes remain unchanged. Navigation, search, Halt-controls action,
  replay controls and status remain outside the scrolling workspace.
- Halt navigation reveals its panel after layout settles, without pressing a
  request/clear button or claiming engine acknowledgement.
- Matching charcoal toolbar/menu chrome and amber page breadcrumb. Atlas and
  terminal widget internals were not restyled or rewritten.
- Saved sidebar widths220–360 logical pixels, explicit width menu, and recent
  pages using stable IDs. All33 unique identities fit; no hidden history cap.
- Schema2 presentation preferences with schema1 migration, validation, recovery
  notice and backup. Existing page, mode, groups and favourites survive migration.
- Three-pane terminal size persistence under terminal-three-pane/v1. Invalid,
  zero, negative, excessive or wrong-count sizes are refused. Missing state keeps
  defaults. Terminal panes cannot accidentally collapse to zero width.
- Reset layout only restores width/mode/pane defaults while preserving page and
  favourites. No ticket input, instrument choice, credentials, order or halt state
  is persisted.

## Files and boundaries

New desktop/workspace_pages.hpp and workspace_layout.hpp; updates to
navigation_state.hpp, workspace_navigation.hpp, main_window.hpp, CMakeLists.txt,
test_navigation.cpp and test_navigation_shell.cpp.

The frozen cards are in CX06_LAYOUT.md. These are GUI/cold-path changes; no tick
callback, model scheduling, transport, broker, order/kill logic, dataset, Atlas,
reference application or retired client was edited. Claude's existing shared
CMake changes remain. No commits or default-launcher replacements.

## Tests

Named test functions: schema_migration_and_layout, recent_route_order,
shell_laptop_geometry, permanent_controls, terminal_layout_roundtrip,
scale_variants. CTest runs the real-window harness at100%,125%,200%, checking
the actual device scale. It uses temporary INI preferences and an empty,
temporary working directory, never real account/order/market data.

Geometry assertions cover1024x640,1280x720,1366x768,1920x1080 logical pixels in
expanded/compact/hidden modes, plus every existing page at1280x720.
Control rectangles must lie inside the toolbar, not merely report visible while
hidden in its overflow menu. Scroll access to wider terminal content is tested.
Malformed layout restore must leave previous pane sizes unchanged.

Final build, test results and renders are recorded in LAYOUT_VERIFICATION.md.

## Limitations and next dependency

This completes these independent CX06 desktop-layout cards, not CX03 execution
integration or CX07 live option-chain/order workflows. Scrollability is not a
new mobile/reflow design for every existing analytics page. Arbitrary docking,
multi-monitor window-position recovery and shared named workspace profiles are
not implemented. Preferences currently cover navigation and terminal pane sizes.

Engine kill acknowledgement/risk state is not invented from a request file.
Actual integrated feed/kill/error behaviour still depends on CX03's verified
backend contracts. No additional live subscription, broker connection or data
repair has been authorised by this UI work.

No manual screen-reader/physical-DPI, Linux, sanitizer, engine-latency or
full-project release gate is claimed. All phase08 requirements remain separate.
The screenshot's missing-data labels are expected: the native test deliberately
has no instrument master/account fixtures or network.

Next backend-dependent work: CX07 after CX02/CX03 interfaces are stable;
FYERS integration remains subject to verified protocol/credentials and separate
live-use authority. Historical OHLCV anomalies remain recorded, not rewritten.
