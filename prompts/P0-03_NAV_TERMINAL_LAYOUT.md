# P0-03 — Navigation and terminal acceptance layout

Owner: Codex. Frozen product target, 2026-09-23. Terminal implementation remains
in P4-05..07; this document does not claim the existing terminal matches it.

## Visible navigation

Keep the seven existing groups, native expansion arrows, compact rail and Atlas
model submenus. Market shows Terminal and Microstructure; Accounts shows Brokers
and Execution. Other existing pages remain until their replacement task lands.
The current Ratio Spread page is replaced by Pair Trading in P5, not renamed
prematurely while it still has different behaviour.

Retain the 35 internal page indices for existing model links. Visible traversal,
search, favourites and recent history expose 30 destinations. Route migrations:

| Old route or label | Destination |
|---|---|
| market.grid / Live Grid / index 0 | market.terminal / index 2 |
| market.chart / Chart / index 1 | market.terminal / index 2 |
| accounts.kite / Kite Account / index 5 | accounts.brokers / index 34 |
| accounts.connections / Link Kite / index 28 | accounts.brokers / index 34 |
| accounts.fyers / FYERS Primary / index 33 | accounts.brokers / index 34 |

Migrate saved page, favourites and recent history, deduplicate aliases, retain
valid width/mode/group preferences, and show a migration notice. Invalid settings
continue using the existing backup/recovery mechanism. CLI names and numeric
routes use the same destination resolver. Keep tick ingestion/model initialisation
intact while removing these UI destinations; full headless separation is P3-04.

## Chrome

Permanent toolbar: hide/show navigation, Halt controls, Find workspace, favourite,
breadcrumb. Remove the Layout toolbar button. Put compact/expanded/reset options
in the existing Workspaces menu. Drag the visible right edge of the navigation
between 220 and 360 logical pixels; remember width. Arrow keys on the focused
handle resize by 10 pixels; double-click resets to 258. Handle disappears when
sidebar is compact or hidden. Preserve Ctrl+K, Ctrl+Shift+B and F11.

## Greeksoft evidence and target mapping

Read-only evidence: `GETS Settings/GETS ColumnProfile.txt` and the header of
`GreekBackup/ASN019/26122024/GreekPosition.csv` under the reference root.
The profile contains widths and GreekMarketWatch columns, not a screenshot or
documented pixel scale. Do not treat its raw width numbers as Qt pixels.

| Reference field | Altair default position column |
|---|---|
| Exchange / MktSeg | Exchange and segment |
| Client / User | Broker plus masked account identifier |
| Symbol / ScripName / InstrumentName | Canonical instrument with display symbol |
| Ser_Exp / StrikePrice / OptionType | Expiry / strike / type when applicable |
| NetQty / Units | Signed net quantity in units; never implicit lots |
| NetPrice / TradPrice | Average entry / latest mark, distinctly labelled |
| MtoM | Unrealised P&L in INR with valuation timestamp |
| UserIV / RealizedIV / TheoriticalPrice | Options workspace, not default Terminal |

Target rows: 26 logical pixels, sortable/resizable columns, fixed-width numeric
font, right-aligned paise-formatted money, explicit unknown/stale cells. Keep
each broker/account/product position separate even when symbols match.

```text
ALTAIR  [hide navigation] [Halt] [Find] [Save]       Market / Terminal
------------------------+----------------------------------------------
Search workspace        | AVAILABLE FUNDS             as of / stale flag
v Market                | FYERS: amount or unknown | Kite: amount or unknown
  Terminal              | Comparable INR total: amount (partial if missing)
  Microstructure        |----------------------------------------------
v Accounts              | OPEN POSITIONS    Broker [All]  Search [      ]
  Brokers               | Broker Account Exchange Symbol Product Qty ...
  Execution             | [no open positions / disconnected / rows]
...                     | No chart, live grid, watchlist or order ticket
                 [drag] | in the default Terminal view.
------------------------+----------------------------------------------
Feed provenance / staleness                            Paper / live mode
```

Available cash, collateral, used margin and available trading balance stay
separate. A missing broker balance is unknown, not zero. A cross-broker total is
informational and does not imply funds can cover orders on another broker.

## Implementation slices and acceptance

- P4-01a manifest: navigation_registry.hpp, navigation_state.hpp,
  workspace_navigation.hpp, tests/test_navigation.cpp (all under desktop/).
  Interfaces: `constexpr int nav_destination(int) noexcept`,
  `bool nav_visible(int) noexcept`. Invalid index returns -1; aliases above.
  Existing index/ID lookup retains compatibility. Test every alias, saved-state
  migration and all 30 tree/menu/search destinations.
- P4-01b/P4-02 manifest: desktop/navigation_resize_handle.hpp,
  desktop/workspace_navigation.hpp, desktop/main_window.hpp,
  desktop/tests/test_navigation_shell.cpp. Handle emits a requested width through
  a GUI-thread callback; navigation's existing width setter refuses invalid
  widths. MainWindow resolves destinations before changing the stack.
  Test drag/persistence, bounds, keyboard, reset, hide/show and no Layout button.
- P4-01c manifest: desktop/navigation_search.hpp, desktop/tests/test_navigation.cpp.
  Retired labels remain search aliases returning a single canonical destination;
  searching FYERS/Kite opens Brokers and market.chart opens Terminal.
- Existing shell tests at 100/125/200% and 1024..1920px must pass. Capture the real
  widget rendering for review. Data/forecast feed paths must remain unchanged.

Frozen decisions above implement the user brief; no external deployment or
account operation is part of these navigation cards.
