# CX06 N5–N8 — responsive native workspace and saved layout

## Context

User asks to finish remaining work and authorises choosing a phase. Continue
CX06 without entering Claude's active CX02/CX03 files. Previous native shell
was forced to 1,623 logical pixels by inactive pages. Fix that and complete
presentation-state persistence. Existing Atlas and terminal trading logic stay
unchanged. The user authorises direct implementation despite historical actor roles.

## File manifests and frozen contracts

N5: desktop/navigation_state.hpp, desktop/workspace_navigation.hpp,
desktop/tests/test_navigation.cpp.
- Extend NavigationState with int expanded_width=258 and QStringList recent.
- read/write_navigation_state keep signatures, schema2 migrates schema1.
- WorkspaceNavigation: bool set_expanded_width(int width);
  int expanded_width() const noexcept; QStringList recent_pages() const.
- Width is logical pixels, valid220..360; invalid input refuses without change.
- Recent IDs are unique, most recent first, maximum33 proven by registry size;
  no silent history truncation. Schema1 gets defaults while preserving page,
  mode, favourites, groups. Schema2 requires valid new fields. Unknown schema
  visibly recovers with backup. Menus expose recent routes.

N6: desktop/workspace_pages.hpp (new), desktop/main_window.hpp,
desktop/tests/test_navigation_shell.cpp.
- WorkspacePages final : public QStackedWidget; constructor QWidget* parent=nullptr,
  QSize sizeHint() const override, QSize minimumSizeHint() const override.
- Only the active page constrains the scrolling viewport. Oversize content is
  scrollable; it must not force the top-level window beyond the requested size.
- Preserve routing, chart refresh, focus and all33 pages. A typed-accessor is
  unnecessary: main owns existing QStackedWidget* and the wrapper internally.
- Keep recovery/search/Halt controls and feed/provenance status outside scroll.
- Opening Halt controls reveals the panel after deferred layout settles; it
  never clicks an action or writes a halt request.
- Test1024x640,1280x720,1366x768,1920x1080 logical sizes in all nav modes.

N7: desktop/workspace_layout.hpp (new), desktop/main_window.hpp,
desktop/tests/test_navigation_shell.cpp.
- bool restore_terminal_layout(QSplitter*, QSettings&): cold GUI-thread API,
  three horizontal panes only, stable terminal-three-pane/v1 identity; valid
  positive sizes1..100000 logical pixels, exactly3 values, otherwise refuse.
- bool save_terminal_layout(QSplitter*, QSettings&): same contract.
- Save only sizes/identity, never order/selection/arming/account state.
- Width actions and reset-layout action are explicit menu controls. Reset
  changes layout only, not active page/favourites or trading state.
- Chrome styling limited to menu/toolbars and their controls; no inherited
  rewrite of Atlas or trading widgets. No animation/dependency.

N8: desktop/CMakeLists.txt plus existing shell test (test registration).
- Add125% and200% offscreen DPI test variants using the same isolated harness.
- Build only build/codex-cx06. Targeted regressions, native renders and reported
  unavailable gates. No full release/engine latency claim.

Documentation card: this directory's handoff/verification and append-only
change_by_codex.txt. Do not edit shared Claude planning/audit ledgers.

## Acceptance

1. schema_migration_and_layout: v1 preserved; v2 width/recent roundtrip; corrupt
   width/recent/schema visibly refused; duplicate IDs canonicalized.
2. recent_route_order: repeat visits reorder, no duplicates, all33 fit; every
   recent menu entry activates its existing legacy destination.
3. shell_laptop_geometry: every page remains reachable at named sizes; outer
   window does not grow, long active-page content remains scrollable.
4. permanent_controls: nav recovery and Halt action geometries remain visible
   and clickable in every mode, with status visible; no halt file created.
5. terminal_layout_roundtrip: valid pane sizes restore; zeros, malformed,
   unknown identity, wrong counts and out-of-range sizes refuse unchanged.
6. scale_variants: same native shell assertions pass100%,125%,200% with actual
   font metrics. Render laptop views and inspect text/control clipping.

## Constraints / forbidden

All work cold GUI path. No tick callbacks, broker handles, data repair, dependency
installation, reference-binary use, Atlas edits, order/kill/transport changes,
shared build writes, credentials, network actions or commits.
Existing large shell receives surgical integration rather than a rewrite.
No change to source/data semantics. Layout scroll is not market-data dropping.

## Rules

RULES — violating any of these fails review:
1. Produce complete files. No "...", no "rest unchanged", no placeholder bodies.
2. Do not create, rename, or delete any file not in the File Manifest.
3. Do not change any signature in the Interface Contract. If you believe a
   signature is wrong, implement it as specified AND add a comment block at the
   top of the file titled "CONTRACT OBJECTION" explaining why. Do not act on it.
4. Do not add any third-party dependency. Only what vcpkg.json already lists.
5. No exceptions on the hot path. Return std::expected<T, Error>.
6. No dynamic allocation inside any function marked ALTAIR_HOT.
7. No using namespace at file scope in a header.
8. Every public function gets a doc comment stating units and preconditions.
9. Write the acceptance tests exactly as named. Do not rename or merge them.
10. If ambiguous, implement conservatively and document assumptions.
