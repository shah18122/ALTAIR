# CX-06 native navigation and OHLCV reconciliation — 2026-09-16

User authorises implementation, including arrow menus/submenus and cross-timeframe OHLCV consistency. Claude owns CX-02/CX-03. No commits, live trading, credentials, dataset replacement or broker calls authorised here.

## Cards and manifests

- N1 registry: `desktop/navigation_registry.hpp`, `desktop/tests/test_navigation.cpp`. Freeze legacy indices 0–32; stable IDs and seven ordered groups follow the existing design NAVIGATION.json. `QStringList nav_page_names()`, `int nav_page_index(const QString&)`, `QString nav_page_id(int)` preserve names/indices and additionally accept stable IDs.
- N2 widget: `desktop/workspace_navigation.hpp`, `desktop/navigation_search.hpp`, `desktop/navigation_state.hpp`, existing N1 test. Native Qt only, no Q_OBJECT required. Class WorkspaceNavigation exposes select_page(int), current_page(), set_mode(NavigationMode), mode(), toggle_visibility(), show_search(), toggle_favourite(), restore_state(QSettings&), save_state(QSettings&) const, populate_menu(QMenu*). Activation callback takes legacy index; caller owns the actual page routing. State-change callback is cold UI only.
- N3 shell integration: `desktop/main_window.hpp`, `desktop/CMakeLists.txt`. Replace flat navigation only; retain existing pages, chart-entry hook, replay toolbar/status and Atlas files unchanged. Permanent navigation/search/favourite controls; native menus and accessible arrows. F11 remains fullscreen; remove conflicting window Escape toggle. Settings isolated per user, no orders or credentials persisted.
- N4 shell acceptance: `desktop/tests/test_navigation_shell.cpp`, `desktop/CMakeLists.txt`. Instantiate the real MainWindow offscreen with empty tape, temporary INI preferences and temporary relative dataset/account paths. Test all routes, hidden-sidebar recovery toolbar, Halt-controls focus without request-file writes, shortcut ownership and full-window capture. Never start a stream, fetcher, order or halt action.
- D1 cold-path data reconciliation: new files under `desktop/data/` and `desktop/tests/`, then separately register in desktop CMake. Contract and acceptance details will be recorded before writing those files. Source datasets and existing loaders/writers remain read-only for this card.
- Documentation handoff: append only a clearly identified entry to root `change_by_codex.txt` after rereading its latest contents; own evidence under `prompts/cx06/`. Shared audit and CX02 files untouched.

## Acceptance

N1: exactly33 unique IDs; all legacy names/indices preserved; unknown IDs return -1. N2: arrows toggle groups without activating a page; every page reachable by tree/menu/search; saved-page deduplication; expanded/compact/hidden restore semantics; corrupt settings recover to expanded Terminal; native keyboard support and isolated settings round trip. N3: desktop compiles, existing relevant tests pass, no new broker/OMS link, no hot-path change. Native offscreen rendering checks layout; report unsupported interactive/high-DPI cases honestly.

Build exclusively in `build/codex-cx06` to avoid Claude's shared build directories. No dependency installation. Performance claims limited to measured UI checks; engine latency and live integration remain dependent on CX-03.

## Constraints

Existing large shell is edited surgically rather than rewritten. New components stay focused. No native Atlas edits. No order-ticket, halt-state-machine, price client, quant/model or data-writer edits. UI can open existing Halt controls, never acknowledge or submit a halt itself. All computations here are cold path; no engine scheduling changes.

The protocol's complete implementations, frozen manifests, named tests, public preconditions, zero-warning builds, strong units, checked bounds and all eight review gates remain in force. Unavailable gates are not passes. Historical actor assignments are superseded by the user's direct implementation authorisation.
