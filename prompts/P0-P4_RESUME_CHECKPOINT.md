# Plan resume checkpoint — 2026-09-23

## Delivered

- Read-side broker evidence v1 in `core/types/broker_state.hpp`. Identity binds
  provider, local account slot and login generation. Separate auth/feed/account
  expiry windows; risk evidence binds the route revision. Helpers are pure and
  allocate nothing. Funds use optional integer-paise Notional fields.
- Synthetic tests for expiry boundaries, unknown states/schema, replay data,
  provider mismatch, stale account/feed/risk evidence, late previous-login
  results, route changes and paper/live separation.
- FYERS saved sessions display amber and explicitly unverified. Malformed,
  future-dated, timezone-less or oversized metadata is refused. Broker overview
  no longer advertises a live source based on saved files or an old user ID.
- Kite naive login timestamps compare in an explicit timezone, preserving IST
  calendar semantics when the host timezone differs.
- One visible Brokers destination; Live Grid/Chart routes migrate to Terminal.
  35 internal indices retained, 30 visible destinations. Sidebar, compact menus,
  search, favourites, recent pages, CLI and saved state share alias behaviour.
- A visible six-pixel sidebar edge supports drag, arrow-key resizing, reset and
  persistent 220..360 logical-pixel widths. The Layout toolbar button is gone;
  presentation/reset actions live under Workspaces > Navigation appearance.
- GETS column/position evidence mapped to an explicit Terminal wireframe and
  acceptance specification in `P0-03_NAV_TERMINAL_LAYOUT.md`.

## Build and verification

- `build/codex-cx06`: desktop and all configured targets rebuilt successfully.
- `build/net`: desktop, broker evidence and broker status test targets rebuilt.
- Broker evidence/status tests pass in both configurations.
- Native navigation suite passes, including aliases, mouse/key events and
  settings migration. Shell tests pass at 100%, 125% and 200% display scales,
  retaining hide/show, Halt access and 1024..1920px window sizes.
- Visual inspection: `build/codex-cx06/navigation-resumed.png` and
  `build/codex-cx06/workspace-resumed.png`, captured from actual Qt test widgets
  with isolated settings/account paths. Navigation groups/arrows, drag edge,
  selected-page treatment and toolbar are visible. These are offline fixtures.
- Full CTest after rebuilding all configured targets: **145/145 passed**, zero
  failures, 235.55 seconds (`build/codex-cx06`, RelWithDebInfo). Network build
  verification was the two focused broker tests, not a full network CTest run.

## Exact executable identities

Local build timestamps on this host:

- `build/codex-cx06/desktop/altair_desktop.exe`: 2026-09-23 17:31:18,
  3,392,000 bytes.
- `build/net/desktop/altair_desktop.exe`: 2026-09-23 17:33:26, 3,449,344 bytes.

Source base remains HEAD `a34af5c5c9fa8c7498e8cc4df00907548a41e1a5` plus the
preserved dirty working tree. No commit or push was performed.

## Remaining boundaries

P0-02 is partial: complete account/position/log payloads, service publication and
OMS consumption are still required. Readiness helpers alone cannot place orders.
Current session readers still parse whole files containing credential bytes;
separate metadata publication remains a required part of the secure broker work.

The hidden legacy widgets remain constructed so the navigation change preserves
existing data/model initialisation. P3-04 still owns complete headless separation.
The Terminal itself has not yet been rebuilt into the positions/funds wireframe.
Broker forms, live FYERS auth/account/feed integration, Mac verification, the
remaining model work and arbitrage phases remain open in the root plan.

No live broker account, real credential fixture, order, or external deployment
was used for these changes. No reference GETS files were modified.
