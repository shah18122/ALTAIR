# CX-01 desktop and retired client audit (IN PROGRESS)

Reader: cx01_ui_client. Source baseline and exact received ranges are in CX01_UI_COVERAGE.json. No runtime files changed, builds/tests performed, accounts accessed or reference folders read. CLAUDE.md and prompts/PROTOCOL.md read fully before review. Findings below remain static audit evidence, not deployment clearance.

## Initial evidence

- UI-001 [P1]: desktop/main_window.hpp:1164-1174 and other compute handlers execute heavyweight fits/report generation synchronously in the QApplication thread. One processEvents() before a fit cannot service priceUpdated, timer or halt-button events during the fit; the UI itself states it is busy until finishes. No concurrency, cancellation or event responsiveness test is established by these handlers. The application currently lacks live execution assembly, but this is a required safety/latency blocker before engine integration.
- UI-002 [P2]: desktop/main_window.hpp:488-505 backward seek resets data but not MarketClock; seek-to-zero leaves old phase/time state, whereas restart() resets it. Counterexample: advance after session close, seek to zero; clock_.has_engine_time stays true and screen renders epoch from last_ts_ns_=0 rather than no engine time.
- UI-003 [P2]: desktop/main_window.hpp:427-432 replaces tape/count on reload but never updates scrub_->maximum(), initialized only at1428. A newly longer tape cannot be scrubbed to its new end.
- UI-004 [P2]: desktop/main_window.hpp:811-825 returns without clearing candles when applied_==0; a backward reset leaves a chart showing future candles until another tick/refresh. It also labels actual ReplayTick tape synthetic irrespective of tape_real_.
- UI-005 [P3]: desktop/atlas.hpp:133 reads invalid family UserRole as integer0, while build_tree():169-176 never assigns -1 to family nodes. Double-clicking a family opens Live Grid. Atlas tests never exercise a double-click/callback.

## Current navigation/integration map

33 pages in a fixed-width190px flat QListWidget; no categories, favourites, nav search or persistent hide/show. Model Atlas groups its own rows but only stores numeric page indices; MainWindow checks bounds not semantic alignment. UI actual prices enter Terminal PriceClient independently of replay-driven Live Grid. Direct-link allowlist excludes broker/OMS but not filesystem/process/network capabilities; treat as architectural discipline rather than security isolation.

## Pending

Exhaustive desktop/client reading, test limitations and full Atlas per-entry implementation/integration/readiness table still in progress. Do not call this audit complete.
