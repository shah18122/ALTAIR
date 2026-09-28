# G16 desktop_quant_pages — CX-01 findings

Reader: claude-subagent:G16_desktop_quant_pages · Date 2026-09-14 · Baseline HEAD a34af5c5c9fa8c7498e8cc4df00907548a41e1a5

## 1 Scope & coverage

All 13 assigned files are fully read (8,089 lines), in chunks of 300 lines or fewer. Every sha256 matched the
inventory before and after reading. Nothing was built or run.

| File | Lines | Read |
|---|---|---|
| desktop/quant_pages.hpp | 4258 | 1-4258 (15 chunks) |
| desktop/tests/test_pages.cpp | 583 | 1-583 |
| desktop/chart/chart_widget.hpp | 484 | 1-484 |
| desktop/data/fits.hpp | 390 | 1-390 |
| desktop/tests/test_chart.cpp | 379 | 1-379 |
| desktop/model_status.hpp | 373 | 1-373 |
| desktop/cost_panel.hpp | 349 | 1-349 |
| desktop/chart/candles.hpp | 306 | 1-306 |
| desktop/analytics_panel.hpp | 235 | 1-235 |
| desktop/tests/test_fits.cpp | 211 | 1-211 |
| desktop/tests/test_overnight_page.cpp | 207 | 1-207 |
| desktop/tests/test_options_page.cpp | 181 | 1-181 |
| desktop/data/series_io.hpp | 133 | 1-133 |

I also ran context-only greps, not reviews, on these files outside the group: main_window.hpp (callers,
`processEvents`, `setEnabled`), panels.hpp (callers), backtest/montecarlo.hpp (`metrics` works in log space),
live_forecast.hpp and atlas_data.hpp (verdict strings), and kill_switch.hpp and order_ticket.hpp (the only
desktop code that writes files).

## 2 Findings table

| ID | Sev | Class | Title |
|---|---|---|---|
| C16-001 | P1 | CONFIRMED DEFECT | Heavy report generation, including a child process that can block for up to 125 s, runs synchronously on the GUI thread with no worker or cancellation |
| C16-002 | P2 | CONFIRMED DEFECT | AnalyticsPanel runs 2,000 bootstrap passes over the full daily series inside its constructor, at window build |
| C16-003 | P2 | CONFIRMED DEFECT | The Cointegration page always uses the NIFTY future but switches the spot to the selected instrument, so both the heading and the verdict are wrong |
| C16-004 | P2 | DESIGN GAP | The Markov row is a hard-coded green "trained on real data" while its own walk-forward computes no directional edge |
| C16-005 | P3 | CONFIRMED DEFECT | Several pages still say "NIFTY" when BANKNIFTY or INDIA VIX is selected |
| C16-006 | P3 | CONFIRMED DEFECT | Verdict sentences are fixed text and can contradict the numbers printed above them |
| C16-007 | P3 | CONFIRMED DEFECT (rule 11) | Every UI CSV loader silently drops bad rows; no count, and a missing file looks the same as an empty one |
| C16-008 | P3 | HYPOTHESIS | Date-only stamps probably parse at the host's local midnight, which would make the IST branch in `ui_load_stamped` dead code |
| C16-009 | P3 | CONFIRMED DEFECT | Status text on screen is stale or self-contradictory ("1 of 12 trained", dated "has" literals, a LibTorch claim) |
| C16-010 | P3 | CONFIRMED DEFECT | fits.hpp promises an `in_sample` flag that does not exist; the VIX fit is fully in-sample and joins observations across dropped values |
| C16-011 | P3 | CONFIRMED DEFECT | A missing value is shown as a measured zero in several places |
| C16-012 | P3 | CONFIRMED DEFECT | The kinematics section reads a hard-coded "2026-09.csv" and assumes 60 s spacing across session gaps |
| C16-013 | P3 | CONFIRMED DEFECT | Lot-size and cost literals on demo/research pages; one comment falsely says the lot comes from the spec store |
| C16-014 | P3 | CONFIRMED DEFECT | The Flagging scorecard sets regime boundaries from the whole sample while its comment calls them causal |
| C16-015 | P3 | CONFIRMED DEFECT | Cost panel: the "09:15" trade is 09:15 UTC, and the price box takes whole rupees only |
| C16-016 | P3 | HYPOTHESIS | Candle width is based on candle count but x position on time, so candles overlap on series with session gaps |
| C16-017 | P3 | DESIGN GAP | The dataset inventory is described as "counted from disk, not typed" but walks a typed list of partitions |

## 3 Finding details

### C16-001 — P1 CONFIRMED DEFECT — synchronous heavy work on the GUI thread
- **Evidence (the page functions, all in desktop/quant_pages.hpp):**
  - `neural_report` starts a child process and waits on it: 2987 `if (!proc.waitForStarted(5000)) {` and
    2990 `if (!proc.waitForFinished(120000)) {`. It can block for up to 125 s; the page itself says about 6 s.
  - `aggregator_report` runs `forecast_spot` three times, five folds each, on the daily series (4005-4020).
  - `strategies_report` and `overnight_report` parse about 214k five-minute rows with ifstream (968, 1163),
    and strategies also runs 12 sweeps.
  - `regime_report` fits an HMM with restarts on the full daily series and runs k-means for k = 2..5 (764-826).
  - `volatility_report` fits GARCH and GJR (487-488).
  - `ml_report` fits two GBDTs of 3,000 rows × 150 trees (693, 705).
- **Callers:** main_window.hpp button lambdas at 1079-1083, 1104-1108, 1117-1121, 1130-1134, 1171,
  1192-1196, 1208-1212 and 1314-1318. Each one calls `set_text("Fitting...")`, then
  `QApplication::processEvents()`, then the report, synchronously. The on-screen text admits
  "The window is busy until it finishes" (main_window 1168, 1316).
- **No worker, no cancellation:** grep of desktop/*.hpp finds no QtConcurrent, QThread, moveToThread,
  std::thread, std::async or QFutureWatcher. The only limit anywhere is the 120 s process timeout. The buttons
  are not disabled while running; the only `setEnabled(false)` calls are at main_window 517 and 834, which are
  unrelated. Because `processEvents()` runs inside the click handler, a click already queued can re-enter the
  handler. That re-entrancy is a hypothesis; the rest of this finding is confirmed.
- **Trigger:** press Neural, Aggregator, Strategies, Overnight or Regimes while a stream or replay is running.
- **Impact:** the process that holds positions (the in-process decision in CLAUDE.md) stops painting,
  draining stream updates and running timers, and the kill-switch request UI cannot be clicked, for seconds to
  minutes. `neural_report` also runs whatever `altair_neural_5m_test` executable it finds by probing paths
  (2966-2976, including `applicationDirPath()/../models/`) and shows that test's stdout as the model result.
- **Tests:** test_pages.cpp calls every report but never times one.
- **Reproducer:** from a QTimer firing every 10 ms, record the largest gap between ticks while
  `aggregator_report(ds, "/spot/nifty/1d/", "x")` runs on the GUI thread. Assert it is under 100 ms; this
  fails today. The fix is QtConcurrent plus a QFutureWatcher, with the button disabled while running.

### C16-002 — P2 CONFIRMED DEFECT — Monte Carlo in the AnalyticsPanel constructor
- **Evidence:** analytics_panel.hpp:87 `run();` inside the constructor. `run()` loads spot/nifty/1d/all.csv
  (92-94) and loops `kPaths = 1000` times over iid_bootstrap + metrics + block_bootstrap + metrics, with
  n ≈ 8.7k (161-183). main_window.hpp:1004 constructs the panel eagerly.
- **Trigger:** every application start, even if nobody opens the page.
- **Impact:** startup blocks on about 17.5M resampled returns. Duration not measured.
- **Tests:** none.
- **Fix/test:** run on first show or on a worker; measure construction time in a test.

### C16-003 — P2 CONFIRMED DEFECT — cointegration pairs the wrong series
- **Evidence:** quant_pages.hpp:2454-2455 always load `/fut/nifty/1d/all.csv`, while 2456-2457 load
  `spot_path(dataset_root, sym, "1d")`. The heading at 2481 still reads "NIFTY FUTURE AGAINST NIFTY SPOT", and
  a negative result at 2509-2511 prints "which for this pair would mean the implementation is wrong".
- **Trigger:** select NIFTY BANK or INDIA VIX on the Cointegration page and press run.
- **Impact:** the page tests the NIFTY future against another index. A correct "not cointegrated" is reported
  as a broken implementation, and a spurious "cointegrated" is reported "as arbitrage requires".
- **Tests:** test_pages.cpp:162 only calls the default (nifty).
- **Test:** `cointegration_report(ds, "banknifty")` must either refuse or name BANKNIFTY in the heading.

### C16-004 — P2 DESIGN GAP — Markov row shown as a usable signal
- **Evidence:** model_status.hpp:129 hard-codes `ModelState::TrainedOnRealData` (green) for Markov. The file's
  own definition at 60-66 says that state "paints green and reads as a working signal". fits.hpp:259-260
  computes `no_directional_edge`, and the comment at 176-177 says that on this data the model "does not" have
  one. The India VIX row (259) is also green, from an in-sample fit only.
- **Impact:** the Models page shows green, meaning trained and working, for a model whose out-of-sample
  answer is no edge. Nothing ties the colour to the computed verdict.
- **Test:** derive the state from the walk-forward result; assert Markov is `TrainedNoEdge` when
  `no_directional_edge` is true.

### C16-005 — P3 CONFIRMED DEFECT — NIFTY-only labels
- **Evidence:** all in quant_pages.hpp.
  - 557 `"%1 daily NIFTY returns"` (risk page)
  - 775 `"real NIFTY (%1)"` (regime page)
  - 942 `"――― 3. REAL DAILY NIFTY ―――"` (strategies page)
  - 3097 "On one-minute NIFTY closes" (features page)
  - 4198 "One NIFTY lot of 75" on the aggregator page, although it takes `label`/`sub` parameters
- **Trigger:** select BANKNIFTY or INDIA VIX. The banner names the right instrument; the body text does not.
- **Tests:** test_pages 4j checks the banner on the volatility page only.

### C16-006 — P3 CONFIRMED DEFECT — fixed verdicts that ignore the computed numbers
- **Evidence:**
  - analytics_panel.hpp:218 always prints "IID bootstrap is out by %3 points, and it UNDERSTATES", and 221
    passes the difference through `std::fabs`, so the sign is lost.
  - quant_pages.hpp:576-580 always says the normal VaR "overstates at 95% and understates at 99%", whatever the
    computed ratio is (584 also has the typo "SlippageShortfall").
  - 1373-1374 always says "The gross edge is real and significant", whatever t is.
  - 4149-4152 always says "there is an edge" at rho 0 and "there is not" at rho 0.95.
  - 4240-4244 always says one member "clears the threshold six simultaneous tests require", although
    `over_bonf` is computed just above.
  - 2084 says "Twenty cells were examined" beside the computed `cells_total`.
  - 723-727 quotes meta-labelling numbers as fixed text; 655-658 quotes "218x" and "+400%/-350%".
- **Impact:** these pages claim "nothing is a literal", but on different data these sentences contradict the
  table above them.
- **Test:** feed the page a series where the computed quantity has the opposite sign, and assert the verdict
  text changes.

### C16-007 — P3 CONFIRMED DEFECT (rule 11) — silent row drops in UI loaders
- **Evidence:**
  - series_io.hpp:49 returns empty on open failure; rows are dropped without a count at 56, 59 and 75.
  - quant_pages.hpp `ui_load_closes` 127-145, `ui_load_partitioned` 161-191, `ui_load_dated_closes` 195-215 and
    `ui_load_sessions` 1093-1133 all use `std::atof`, which accepts "12abc" as 12, and `continue` past bad rows
    without counting them.
  - `ui_load_closes` has no date column, so file order is never checked.
- **Impact:** a truncated or corrupted row is dropped without a trace, and a missing file looks like an empty
  one. basis_report (1752-1755) is the one place that counts and shows its unmatched sessions.
- **Tests:** none for drop counts.

### C16-008 — P3 HYPOTHESIS — date-only stamps are host-local
- **Evidence:** series_io.hpp:62 first tries `QDateTime::fromString(stamp, Qt::ISODate)`. The IST
  `setTimeZone` at 71-73 only runs on the fallback path, when that first parse fails.
- **Reasoning (not verified; no Qt source on this box):** Qt's ISODate parser accepts `yyyy-MM-dd` and returns
  the start of that day in LOCAL time. If so, the fallback never runs for daily files.
- **Trigger:** on a host with a UTC offset greater than +05:30, `ist_day` files each daily bar under the
  previous day, and `prev_close_before` (terminal.hpp:434) picks the wrong reference close. On IST or UTC hosts
  the day is right, but the stored instant differs by host.
- **Tests:** test_terminal builds its stamps in code and never exercises this parse.
- **Reproducer:** load a one-row CSV `2026-09-08,1,1,1,100` and assert
  `stamps_ns[0] == 2026-09-07T18:30:00Z`.

### C16-009 — P3 CONFIRMED DEFECT — stale or contradictory status text
- **Evidence:**
  - model_status.hpp:340 `"1 of 12 trained on real data (Markov, P8-13); the "`. The catalogue has 13 rows:
    2 TrainedOnRealData and 4 TrainedNoEdge.
  - 344 says the aggregator "needs two or more members", contradicting the aggregator row at 248-255.
  - Header comments at 14-15 and 28-29 are stale in the same way.
  - The "has" column holds dated literals: 145 "8,755 ... to 2026-08-31" and 262 "527 daily bars, 2024-07-18
    to 2026-08-31". fits.hpp:14 says "~3,000 India VIX bars" for the same series. dataset/ was not opened, so I
    cannot say which is right.
  - quant_pages.hpp:2717-2720 says the neural models are "LibTorch training" that belongs "not a button";
    neural_report at 2931-2936 in the same file says they are hand-rolled and runs them from a button.

### C16-010 — P3 CONFIRMED DEFECT — in-sample flag promised but absent
- **Evidence:**
  - fits.hpp:36 says "`FitResult::in_sample` is true on everything this module produces"; no such type or
    field exists anywhere in desktop/.
  - The VIX AR(1) is fitted on the full series (316-317), its regime thresholds are full-series quantiles
    (327-332), and coverage is scored with that same in-sample model (351-384).
  - 306-309 drop non-positive values and then treat the survivors as consecutive AR(1) steps across the gap,
    without counting what was dropped.
  - `walk_forward_markov` has no desktop test; test_fits.cpp never calls it.

### C16-011 — P3 CONFIRMED DEFECT — a missing value shown as zero
- **Evidence:**
  - fits.hpp:115-125: if the boundaries fail, `relabelled` stays 0 and prints as "0 (0.0%)".
  - analytics_panel.hpp:117, 119-121 and 197 fall back to 0.0 when mean, sd or ACTUAL is missing. At 168-183 a
    failed path is skipped, yet the mean still divides by 1,000.
  - quant_pages.hpp:1926-1932 and 1943-1949: the Page-Hinkley/ADWIN `first == 0` sentinel, so an alarm at
    observation 0 prints the same as no alarm.
  - 3136: vel/se prints 0.00 when se is 0.
  - 1216: `(on_m - fb_m) / on_m` has no zero guard.
  - 497: when `gamma_se` is missing, t is 0, which prints "not distinguishable from zero".

### C16-012 — P3 CONFIRMED DEFECT — kinematics on a literal month with invented spacing
- **Evidence:** quant_pages.hpp:3103 reads `spot_path(dataset_root, sym, "1m", "2026-09.csv")`; 3112 sets
  `tp.ts = Timestamp{static_cast<std::int64_t>(i) * 60'000'000'000LL};`.
- **Impact:** from October the page keeps analysing September and calls it "recent" (3105). Overnight and
  weekend gaps count as one minute, so the 30- and 60-minute windows differentiate straight across session
  boundaries.

### C16-013 — P3 CONFIRMED DEFECT — lot, freeze and cost literals on pages
- **Evidence:**
  - quant_pages.hpp:3613 `const Qty lot{75};        // the spec store's NIFTY lot` — the value is typed
    here, not read from the spec store.
  - 2298-2299 `LotSize{75}` and `Qty{1800}`, while the on-screen text at 2363-2365 says the freeze quantity
    "comes from the spec store, never a literal".
  - 4177 and 4198 use a lot of 75; 4008 `cost_bps = 5.5`; 865 `kCost = 5.5`.
  - cost_panel.hpp:105 defaults qty to 75.
- **Severity reason:** none of these are wired to orders; UI cannot trade. Still rule 1 / rule 5 hygiene.

### C16-014 — P3 CONFIRMED DEFECT — look-ahead in regime boundaries
- **Evidence:** quant_pages.hpp:1983 says "Regimes, computed CAUSALLY", but 1998-2001 set the volatility
  tercile boundaries from the whole sample (`sorted_v`). The per-regime IC table therefore labels early bars
  using boundaries learned from later data. The page does say "backtest scorecard" (1974).

### C16-015 — P3 CONFIRMED DEFECT — cost panel details
- **Evidence:** cost_panel.hpp:176-181 labels the trade "A trade at 09:15", but the value is days × 86400e9 plus
  09:15, which is 09:15 UTC (14:45 IST). It is harmless for the two fixed dates used, but mislabelled.
  111-114: the price is an integer-rupee QSpinBox, so a Rs 50.25 premium cannot be entered.
- Separately, the charges loader blocker text (165-172) is honest: when the loader is absent, the panel shows
  nothing and says why.

### C16-016 — P3 HYPOTHESIS — overlapping candles on gapped series
- **Evidence:** chart_widget.hpp:318 `slot = span / n` sizes candles by count, while 330 and 335 place them at
  `to_x(c.start_ns)`, by time. With overnight gaps, the time span (179) is far wider than n bars, so
  `body = slot*0.68` (319) is wider than the real spacing between bars.
- **Trigger:** 60m or 1m bars covering several sessions, if main_window feeds those. main_window.hpp:894
  belongs to another group and was not read.
- **Related:** `build_candles` (candles.hpp:121) assumes ticks arrive in time order. An out-of-order tick opens
  a duplicate bucket, and `check_conservation` still agrees, so it cannot catch that.

### C16-017 — P3 DESIGN GAP — "counted from disk" inventory uses a typed list
- **Evidence:** quant_pages.hpp:3156-3160 say "COUNTED FROM DISK, NOT TYPED", but 3269-3273 hard-code five
  segment/symbol pairs and five intervals. A new partition (for example fut/banknifty) never appears.

## 4 Lead revalidation

- **Codex UI-001 — CONFIRMED on the page side, and worse than stated.**
  - Every quant report runs synchronously inside a button lambda (main_window 1018-1339).
  - There is no worker thread and no cancellation anywhere in desktop/*.hpp.
  - `neural_report` blocks in `waitForFinished(120000)`.
  - AnalyticsPanel does its Monte Carlo at construction (C16-002).
  - `processEvents()` is called before each heavy call; that permits re-entrancy (hypothesis) and does nothing
    to keep the window responsive during the work.
  - Heavy functions and their data sizes are listed under C16-001. Reports that open without pressing a button
    (execution, options, DCF, sizing, microstructure, arbitrage) are pure arithmetic and light.
- **Codex UI-004 — PARTIALLY CONFIRMED.**
  - float32 handling is correct: `to_x`/`to_y` subtract in int64 before converting (candles.hpp:249-260), and
    test_chart [1] and [2] measure it.
  - Full rebuild:
    - `set_candles` (chart_widget 85-92) replaces the whole vector and resets zoom.
    - `recompute_view` (166-182) copies the visible slice on every pan and drag event.
    - `mouseMoveEvent` calls `update()` on every move.
    - The paint draws every visible candle (8,755 when fully zoomed out) and runs the O(n) `snap_crosshair`.
  - How often main_window:824 rebuilds from the tape was not verified; that file belongs to another group.
  - Candle bucketing is half-open and uses floor division, with a conservation check (C16-016 notes the
    out-of-order blind spot).
- **Group leads:**
  - series_io: see C16-007 and C16-008. `prev_close_before` is keyed correctly on the tick's IST day, provided
    the stamps themselves are right.
  - fits: see C16-004 and C16-010.
  - model_status honesty: see C16-004 and C16-009.
  - analytics_panel: see C16-002, C16-006 and C16-011.
  - cost_panel: the blocker text is honest; see C16-015.

## 5 Architecture facts

- **Threads.** Every page in this group runs on the Qt GUI thread. No file here spawns a thread.
  `neural_report` is the only one that spawns a process.
- **Engine headers used (read-side only).** quant_pages.hpp includes analytics/ (american, ewma, hurst,
  garch, greeks, greeks2, iv, svi, svi_fit, hmm, kalman, sabr, derivatives), book/ (flow, microstructure),
  features/registry, flagging/ (drift, scorecard), models/ (dcf, gbdt, regime_rl, aggregator,
  forecast_scorecard, spot_forecast), risk/ (covariance, limits, sizing, slippage, optimise, stress, var,
  charges_toml when that loader is compiled in), strategies/ (parity, calendar, basis, regime, cointegration,
  meanrev, overnight, momentum) and backtest/validation. There is no `oms/` include.
  - fits.hpp includes models/markov, markov_eval and strategies/vix_forecast.
  - analytics_panel.hpp includes analytics/rolling and backtest/montecarlo.
  - cost_panel.hpp includes risk/cost and, when available, charges_toml.
- **dataset/ paths the code reads** (I did not open any of them):
  - spot/{nifty,banknifty,indiavix}/1d/all.csv
  - spot/{sym}/5m/*.csv (monthly partitions)
  - spot/{sym}/1m/2026-09.csv
  - fut/nifty/1d/all.csv
  - the inventory walk over spot/{nifty,banknifty,indiavix}, fut/nifty and opt/nifty, at 1m, 5m, 15m, 60m
    and 1d
  - via tests: spot/indiavix/1d/all.csv and spot/nifty/5m
- **Where each page's data comes from:**
  - Real market data: volatility, risk, regimes, strategies, overnight, basis, flagging, cointegration, memory,
    features (kinematics), aggregator, AnalyticsPanel.
  - Synthetic or stated inputs: execution, portfolio (not labelled synthetic on screen), ML trees demo,
    options, microstructure, sizing, DCF, arbitrage scans. Apart from portfolio, these pages say so on screen.
  - Test-binary output: neural.

## 6 TRAINING STATUS as shown in the UI

**Nothing is persisted.** No file in this group writes a model, and across desktop/*.hpp the only file writes
are in kill_switch.hpp:236 and order_ticket.hpp:141. Every fit is recomputed in memory on each press and
thrown away. model_status.hpp:224 says "no .pt or .onnx anywhere in the tree", but that is a fixed string, not
a check.

| Model | Fitted at runtime by the desktop? | Data | What the UI says |
|---|---|---|---|
| Markov regime chain | Yes: fits.hpp `fit_markov` and `walk_forward_markov`, called from panels.hpp:449/559 | real NIFTY daily (test uses spot/nifty/1d/all.csv) | Catalogue: fixed green "trained on real data". Fit pane: in-sample chi² rejects independence, shuffled control does not. Walk-forward computes `no_directional_edge` (C16-004) |
| India VIX AR(1), level and log | Yes: `fit_vix_both_spaces`, panels.hpp:618 | real India VIX daily | Catalogue: green. The fit is in-sample only (C16-010) |
| GARCH(1,1) / GJR vs EWMA | Yes: volatility_report | real daily for the selected instrument | Fitted on the first half, scored on the second (QLIKE). Prints "EWMA WINS" or "GARCH wins" from the computed comparison |
| 2-state HMM and k-means | Yes: regime_report | real daily, full sample, in-sample | Separation compared with a white-noise baseline; clusters described as return-blind |
| GBDT spot forecast (6 members) | Yes: aggregator_report, 3 × `forecast_spot`, 5-fold walk-forward | real daily | Computed t per member and Bonferroni count, correlation swept; closing verdict is fixed text (C16-006). Catalogue: "trained — NO EDGE FOUND" |
| GBDT feature-importance demo | Yes: ml_report | synthetic 3,000 rows | Meta-labelling "did NOT replicate" is fixed text |
| MLP / GRU (neural tier) | Runs the `altair_neural_5m_test` binary on each press | 211k real five-minute returns (per page text) | Shows the test's stdout verbatim; catalogue "trained — NO EDGE FOUND" |
| Momentum / mean reversion | Rule evaluation, not training | real daily | "SKELETON ... Nothing here is trained"; t and excess t shown |
| Overnight gap | Rule evaluation | real 5-minute data | Page says in-sample, no walk-forward; "gross edge is real and significant" is fixed text |
| Engle-Granger | Test run | real fut/nifty against the selected spot, last 2,000 sessions | Wrong pairing for non-NIFTY selections (C16-003) |
| Drift detectors (PSI, KS, Page-Hinkley, ADWIN) and scorecard | Computed | real daily | "backtest scorecard, not a production one" |
| SVI, Black-76/IV, American, microstructure, DCF, sizing, portfolio optimisers | Computed from stated or synthetic inputs | none | Say "not a market measurement" or "typed", except portfolio |
| Attention, registry, ONNX serving, aggregator row, DCF row, cointegration row | No | — | Fixed catalogue states: never trained or blocked on data |

- **"No better than naive" verdict.** The spot forecast's direction verdict ("no better than always
  predicting ...") lives in live_forecast.hpp:626-644, which belongs to another group and which I did not
  review. No file in this group prints "NO BETTER THAN NAIVE".
- **Catalogue counts** (fixed, not derived from any fit): 13 rows — 2 trained on real data, 4 trained with no
  edge, 1 synthetic only, 4 never trained, 2 blocked on data. The pipeline text beside it still says "1 of 12"
  (C16-009).

## 7 Test-suite observations

- test_pages, test_fits and test_overnight_page depend on the gitignored dataset/, so whether they pass
  depends on the machine. test_pages [4g] rightly builds its own temporary directory instead.
- test_pages checks that pages render and contain key phrases. It does not check timing, the cointegration page
  with a non-NIFTY instrument, per-instrument labels beyond the volatility banner, or whether verdict sentences
  match the numbers.
- test_fits covers `fit_markov` and `fit_vix_both_spaces`, but `walk_forward_markov` — the out-of-sample answer
  the Train button shows — has no desktop test.
- test_chart covers float32, buckets, conservation, crosshair and zoom well. It has no test for gapped series
  or out-of-order ticks.
- test_overnight_page is the strongest here: an independent reader compared column by column, an exact
  identity check, and the page's headline figure matched against the strategy layer. Its reference reader uses
  `std::stod`, which throws on a malformed cell, so bad data would crash the test rather than fail a check.
- test_options_page derives its tolerances from vega instead of choosing them — good.
- No tests at all for AnalyticsPanel, CostPanel, model_status, `ui_load_stamped` on date-only stamps, or
  loader drop counts.

## 8 Open questions

1. Does Qt 6.8 `QDateTime::fromString("2026-09-08", Qt::ISODate)` return a valid local-time value? This decides
   C16-008.
2. panels.hpp, another group: does it show "in-sample" and `no_directional_edge` next to the green Markov and
   VIX rows? It also calls `processEvents` at 436.
3. main_window.hpp:824: is `set_candles` called on every replay tick or timer tick? That is UI-004 rebuild
   frequency.
4. How many India VIX daily bars are there: 527 (model_status) or about 3,000 (fits.hpp comment)?
5. Is a `ChargeSchedule::valid_from` IST midnight or UTC midnight? It affects the effective date printed at
   quant_pages 3609-3612.
