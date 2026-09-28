# P6-01 — Atlas reconciliation against the actual tree

Owner: agent C. Manifest: this one file. No C++, CMake or config file was touched,
and `desktop/atlas_data.hpp` is **unmodified** (§11). Corrections are listed here
and **not applied**.

> **Provenance of this file.** This path already held the *dispatched card text*
> when the audit began. Nothing was deleted: the card body is preserved verbatim
> below, after the deliverable, under "Appendix A". The deliverable is everything
> above that appendix.

## 1. Method — what was read, and how each judgement was made

- `desktop/atlas_data.hpp` (460 lines) parsed **mechanically**, not by eye: a
  multiline regex over the file text extracted **85** `AtlasRow{...}` records,
  each with the line number it starts on. Every count below comes from that parse.
- `desktop/navigation_registry.hpp` (95 lines) — 35 pages, `nav_destination()`,
  `nav_visible()`, `kNavigationOrder`.
- `desktop/main_window.hpp` — constructor order (`build_nav(); build_watchlist();
  build_chart(); build_pages();`, lines 170-176), all 35 `pages_->addWidget()`
  calls (index = call order), `show_page()` (196-210), the page-range check
  (1401-1424), and every report lambda (1042-1374).
- `desktop/quant_pages.hpp` — the 20 report functions, located by line
  (execution 421, volatility 466, risk 549, portfolio 594, ml 667, regime 740,
  strategies 863, overnight 1146, options 1411, basis 1672, flagging 1836,
  microstructure 2147, sizing 2274, cointegration 2449, memory 2637, dcf 2754,
  neural 2949, features 3023, arbitrage_scans 3318, aggregator 3961) and searched
  keyword-by-keyword for calls into the model headers.
- `desktop/atlas.hpp`, `desktop/tests/test_atlas.cpp`, `cmake/AtlasAudit.cmake`,
  `desktop/CMakeLists.txt` (107-110, 594-631).
- For every named header: the header itself, its API names, and its test file.
- **Include graph**: 2,463 `#include` lines parsed across `core, models, analytics,
  strategies, risk, book, backtest, features, flagging, oms, feed, desktop, app,
  server, research, instruments, tests`, partitioned into tests (path contains
  `\tests\`) and non-tests. This is the evidence for the "live caller" column.
- **Absence checks**: exact-string search for each Absent row's model name across
  those same 17 directories.

Excluded from every count: `build/`, `vcpkg_installed/`, `Quants/`,
`RXT_trade/`, `RXT_trade 2/`, `GETSClient_5.0.191022_64bit_040924/`.

Decision rules, in this order:

1. **File exists** — `Test-Path` on the repo-relative string, now.
2. **Model present** — open the file; the named model must be there by name, by
   its own vocabulary, or by a distinctive API. A keyword miss alone is never
   proof of absence (row 20 is the trap: `grep ar1` finds nothing in
   `models/horizon_eval.hpp`, which is nevertheless a walk-forward AR(p)).
3. **Implemented** — model present **and** a test that exercises it. The test was
   opened; "includes the header" was only the first filter.
4. **Partial** — the row must state its gap. All ten do; the gap was then verified
   against the code.
5. **Absent** — re-searched now. Where a name could plausibly live under another
   implementation, that is written as "I did not find it".
6. **Page** — `nav_destination()`/`nav_visible()` arithmetically, then the widget
   at that index from the `pages_->addWidget()` order, then the report that widget
   runs, then whether that report calls the named header.

## 2. Counts, re-derived — they do **not** differ from 63/10/12

| Quantity | Card said | Re-derived | Note |
|---|---|---|---|
| Rows | ~85 | **85** (`atlas_data.hpp:88-455`) | parse found all 85 |
| Implemented | 63 | **63** | |
| Partial | 10 | **10** | |
| Absent | 12 | **12** | |
| Distinct named files | — | **49, all EXIST** | the other 36 rows are Absent (12) or carry `file == ""` with page -1 |
| Distinct non-negative pages | — | **20**: 6,7,9,10,12,13,14,15,16,17,18,19,21,23,24,25,26,29,30,31 | 20 rows use -1 |
| Folded page values | — | **0** | folded set is {0,1,5,28,33} |
| Out-of-range page values | — | **0** | valid 0..34 |
| Pages wrong for another reason | — | **31 rows** | §4, §5 — 20 point at a page that does not run the model, 5 cover only part of a bundle, 6 deny a page that exists |

`cmake/AtlasAudit.cmake` really does check what the header comment claims: rule 1
every BUILT/PARTIAL path exists, rule 2 an ABSENT row names no file, rule 3 the
path is `Dir/File.hpp`-shaped (`cmake/AtlasAudit.cmake:11-17,83-98`), and it fails
loudly if it parses zero rows (`:101-108`). Every path in the table exists, so the
audit is telling the truth today. It checks **paths only** — it has no opinion on
`status`, `page` or whether a model is actually in the named file.

## 3. `page` against `nav_visible()` — clean on the narrow question

`navigation_registry.hpp:60-71` folds `0→2`, `1→2`, `5→34`, `28→34`, `33→34`;
`nav_visible(i)` is false for exactly {0, 1, 5, 28, 33}. The 65 non-negative Atlas
page values use only 6…31: **no row points at a folded index, and none is out of
range.** On §18 P4-01's literal question the column is sound.

Two related facts:

- **The guard is weaker than the danger.** `main_window.hpp:1411-1424` aborts only
  if `r.page < -1 || r.page >= nav_page_names().size()` (35). A folded index passes
  it, then `show_page()` (`:196-210`) silently redirects and prints "X moved to Y"
  in the status bar. `test_atlas.cpp:80-84` explains why the check lives there.
  Latent, not live: no current row hits it.
- **The header comment is stale.** `atlas_data.hpp:80` says "the current 33-page
  layout (0..32)"; the registry holds 35 pages (0..34) and 30 visible.

The wider question — *does that page run that model* — is where the defects are.
Index → widget comes from the `pages_->addWidget()` order (constructor 170-176 puts
`build_watchlist()` first, `build_chart()` second):

| idx | widget | nav id (`navigation_registry.hpp:19-53`) | what it runs |
|---|---|---|---|
| 0 | watchlist/grid | market.grid (folded) | live grid |
| 1 | chart | market.chart (folded) | chart |
| 2 | TerminalPage | market.terminal | tape/ticket |
| 3 | ModelPanel | models.overview | `panels.hpp` → `data/fits.hpp` |
| 4 | data-flow page | system.data-flow | dataset inventory |
| 5 | KitePanel | accounts.kite (folded) | Kite account |
| 6 | CostPanel | risk.cost | `risk/cost.hpp` (`cost_panel.hpp:1`) |
| 7 | AnalyticsPanel | options.analytics | MC/bootstrap/drawdown (`analytics_panel.hpp:1-9,25-40`) |
| 8 | ChainPanel | strategies.ratio-spread | chain + butterfly margin |
| 9 | ComputePage "VALUE — DCF" | models.dcf | `dcf_report()` |
| 10 | ComputePage "AGGREGATOR" | models.aggregator | `aggregator_report()` |
| 11 | AuditPanel | system.audit | audit trail |
| 12 | ComputePage "EXECUTION — P13" | accounts.execution | `execution_report()` |
| 13 | ComputePage "VOLATILITY — P14" | options.volatility | `volatility_report()` |
| 14 | ComputePage "RISK — P15" | risk.var | `risk_report()` |
| 15 | ComputePage "PORTFOLIO — P15" | risk.portfolio | `portfolio_report()` |
| 16 | ComputePage "ML — P16" | models.trees | `ml_report()` |
| 17 | ComputePage "REGIMES" | models.regimes | `regime_report()` |
| 18 | StreamForecastPage | models.forecast | `models/stream_forecast.hpp` + `live_forecast.hpp` |
| 19 | ComputePage "STRATEGIES" | strategies.overview | `strategies_report()` |
| 20 | ComputePage "OVERNIGHT GAP" | strategies.overnight | `overnight_report()` |
| 21 | ComputePage "OPTIONS — P23-01" | options.chain | `options_report()` |
| 22 | ComputePage "BASIS" | strategies.basis | `basis_report()` |
| 23 | ComputePage "FLAGGING" | system.flagging | `flagging_report()` |
| 24 | ComputePage "MICROSTRUCTURE" | market.microstructure | `microstructure_report()` |
| 25 | ComputePage "SIZING AND LIMITS" | risk.sizing | `sizing_report()` |
| 26 | ComputePage "COINTEGRATION" | strategies.cointegration | `cointegration_report()` |
| 27 | ComputePage "MEMORY" | models.memory | `memory_report()` — Hurst + EWMA |
| 28 | KiteLinkPanel | accounts.connections (folded) | Kite link |
| 29 | ComputePage "NEURAL TIER" | models.neural | `neural_report()` → runs `altair_neural_5m_test` |
| 30 | ComputePage "FEATURES AND KINEMATICS" | models.features | `features_report()` |
| 31 | ComputePage "PARITY AND CALENDAR" | strategies.parity-calendar | `arbitrage_scans_report()` |
| 32 | AtlasPanel | models.atlas | this table |
| 33 | FyersLinkPanel | accounts.fyers (folded) | FYERS link |
| 34 | BrokerPage | accounts.brokers | broker page |

## 4. Disagreements — every row where Claimed ≠ Verified

There **are** disagreements. The status column is broadly honest: 62 of 63
Implemented rows have the algorithm in the named file and a test that exercises it,
all 10 Partial rows state their gap correctly, and all 12 Absent rows survive a
re-search. One status is wrong (§4.1), and the `page` column is wrong for **31**
rows (§4.2 and §5). No row names a file that does not exist.

### 4.1 Status / file / model claim

**D-1. Row 10, "Linear / ridge regression", Implemented → Verified PARTIAL.**
`strategies/cointegration.hpp` contains an OLS regression of one series on another
(`Regression` at `:96`, `regression()` at `:104`) and nothing else: the string
`ridge` and the string `Ridge` do not occur anywhere in the file (0 hits). The
ridge-penalised solve in this tree lives in the neural readouts — `models/mlp.hpp:92-99`
("A random hidden layer with a solved readout is a random-features model"),
`:192` ("Solve (G + ridge*I) w = rhs"), and `models/recurrent.hpp:249-255`
("Ridge readout over a fixed recurrent reservoir"). Test evidence exists for the OLS
half only (`strategies/tests/test_cointegration.cpp`, 37 keyword hits). *A reader
pressing this row's page button for "ridge" gets OLS.* Correction in §9 (C-1).

### 4.2 The page column — 31 rows (detail in §5)

Claimed page ≠ verified page for these rows. Grouped by failure mode:

- **Page does not run that model at all (20).** Rows 5, 6, 9, 17, 18, 20, 22, 23,
  25, 32, 37, 40, 46, 49, 58, 61, 62, 69, 70, 76 — §5 group A has the reason for
  each. The page exists, is visible, and shows something else entirely. Several are
  worse than a dead link because the page *looks* like the answer (page 16
  "ML — Trees" fits a GBDT and nothing else, so rows 6, 17, 18 all land on a page
  that cannot show them).
- **Page runs a different implementation of the same idea (2).** Row 9 (Regime
  detection → `strategies/regime.hpp`) lands on page 17, which runs
  `analytics/hmm.hpp` plus a k-means blocker, not the `TrendRegime` classifier
  (`strategies/regime.hpp:80-129`); no `MarketRegime`/`classify` call appears in
  `regime_report()` (`desktop/quant_pages.hpp:740-862`). Row 25 (Markov regime
  chain → `models/markov.hpp`) also lands on 17, whose only fit is the HMM
  (`fit_hmm`, `:765-766`); the Markov fit is drawn on page 3 through
  `desktop/data/fits.hpp:12,43-56` (`MarkovFit`), which `desktop/panels.hpp`
  includes.
- **Page is a UI re-implementation, not the module (2).** Rows 38 and 39 point at
  page 12, whose own header says the opposite of the row's promise:
  "`oms/execution.hpp` and `oms/capacity.hpp` are NOT included, even though P19-01
  is the execution page -- it renders a SCHEDULE SHAPE computed from arithmetic the
  UI owns, not from the order-placing module" (`desktop/quant_pages.hpp:24-29`).
  `execution_report()` then hard-codes `total = 1000, n = 7` and the VWAP curve
  `30 / 12 / 8 / 8 / 14 / 28` (`:425-441`) and prints Almgren-Chriss prose
  (`:450-462`). `oms/execution.hpp` and `oms/shortfall.hpp` have **no** non-test
  includer at all, so no page can be running them today.
- **Page exists but was recorded as -1 (6).** Rows 27, 31, 48, 73, 74, 75 all say
  no page runs them; page 7 does. `desktop/analytics_panel.hpp:1-9` names
  "Monte Carlo, z-score and the rolling moments" and lists `backtest/montecarlo.hpp`
  with "GBM, Heston, Merton jump-diffusion, IID and block bootstrap, plus the
  performance … drawdowns"; `:169-171` calls `iid_bootstrap` and reads
  `max_drawdown`. This is an *under*-claim, the safe direction, but it hides six
  working models from the reader.
- **Page covers part of a bundled row (3).** Row 44 ("three ways"): page 14 calls
  `historical_var` and `parametric_var` (`:563-564`) but never `monte_carlo_var`
  (0 hits in any `desktop/*.hpp`), although all three exist in `risk/var.hpp:127,159,196`
  and are tested (`risk/tests/test_var.cpp:58-60`). Row 53: page 21 tables
  `delta` and `vega` only (`:1485-1490`). Row 38's VWAP/POV: `pov_step` exists and
  is tested (`oms/tests/test_execution.cpp:39`) but no page shows POV.
- **Page is indirect (4).** Rows 13-16 point at page 29, which does not link the
  neural headers; `neural_report()` shells out to the `altair_neural_5m_test`
  binary and renders its stdout (`desktop/quant_pages.hpp:2949-3023`). The models
  do get exercised, so the page is defensible — but the claim "press it and watch
  the model answer" is a subprocess, not an in-process call. I count these as
  page-OK-with-note, not as broken.

### 4.3 Verified claims that the Atlas states well (no action)

Worth recording because the card's premise is that optimistic labels are the risk:
rows 13, 14, 15, 16's Partial texts are each verifiable in the header
(`models/mlp.hpp:92-99` frozen hidden layer + ridge readout; `models/recurrent.hpp:12-14`
"seeded initialisation … solve it by ridge regression … echo-state / reservoir
computing"; `models/attention.hpp:1-40` masked single-head attention with 0 hits for
`multi_head`, `positional` and `layer_norm`; the same file is a 1-D dilated causal
convolution with 0 hits for `conv2d`/`pool`). Row 62's "trinomial lattice is still
open" is true: `trinomial` has 0 hits in every source directory — its only
occurrence in the tree is that Atlas row. Row 31's "no Heston option pricer" is
true: `backtest/montecarlo.hpp:175-187` is the generator and no pricer takes
`HestonParams`. Rows 41/42/43's absences hold, and row 41's cause is written in the
source it was deferred from (`book/microstructure.hpp:5`, "queue position needs live
order state (P2-09c, really Phase 4)").

## 5. Broken page targets

31 rows carry a page-value defect, in four kinds. Definitions, then the lists with
the evidence for each.

**A. The page does not run that model (20 rows).** Claimed → verified.

| Row | Model | Claimed | Verified | Why |
|---|---|---|---|---|
| 5 | Statistical arbitrage | 19 | **-1** | page 19 shows momentum + z-score mean reversion (`quant_pages.hpp:871,914-929`); no `strategies/score.hpp` `combine()` call in 863-1145 |
| 6 | Factor models (PCA) | 16 | **-1** | `ml_report()` calls `fit_gbdt` only (`:694,706`); 0 pca hits |
| 9 | Regime detection | 17 | **-1** | page 17 fits the HMM + a k-means blocker (`:765`); `strategies/regime.hpp`'s `MarketRegime` is never called |
| 17 | Random forest | 16 | **-1** | `ml_report()` is GBDT only (`:694,706`) |
| 18 | Logistic regression | 16 | **-1** | same page |
| 20 | AR | 18 | **-1** | page 18 is `models/stream_forecast.hpp` + `live_forecast.hpp`; `models/horizon_eval.hpp` has **no** non-test includer |
| 22 | VAR | 18 | **-1** | page 18; no `fit_var1` call in `quant_pages.hpp` |
| 23 | Kalman | 7 | **-1** | page 7 is the options Analytics panel; `quant_pages.hpp:43` is an `#include` only, 0 calls |
| 25 | Markov regime chain | 17 | **3** | page 17 fits the HMM only; the Markov fit is drawn by `ModelPanel` (page 3) via `desktop/data/fits.hpp:12,43-56` |
| 32 | Hurst exponent | 17 | **27** | page 27 is "MEMORY — Hurst and EWMA" (`:2640-2652`, `hurst_rs`); `regime_report()` has 0 hurst hits |
| 37 | Slippage & market impact | 12 | **-1** | `risk/slippage.hpp` is included at `:60` and never called (the other 2 mentions are prose) |
| 40 | Implementation shortfall | 12 | **-1** | `oms/shortfall.hpp` has no non-test includer |
| 46 | Stress testing | 14 | **-1** | `risk/stress.hpp` included at `:62`, never called; `risk_report()` computes VaR+ES only (`:549-594`) |
| 49 | Beta / sector neutralisation | 15 | **-1** | 0 mentions in `desktop/` outside the Atlas row; `risk/neutralise.hpp` has no non-test includer |
| 58 | Local volatility (Dupire) | 21 | **-1** | 0 `dupire`/`local_vol` hits in `quant_pages.hpp`; page 21 does call SABR (`:1549`) |
| 61 | India VIX replication | 13 | **-1** | page 13 is GARCH vs EWMA; the 7 `vix` hits in `quant_pages.hpp` are the INDIA VIX *instrument label* (`:229-262`); `analytics/vix.hpp` has no non-test includer |
| 62 | Binomial / trinomial | 21 | **-1** | `analytics/binomial.hpp` has no non-test includer; 0 binomial/crr/lattice hits in `quant_pages.hpp` |
| 69 | Black-Litterman | 15 | **-1** | 0 `black_litterman` hits in `desktop/`; `risk/stress.hpp` included at `:62`, never called |
| 70 | Mean-variance optimisation | 15 | **-1** | `portfolio_report()` calls `ledoit_wolf` (`:626`), `min_variance` (`:630`), `risk_parity` (`:631`) — not `mean_variance` |
| 76 | Walk-forward & purged CV | 16 | **26** | `ml_report()` has 0 fold/walk/purge lines; the section "WALK-FORWARD, PURGE AND EMBARGO" is in `cointegration_report()` (`:2560-2583`, with `walk_forward_count`/`walk_forward_fold` calls) |

**B. The page runs only part of a bundled row (5 rows).** Row 26 (OU, page 26:
no `half_life`/pair-candidate call — the `half_life` at `:496` is GARCH persistence
on page 13); row 38 (POV absent from page 12, and the page is a UI demo — see A/§4.2);
row 39 (Almgren-Chriss appears as printed prose at `:450-462`, not a computed
schedule); row 44 (2 of 3 VaR methods on page 14); row 53 (`delta`/`vega` tabled at
`:1485-1490`; `gamma`, `theta`, `rho` are computed by `analytics/greeks.hpp` but not
shown).

**C. Page exists but is recorded as -1 (6 rows).** Rows 27 (GBM), 31 (Heston), 48
(drawdown), 73 (Monte Carlo), 74 (bootstrapping), 75 (jump diffusion) → all **7**,
per `desktop/analytics_panel.hpp:1-9,25-40,155-171`. Index 7 is
`options.analytics` "Analytics" (`main_window.hpp:1043`).

**D. Indirect but working (4 rows, not counted as defects).** Rows 13-16 → page 29,
which runs the neural test binary (`quant_pages.hpp:2949-3023`) rather than linking
the headers.

**Not broken, checked and cleared:** the 20 rows that use -1 for genuinely
page-less work are honest (e.g. row 60 `backtest/mc_pricing.hpp` has no non-test
includer; row 7 `strategies/fundamentals.hpp` has none either). The other 34
non-negative values point at a page that does call the named header — including
the whole options block (rows 52, 53, 55, 56, 57, 59 → 21: `black76` `:1442`,
`implied_vol_black76` `:1453`, `delta`/`vega` table `:1485-1490`, SVI `:1506-1508`,
SABR `:1549`, American `:1575`), the microstructure block (33-36 → 24), sizing
(64-66 → 25: `kelly_divisor` `:2306`, `target_vol` `:2307`), VaR/ES (44/45 → 14),
Ledoit-Wolf (47 → 15: `ledoit_wolf` `:626`), and the neural block (13-16 → 29, kind
D above).

## 6. The reconciliation table — all 85 rows, four properties never collapsed

Codes. **C** claimed / **V** verified: `impl` `part` `absent`. **File**: `y` = exists
and the whole named model is in it; `y*` = exists but implements only part of a
bundled claim (rows 6, 10, 11, 15); `-` = no file. **Pg**: `ok`; `A` page
does not run it; `B` page covers only part; `C` page exists but row says -1 (with
the correction after `→`); `none` = -1 is honest; `D` indirect (runs the test
binary). **Live**: `engine` = a non-UI module includes/calls it, `ui` = desktop
page includes and calls it, `ui*` = desktop includes it but never calls it, `none` =
only tests. **A** algorithm: `y`/`part`/`n`. **T** training data: `real` (the
dataset on disk is the input), `synth` (tests build the series), `real+synth`,
`n/a` (nothing to fit), `-` (absent). **E** evaluation: `test`, `test+oos` (an
out-of-sample or walk-forward check exists), `none`. **L** live readiness: same
vocabulary as **Live** — this is the column §18 asks to keep separate from
"compiles and is tested".

Dataset reality behind the **T** column (`dataset/`, re-derived): `spot/nifty/1d`
8,768 lines = 8,767 daily bars plus header; `spot/nifty/5m` 215,302 lines over 140
files; `spot/nifty/1m` 1,081,107 lines over 141 files (**no tick data anywhere**);
`spot/nifty/15m` 71,871; `fut/nifty/1d` 2,876; `spot/banknifty/1d` 6,629 over 2
files; `spot/indiavix/1d` 2,901. Tests that read it are named `*_real` or say so
(`models/tests/test_neural_5m.cpp:3,12` — "TRAIN THE NEURAL TIER ON THE REAL
5-MINUTE SERIES … 211,492 five-minute returns").

| # | Fam | Model | C | V | File | Test | Pg | Live | A | T | E | L |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | Mean reversion | impl | impl | y | test_directional.cpp | ok | ui | y | synth+real | test | ui |
| 2 | 1 | Momentum / trend | impl | impl | y | test_directional.cpp | ok | ui | y | synth+real | test | ui |
| 3 | 1 | Cointegration | impl | impl | y | test_cointegration.cpp | ok | ui | y | synth+real | test | none |
| 4 | 1 | Pairs trading | part | part | y | test_cointegration.cpp | B | ui | part | synth | test | none |
| 5 | 1 | Statistical arbitrage | part | part | y | test_score.cpp | A | engine | part | synth | test | none |
| 6 | 1 | Factor models (FF, PCA) | part | part | y* | test_classical.cpp:68 | A | none | part | synth | test | none |
| 7 | 1 | Relative value | impl | impl | y | test_fundamentals.cpp | none | none | y | synth | test | none |
| 8 | 1 | Spread / calendar | impl | impl | y | test_calendar.cpp | ok | ui | y | synth | test | none |
| 9 | 1 | Regime detection | impl | impl | y | test_regime.cpp | A | engine | y | synth+real | test | none |
| 10 | 1 | Linear / ridge | impl | **part** | y* | test_cointegration.cpp | ok | ui | part | synth+real | test | none |
| 11 | 2 | GBDT / LightGBM | impl | impl | y* | test_gbdt.cpp | ok | engine | y | real | test+oos | engine |
| 12 | 2 | Decision tree | impl | impl | y | test_gbdt.cpp | ok | engine | y | real | test+oos | engine |
| 13 | 2 | MLP / feedforward | part | part | y | test_recurrent.cpp, test_neural_5m.cpp | D | none | part | real+synth | test | none |
| 14 | 2 | LSTM / GRU | part | part | y | test_recurrent.cpp | D | none | part | real+synth | test | none |
| 15 | 2 | Transformer / attention | part | part | y* | test_attention.cpp | D | none | part | real+synth | test | none |
| 16 | 2 | CNN | part | part | y | test_attention.cpp | D | none | part | real+synth | test | none |
| 17 | 2 | Random forest | impl | impl | y | test_classical.cpp:47 | A | none | y | synth | test | none |
| 18 | 2 | Logistic regression | impl | impl | y | test_classical.cpp:27 | A | none | y | synth | test | none |
| 19 | 2 | SVM / KNN / autoencoder | absent | absent | - | - | none | none | n | - | - | none |
| 20 | 3 | AR (autoregression) | impl | impl | y | test_horizon_eval.cpp | A | none | y | real+synth | test+oos | none |
| 21 | 3 | ARMA / ARIMA / SARIMA | absent | absent | - | - | none | none | n | - | - | none |
| 22 | 3 | VAR | impl | impl | y | test_classical.cpp | A | none | y | synth | test | none |
| 23 | 3 | Kalman filter | impl | impl | y | test_kalman.cpp | A | ui* | y | synth | test | none |
| 24 | 3 | Hidden Markov model | impl | impl | y | test_surface_hmm.cpp | ok | ui | y | real+synth | test | none |
| 25 | 3 | Markov regime chain | impl | impl | y | test_markov.cpp | A→3 | engine | y | real | test | ui |
| 26 | 3 | Ornstein-Uhlenbeck | part | part | y | test_cointegration.cpp | B | ui | part | synth | test | none |
| 27 | 3 | Brownian motion / GBM | impl | impl | y | test_montecarlo.cpp | C→7 | engine | y | synth | test | none |
| 28 | 3 | Historical vol & EWMA | impl | impl | y | test_ewma.cpp | ok | ui | y | real+synth | test | ui |
| 29 | 3 | GARCH / GJR-GARCH | impl | impl | y | test_garch.cpp | ok | ui | y | real+synth | test | ui |
| 30 | 3 | EGARCH | absent | absent | - | - | none | none | n | - | - | none |
| 31 | 3 | Heston stochastic vol | part | part | y | test_montecarlo.cpp:56 | C→7 | engine | part | synth | test | none |
| 32 | 3 | Hurst exponent | impl | impl | y | test_hurst.cpp | A→27 | engine | y | real+synth | test | engine |
| 33 | 4 | Order-book imbalance | impl | impl | y | test_microstructure.cpp | ok | engine | y | synth | test | engine |
| 34 | 4 | Microprice | impl | impl | y | test_microstructure.cpp | ok | engine | y | synth | test | engine |
| 35 | 4 | VPIN | impl | impl | y | test_flow.cpp | ok | engine | y | synth | test | engine |
| 36 | 4 | Kyle's lambda | impl | impl | y | test_flow.cpp | ok | engine | y | synth | test | engine |

| 37 | 4 | Slippage & market impact | impl | impl | y | test_slippage.cpp | A | ui* | y | synth | test | none |
| 38 | 4 | TWAP / VWAP / POV | impl | impl | y | test_execution.cpp | B | none | y | synth | test | none |
| 39 | 4 | Almgren-Chriss | impl | impl | y | test_execution.cpp | B | none | y | synth | test | none |
| 40 | 4 | Implementation shortfall | impl | impl | y | test_execution.cpp | A | none | y | synth | test | none |
| 41 | 4 | Queue position | absent | absent | - | - | none | none | n | - | - | none |
| 42 | 4 | Fill probability | absent | absent | - | - | none | none | n | - | - | none |
| 43 | 4 | Trade arrival (Hawkes) | absent | absent | - | - | none | none | n | - | - | none |
| 44 | 5 | VaR — 3 ways | impl | impl | y | test_var.cpp:58 | B | ui | y | real+synth | test | ui |
| 45 | 5 | Expected shortfall (CVaR) | impl | impl | y | test_var.cpp | ok | ui | y | real+synth | test | ui |
| 46 | 5 | Stress testing | impl | impl | y | test_stress.cpp | A | ui* | y | real | test | none |
| 47 | 5 | Correlation & covariance | impl | impl | y | test_optimise.cpp:7 | ok | engine | y | real+synth | test | engine |
| 48 | 5 | Drawdown | impl | impl | y | test_montecarlo.cpp:53 | C→7 | engine | y | synth | test | none |
| 49 | 5 | Beta / sector neutralisation | impl | impl | y | test_hedge_book.cpp | A | none | y | synth | test | none |
| 50 | 5 | Factor risk model | absent | absent | - | - | none | none | n | - | - | none |
| 51 | 5 | Conformal risk control | impl | impl | y | test_conformal.cpp | ok | engine | y | real+synth | test | ui |
| 52 | 6 | Black-Scholes / Black-76 | impl | impl | y | test_greeks.cpp | ok | engine | y | n/a | test | engine |
| 53 | 6 | Greeks — d, g, v, th, r | impl | impl | y | test_greeks.cpp | B | engine | y | n/a | test | engine |
| 54 | 6 | Higher greeks — vanna, volga, charm | impl | impl | y | test_greeks2.cpp | ok | engine | y | n/a | test | engine |
| 55 | 6 | Implied volatility | impl | impl | y | test_iv.cpp | ok | engine | y | n/a | test | engine |
| 56 | 6 | SVI volatility smile | impl | impl | y | test_svi_fit.cpp | ok | ui | y | real | test | ui |
| 57 | 6 | SABR | impl | impl | y | test_surface_hmm.cpp:65 | ok | ui | y | real | test | ui |
| 58 | 6 | Local volatility (Dupire) | impl | impl | y | test_surface_hmm.cpp:66 | A | ui | y | real | test | ui |
| 59 | 6 | American approximation | impl | impl | y | test_american.cpp | ok | ui | y | n/a | test | ui |
| 60 | 6 | Monte Carlo option pricing | impl | impl | y | test_mc_pricing.cpp | none | none | y | synth | test | none |
| 61 | 6 | India VIX replication | impl | impl | y | test_vix.cpp | A | none | y | n/a | test | none |
| 62 | 6 | Binomial / trinomial tree | part | part | y* | test_binomial.cpp | A | none | part | n/a | test | none |
| 63 | 6 | Finite-difference PDE pricer | absent | absent | - | - | none | none | n | - | - | none |

| 64 | 7 | Kelly criterion | impl | impl | y | test_sizing.cpp | ok | engine | y | n/a | test | engine |
| 65 | 7 | Fixed frac | impl | impl | y | test_sizing.cpp | ok | engine | y | n/a | test | engine |
| 66 | 7 | Risk parity sizing | impl | impl | y | test_sizing.cpp | ok | engine | y | synth | test | engine |
| 67 | 7 | Target volatility sizing | impl | impl | y* | test_target_vol.cpp | ok | engine | y | real+synth | test+oos | engine |
| 68 | 7 | Target drawdown sizing | part | part | y | test_drawdown.cpp | B | engine | part | synth | test | none |
| 69 | 7 | Minimum variance portfolio | impl | impl | y | test_optimise.cpp | A | none | y | synth | test | none |
| 70 | 7 | Max Sharpe ratio portfolio | impl | impl | y | test_optimise.cpp | A | engine | y | synth | test | engine |
| 71 | 7 | Equal risk contribution | impl | impl | y | test_optimise.cpp | ok | engine | y | synth | test | engine |
| 72 | 7 | Mean‑var optimisation | impl | impl | y* | test_classical.cpp:71 | ok | engine | y | synth | test | ui |
| 73 | 8 | Backtest engine | impl | impl | y | test_backtest.cpp | D | none | y | real+synth | test+oos | none |
| 74 | 8 | Market simulator | impl | impl | y | test_market_sim.cpp | D | none | y | synth | test | none |
| 75 | 8 | Walk‑forward analysis | impl | impl | y | test_walkforward.cpp | none | none | y | real | test+oos | none |
| 76 | 8 | Cross‑validation (k‑fold) | impl | impl | y | test_stratify.cpp | A | none | y | synth | test | none |
| 77 | 8 | Bootstrap resampling | impl | impl | y* | test_bootstrap.cpp | none | none | y | real | test | none |
| 78 | 8 | Out‑of‑sample testing | impl | impl | y* | test_gbdt.cpp:201 | none | none | y | real | test+oos | none |
| 79 | 9 | Q‑learning | impl | impl | y* | test_regime_rl.cpp | none | none | part | synth | test | none |
| 80 | 9 | PPO (proximal policy optimisation) | absent | absent | - | - | none | none | n | - | - | none |
| 81 | 9 | DQN (deep Q‑network) | absent | absent | - | - | none | none | n | - | - | none |
| 82 | 9 | Multi‑agent | impl | impl | y* | test_multi_agent.cpp | none | none | y | synth | test | none |
| 83 | 9 | Exploration‑exploitation | impl | impl | y* | test_regime_rl.cpp | none | none | y | synth | test | none |
| 84 | 9 | Reward shaping | impl | impl | y* | test_regime_rl.cpp | none | none | y | synth | test | none |
| 85 | 9 | Counterfactual regret minimization | impl | impl | y | test_cfr.cpp | none | none | y | synth | test | none |
**AUDIT COMPLETE.** Correction §6.1 and the final sections §7–§11 follow
immediately below, appended 2026-09-24 under correction card `P6-01c1`.



### 6.1 Correction (appended 2026-09-24 under P6-01c1) — §6's last 22 rows are not Atlas rows

Found while appending §7–§11, and recorded rather than quietly rewritten. Rows
64–85 of the §6 table below name models that `desktop/atlas_data.hpp` does not
contain, and omit the 22 it does. Verified by exact-string search over the file
(mtime 2026-09-22 10:03, unchanged since this audit): `Fixed frac`,
`Max Sharpe`, `Equal risk`, `Backtest engine`, `Market simulator`,
`Multi-agent`, `Reward shaping` and `Counterfactual` each occur **0** times, and
none of the 22 rows in the table below appears anywhere in §6. §2's counts are
unaffected — 63/10/12 re-derived from the file still holds — but acceptance 1
("every Atlas row appears in the reconciliation table") **fails** for rows
64–85. Note also that **§5's row numbers for those rows are the real ones**
(69 is Black-Litterman, 73 Monte Carlo, 74 bootstrapping, 75 jump diffusion),
so §5 and §6 disagree with each other. §9 uses §5's numbering, which is the
file's.

The 22 real rows, re-parsed 2026-09-24 from `AtlasRow{...}` records 64–85, in
file order:

| # | Model | Status | Page | File |
|---|---|---|---|---|
| 64 | Position sizing | Impl | 25 | risk/sizing.hpp |
| 65 | Kelly criterion | Impl | 25 | risk/sizing.hpp |
| 66 | Volatility targeting | Impl | 25 | risk/sizing.hpp |
| 67 | Risk parity | Impl | 15 | risk/optimise.hpp |
| 68 | Minimum variance | Impl | 15 | risk/optimise.hpp |
| 69 | Black-Litterman | Impl | 15 | risk/stress.hpp |
| 70 | Mean-variance optimisation | Impl | 15 | risk/optimise.hpp |
| 71 | News / NLP / sentiment | Absent | -1 | — |
| 72 | Event models | Absent | -1 | — |
| 73 | Monte Carlo | Impl | -1 | backtest/montecarlo.hpp |
| 74 | Bootstrapping | Impl | -1 | backtest/montecarlo.hpp |
| 75 | Jump diffusion | Impl | -1 | backtest/montecarlo.hpp |
| 76 | Walk-forward & purged CV | Impl | 16 | backtest/validation.hpp |
| 77 | Agent-based simulation | Absent | -1 | — |
| 78 | Q-learning | Impl | 17 | models/regime_rl.hpp |
| 79 | DQN / PPO / actor-critic | Absent | -1 | — |
| 80 | Transaction cost calculator | Impl | 6 | risk/cost.hpp |
| 81 | Model scorecards & drift | Impl | 23 | flagging/drift.hpp |
| 82 | Forecast scorecard | Impl | 18 | models/forecast_scorecard.hpp |
| 83 | Ensemble aggregator | Impl | 10 | models/aggregator.hpp |
| 84 | Feature registry | Impl | 30 | features/registry.hpp |
| 85 | DCF valuation | Impl | 9 | models/dcf.hpp |

## 7. Stable model IDs

**Rule.** An ID is `family.model` — two lowercase ASCII slugs, the first from the
family's own name, the second from the row's own label. An ID never encodes a row
number and never a page index, because those are exactly the two things that
moved: P4-01 folded five of thirty-five indices out of the visible set, and this
audit found 31 wrong page targets. Rewording a label in case or punctuation must
not change its ID; a row split under §8 gives each half its own suffix.
Prefixes: `alpha` · `ml` · `ts` · `micro` · `risk` · `opt` · `port` · `altdata` ·
`sim` · `rl` · `x`.

| # → ID | # → ID | # → ID | # → ID | # → ID |
|---|---|---|---|---|
| 1 `alpha.mean_reversion` | 2 `alpha.momentum` | 3 `alpha.cointegration` | 4 `alpha.pairs` | 5 `alpha.stat_arb` |
| 6 `alpha.factor_models` | 7 `alpha.relative_value` | 8 `alpha.calendar_spread` | 9 `alpha.regime` | 10 `ml.linear` |
| 11 `ml.gbdt` | 12 `ml.decision_tree` | 13 `ml.mlp` | 14 `ml.rnn` | 15 `ml.attention` |
| 16 `ml.cnn` | 17 `ml.random_forest` | 18 `ml.logistic` | 19 `ml.svm_knn_autoencoder` | 20 `ts.ar` |
| 21 `ts.arima` | 22 `ts.var` | 23 `ts.kalman` | 24 `ts.hmm` | 25 `ts.markov_chain` |
| 26 `ts.ornstein_uhlenbeck` | 27 `ts.gbm` | 28 `ts.ewma` | 29 `ts.garch` | 30 `ts.egarch` |
| 31 `ts.heston` | 32 `ts.hurst` | 33 `micro.imbalance` | 34 `micro.microprice` | 35 `micro.vpin` |
| 36 `micro.kyle_lambda` | 37 `micro.slippage` | 38 `micro.twap_vwap_pov` | 39 `micro.almgren_chriss` | 40 `micro.shortfall` |
| 41 `micro.queue_position` | 42 `micro.fill_probability` | 43 `micro.hawkes` | 44 `risk.var` | 45 `risk.expected_shortfall` |
| 46 `risk.stress` | 47 `risk.covariance` | 48 `risk.drawdown` | 49 `risk.neutralisation` | 50 `risk.factor_model` |
| 51 `risk.conformal` | 52 `opt.black_scholes` | 53 `opt.greeks` | 54 `opt.higher_greeks` | 55 `opt.implied_vol` |
| 56 `opt.svi` | 57 `opt.sabr` | 58 `opt.local_vol` | 59 `opt.american` | 60 `opt.mc_pricing` |
| 61 `opt.vix` | 62 `opt.lattice` | 63 `opt.finite_difference` | 64 `port.position_sizing` | 65 `port.kelly` |
| 66 `port.vol_target` | 67 `port.risk_parity` | 68 `port.min_variance` | 69 `port.black_litterman` | 70 `port.mean_variance` |
| 71 `altdata.news_nlp` | 72 `altdata.event_models` | 73 `sim.monte_carlo` | 74 `sim.bootstrap` | 75 `sim.jump_diffusion` |
| 76 `sim.walk_forward` | 77 `sim.agent_based` | 78 `rl.q_learning` | 79 `rl.ppo_dqn_actor_critic` | 80 `x.transaction_cost` |
| 81 `x.drift` | 82 `x.forecast_scorecard` | 83 `x.aggregator` | 84 `x.feature_registry` | 85 `x.dcf` |

**85 IDs, 85 distinct** — derived by sorting the 85 strings and counting unique
values, not asserted. The count is what makes the scheme sufficient: the only two
labels here that could have collided are `Risk parity` (67, `port.risk_parity`)
and a hypothetical `Risk parity sizing`, which is not a row in this file.

## 8. Bundled-label split (for P6-05)

14 of the 85 labels name more than one model. Each becomes the entries below —
its own ID from §7, its own status, its own file. This is a **proposal**: it
changes the shape of `atlas_data.hpp`, which this card must not touch.

| Row | Label | Becomes |
|---|---|---|
| 6 | Factor models (Fama-French, PCA) | `alpha.factor_ff` Partial `models/classical.hpp`; `alpha.factor_pca` Implemented `models/classical.hpp` |
| 10 | Linear / ridge regression | `ml.linear` Implemented `strategies/cointegration.hpp` (OLS: `Regression` :96, `regression()` :104); `ml.ridge` Implemented `models/mlp.hpp` (:92-99, :192) |
| 11 | Gradient boosting (GBDT / LightGBM) | `ml.gbdt` Implemented `models/gbdt.hpp`; `ml.lightgbm` **Absent** — the booster here is this engine's own, 0 `lightgbm` hits |
| 14 | LSTM / GRU | `ml.lstm` Partial `models/recurrent.hpp`; `ml.gru` Partial `models/recurrent.hpp` |
| 19 | SVM / KNN / autoencoder | three Absent entries, no file: `ml.svm`, `ml.knn`, `ml.autoencoder` |
| 21 | ARMA / ARIMA / SARIMA | three Absent entries, no file: `ts.arma`, `ts.arima`, `ts.sarima` |
| 29 | GARCH / GJR-GARCH | `ts.garch` Implemented `analytics/garch.hpp`; `ts.gjr_garch` Implemented `analytics/garch.hpp` — the asymmetry is in the same header |
| 38 | TWAP / VWAP / POV | `micro.twap`, `micro.vwap` Implemented `oms/execution.hpp`; `micro.pov` Implemented `oms/execution.hpp` (`pov_step`) but **no page** — see §9 |
| 44 | VaR — historical, parametric, Monte Carlo | `risk.var_historical` Impl `risk/var.hpp:127`; `risk.var_parametric` Impl `risk/var.hpp:159`; `risk.var_monte_carlo` Impl `risk/var.hpp:196` |
| 52 | Black-Scholes / Black-76 | `opt.black_scholes` Implemented `analytics/greeks.hpp`; `opt.black76` Implemented `analytics/greeks.hpp` |
| 53 | Greeks — delta, gamma, vega, theta, rho | `opt.delta`, `opt.gamma`, `opt.vega`, `opt.theta`, `opt.rho` — five entries, all Implemented `analytics/greeks.hpp` |
| 54 | Higher greeks — vanna, volga, charm | `opt.vanna`, `opt.volga`, `opt.charm` — three entries, all Implemented `analytics/greeks2.hpp` |
| 62 | Binomial / trinomial tree | `opt.binomial` Partial `analytics/binomial.hpp` (CRR prices European and American); `opt.trinomial` **Absent** — 0 `trinomial` hits in the tree outside this label |
| 79 | DQN / PPO / actor-critic | three Absent entries, no file: `rl.dqn`, `rl.ppo`, `rl.actor_critic` |

**Examined and NOT split — a slash that names one model twice.** Recorded,
because "names more than one model" is a judgement and a silent pass is how a
bundle gets missed: 8 `Spread / calendar trading` (one instrument pair) · 13 `MLP
/ feedforward network` · 15 `Transformer / attention` · 16 `CNN` (one name) · 22
`VAR (vector autoregression)` (an expansion of the abbreviation) · 27 `Brownian
motion / GBM` (one process) · 45 `Expected shortfall (CVaR)` (one measure, two
names) · 47 `Correlation & covariance (Ledoit-Wolf)` (the estimator is
Ledoit-Wolf) · 64 `Position sizing` (the methods are rows 65–66) · 76
`Walk-forward & purged CV` (one validation procedure) · 81 `Model scorecards &
drift` (one page, one job).

## 9. Corrections list — ready to apply, not applied

`row #` below is the **file's** row number (`atlas_data.hpp` order, §6.1), which
is also the numbering §5 uses. Every row is one line of the required form
`row # | field | old value | new value | reason | evidence (file:line)`.
Nothing here has been applied: this card's manifest is this one document.

### 9.1 The one status/file correction (D-1)

| row # | field | old value | new value | reason | evidence (file:line) |
|---|---|---|---|---|---|
| 10 | status | Implemented | **Partial** | the straight-line OLS is here and tested; the ridge-penalised solve is not in this file | `ridge`/`Ridge` = **0 hits** in `strategies/cointegration.hpp`; ridge solve at `models/mlp.hpp:92-99,192` and `models/recurrent.hpp:249-255` |
| 10 | file | strategies/cointegration.hpp | keep for OLS, **models/mlp.hpp** for the ridge half (§8's `ml.linear` / `ml.ridge`) | a row's file must hold the model it names | as above |
| 10 | test | test_cointegration.cpp | test_cointegration.cpp (OLS) **+** `models/tests/test_classical.cpp:68` / `test_neural_5m.cpp` (ridge readout) | a test's existence is not proof it exercises the model | `strategies/tests/test_cointegration.cpp` (37 hits, all OLS) |

### 9.2 Page corrections — the 31 rows of §5

**Group A — the page exists and visible, but does not run that model (20 rows).
New value `-1` for all of them**, with the reason each:

| row # | field | old | new | reason | evidence (file:line) |
|---|---|---|---|---|---|
| 5 | page | 19 | **-1** | page 19 shows momentum + z-score mean reversion; `strategies/score.hpp`'s `combine()` is never called | `quant_pages.hpp:871,914-929` |
| 6 | page | 16 | **-1** | `ml_report()` calls `fit_gbdt` only, and there are 0 `pca` hits | `quant_pages.hpp:694,706` |
| 9 | page | 17 | **-1** | page 17 fits the HMM plus a k-means blocker; `strategies/regime.hpp`'s `MarketRegime` is never called | `quant_pages.hpp:765`; `strategies/regime.hpp:80-129` |
| 17 | page | 16 | **-1** | `ml_report()` is GBDT only, so a forest claim lands on a page that cannot show it | `quant_pages.hpp:694,706` |
| 18 | page | 16 | **-1** | same page, same reason | `quant_pages.hpp:694,706` |
| 20 | page | 18 | **-1** | page 18 is `models/stream_forecast.hpp` + `live_forecast.hpp`; `models/horizon_eval.hpp` has **no** non-test includer | `quant_pages.hpp` (0 hits for horizon_eval) |
| 22 | page | 18 | **-1** | no `fit_var1` call anywhere in `quant_pages.hpp` | `quant_pages.hpp` (0 hits) |
| 23 | page | 7 | **-1** | page 7 is the options Analytics panel; `quant_pages.hpp:43` is an `#include` only, 0 calls | `quant_pages.hpp:43` |
| 25 | page | 17 | **3** | page 17 fits the HMM only; the Markov fit is drawn by `ModelPanel` on page 3 | `desktop/data/fits.hpp:12,43-56`, included by `desktop/panels.hpp` |
| 32 | page | 17 | **27** | page 27 is "MEMORY — Hurst and EWMA"; `regime_report()` has 0 `hurst` hits | `quant_pages.hpp:2640-2652` |
| 37 | page | 12 | **-1** | `risk/slippage.hpp` is included at `:60` and never called (the other two mentions are prose) | `quant_pages.hpp:60` |
| 40 | page | 12 | **-1** | `oms/shortfall.hpp` has no non-test includer at all | include-graph parse; `oms/tests/test_execution.cpp` only |
| 46 | page | 14 | **-1** | `risk/stress.hpp` included at `:62`, never called; `risk_report()` computes VaR + ES only | `quant_pages.hpp:62,549-594` |
| 49 | page | 15 | **-1** | 0 mentions in `desktop/` outside this Atlas row; `risk/neutralise.hpp` has no non-test includer | `desktop/` search; include-graph parse |
| 58 | page | 21 | **-1** | 0 `dupire`/`local_vol` hits; page 21 does call SABR, which is a different model | `quant_pages.hpp:1549` |
| 61 | page | 13 | **-1** | page 13 is GARCH vs EWMA; the 7 `vix` hits are the INDIA VIX *instrument label*; `analytics/vix.hpp` has no non-test includer | `quant_pages.hpp:229-262` |
| 62 | page | 21 | **-1** | `analytics/binomial.hpp` has no non-test includer; 0 binomial/crr/lattice hits in `quant_pages.hpp` | include-graph parse |
| 69 | page | 15 | **-1** | 0 `black_litterman` hits in `desktop/`; `risk/stress.hpp` included at `:62`, never called | `quant_pages.hpp:62` |
| 70 | page | 15 | **-1** | `portfolio_report()` calls `ledoit_wolf`, `min_variance` and `risk_parity` — not `mean_variance` | `quant_pages.hpp:626,630,631` |
| 76 | page | 16 | **26** | `ml_report()` has 0 fold/walk/purge lines; the "WALK-FORWARD, PURGE AND EMBARGO" section is in `cointegration_report()` | `quant_pages.hpp:2560-2583` |

**Group B — the page covers only part of a bundled row (5 rows).** Two of these
resolve through §8's split rather than a single new integer.

| row # | field | old | new | reason | evidence (file:line) |
|---|---|---|---|---|---|
| 26 | page | 26 | **-1** | no `half_life`/pair-candidate call runs an OU fit; the `half_life` at `:496` is GARCH persistence on page 13 | `quant_pages.hpp:496` |
| 38 | page | 12 | **-1** for all three of `micro.twap`/`micro.vwap`/`micro.pov` | page 12 renders a SCHEDULE SHAPE from arithmetic the UI owns and does not include `oms/execution.hpp`; `pov_step` has no page anywhere | `quant_pages.hpp:24-29,425-441`; `oms/tests/test_execution.cpp:39` |
| 39 | page | 12 | **-1** | Almgren-Chriss appears as printed prose, not a computed schedule | `quant_pages.hpp:450-462` |
| 44 | page | 14 | **14** for `risk.var_historical` and `risk.var_parametric`; **-1** for `risk.var_monte_carlo` | page 14 calls two of the three; `monte_carlo_var` has 0 hits in any `desktop/*.hpp` | `quant_pages.hpp:563-564`; `risk/var.hpp:127,159,196`; `risk/tests/test_var.cpp:58-60` |
| 53 | page | 21 | **21** for `opt.delta` and `opt.vega`; **-1** for `opt.gamma`, `opt.theta`, `opt.rho` | page 21 tables delta and vega only, though all five are computed | `quant_pages.hpp:1485-1490`; `analytics/greeks.hpp` |

**Group C — the page exists but the row said `-1` (6 rows). New value `7` for all
six**: page 7 is the Analytics panel, which runs them.

| row # | field | old | new | reason | evidence (file:line) |
|---|---|---|---|---|---|
| 27 | page | -1 | **7** | the panel names Monte Carlo and generates GBM paths | `desktop/analytics_panel.hpp:1-9,25-40` |
| 31 | page | -1 | **7** | the panel lists Heston among the generators it runs | `desktop/analytics_panel.hpp:1-9` |
| 48 | page | -1 | **7** | the panel reads `max_drawdown` | `desktop/analytics_panel.hpp:169-171` |
| 73 | page | -1 | **7** | IID and block bootstrap are both called there | `desktop/analytics_panel.hpp:155-171` |
| 74 | page | -1 | **7** | same call site as 73 | `desktop/analytics_panel.hpp:169-171` |
| 75 | page | -1 | **7** | the panel lists Merton jump-diffusion among its generators | `desktop/analytics_panel.hpp:1-9` |

An under-claim is the safe direction, but it hides six working models from the
reader — which is the whole value of the page column.

### 9.3 The stale header comment

| row # | field | old value | new value | reason | evidence (file:line) |
|---|---|---|---|---|---|
| — | header comment | `atlas_data.hpp:80` "the current 33-page layout (0..32)" | "35 pages (0..34), 30 visible; `nav_visible()` is false for {0,1,5,28,33}" | the registry holds 35 stable IDs and folds five of them | `desktop/navigation_registry.hpp:19-53,60-71` |

### 9.4 Recommendations for OTHER cards — NOT Atlas corrections

1. **`desktop/main_window.hpp:1411-1424`** — the page-range guard aborts only on
   `r.page < -1 || r.page >= nav_page_names().size()`, so a *folded* index
   passes and `show_page()` (`:196-210`) silently redirects it with a
   status-bar note. Latent, because no row currently uses one, but the guard is
   weaker than the danger it guards. A `desktop/` card should refuse folded
   indices at the guard, not at the redirect.
2. **P6-02 must land §7's IDs before these integers matter.** Until the `page`
   column holds an ID, every `-1` above is a value in a column that P4-01 has
   already shown can be renumbered underneath it.

## 10. What could not be determined

1. **Whether any arithmetic is correct.** Every `Implemented` judgement is "the
   named model is in this file and a test exercises it". Not one implied vol was
   compared with a known answer and no GARCH fit was re-estimated independently.
   Numerical accuracy is a different audit and is not claimed here.
2. **Dynamically-reached live callers.** The include graph is the evidence for the
   `Live` column. A header reached through a function pointer, a virtual
   dispatch, or a string lookup would not appear in it. I did not find one; I
   cannot rule one out.
3. **The absence rows rest on search, not on proof.** Rows 19, 21, 30, 41, 42,
   43, 50, 63, 71, 72, 77 and 79 each say "I did not find it". A model living
   under an implementation with different vocabulary would be missed — row 20 is
   the standing example of that trap, and it is why the wording is not "it is
   not there".
4. **Rows 13–16 → page 29 is a subprocess.** `neural_report()` shells out to the
   `altair_neural_5m_test` binary and renders its stdout rather than linking the
   neural headers. Counted page-OK-with-note, not broken. Whether "press it and
   watch the model answer" survives being a subprocess is a product judgement.
5. **§6's last 22 rows could not be reconciled with the file** (§6.1). §9 uses
   §5's numbering, which does match `atlas_data.hpp`; the §6 table's numbering
   for rows 64–85 does not.
6. **The nav can move again.** These are proposed IDs, not an implementation.
   Until P6-02 replaces the integer `page` with an ID, every corrected value
   above is a value in a column that P4-01 already demonstrated can be
   renumbered underneath it.

## 11. Compliance

`desktop/atlas_data.hpp` was **read, never written** by this card: its mtime is
2026-09-22 10:03 and it did not change during this work. `git diff --stat` does
report it as modified against `HEAD` (19 insertions, 19 deletions) — that is a
**pre-existing working-tree edit**, present before this audit began and
untouched here. No C++, CMake or config file was modified; only this one
document changed. §1–§6 and Appendix A are unchanged, with two deliberate
exceptions: §2's count was corrected to 49 distinct named files as the
correction card instructed, and §6.1 was **appended** to record that §6's rows
64–85 are not Atlas rows. §6.1 adds evidence; it does not rewrite the table it
corrects, so the defect stays visible where it was made.

The document now runs past the ~650-line guide in the correction card. §9 was
kept complete as instructed and §7 is already a compact table; the extra ~50
lines are §6.1, which acceptance 1 cannot be reported honestly without.

---

## Appendix A — the dispatched card text, verbatim



# P6-01 — reconcile the Atlas against actual code and call paths

## 0. Ownership and locks — read first

You are **one of three agents working this repo concurrently.** Your manifest is
**one file**, and it is a document. You write no C++ at all.

| Agent | Owns (do not touch) |
|---|---|
| A | `core/types/broker_positions.hpp`, `core/types/tests/test_broker_positions.cpp` |
| B | `prompts/P4-04_GETS_TABLE_SPEC.md` |
| **C — you** | **`prompts/P6-01_ATLAS_RECONCILIATION.md`** |

Everything outside your one file is **read-only to you** — including
`desktop/atlas_data.hpp`, which you are auditing and **must not correct**. Your
output is the list of corrections; applying them is a later card with its own
owner. Do not build, do not run `ctest`, do not edit CMake.

## 1. Context

`final.md` §18 requirement 13 wants every Atlas entry to reach an exact,
navigable model detail page, with algorithm, training, evaluation and live
readiness **distinguished**. P6-02 through P6-05 are all blocked on knowing
what is actually there.

`desktop/atlas_data.hpp` already claims to be that inventory. It holds ~85 rows
of `{family, model, status, file, page}` with `AtlasStatus ∈ {Implemented,
Partial, Absent}` — currently **63 Implemented, 10 Partial, 12 Absent** — and
its header comment states the absences were "verified by search before being
written down".

**That claim is the thing you are testing.** §18 P6-01 is explicit: every row
needs evidence, and *labels alone are not proof*. A catalogue that is wrong in
the optimistic direction is worse than no catalogue, because every consumer
downstream trusts it.

## 2. File manifest

| File | Action |
|---|---|
| `prompts/P6-01_ATLAS_RECONCILIATION.md` | **create** |

Nothing else. Corrections to `atlas_data.hpp` are **listed in your document**,
never applied.

## 3. What to check, per row

For each of the ~85 rows in `desktop/atlas_data.hpp`:

1. **Does `file` exist?** Repo-relative path, on disk, right now.
2. **Does the named model actually live there?** Open it. A row naming
   `models/foo.hpp` for "GARCH" is wrong if that header has no GARCH.
3. **Is `status` honest?** This is the core of the card:
   - **Implemented** requires the named algorithm present *and* a test that
     exercises it. Find the test and name it. No test → not Implemented.
   - **Partial** must say *what is missing*. The existing header comment gives
     two good examples — a frozen-weight LSTM reservoir is not a trained LSTM;
     single-head causal attention is not a Transformer. Check each Partial row
     states its gap, and add the gap where it does not.
   - **Absent** must be verified by search now, not inherited. Confirm it.
4. **Does `page` resolve?** `page` is an integer into the navigation registry.
   **This is where a real defect is most likely.** `desktop/navigation_registry.hpp`
   now holds 35 stable page IDs of which only 30 are visible: `nav_destination()`
   folds 0 and 1 into 2 (Terminal) and 5, 28 and 33 into 34 (Brokers), and
   `nav_visible()` is false for those five. §18 P4-01 explicitly warned that
   Atlas integer targets had to be updated or replaced with stable IDs.
   **Check every non-negative `page` value against `nav_visible()`**, and report
   any row pointing at a folded or out-of-range index.
5. **Is there a live caller?** Does anything outside tests construct or invoke
   it — a strategy, a panel, the aggregator? "Compiles and is tested" and "is
   used by the engine" are different facts and §18 P6-01 wants them separate.

## 4. Separate the four properties §18 demands

For every row, report these as **four independent columns**, never collapsed:

| Property | Question |
|---|---|
| **Algorithm** | is the maths implemented in this tree? |
| **Training** | can it be fitted, and on what data — real, synthetic, or none? |
| **Evaluation** | is there an out-of-sample, cost-aware or statistical check? |
| **Live readiness** | does anything on the live path call it? |

A row can be Implemented on algorithm and nothing else. That is the common case
and the reason the four columns exist.

**Training data is the sharpest question here.** `final.md` §12 Phase 8 records
that eleven of thirteen models were validated on **synthetic data only**, that
`dataset/` holds 8,756 daily NIFTY bars and no tick data at all, and that only
NIFTY spot and India VIX have real history. Check each row against that, and
mark any model whose "training" has only ever seen generated data.

## 5. What the document must contain

1. **Method** — exactly what you read and how you decided each status. One
   short section, so a later reader can re-run your judgement.
2. **The reconciliation table**, one row per Atlas row:

   | # | Family | Model | Claimed | Verified | File exists | Test | Page OK | Live caller | Data |
   |---|---|---|---|---|---|---|---|---|---|

   `Claimed` is what `atlas_data.hpp` says; `Verified` is what you found.
3. **Disagreements**, called out separately and first — every row where
   `Claimed ≠ Verified`, with the evidence. If there are none, say so plainly;
   that is a real and valuable result.
4. **Broken page targets** — every row whose `page` is folded, out of range, or
   points at a page that does not run that model.
5. **Stable model IDs.** §18 P6-01 requires every row to have one. Propose a
   scheme (a short stable string per row — not an integer index, which is what
   broke) and assign one to every row. Do not implement it.
6. **The bundled-label split.** §18 P6-05 says bundled Atlas labels must become
   individual specifications. Identify every row naming more than one model
   ("ARMA, ARIMA, SARIMA", "DQN, PPO, actor-critic") and list the separate
   entries it should become.
7. **The corrections list** — precise, per-row, ready for a later card to apply:
   old value → new value, with the reason. **You do not apply them.**
8. **What you could not determine**, explicitly.

## 6. Constraints

- **Read the disk. Do not trust `atlas_data.hpp`'s own comments**, this card's
  summary of them, or the counts quoted in §1 — re-derive the counts yourself
  and report them, including if they differ from 63/10/12.
- Cite a file and line for every `Verified` judgement.
- A test's existence is not proof it exercises the model. Open it.
- Do not run models, do not train anything, do not start the desktop app.
- Do not propose new models. This card inventories what is here.
- `Quants/`, `RXT_trade/`, `RXT_trade 2/` are **reference only** — nothing in
  them counts as an Altair implementation, no matter how complete it looks.
- Keep the disagreements and corrections sections complete even if the table
  must be abbreviated for length. Aim under ~500 lines; if the full table cannot
  fit, keep every disagreeing row in full and compress only the agreeing ones.

## 7. Acceptance

1. Every Atlas row appears in the reconciliation table with all four properties.
2. Row counts by status are re-derived and stated, and any difference from
   63/10/12 is called out.
3. Every `page` value is checked against `nav_visible()` and the result reported.
4. Every `Implemented` row either names its test or is downgraded with a reason.
5. Every `Partial` row states what is missing.
6. Every row has a proposed stable model ID.
7. Every bundled label is listed with its proposed split.
8. `atlas_data.hpp` is **unmodified** — state this explicitly at the end.

## 8. Deliverable format

The complete file. No elisions.

```
RULES — violating any of these fails review:
1. Produce the complete document. No "...", no "rest unchanged".
2. Do not create, rename, or delete any file not in the File Manifest.
3. Do not modify any C++, CMake or config file — including atlas_data.hpp.
4. Read the actual files; a claim inherited from a comment is not evidence.
5. Cite file and line for every verified judgement.
6. "I did not find it" and "it is not there" are different claims. Use the one
   you can support.
7. Report counts you derived yourself, even where they contradict this card.
8. If a requirement is ambiguous, record both readings and say which you used.
```
