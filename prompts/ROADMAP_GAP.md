# The roadmap, against what Altair actually has

**Written 2026-09-08**, after Phases 13–19 closed. Every "built" claim below was
checked against the tree rather than recalled, and the checks are named.

---

## 0. The thing to settle first: these are two different documents

The roadmap is a **learning path** — Stages 1–7, in sequence, each one teaching
a skill before the next depends on it. Altair is a **production trading
engine**. They overlap heavily and they are not the same object, and reading
one as a progress bar for the other produces exactly the wrong conclusion.

Concretely: **Altair has Stage 4, 5 and 6 infrastructure and is missing a
Stage 3 deliverable.** That is not a scheduling accident, it is what happens
when a system is built engine-first. The risk layer, the execution algorithms,
the cost model, the walk-forward harness and six model families are all in
place — and there is no standalone momentum or mean-reversion strategy in
`strategies/` that produces a tradeable signal.

That single sentence is the most useful output of this document.

---

## 1. Stage by stage

### Stage 1 — Market math & probability · **DONE, and past it**

| Roadmap item | State |
|---|---|
| Returns, log returns, rolling vol, drawdowns | `analytics/rolling.hpp`, `ewma.hpp`, `backtest/montecarlo.hpp` |
| Distributions, expected value, variance | Measured on the real series: **excess kurtosis 11.34**, 136 of 8,755 days beyond 3σ (1.55% against a normal's 0.27%) |
| Covariance | `risk/covariance.hpp` — sample, Ledoit-Wolf shrinkage, condition number |
| Random walks vs real data | `backtest/montecarlo.hpp` — GBM, **Heston**, jump diffusion |
| Volatility clustering | GARCH persistence measured; EWMA/GARCH horse race run |

**Builds 1–3** (coin flip, dice, correlation heatmap) are teaching exercises.
Their *content* is present at a far higher level; the exercises themselves are
not, and building them now would teach nothing this tree has not already
demonstrated on real data.

### Stage 2 — Data pipeline · **DONE, in C++ not Python**

| Roadmap item | State |
|---|---|
| Fetch, clean, align | `altair_kite_fetch` — **2.8M bars**, coverage-verified against a derived NSE calendar |
| Missing values | `config/nse_calendar.csv` classifies sessions by **start time, not bar count**; a 2-day fetch-seam gap was found and closed |
| Point-in-time | `instruments/` spec store, effective-dated `charges.toml` |

**Build 4 (portfolio tracker)** now exists as the **Kite Account** page —
funds, positions, holdings, orders, with the snapshot's age on screen.
**Build 5 (Monte Carlo retirement)** is the same arithmetic as
`backtest/montecarlo.hpp`. **Build 6 (SMA + Sharpe)** — see Stage 3.

### Stage 3 — Signals & backtesting · **THE GAP**

| Roadmap item | State |
|---|---|
| Backtest engine, walk-forward, costs | **Done and then some** — `backtest/engine.hpp`, purged CV with embargo, `risk/cost.hpp` with an effective-dated schedule |
| Stress periods | **Done** — GFC 2008 −50.79% / 59.86% drawdown, COVID 2020 −17.57% / 37.63%, on dated windows |
| Monte Carlo simulator (Build 7) | **Done** — GBM/Heston/jump, IID **and block** bootstrap |
| Regime detection | **Done** — Hurst detector (P6-03b), k-means, HMM |
| Pairs / cointegration | **Done** — Engle-Granger, Johansen, **Ornstein-Uhlenbeck half-life** |
| **Mean reversion strategy** | **NOT BUILT** — no z-score fade strategy in `strategies/` |
| **Momentum / trend following** | **NOT BUILT** — momentum appears only as a *feature* inside `regime.hpp`, `score.hpp`, `mlp.hpp` and P16's meta-label test |
| **Fisher transform (Build 9)** | **NOT BUILT** — `rolling.hpp` has Fisher-*Pearson* skewness, which is a different thing entirely |
| CAGR / Sharpe reporting | Partial — Sharpe and Sortino exist in `montecarlo.hpp` and `training.hpp`, but no strategy tearsheet |

**This is the hole.** The roadmap says *"build at least one mean-reversion and
one trend-following system before moving on."* Altair moved on. It has the
machinery to evaluate a strategy honestly — cost model, walk-forward, stress
windows, capacity ceiling — and nothing in `strategies/` that emits a
directional signal to run through it.

Everything downstream is currently being tested against **model forecasts**
(GBDT, MLP, GRU) rather than against **strategies**, and those forecasts have
all come back flat or negative.

### Stage 4 — Risk & portfolio · **DONE, with a finding**

| Roadmap item | State |
|---|---|
| VaR — historical, parametric, MC | **All three**, and the divergence is the deliverable: at 99% the normal **understates by 22%**, ES gap 52% |
| Expected Shortfall / CVaR | Done, with the **subadditivity counterexample**: VaR(A)+VaR(B) = −2.00 against VaR(A+B) = 49.50 |
| Drawdown models | Done — max drawdown, and the block-bootstrap prediction (59.85%) matched the realised GFC figure (59.86%) |
| Covariance — Ledoit-Wolf | Done; sample covariance degrades **218×** as p/n goes 0.01 → 1.33 |
| Correlation in crises | Done — NIFTY/VIX is −0.5698 full-sample, −0.4197 on the worst decile |
| Mean-variance, risk parity, min-var | Done — **and 1/N beats all of them out of sample** |
| Black-Litterman | Done — Ω has no default; a zero is refused as infinite certainty |
| Kelly, vol targeting, position sizing | Done — `risk/sizing.hpp`, ¼-Kelly, the minimum binds and is named |
| **PCA / eigendecomposition** | **NOT BUILT** — power iteration exists for a condition number, no factor extraction |
| **Cross-sectional factor models** | **NOT BUILT** — needs an equity universe, which `dataset/` does not have |

**Build 10 (vol targeting)** done. **Build 11 (factor analysis)** blocked on the
same missing equity universe as pairs trading.

### Stage 5 — Advanced strategies & institutional models · **MOSTLY DONE**

| Roadmap item | State |
|---|---|
| Black-Scholes / Black-76 | `analytics/greeks.hpp` |
| Binomial tree | `analytics/american.hpp` |
| Monte Carlo pricing | **Done** — validated to inside its own error bar against the analytic price; convergence measured at 2.01 vs the 2.00 that 1/√n predicts |
| Local vol (Dupire) | Done — refuses calendar and butterfly arbitrage rather than clamping |
| SABR | Done — **and the arbitrage check fires at ν = 1.5**, far below where I guessed |
| Greeks, 1st and 2nd order | `greeks.hpp`, `greeks2.hpp` — vanna, charm, vomma |
| Vol smile / skew | `svi.hpp`, `svi_fit.hpp` |
| XGBoost / LightGBM | **Done** — histogram binning *and* leaf-wise growth |
| LSTM / GRU | `models/recurrent.hpp` |
| Transformers | `models/attention.hpp` |
| **Autoencoders** | **NOT BUILT** |
| **Random Forest** | **NOT BUILT** — GBDT covers the ground; RF is a bagging variant of the same trees |
| TWAP / VWAP / POV / Almgren-Chriss | **All four**, in `oms/` |
| Order book imbalance, microprice | `book/microstructure.hpp` — plus VPIN and Kyle's λ |
| GARCH / EGARCH / GJR | Done — **and EWMA beats them on QLIKE** |
| Kalman filter | Done, Joseph form; NIS diagnostic caught a 45× parameter error |
| HMM | Done — 2.70× separation on real NIFTY against 1.22× on noise |
| **Alternative data** | **NOT BUILT — the one entirely absent family** |
| **ARIMA / SARIMA / VAR** | **NOT BUILT** — AR(1) only |

**Build 12 (order book simulator)** — the book, imbalance and impact model
exist; a matching-engine simulator does not.

### Stage 6 — Advanced visualisations · **PARTIALLY, as panels not visuals**

| Roadmap build | State |
|---|---|
| 1. Volatility Regime Phase Map | The **classification** is done (HMM, k-means, Hurst); the phase-map visual is not |
| 2. Options Flow Surface Engine | Pricing and Greeks done, SVI surface done; **needs a live options chain** — same blocker as VRP |
| 3. 3D Latent Market Manifold | Not built |
| 4. AI Trade Signal Map | Not built |
| 5. Regime Switching RL Agent | **Built and the answer was no** — see §2 |

### Stage 7 — Career tracks · **Track B partially, A and C not started**

Track B's **DCF** is done including the reverse DCF and a sensitivity grid.
LBO, comps and precedent transactions are not built and are a different
project. Tracks A and C are career artefacts (a research paper, a trade
journal, a club) rather than engine code.

---

## 2. Five roadmap techniques that have now been TESTED here and did not transfer

This is the part the roadmap cannot tell you, and it should change what gets
built next.

| Technique | Roadmap's framing | Measured on NIFTY |
|---|---|---|
| GARCH family for vol forecasting | "essential when running vol targeting" | **EWMA wins on QLIKE** (−8.273 vs −8.219). Three fitted parameters beaten by one constant |
| Mean-variance / risk parity / BL | "where math becomes positions" | **1/N beats every one** out of sample. Min-var collapsed 20 assets into 5.97 effective bets and got *higher* variance |
| Meta-labeling (ML filter on a signal) | Stage 5 alpha toolkit | **+1.58σ, not significant** (0.5177 → 0.5309) |
| RL for execution timing | Stage 6 flagship build | Q-learner lost to a ten-line heuristic — **and both lost to TWAP** by 5 and 8 bps |
| LightGBM leaf-wise growth | "most widely used for tabular financial data" | **Level-wise beats it at every horizon** out of sample |

None of these mean the techniques are wrong. They mean that on *this*
instrument, at *these* horizons, with *these* features, the simple baseline
won — which is the normal outcome and the reason each was run with its control
attached.

**The practical consequence:** Stage 6's RL agent is already answered. Building
it again as a "flagship portfolio piece" would be rebuilding a rejection.

---

## 3. What the forecasting results say about sequencing

Across three independent model families, at four horizons, net of cost:

```
5-minute    35.3% of moves exceed the 5.5 bps round trip
            perfect oracle +0.409 bps/bar
            GBDT net -5.416    directional +1.80σ
15-minute   56.8% exceed
            perfect oracle +5.076
            GBDT net -5.407    directional -2.11σ   (worse than chance)
60-minute   74.5% exceed
            GBDT net -4.818    directional -0.91σ
daily       95.2% exceed
            GBDT net +4.478    directional +1.57σ   (not significant)
```

RMSE is worse than a constant at **every** horizon. P8-15 and P8-16 found the
same with an AR, an MLP and a GRU.

**Reading:** there is no intraday directional edge in lagged NIFTY returns.
That is a strong, repeatedly replicated negative result, and it means more
model families will not help. The roadmap's Stage 5 answer — "XGBoost and
neural nets predict moves before they're obvious" — has been tested here four
ways and does not hold on this data.

---

## 4. What to build next, in order

**1. A momentum strategy and a mean-reversion strategy, in `strategies/`.**
The Stage 3 hole. Not because they will work — the regime detector already
found the Hurst label does not separate forward returns — but because the
entire evaluation stack downstream has never been run against an actual
strategy, only against model forecasts. Until it has, the capacity ceiling,
the shortfall attribution and the optimiser comparison are all machinery with
nothing in them.

**2. Cross-sectional data.** Every remaining blocked item — factor models,
pairs trading, PCA, the Combined Book, Build 11 — is blocked on the same thing:
`dataset/` holds three correlated series (NIFTY spot, NIFTY futures, VIX) and
no equity universe. One fetch fixes five items.

**3. A live options chain.** Unblocks VRP (Stage 6 Build 2, and QUANTLAB's only
DSR > 0.95 edge) and fills the watchlist bid/ask the `/quote` parser is now
waiting on. Needs a session issued the same day.

**Deliberately not next:** alternative data (a data-acquisition problem before
a modelling one), the RL agent (answered), and more forecasting model families
(four have now agreed).

---

## 5. Honest summary

By roadmap coverage, Altair sits somewhere in **late Stage 5**, with parts of
Stage 6 done and one Stage 3 deliverable missing.

By *usable trading edge*, it sits at zero — every strategy tested is flat or
negative net of cost, which is the honest state of a system that has been
built carefully and has not yet found alpha. The machinery to recognise an
edge if one appears is in place and tested; that is what the last twenty
phases bought.

And none of it is cleared to trade: `ops/go-live.md` still has three exposed
API secrets unrotated, `charges.toml` reading `UNVERIFIED`, the Phase 0 clang
gate open, no live subscription, and no contract note ever reconciled.
