# Phases 13–19 — the QUANTLAB rebuild

**Written 2026-09-06.** Smit's decision: rebuild QUANTLAB inside Altair rather
than keep it as a separate Python project.

This document is the card plan. It is written before any of the work because
CLAUDE.md says the interface contract in each card — not anyone's memory — is
what keeps card 90 compiling against card 3, and because a 40-card program
started without one becomes forty improvisations.

---

## 0. What this reverses, and what it costs

CLAUDE.md's layout says:

> `Quants/` SEPARATE PROJECT (QUANTLAB) — not part of Altair, own repo

That line is now wrong and this plan is why. **Recorded rather than quietly
deleted**, because the separation was a decision with reasons and reversing it
has a price:

- **QUANTLAB is Python. Altair bans Python in the runtime.** So this is not a
  port, it is a REIMPLEMENTATION. `polars`, `statsmodels`, `arch`, `cvxpy`,
  `scikit-learn`, `LightGBM` and `PyTorch` have no C++23 equivalent that drops
  in. Every model is rewritten against `core/types`, in integer paise where it
  touches money, allocation-free where it touches `ALTAIR_HOT`.
- **QUANTLAB's tests do not come with it.** 726 Python tests become C++
  acceptance tests, rewritten. The findings survive; the code does not.
- **Two engines become one.** QUANTLAB's event-driven backtester and Altair's
  `backtest/engine.hpp` do the same job. Altair's stays; QUANTLAB's dies.

What is worth carrying is not the code. It is the **results** — and
particularly the negative ones, listed in §2.

---

## 1. Altair already has most of the taxonomy

The roadmap reads as though starting from zero. It is not, and building what
exists is the most expensive mistake available here.

| Family | Altair HAS | Genuinely missing |
|---|---|---|
| Options / derivatives | `analytics/greeks.hpp`, `greeks2.hpp` (2nd order), `american.hpp` (binomial), `svi.hpp` + `svi_fit.hpp`, `iv.hpp`, `derivatives.hpp` | SABR, local vol (Dupire), **MC option pricing** |
| Microstructure | `book/l2_book.hpp`, `microstructure.hpp` (imbalance, microprice, VPIN, Kyle λ), `flow.hpp`, `risk/slippage.hpp`, `risk/cost.hpp` | **TWAP / VWAP / POV / Almgren-Chriss**, queue position, fill probability |
| Simulation | `backtest/montecarlo.hpp` — GBM, **Heston**, **jump diffusion**, IID + **block** bootstrap, drawdown, Sharpe/Sortino | **MC VaR**, variance reduction, agent-based |
| Statistical alpha | `strategies/` — cointegration, basis, calendar, parity, regime, score, vix_forecast, fundamentals | OU process, explicit cross-sectional factor models |
| Time series | AR(1), `analytics/ewma.hpp`, `rolling.hpp`, `hurst.hpp`, `models/markov.hpp` | **GARCH / EGARCH / GJR**, **Kalman filter**, ARIMA/VAR, HMM (hidden, not observable) |
| ML | `models/` — mlp, recurrent (GRU/LSTM), attention, ridge readout, training harness (purged CV + walk-forward), registry, serving | **gradient-boosted trees**, autoencoders |
| Risk | `risk/limits.hpp`, `portfolio.hpp`, `sizing.hpp` (¼-Kelly, vol targeting), `neutralise.hpp`, `hedge.hpp` | **VaR / CVaR / ES**, stress testing, MV / RP / HRP / Black-Litterman |
| Portfolio | Kelly, vol targeting, position sizing | Optimisers, risk budgeting |
| Alt data | — | everything (NLP, sentiment, events) |
| Reinforcement learning | — | everything — **and see §2** |

**Four gaps carry the value**: execution algos, GARCH+Kalman, VaR+optimisers,
gradient-boosted trees. They are Phases 13–16. Everything else is Phase 17+ or
explicitly parked.

---

## 2. QUANTLAB's findings are the real import

These are results Altair gets for free, and several of them CANCEL work the
roadmap proposes. Carrying them across is worth more than any file.

| # | QUANTLAB finding | What it means for Altair |
|---|---|---|
| Q1 | **PPO rejected.** Median +0.260, loses to a ten-line heuristic at +0.524 | **Phase 18 (RL) starts as a REPLICATION of a rejection**, not a build. If a ten-line heuristic wins again, RL is closed. |
| Q2 | **EWMA beat GARCH on QLIKE** in the vol horse race | P14 must include the horse race, and GARCH shipping worse than the `ewma.hpp` Altair already has is a legitimate, expected outcome |
| Q3 | **1/N beat every optimizer** (MV/RP/HRP/BL); the 4 sleeves did not combine into deployable alpha, CI spanned zero | P15's optimisers ship WITH the 1/N control, and a card that cannot beat 1/N reports that |
| Q4 | **Pairs not cointegrated** per Engle-Granger; carry ≈ coin flip; Donchian rejected | Altair's `cointegration.hpp` should be run against the same test before any pairs strategy is sized |
| Q5 | **VRP was the first DSR>0.95 edge** (Sharpe 1.02) — conditional on defined-risk wings | The single most promising import. Needs an options-chain adapter, which P11Q-05d showed needs a LIVE subscription, not history |
| Q6 | **Meta-labeling lifted hit rate 0.41 → 0.56** on mean-reversion; 2nd deflation-surviving edge | P16's gradient-boosted trees should be aimed at META-LABELING first, not direction |
| Q7 | **~$68M capacity ceiling**, an NSE-liquidity bound; a size-blind linear cost model could not express it | Altair's `slippage.hpp` must be checked for the same size-blindness |
| Q8 | **Regime conditioning through the optimizer HURT** (−0.077 SR); a gross-neutral tilt HELPED (+0.397) | Route matters more than signal. P6-03b already found the label does not separate forward returns — consistent |
| Q9 | Delay-dominated shortfall: decision at prior close, fill at next open | This is what P13's execution algos exist to fix |

**Q1, Q2, Q3 and Q4 mean four proposed builds are expected to fail.** They are
still built, because a replication on Indian data is a real result and because
"someone else's backtest said so" is not evidence for this book. But they are
budgeted as experiments, not as features.

---

## 3. The phases

Sizing follows CLAUDE.md: split if any file exceeds ~400 lines, more than 4
files in a manifest, more than 12 requirements, or more than 8 acceptance
tests.

### Phase 13 — Execution algorithms · 7 cards · `oms/`

The largest real gap and the one with a measurable payoff (Q9). **`oms/` is the
only directory that may place an order**, so this is where they live —
unconditionally, per the one-component-one-directory rule.

| Card | Deliverable |
|---|---|
| P13-01 | Parent/child order model: a parent intent, a schedule, child slices, and the invariant that Σ(children) == parent exactly, in integer quantity |
| P13-02 | TWAP — uniform slicing against a session clock read off the tick, never a wall clock |
| P13-03 | VWAP — slicing against a volume CURVE, plus the finding that an index reports no volume so VWAP is undefined for NIFTY spot |
| P13-04 | POV (participation) — capped at a fraction of observed volume, with the feedback loop that makes it unstable when volume collapses |
| P13-05 | Almgren-Chriss — closed form, reproduced against the paper's own worked example; risk-aversion parameter is Smit's decision, not a default |
| P13-06 | Implementation shortfall attribution: decision → delay → execution → fees, and the parts must SUM to the total (QUANTLAB asserted this; so do we) |
| P13-07 | Execution-aware cost model + capacity ceiling — Q7: a size-blind cost model cannot express a capacity bound |

**Exit:** measured shortfall improvement over a naive at-the-open fill, on
replay, net of the real charge schedule.

### Phase 14 — Volatility & filtering · 6 cards · `analytics/`

| Card | Deliverable |
|---|---|
| P14-01 | GARCH(1,1) — MLE fit, stationarity constraint α+β<1 refused not clamped |
| P14-02 | EGARCH + GJR-GARCH — the leverage effect, which symmetric GARCH cannot represent |
| P14-03 | **The vol horse race**: EWMA vs GARCH family vs HAR-RV on QLIKE and MSE, on real NIFTY. Q2 predicts EWMA wins |
| P14-04 | Kalman filter — linear-Gaussian state space, with the numerically stable Joseph form covariance update |
| P14-05 | Kalman applied to a time-varying hedge ratio, against `strategies/cointegration.hpp` (Q4) |
| P14-06 | HMM — Baum-Welch, hidden states, distinct from the OBSERVABLE Markov chain in `models/markov.hpp`, and compared against it |

**Exit:** a vol forecast with a measured QLIKE, and an honest statement of
which estimator won.

### Phase 15 — Risk & portfolio construction · 7 cards · `risk/`

| Card | Deliverable |
|---|---|
| P15-01 | VaR — historical, parametric, Monte Carlo, all three, and the divergence between them IS the deliverable |
| P15-02 | Expected Shortfall / CVaR — coherent where VaR is not; the subadditivity counterexample is an acceptance test |
| P15-03 | Covariance estimation — sample, Ledoit-Wolf shrinkage, RMT filtering; sample covariance is unusable at p≈n and the test shows it |
| P15-04 | Stress testing + scenario analysis on the real 2008/2020 windows in `dataset/` |
| P15-05 | Mean-variance + minimum-variance optimiser, **with the 1/N control** (Q3) |
| P15-06 | Risk parity + HRP, same control |
| P15-07 | Black-Litterman — market equilibrium plus views, and the view-uncertainty matrix nobody sets honestly |

**Exit:** the Combined Book, and a verdict on whether any optimiser beats 1/N
on Indian data. Q3 says it will not.

### Phase 16 — Gradient boosting & meta-labeling · 5 cards · `models/`

| Card | Deliverable |
|---|---|
| P16-01 | Regression trees — CART, exact split search, no allocation on the predict path |
| P16-02 | Gradient boosting — squared and logistic loss, shrinkage, subsampling |
| P16-03 | Histogram binning (the LightGBM trick) — the only reason GBDT is fast enough to matter |
| P16-04 | Feature importance + a SHAP approximation, and why gain-based importance lies on correlated features |
| P16-05 | **Meta-labeling** (Q6) — the highest-value ML use here: a filter on an existing signal, not a direction predictor |

**Exit:** GBDT beats the ridge baseline out-of-sample under purged CV, or it
does not and that is reported.

### Phase 17 — Derivatives completion · 4 cards · `analytics/`

| Card | Deliverable |
|---|---|
| P17-01 | Monte Carlo option pricing — path-dependent payoffs, antithetic + control variates, with the standard error reported alongside every price |
| P17-02 | SABR — the industry standard for smile interpolation |
| P17-03 | Local volatility (Dupire) from the SVI surface `analytics/svi_fit.hpp` already fits |
| P17-04 | **VRP strategy** (Q5) — QUANTLAB's best edge. BLOCKED on a live options-chain subscription; the card exists so the blocker is tracked, not forgotten |

### Phase 18 — Regime & RL · 3 cards

| Card | Deliverable |
|---|---|
| P18-01 | Jump model + GMM/k-means regime clustering, ranked by a **pre-registered, return-blind** rule as QUANTLAB did |
| P18-02 | Q-learning / DQN on execution timing only — never direction, never size |
| P18-03 | **The ten-line heuristic control** (Q1). If it wins again, RL is closed permanently and the card records that |

### Phase 19 — GUI for every model · 6 cards · `desktop/`

Smit's standing rule: every model gets a page. One card per page group, each
reading the SAME artifacts the tests produce — never a second computation.

| Card | Deliverable |
|---|---|
| P19-01 | Execution page — parent/child ladder, live shortfall attribution |
| P19-02 | Volatility page — the horse race, QLIKE table, forecast vs realised |
| P19-03 | Risk page — VaR/ES three ways, the divergence, stress windows |
| P19-04 | Portfolio page — optimiser weights vs 1/N, efficient frontier |
| P19-05 | ML page — GBDT importance, meta-label lift, purged-CV folds |
| P19-06 | Model catalogue rebuild — `model_status.hpp` grows from 12 rows to ~40 and a hand-maintained vector stops being the right shape |

---

## 4. Totals and honest sequencing

| Phase | Cards | Depends on |
|---|---|---|
| 13 Execution | 7 | — |
| 14 Vol & filtering | 6 | — |
| 15 Risk & portfolio | 7 | 14 (covariance needs vol) |
| 16 GBDT | 5 | — |
| 17 Derivatives | 4 | 14 (SABR needs a vol surface) |
| 18 Regime & RL | 3 | 14 (HMM), 16 |
| 19 GUI | 6 | all |
| **Total** | **38** | |

38 cards on top of the current 156 → **194**.

**This is not a sprint.** At the observed rate of this project it is months of
work, and four of the phases are expected to produce negative results (§2).
That is not an argument against doing them — a replication on Indian data is a
real result and QUANTLAB's numbers are not evidence for this book — but it is
an argument against calling any of it "features".

**Order recommendation:** 13 → 14 → 16 → 15 → 17 → 18, with 19 built
incrementally alongside rather than saved for the end. P13 first because it is
the only phase with a payoff that does not depend on finding new alpha: Q9's
delay-dominated shortfall is a cost being paid today on every fill.

---

## 5. Parked deliberately

- **Alternative data** (NLP, sentiment, satellite). No source, no budget, and
  it is a data-acquisition problem before it is a modelling one.
- **VRP live** (P17-04) — needs a live options chain, which P11Q-05d showed
  cannot come from historical candles: a `Touch` is a bid AND an ask, and a
  candle carries one close.
- **Crypto** — permanently out of scope, per QUANTLAB's own rule.
- **QUANTLAB's Python tree** — stays where it is, read-only, as the record of
  the findings in §2. Nothing in `Quants/` is built, linked, or listed in any
  card manifest.
