# CX-01 — idea.txt and Model Atlas readiness assessment

Reader: claude-lead · Date: 2026-09-14 · Baseline HEAD `a34af5c5c9fa8c7498e8cc4df00907548a41e1a5`
Inputs: `idea.txt` (559 lines, read in full); `desktop/atlas_data.hpp` (460 lines, read in full by the lead for this assessment, and separately covered by reviewer G18); LEDGER; reviewer findings as they land.

**The Model Atlas is not changed by this assessment** (user instruction, 2026-09-14). The Atlas describes what code exists. This document adds two columns it does not carry: **integration** (is the thing wired into anything that runs) and **live readiness** (could it inform a real decision).

## 0. Constraints applied to every decision below

1. Mandatory pre-trade risk checks run synchronously **before** order submission. Slower risk analytics may add to them, never replace them.
2. Slow training and slow context never block the decision path. Results are published immutably and read without waiting.
3. A causal rolling window is **not** look-ahead. Leakage is decided by information timing, preprocessing fit windows and label overlap, not by "fixed vs expanding".
4. Selective model recomputation must not silently drop market, book, position or risk state updates. Only model *evaluation* may be skipped, and skips are counted.
5. DPDK, RDMA, extra threads and larger models need measured evidence of a bottleneck, not architectural fashion.
6. Net-of-cost validation, uncertainty, reproducibility (rule 10) and rollback outrank headline accuracy.

## 1. The fact that dominates every readiness verdict

**No executable in this tree assembles feed → book → features → model → strategy → risk → OMS.**
- `app/price_service_main.cpp` republishes decoded Kite ticks (or CSV bar closes in replay) to display clients.
- `app/main.cpp` is the Phase-0 synthetic replay demonstration with a null strategy.
- The desktop fits models **on demand, on the GUI thread, for display**.
- No application drains order intents.

(Codex memory; LEDGER P4-05, P32-07, P37; revalidation by reviewers G13, G15, G17 — see `CX01C_FINDINGS.md`.)

So **every** model, strategy and risk primitive is at best *implemented and tested*, and at most *displayed*. None is *connected to a decision*, so live readiness is **NOT READY** across the board. The useful question per item is what stands between it and a shadow run.

## 2. idea.txt — the model summaries (sections 1–12)

These are descriptive, not proposals. Accuracy against source:

| idea.txt claim | Verdict |
|---|---|
| MLP: hidden layer fixed at seeded init, ridge readout only | **Accurate** (Atlas PARTIAL; G09 revalidates) |
| LSTM/GRU: reservoir, readout by ridge; forget bias +1 vs GRU −1 | **Accurate** as design; LEDGER P8-04/05 measurements |
| GBDT "leaf-wise: grow deepest leaf first" | **Imprecise.** Leaf-wise growth expands the leaf with the largest loss reduction, not the deepest |
| Conformal: "uncalibrated 99% VaR exceeded 5.31%… wrapper brought it to 1.09%" | **Misattributed.** These are the Schmitt paper's reported numbers, not an Altair measurement. Altair's own figures are synthetic 19.12% → 5.10% at a 5% target and, on real NIFTY, per-regime error 15.65 pp → 2.00 pp (LEDGER P35) |
| Conformal: "λ=25 bars 0.77 pp; λ=250 4.50 pp" | **Altair's synthetic measurement** (P35-02), correctly quoted |
| Markov: past-only boundaries; χ² test | **Accurate.** Note P8-14: it survives out of sample on log-score but has **no directional edge** |
| DCF FCFF/WACC vs FCFE/Ke, 24.3% error | **Accurate** (P10-02) |
| Labels: first touch, 28% flip | **Accurate** (P8-01) |
| Dataset: scaler-on-whole-set leak, gap 0.5683 / 1.9552 | **Accurate** (P8-02) |
| Training: third split touched once | **Accurate** (P8-03) |
| Attention: single-head causal; no multi-head, positional encoding or autoencoder | **Accurate** |
| Forecast scorecard: random-walk null, paired DM-style test, 4 verdicts | **Accurate.** On NIFTY daily the verdict is NO BETTER THAN NAIVE |

## 3. idea.txt — the architecture proposals

| # | Proposal (idea.txt section) | Decision | Reason and dependencies |
|---|---|---|---|
| I-1 | Small specialised **event-driven circuits** (book / trades / volatility) instead of one large model (arch.; §1, §10) | **ADOPT AFTER PREREQUISITES** | Consistent with ROADMAP §3.3 and the event-driven engine. Needs, in order: CX-02 correctness fixes (snapshot publication, model-slot lifetime); a canonical tick → book → feature path (CX-03); feature freshness and horizon contracts; live depth for a **tradable** instrument. Kite index subscriptions carry no depth (LEDGER P32-07), and no future/option depth has ever been captured. |
| I-2 | **Volatility circuit first**, adjusting position size (§11 phase 1) | **ADOPT NOW** (first CX-04 candidate, after CX-02) | Estimators exist and are validated on real data: EWMA wins QLIKE over GARCH/GJR (P14), conformal band calibration (P35). Feeds vol-targeting in `risk/sizing.hpp`. Needs a shadow evaluation net of cost and per regime, plus the synchronous risk path. Safe first use: sizing reduction only, never a new entry. |
| I-3 | **Order-book imbalance circuit** (§11 phase 2) | **ADOPT AFTER PREREQUISITES** | OBI/microprice/VPIN exist (`book/`). Blocked on live depth data and on stale-depth admission rules (lead: stale depth with a newer sequence, reviewer G03). The Nyquist argument (10–200 ms decay) means bar data cannot validate it (P23 Microstructure page). |
| I-4 | **Trade-momentum circuit** (§11 phase 3) | **DEFER** | Kite full mode gives last trade price/qty/time, not a trade tape. Aggressor side is inferred (P11-11: the quote rule is silent on 30% of trades). The directional strategies P21/P22 found nothing beating buy-and-hold intraday. Revisit with captured tick data. |
| I-5 | **Slow cognition layer**: regime, fundamentals, correlations, LLM sentiment (§11 phase 4) | **Regime/correlation: ADOPT AFTER PREREQUISITES. Fundamentals: DEFER. LLM sentiment: REJECT for now** | Regime detectors exist and are causal (P6-03, HMM P14-06). Fundamentals have a point-in-time store (P10-01) but no data source. News/NLP: Atlas ABSENT, no corpus, contradicts the no-Python-runtime rule without a separate subsystem decision. |
| I-6 | Fast inference + **slow training/context** without blocking (§"The Solution") | **ADOPT** as a constraint for CX-03/CX-04 | Must go through a correct publication mechanism. `models/serving.hpp` slot reuse and generation metadata are unsafe today (lead Q01/Q02, reviewer G08), so fix that first (CX-02). |
| I-7 | Per-circuit **latency targets** (<100 µs book MLP, <500 µs GBDT, <2 ms vol) (§1) | **DEFER as targets; MEASURE** | Plausible (GBDT predict is an index walk), but none is measured end-to-end. ROADMAP §11 budgets are the comparison; measure first. |
| I-8 | **5 ms total budget** incl. DPDK 0.1 ms and network 1 ms (§2) | **REJECT as an acceptance target** | Retail Kite REST round trip is 10–50 ms (ROADMAP §11, §15) and FYERS is similar. The budget assumes co-location Altair does not have. Internal ingress-to-decision latency is measurable and should be reported separately from broker transport. |
| I-9 | **Model types by layer**: MLP/GBDT fast, LSTM intermediate, GBDT/Transformer/DCF slow (§3) | **GBDT fast path: ADOPT AFTER PREREQUISITES. MLP/LSTM: DEFER. Transformer: REJECT for now** | GBDT is the most-measured model. The MLP/LSTM here are reservoirs with frozen weights (Atlas PARTIAL). The "transformer" is a single-head primitive with no trained model and no data to train one. Every forecast measured so far is at or below a random walk after cost (P8-15/15b, P33, P35, P41). |
| I-10 | "Traditional **fixed windows = look-ahead**; use expanding, NOT rolling" (§4) | **REJECT the assertion. ADOPT the leak-prevention practices** (they already exist) | Rolling causal windows are not look-ahead. Leakage comes from fitting preprocessing on future rows, overlapping labels, or centred windows. Altair already has past-only scalers (P8-02), purged CV with embargo (P6-06) and both walk-forward modes. P8-14 measured that rolling *beat* expanding on magnitude (+0.0122). Choose per evidence. |
| I-11 | **DPDK, CPU pinning, NUMA, RDMA, one core per circuit** (§5) | **DEFER** | Dev box: i5-8500T, 6 cores, Windows 10, Balanced power plan. Prod OS is an open decision (ROADMAP §14 #1). There is no exchange feed to bypass a kernel for. More threads add cross-thread hand-offs over lock-free primitives that have **never run under TSAN** (LEDGER Phase 0 gate). Revisit when a profile names the kernel network path as the bottleneck. |
| I-12 | **Execute first, async risk check cancels if violated** (§6) | **REJECT as the primary order gate** | An order sent before risk is known cannot be un-sent at an exchange. Violates CLAUDE.md rule 5 and ROADMAP §8. |
| I-13 | **Pre-approved hard limits** (max position per circuit, orders/sec, max daily loss kill switch) (§6) | **ADOPT**, as the *synchronous* pre-send check | Largely present as primitives: `risk/limits.hpp`, `oms/throttle.hpp`, the kill switch, and the sizing minimum-binds rule. Not connected to any submission path. Must be assembled in CX-03 before any strategy output can reach `oms/`. |
| I-14 | **Event detection**: run a circuit only on meaningful change (§7) | **ADOPT WITH CONSTRAINT 4** | Book, position and risk state update on every event. Only model evaluation is gated. Thresholds are causal and configured (no literals, rule 1). Skipped evaluations are counted and visible. Horizon bands (P5-01) still apply. |
| I-15 | **Online weight updates** ("very slight gradient descent") on fast circuits (§8) | **DEFER** | Needs versioned registry, reproducibility tuple, shadow, canary and rollback **connected**. The primitives exist (P8-09, P9-05/06) but are not wired. Session-close shrunk weight updates for the aggregator (P9-02) are a safer first step. |
| I-16 | **Weekly conformal recalibration / monthly retrain** (§8) | **Conformal: ADOPT AFTER PREREQUISITES. Scheduled retrain: DEFER** | Conformal exists with measured decay behaviour (λ 0.02 default). Recalibration must run in shadow with per-regime coverage. P9-07 already makes a schedule fire a *candidate*, never a deployment. |
| I-17 | **Paper trade one month; keep a circuit only if it beats a random walk (DM test)** (§9, §12) | **ADOPT**, strengthened | Add net-of-cost results, regime breakdown, deflated/multiple-testing correction (P7-03) and the go-live ramp (ops/go-live.md). Requires the assembled pipeline, live data and a working paper-execution adapter, none of which exist yet. |
| I-18 | "Fly brain specs" and the profit analogy (arch. preamble) | **Not a requirement** | A hypothesis, not acceptance evidence. |

## 4. Model Atlas — status × integration × readiness

**Sources:** the Atlas rows (`desktop/atlas_data.hpp`, 85 rows: 59 BUILT, 8 PARTIAL, 18 ABSENT) and the integration and defect evidence from code reviewers G03, G05–G13, G15, G16 and G18. G18 spot-checked 26 rows and found **no status mismatch**. It left two page indices as open questions: Hurst may belong on page 27 and walk-forward on page 26. The header comment's "32 pages" is stale.

**Columns:**
- **Atlas says** is the status as written (unchanged).
- **Runs where** is what actually executes it. "page N" means an on-demand desktop page running synchronously on the GUI thread (C16-001, C17-001).
- **Live readiness** is **NOT READY for every row**, because no decision path exists (§1), so it is not repeated per row.
- **Disposition** is the Phase-2 decision: **ADOPT NOW**, **AFTER PREREQ** (adopt after prerequisites), **DEFER**, or **REJECT (for now)**.

| Family · model (Atlas row) | Atlas says | Runs where | Defects / evidence that bear on it | Disposition |
|---|---|---|---|---|
| **1 · Mean reversion; Momentum** | BUILT | page 19; tests (real-data verdicts printed, not asserted) | Nothing beats buy-and-hold (P21); round-trip cost charged per trade (C10-013) | DEFER — no edge; fix C10-013 before any re-test |
| 1 · Cointegration; Linear/ridge | BUILT | page 26; tests | Johansen statistic restricted (C10-003); ADF wrong at ≥2 lags (C10-004); page pairs NIFTY future with any spot (C16-003) | AFTER PREREQ — research only, after those fixes |
| 1 · Pairs trading; Statistical arbitrage | PARTIAL | page 26 / 19 diagnostics | No entry/exit rule, no book | DEFER |
| 1 · Factor models | ABSENT | — | — | DEFER (needs cross-sectional data) |
| 1 · Relative value (fundamentals) | BUILT | tests | Point-in-time design sound; no data source; quarterly/annual mixed (C10-007) | DEFER — blocked on data |
| 1 · Spread / calendar | BUILT | page 31; tests | Cross-venue cost counts 2 of 4 trades (C10-002); option calendar uncosted | AFTER PREREQ — research scanner |
| 1 · Regime detection | BUILT | page 17; tests | Labels causal (G11) | AFTER PREREQ — sizing input once the pipeline exists |
| **2 · Gradient boosting; Decision tree** | BUILT | page 16 (synthetic demo); forecast pages refit on click; tests | No NaN handling (C09-002); `predict_row` allocates (C09-004); depth cap hides leaf-wise growth (C09-005); forecasts at or below random walk | AFTER PREREQ — fast-path candidate (I-9) after the fixes |
| 2 · MLP; LSTM/GRU; Transformer; CNN | PARTIAL | tests; neural page shells out to a test | Reservoirs, not backprop; attention has no parameters; out-of-range `Causality` runs unmasked (C09-007); NaN SSE reported as 0 (C09-001) | DEFER |
| 2 · Random forest; Logistic; SVM/KNN/autoencoder | ABSENT | — | — | DEFER — add a classifier only once a baseline beats the random walk net of cost |
| **3 · AR** | BUILT | page 18; tests | `horizon_eval` writes past `x[16]` (C08-010); "no forecast at this horizon" | AFTER PREREQ — fix C08-010; keep as the honest baseline |
| 3 · ARMA/ARIMA/SARIMA; VAR | ABSENT | — | — | DEFER |
| 3 · Kalman filter | BUILT | page 7; tests | NaN accepted and sticky (C07-002) | AFTER PREREQ — hedge-ratio research after the fix |
| 3 · Hidden Markov model | BUILT | page 17; tests | All-restarts-failed path indexes empty vectors (C06-001) | AFTER PREREQ — fix first |
| 3 · Markov regime chain | BUILT | page 17 (catalogue badge green); tests | No directional edge out of sample (P8-14); badge contradicts it (C16-004); verdict printed, not asserted | AFTER PREREQ — regime descriptor only, never a signal |
| 3 · Ornstein-Uhlenbeck | PARTIAL | page 26 (half-life) | — | DEFER |
| 3 · Brownian motion / GBM | BUILT | desktop analytics panel (bootstrap in constructor, C16-002); tests | — | ADOPT (research tool) |
| **3 · Historical volatility & EWMA** | BUILT | page 13; tests | Wins QLIKE out of sample (P14); time-decayed | **ADOPT NOW** — first CX-04 circuit (I-2), sizing-reduction only |
| 3 · GARCH / GJR | BUILT | page 13; tests | Loses to EWMA on QLIKE; GJR asymmetry SE wrong near 0 (G06) | DEFER — keep as comparator |
| 3 · EGARCH | ABSENT | — | GJR covers asymmetry; EWMA wins | REJECT (for now) |
| 3 · Heston | PARTIAL | tests (paths) | No pricer | DEFER |
| 3 · Hurst exponent | BUILT | page 17 (index question, G18); tests | A constant series is reported as significant persistence (C07-003) | AFTER PREREQ — fix first |
| **4 · Order-book imbalance; Microprice** | BUILT | page 24 (constructed demo books); `L2Book` tests only | No live depth has ever arrived for a tradable instrument; stale depth admitted as tradable (C03-001); level counts unchecked (C03-005) | AFTER PREREQ — I-3, needs live depth plus C03 fixes |
| 4 · VPIN; Kyle's λ | BUILT | page 24 demo text; `features/book_flow` (uncalled) | Silent 64-bucket clamp (C03-010); λ has no SE (C03-014) | AFTER PREREQ |
| 4 · Slippage & market impact | BUILT | page 12; tests | Accepts zero prices, biasing impact low (G12) | AFTER PREREQ — fix; seed from real fills only |
| 4 · TWAP/VWAP/POV; Almgren-Chriss; Implementation shortfall | BUILT | page 12; tests | Schedules ignore lot multiples (C13-014); Almgren-Chriss sinh overflow gives NaN and a UB cast (C13-012); VWAP accepts +inf (C13-016) | AFTER PREREQ — after OMS assembly (CX-03) |
| 4 · Queue position; Fill probability; Hawkes | ABSENT | — | Needs live order state and tick data | DEFER |
| **5 · VaR (3 ways); Expected shortfall** | BUILT | page 14; tests | — | AFTER PREREQ — report, not a gate |
| 5 · Stress testing | BUILT | page 14; tests | Test exits 0 without `dataset/` (C12-021) | AFTER PREREQ |
| 5 · Correlation/covariance (Ledoit-Wolf); Beta/sector neutralisation | BUILT | page 15; tests | — | AFTER PREREQ |
| 5 · Drawdown | BUILT | analytics panel; tests | — | ADOPT (research tool) |
| 5 · Factor risk model | ABSENT | — | — | DEFER |
| 5 · Conformal risk control | BUILT | page 18 (Live Forecast); tests | Look-ahead when h > 1 (C08-008); band scored in-sample (C08-009); absolute price units (C08-024) | AFTER PREREQ — calibration in shadow (I-16) |
| **6 · Black-76; Greeks; Higher greeks** | BUILT | page 21; Terminal option chain (SoA); tests | Formulas re-derived correct (G06); model price, not a market quote | AFTER PREREQ — needs a live option chain (P38 blocked on token) |
| 6 · Implied volatility | BUILT | page 21; tests | Sound (G07); 1.6 µs/solve, so 100 strikes ≈ 159 µs vs the ROADMAP chain target | AFTER PREREQ |
| 6 · SVI smile | BUILT | page 21; tests | Never returns NoConvergence (C07-009) | AFTER PREREQ |
| 6 · SABR; Local vol (Dupire) | BUILT | page 21; tests | Refuses off-ATM at ν=0 (C07-001); false arbitrage from truncated strikes (C07-008); Dupire time derivative (C07-007) | AFTER PREREQ — fix first |
| 6 · American approximation | BUILT | page 21; tests | Low-vol puts refused on paise overflow (G06); not on the NSE index path | DEFER |
| 6 · Monte Carlo option pricing | BUILT | tests | Control variate consistent (G05) | ADOPT (research tool) |
| 6 · India VIX replication | BUILT | page 13; tests; **no production caller** | Truncation counters miscount (C07-005); no bid, so zero-bid rules can't apply (C07-006) | DEFER — needs a live chain |
| 6 · Binomial/trinomial; FD PDE | ABSENT | — | Not needed for European index options | REJECT (for now) |
| **7 · Position sizing; Kelly; Vol targeting** | BUILT | page 25; tests | `edge_sigmas ≤ 0` and Kelly divisor < 1 accepted; no quarter-Kelly ceiling (G12) | AFTER PREREQ — **mandatory** before any order; fix first |
| 7 · Risk parity; Minimum variance | BUILT | page 15; tests | 1/N wins out of sample (P15) | DEFER |
| 7 · Black-Litterman | BUILT | page 15; tests | Over-tilts with correlated multi-views (G12); test exits 0 without `dataset/` | DEFER |
| 7 · Mean-variance optimisation | ABSENT | — | Error-maximising; 1/N wins | REJECT (for now) |
| **8 · News/NLP/sentiment; Event models** | ABSENT | — | No corpus, no ingest; no-Python-runtime rule | REJECT (NLP) / DEFER (events) |
| **9 · Monte Carlo; Bootstrapping; Jump diffusion** | BUILT | analytics panel; tests | Jump cap proof holds (G05) | ADOPT (research tools) |
| 9 · Walk-forward & purged CV | BUILT | page 16; forecast/Markov callers; tests | Boundary defects; gap = 0 accepted; embargo not tied to lookback (C05-010) | **ADOPT** as the validation backbone after C05-010 |
| 9 · Agent-based simulation | ABSENT | — | — | REJECT (for now) |
| **10 · Q-learning** | BUILT | page 17 (hard-coded verdict); tests | Evaluation overlaps training; reward omits completion cost (C09-019); loses to TWAP | DEFER |
| 10 · DQN/PPO/actor-critic | ABSENT | — | QUANTLAB rejected PPO | REJECT (for now) |
| **X · Transaction cost calculator** | BUILT | page 6 (cost panel); backtest engine; tests | `[safety]` inert (C12-003 ✔); missing rate becomes 0; CD options ~100× under-costed (G12); delivery flag never set in backtests (C05-009) | **AFTER PREREQ — mandatory**; fix first (rule 5) |
| X · Model scorecards & drift | BUILT | page 23; tests | ADWIN scale-dependent (C05-011); canary loss limit can't fire early (C05-012) | AFTER PREREQ |
| X · Forecast scorecard | BUILT | page 18; tests | iid SE on overlapping horizons (C08-007) | AFTER PREREQ — fix first |
| X · Ensemble aggregator | BUILT | page 10 (equal-weight demo); tests | No std_error from serving (C08-003); weight-error term vanishes on disagreement (C08-004) | AFTER PREREQ |
| X · Feature registry | BUILT | page 30; research templates | Band/lookback enforcement opt-in with no caller (C05-004); `FeatureVector` bound bypass (C05-001) | AFTER PREREQ — required by CX-03 |
| X · DCF valuation | BUILT | page 9 (illustrative inputs); tests | Money as a bare double, rupees vs paise (C09-015); no fundamentals data | DEFER |

**Reading the table:**
- Only **EWMA volatility** (sizing reduction) is adoptable now.
- **Walk-forward/purged CV, research Monte Carlo/bootstrap and drawdown** can be adopted as research tools.
- Everything a live decision would require (**cost calculator, sizing, forecast scorecard, feature registry, conformal band**) is **AFTER PREREQ**: it needs its CX-02 fixes and the CX-03 assembly first.
- Nothing here reverses an Atlas status. The dispositions are additive and the Atlas is left unchanged.

## 5. Training status — answer to "is training of all models perfectly done?"

**Short answer: no.** It is not "done" in the sense a trading system needs, and the code says so itself.

- **Nothing is trained once and kept.** No weights file, `.pt` or `.onnx` artefact, or registry record exists or is loaded anywhere (G08, G16; LEDGER P12-06).
  - Every model is refit in memory on each test run or each desktop button press, then discarded.
  - The only file writes in `desktop/` are the kill-switch request and the order-intent queue (G16).
  - The "no .pt or .onnx anywhere" line on the Models page is a fixed string, not a check.
- **The training machinery is real but runs on toy inputs.** The Trainer (early stopping, third split), Dataset/Scaler, triple-barrier labels, ModelRegistry and ModelServer are exercised only on synthetic data and single-weight toy models (G08).
- **Where real data is used, the verdict is "no tradeable edge."** The code computes this and does not hide it (tables below).
- **The neural tier is not trained by backpropagation.** The MLP, LSTM and GRU are fixed random hidden layers with a trained linear readout (echo-state reservoir); the Atlas marks them PARTIAL. ROADMAP P8 exit (a 10-minute forecast that beats persistence after costs) is **NOT MET** (LEDGER).
- **The evidence is weaker than it looks in places.**
  - The forecast verdict's t-statistic assumes independent errors on overlapping horizons (C08-007).
  - The conformal band is scored on the same period it was fitted on (C08-009).
  - The Markov model's page badge says "trained on real data" in green, beside a walk-forward that finds no directional edge (C16-004).

### 5.1 Models fitted on real data

| Model | Where fitted | Data | Out-of-sample result | Net of cost? |
|---|---|---|---|---|
| GBDT spot forecast (6 members, level- and leaf-wise) | desktop Forecast/Aggregator pages on click; `test_spot_forecast` | real NIFTY spot 5m/15m/60m/1d | RMSE no better than a constant; "trained — NO EDGE FOUND" | spot evaluation yes (5.5 bps literal); price-forecast scorecard **no** |
| GBDT price forecaster + conformal band | desktop Live Forecast page on click; `test_calibration_real`, `test_rich_features --experiment` | real NIFTY daily | Verdict vs random walk: **NO BETTER THAN NAIVE**. P41 richer features made it worse (t −2.28 to −2.53). The band's per-regime calibration error falls 15.65 → 2.00 pp | forecast: no; band: n/a |
| AR(p) horizon model | `test_horizon_eval` only | real NIFTY spot and futures | "NO FORECAST AT THIS HORIZON"; the 15-minute edge reversed to −7.19σ on 9× the data (LEDGER P8-15b) | yes (impact excluded, so an upper bound) |
| MLP (random features) / GRU reservoir ("neural tier") | **tests only** (`test_neural_5m`); the desktop runs that test binary and shows its output | ~211k real 5-minute NIFTY returns | "trained — NO EDGE FOUND". RMSE and direction vs a training-mean constant are **printed, not asserted** (C09-020) | **no** (the test says so) |
| Markov regime chain | desktop on click; `test_markov`, `test_markov_eval` | real NIFTY daily | In-sample: rejects independence. Walk-forward: +0.019 nats/obs but **no directional edge** (sign and magnitude tie constant baselines; LEDGER P8-14). Walk-forward verdict printed, not asserted | **no** ("No strategy may read these numbers as a signal", `markov_eval.hpp:47-52`) |
| India VIX AR(1), level/log | desktop on click | real India VIX daily | **in-sample only** (C16-010); level-space band under-covers in stress | n/a |
| GARCH / GJR vs EWMA | desktop on click; `test_garch` | real daily | first half fit, second half scored: **EWMA wins QLIKE** | n/a |
| HMM (2-state), k-means regimes | desktop on click (k-means also in `test_regime_rl`) | real daily | in-sample; separation × persistence only, **no forward evaluation** | n/a |
| GBDT meta-labelling | tests | real NIFTY 1d, first half train / second half test | lift over the momentum hit rate is **printed, not asserted**; LEDGER P16 +1.58σ, **did not replicate** | no |
| Q-learner vs heuristic (execution timing) | **tests only** (`test_regime_rl`) | 12-day episodes on real NIFTY daily closes | printed: neither beats TWAP (LEDGER P18). **Evaluation is flawed:** the "out-of-sample" episodes overlap training in time, and the reward omits completion cost (C09-019). The desktop prints a hard-coded sentence | shortfall vs arrival only; **no fees/STT** |
| Momentum / mean reversion / overnight | rule evaluation, **not training** | real daily / 5-minute | nothing beats buy-and-hold (P21); overnight gap is in-sample with no walk-forward | yes |

### 5.2 Models not trained on real data

| Model | Status |
|---|---|
| Trainer harness, Dataset/Scaler, labels, registry, serving | synthetic/toy only (G08) |
| GBDT feature-importance demo (desktop) | synthetic 3,000 rows |
| LSTM reservoir + ridge readout | synthetic data only; beats the mean on synthetic validation; **never evaluated on real data** (G09) |
| Attention / TFT gates / dilated conv (TCN) | **not trainable**: no parameters and no fit. The real-data check in `test_neural_5m` is a tautology (C09-020) |
| Aggregator weights | never trained: equal weights, swept ρ, fixed weight SE (G08) |
| DCF | not a trained model. Inputs are illustrative. Money is a bare `double`, fed rupees by one caller and paise by another (C09-015). Point-in-time fundamentals are still blocked |
| SVI, Black-76/IV, American, SABR, microstructure, sizing, portfolio optimisers | computed from typed or synthetic inputs; not market measurements |

**Defects that would corrupt any future training run** (G09):
- A NaN training error is returned as 0.0, which reads as a perfect fit (C09-001).
- GBDT has no finiteness checks: sorting NaN is undefined behaviour, and a NaN label returns a "successful" NaN model (C09-002).
- GBDT `predict_row` allocates on every call, despite an allocation-free contract (C09-004).
- Four real-data tests pass on SKIP when `dataset/` is missing (C09-018).

### 5.3 What "done" would require (Phase 2+, not started)

1. Fix the data first: the dataset stores prices at 6 significant digits (C14-001).
2. A persisted, versioned model artefact registered with its reproducibility tuple (rule 10): model hash, feature version, config hash, spec version, data window.
3. Walk-forward results **net of cost, per regime**, with autocorrelation-robust uncertainty (C08-007) and multiple-testing correction.
4. Shadow → canary → rollback connected (primitives exist and are unconnected, G05/G08).
5. For the neural tier: actual backpropagation, or an honest decision to keep reservoirs. ROADMAP §14 #5 (GPU) and #6 (tick/L2 history) remain open blockers.
