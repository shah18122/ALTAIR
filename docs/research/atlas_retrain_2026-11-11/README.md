# Every Model Atlas row, retrained on the new data (2026-11-11)

Smit asked for every model in the Model Atlas to be retrained on the new data.
This is the result for all 108 rows.

**Before** is the FYERS nightly of 2026-11-10 (`altair-data/reports/2026-11-10`):
up to 17 years of daily bars and about 2,170 days of intraday bars.
**After** is the Google Drive tick data (source `drive-ticks`, NOT FYERS, never
merged into a FYERS series; docs/DATA_POLICY.md): NIFTY spot and NIFTY /
BANKNIFTY front-month futures, 2020, 2022 and 2023 (about 740 sessions),
turned into 1m-1d bars by `tools/drive_ticks_to_bars.py`, plus the raw ticks.

How it was run (cloud, same code as the nightly):

    altair_forecast_curriculum --dataset <drive tree> --no-ivrv --leak-check --skip "INDIA VIX"
    altair_vol_forecast, altair_regimes --leak-check --not-fyers, altair_as_mm --not-fyers,
    altair_trader --leak-check, altair_magnitude, altair_threshold, altair_tick_forecast

Every fit is walk-forward on past rows only. The curriculum leak check is
clean on every track and the trader's decisions are identical at every cut.

Caveats on the Drive tree:
- The "BANKNIFTY" spot track is BANKNIFTY front-month **futures** (no BANKNIFTY
  index ticks in the Drive set). Its 1m bars carry bid-ask bounce, which is
  why mean-reversion models look "significant" on BANKNIFTY 1m only.
- There is no INDIA VIX in the Drive set. A constant placeholder lets the
  tools run; every VIX input is therefore blank (HAR runs without VIX, and
  `altair_vol_slope` gives 0 everywhere, so it is not a result).
- About 740 sessions against FYERS' 2,170+: wider error bars after.

## Headline

1. **Direction: no model beats "no change" on price, on either data set.**
   Every curriculum model's average skill against the random walk is
   negative after, as it was before (best before: Ridge stack +0.02%;
   best after: Stack (confident 2%) +0.31% on a handful of calls).
   Average accuracy moved by about ±1 point; nothing clears the costs.
2. **Size of the move is forecastable, on both.** HAR-seasonal is 32-35%
   below the random walk and 3-11% below the 22-session mean on 1m-60m.
   On daily bars it is no better than the mean.
3. **The trader** (stacks the forecasts): NIFTY daily net Rs 2.08 L vs
   buy-and-hold Rs 2.65 L over the same days, so timing adds nothing;
   intraday arms lose (NIFTY -Rs 13.9 k, BANKNIFTY -Rs 6.2 k).
4. **Market making (Avellaneda-Stoikov)** loses heavily on both indices,
   as on FYERS: adverse selection eats the quoted edge.
5. **Magnitude gate**: 48 gated tests, best t = 1.44 against a bar of 3.28.
   Nothing survives.
6. **Breakout (threshold)** on BANKNIFTY futures: NET Rs 5.71 L over 263
   trades, profit factor 1.29, but 36 points of slippage a fill wipes it
   out. Not a proven edge.

66 rows cannot be retrained on this data. Each one says why: no options
in it (pricers), no order book (microstructure), no stocks (stat arb,
factor, lead-lag, basket), no news or fundamentals, or the row is a
calculator or simulator with nothing to fit.

## All 108 rows

Curriculum rows show the average over the 15 tracks both runs share
(NIFTY and BANKNIFTY 1m, 5m, 15m, hourly, daily, 09:20-close, 10:15-close,
NIFTY FUT daily): accuracy before→after, and RMSE skill against the
random walk before→after (negative = worse than "no change").

| Family | Model | Status | Before (FYERS) → after (Drive) |
|---|---|---|---|
| Statistical alpha | Mean reversion | Retrained | Mean reversion: acc 50.4%→49.9%, skill vs RW -22.6%→-34.3%; Mean reversion (z-score band): acc 49.5%→48.7%, skill vs RW -16.8%→-34.5% |
| Statistical alpha | Momentum / trend following | Retrained | Momentum: acc 49.4%→49.9%, skill vs RW -22.3%→-34.3%; Momentum (tuned lookback): acc 49.9%→50.4%, skill vs RW -22.7%→-43.9% |
| Statistical alpha | Cointegration | Retrained | Pairs (cointegration): acc 49.8%→50.1%, skill vs RW -16.6%→-29.6% |
| Statistical alpha | Pairs trading | Retrained | Pairs (cointegration): acc 49.8%→50.1%, skill vs RW -16.6%→-29.6% |
| Statistical alpha | Statistical arbitrage | Not retrained | Needs the NIFTY 50 stock universe; the Drive index data has no stocks loaded (altair_resid_reversion). |
| Statistical alpha | Factor models (Fama-French, PCA) | Not retrained | Needs a stock cross-section; none in the new index data. |
| Statistical alpha | Relative value | Not retrained | Needs fundamentals; not in the new data. |
| Statistical alpha | Spread / calendar trading | Not retrained | Needs two live contract months side by side; the bars keep only the front month. Possible later from the raw tick files. |
| Statistical alpha | Regime detection | Retrained | altair_regimes on Drive daily (leak check passes): NIFTY 240 rows, BANKNIFTY 323; latest P(stress) 0.00 / 0.01 (end 2023). Curriculum k-means regimes: k-means regimes: acc 51.2%→51.5%, skill vs RW -0.2%→-2.7% |
| Machine learning | Linear / ridge regression | Retrained | Ridge regression: acc 50.4%→49.8%, skill vs RW -1.1%→-8.1%; Ridge (ARO-tuned): acc 50.4%→49.5%, skill vs RW -1.1%→-8.7% |
| Machine learning | Gradient boosting (GBDT / LightGBM) | Retrained | Gradient boosting: acc 50.6%→49.9%, skill vs RW -1.4%→-4.7% |
| Machine learning | Decision tree | Retrained | Decision tree: acc 50.3%→49.2%, skill vs RW -1.5%→-6.4% |
| Machine learning | MLP / feedforward network | Retrained | Neural net (MLP): acc 50.6%→50.2%, skill vs RW -5.3%→-24.6% |
| Machine learning | LSTM / GRU | Retrained | LSTM: acc 50.7%→49.6%, skill vs RW -0.9%→-3.2%; GRU: acc 50.8%→49.4%, skill vs RW -0.8%→-3.9% |
| Machine learning | Transformer / attention | Retrained | Transformer: acc 50.6%→49.9%, skill vs RW -1.4%→-5.0% |
| Machine learning | CNN | Retrained | CNN (trained, dilated causal): acc 50.6%→50.2%, skill vs RW -1.2%→-3.4% |
| Machine learning | Neuro-fuzzy (ANFIS) | Not retrained | Research header, not wired into any trainer; nothing to refit. |
| Machine learning | ARIMA + ANN hybrid | Retrained | ARIMA + ANN hybrid: acc 51.0%→50.9%, skill vs RW -2.6%→-6.3% |
| Machine learning | Artificial rabbits optimisation | Retrained | Ridge (ARO-tuned): acc 50.4%→49.5%, skill vs RW -1.1%→-8.7% |
| Time series & volatility | Parabolic SAR | Not retrained | Indicator with fixed parameters; no fit. |
| Time series & volatility | Gamma scalping P&L | Not retrained | Needs option prices; the new data has none. |
| Machine learning | Stochastic dominance test | Not retrained | A test, not a model. |
| Machine learning | Forecast accuracy tests | Used as the test | Used for every result here (DM, Bonferroni, thirds). |
| Machine learning | Random forest | Retrained | Random forest: acc 51.4%→51.2%, skill vs RW -0.1%→-0.9% |
| Machine learning | Logistic regression | Retrained | Logistic regression: acc 51.1%→51.5%, skill vs RW -0.3%→-1.1% |
| Machine learning | SVM / KNN / autoencoder | Retrained | SVM (RBF): acc 51.2%→50.9%, skill vs RW -21.8%→-33.6%; k-nearest neighbours: acc 50.7%→50.9%, skill vs RW -2.0%→-4.4%; Autoencoder + logistic: acc 51.1%→51.2%, skill vs RW -0.1%→-1.4% |
| Time series & volatility | AR (autoregression) | Retrained | AR(2): acc 50.9%→49.7%, skill vs RW -0.1%→-2.4% |
| Time series & volatility | ARMA / ARIMA / SARIMA | Retrained | ARMA(1,1): acc 50.8%→49.5%, skill vs RW -0.5%→-3.1%; Seasonal AR (SARIMA): acc 51.1%→49.8%, skill vs RW -0.1%→-1.7% |
| Time series & volatility | VAR (vector autoregression) | Retrained | VAR(1): acc 50.3%→49.0%, skill vs RW -0.3%→-2.1% |
| Time series & volatility | Kalman filter | Retrained | Kalman filter (drift): acc 49.7%→50.1%, skill vs RW -1.1%→-1.0% |
| Time series & volatility | Hidden Markov model | Retrained | Hidden Markov model: acc 50.0%→50.9%, skill vs RW -0.2%→-0.3% |
| Time series & volatility | HMM regime, walk-forward | Retrained | Same run as Regime detection (altair_regimes, leak check passes). |
| Time series & volatility | Markov regime chain | Retrained | Markov chain: acc 51.4%→50.8%, skill vs RW -0.0%→-1.2% |
| Time series & volatility | Ornstein-Uhlenbeck | Retrained | Ornstein-Uhlenbeck: acc 49.7%→48.2%, skill vs RW -0.8%→-9.8% |
| Time series & volatility | Brownian motion / GBM | Not retrained | Simulator, no fit. |
| Time series & volatility | Historical volatility & EWMA | Retrained | EWMA: 26-30% below RW, within ±2% of the 22-session mean on every timeframe (no edge over the plain mean). |
| Time series & volatility | HAR vol forecast, every timeframe | Retrained | HAR-seasonal size of next move: 32-35% below RW, 3-11% below the 22-session mean intraday (1m-60m); 2-4% WORSE than the mean on daily. In-sample = out-of-sample within 1-3%. No FYERS run on record yet (server nightly not pulled). |
| Time series & volatility | Tick-by-tick forecasts | Retrained | 39 M Drive ticks (main e58b189): HAR vol next 60 s/300 s 16-21% below last window; next-trade price edge is bid-ask bounce (not tradable). |
| Time series & volatility | GARCH / GJR-GARCH | Retrained | 80% move bands: GJR-GARCH was the best band on 7 of 67 FYERS track variants; on Drive it is best on none (Heston drift and seasonal vol beat it). |
| Time series & volatility | EGARCH | Retrained | 80% move bands: best on 2 track variants both before (FYERS) and after (Drive); 85.5% hit, 25% better score than a constant band. |
| Time series & volatility | Heston stochastic volatility | Retrained | 80% move bands (variance drift only; option calibration needs chains): best on 2 FYERS track variants before, 34 of 49 on Drive after, 25-39% better score than a constant band. |
| Time series & volatility | Hurst exponent | Retrained | Hurst regime switch: acc 50.9%→50.5%, skill vs RW -21.9%→-54.4% |
| Time series & volatility | Action-principle price bound | Not retrained | Research bound, no fit. |
| Microstructure & execution | Order-book imbalance | Not retrained | Needs bid/ask or order-book depth; the Drive files are trades only. Recorder depth only. |
| Microstructure & execution | Microprice | Not retrained | Needs bid/ask or order-book depth; the Drive files are trades only. Recorder depth only. |
| Microstructure & execution | VPIN | Not retrained | Needs bid/ask or order-book depth; the Drive files are trades only. Recorder depth only. |
| Microstructure & execution | Kyle's lambda | Not retrained | Needs bid/ask or order-book depth; the Drive files are trades only. Recorder depth only. |
| Microstructure & execution | Order flow imbalance (OFI) | Not retrained | Needs bid/ask or order-book depth; the Drive files are trades only. Recorder depth only. |
| Microstructure & execution | OBI trader | Not retrained | Needs bid/ask or order-book depth; the Drive files are trades only. Recorder depth only. |
| Microstructure & execution | Information diffusion (lead-lag) | Not retrained | Needs the bank stocks tick by tick; the Drive index set has none loaded. |
| Microstructure & execution | Diffusion trader | Not retrained | Needs the bank stocks tick by tick; the Drive index set has none loaded. |
| Microstructure & execution | Basket alpha (BANKNIFTY vs banks) | Not retrained | Needs the bank stocks tick by tick; the Drive index set has none loaded. |
| Microstructure & execution | OBI option quoter | Not retrained | Needs bid/ask or order-book depth; the Drive files are trades only. Recorder depth only. |
| Microstructure & execution | Slippage & market impact | Not retrained | Execution rule, not a forecast; needs our own fills to calibrate. |
| Microstructure & execution | TWAP / VWAP / POV | Not retrained | Execution rule, not a forecast; needs our own fills to calibrate. |
| Microstructure & execution | Almgren-Chriss | Not retrained | Execution rule, not a forecast; needs our own fills to calibrate. |
| Microstructure & execution | Avellaneda-Stoikov market making | Retrained | 1m futures, 823 sessions: NIFTY net -Rs 3.20 cr (Sharpe -29.9), BANKNIFTY -Rs 3.24 cr (Sharpe -28.9); adverse selection eats the quoted edge. Same verdict as FYERS: loses even before costs. |
| Microstructure & execution | HJB optimal execution | Not retrained | Execution rule, not a forecast; needs our own fills to calibrate. |
| Microstructure & execution | Execution with an alpha signal | Not retrained | Execution rule, not a forecast; needs our own fills to calibrate. |
| Microstructure & execution | Execution state model | Not retrained | Execution rule, not a forecast; needs our own fills to calibrate. |
| Microstructure & execution | Implementation shortfall | Not retrained | Execution rule, not a forecast; needs our own fills to calibrate. |
| Microstructure & execution | Queue position | Not retrained | Needs bid/ask or order-book depth; the Drive files are trades only. Recorder depth only. |
| Microstructure & execution | Probability queue model | Not retrained | Needs bid/ask or order-book depth; the Drive files are trades only. Recorder depth only. |
| Microstructure & execution | Fill probability | Not retrained | Needs bid/ask or order-book depth; the Drive files are trades only. Recorder depth only. |
| Microstructure & execution | Trade arrival (Hawkes) | Retrained | Covered by the tick tool's seconds-to-next-trade forecast (see Tick-by-tick). |
| Risk | VaR -- historical, parametric, Monte Carlo | Not retrained | Risk calculator over live positions; nothing to train. |
| Risk | Expected shortfall (CVaR) | Not retrained | Risk calculator over live positions; nothing to train. |
| Risk | Stress testing | Not retrained | Risk calculator over live positions; nothing to train. |
| Risk | Correlation & covariance (Ledoit-Wolf) | Not retrained | Risk calculator over live positions; nothing to train. |
| Risk | Drawdown | Not retrained | Risk calculator over live positions; nothing to train. |
| Risk | Beta / sector neutralisation | Not retrained | Risk calculator over live positions; nothing to train. |
| Risk | Factor risk model | Not retrained | Risk calculator over live positions; nothing to train. |
| Risk | Conformal risk control | Not retrained | Risk calculator over live positions; nothing to train. |
| Options & derivatives | Black-Scholes / Black-76 | Not retrained | Option pricer; the new data has no options (and FYERS serves no expired ones). |
| Options & derivatives | Greeks -- delta, gamma, vega, theta, rho | Not retrained | Option pricer; the new data has no options (and FYERS serves no expired ones). |
| Options & derivatives | Higher greeks -- vanna, volga, charm | Not retrained | Option pricer; the new data has no options (and FYERS serves no expired ones). |
| Options & derivatives | Implied volatility | Not retrained | Option pricer; the new data has no options (and FYERS serves no expired ones). |
| Options & derivatives | SVI volatility smile | Not retrained | Option pricer; the new data has no options (and FYERS serves no expired ones). |
| Options & derivatives | SABR | Not retrained | Option pricer; the new data has no options (and FYERS serves no expired ones). |
| Options & derivatives | Local volatility (Dupire) | Not retrained | Option pricer; the new data has no options (and FYERS serves no expired ones). |
| Options & derivatives | American approximation | Not retrained | Option pricer; the new data has no options (and FYERS serves no expired ones). |
| Options & derivatives | Monte Carlo option pricing | Not retrained | Option pricer; the new data has no options (and FYERS serves no expired ones). |
| Options & derivatives | India VIX replication | Not retrained | Option pricer; the new data has no options (and FYERS serves no expired ones). |
| Options & derivatives | Straddle seller IV >= RV | Half retrained | RV side retrained (HAR above). IV side needs option chains: still paper-only, unvalidated. |
| Options & derivatives | Binomial / trinomial tree | Not retrained | Option pricer; the new data has no options (and FYERS serves no expired ones). |
| Options & derivatives | Finite-difference PDE pricer | Not retrained | Option pricer; the new data has no options (and FYERS serves no expired ones). |
| Portfolio construction | Position sizing | Not retrained | Portfolio rule; needs several assets' returns, only two indices here. |
| Portfolio construction | Kelly criterion | Not retrained | Portfolio rule; needs several assets' returns, only two indices here. |
| Portfolio construction | Volatility targeting | Retrained | Fed by the HAR forecast above; nothing else to fit. |
| Portfolio construction | Risk parity | Not retrained | Portfolio rule; needs several assets' returns, only two indices here. |
| Portfolio construction | Minimum variance | Not retrained | Portfolio rule; needs several assets' returns, only two indices here. |
| Portfolio construction | Black-Litterman | Not retrained | Portfolio rule; needs several assets' returns, only two indices here. |
| Portfolio construction | Mean-variance optimisation | Not retrained | Portfolio rule; needs several assets' returns, only two indices here. |
| Alternative data | News / NLP / sentiment | Not retrained | Needs news/event data; none. |
| Alternative data | Event models | Not retrained | Needs news/event data; none. |
| Simulation | Monte Carlo | Not retrained | Simulator, no fit. |
| Simulation | Bootstrapping | Not retrained | Simulator, no fit. |
| Simulation | Jump diffusion | Not retrained | Simulator, no fit. |
| Simulation | Walk-forward & purged CV | Used as the test | Used by every run here; curriculum leak check clean on all tracks, trader leak check identical at every cut. |
| Simulation | Agent-based simulation | Not retrained | Simulator, no fit. |
| Reinforcement learning | Q-learning | Not retrained | Tabular regime RL; not wired into a trainer. The deep RL rows are retrained below. |
| Reinforcement learning | DQN / PPO / actor-critic | Retrained | DQN (reinforcement): acc 50.6%→50.0%, skill vs RW -22.2%→-44.1%; PPO (reinforcement): acc 50.5%→49.4%, skill vs RW -12.1%→-19.8%; Actor-critic (reinforcement): acc 50.5%→49.1%, skill vs RW -6.6%→-8.1% |
| Cross-cutting | Transaction cost calculator | Used as the test | Used for every net figure (config/charges.toml). |
| Cross-cutting | Model scorecards & drift | Not retrained | Watches live models; nothing to train. |
| Cross-cutting | Forecast scorecard | Used as the test | Used for every score here. |
| Cross-cutting | Ensemble aggregator | Retrained | Stack: acc 51.6%→50.3%, skill vs RW -0.1%→-1.5%; Vote: acc 51.1%→49.9%, skill vs RW -0.3%→-2.2%; Hedge: acc 51.6%→50.7%, skill vs RW -4.3%→-7.1%; Champion: acc 51.4%→50.7%, skill vs RW -12.8%→-16.9% |
| Cross-cutting | Feature registry | Not retrained | Registry; nothing to train. |
| Cross-cutting | DCF valuation | Not retrained | Needs fundamentals. |

## Files

- `curriculum_drive.txt`, `curriculum_leak_check.csv`: the full curriculum report on the Drive data.
- `vol_forecast_drive.csv`: HAR / EWMA / seasonal size forecasts, every timeframe.
- `trader_drive.txt`, `magnitude_drive.csv`, `as_mm_drive.txt`, `threshold_drive.txt`,
  `regimes_*_drive.csv`: the other trainers.
- Tick-by-tick: `../drive_ticks_tick_forecast_2026-11-11.csv`.
