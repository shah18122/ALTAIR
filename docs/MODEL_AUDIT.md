# Model audit, 2026-11-11

Every ALTAIR model checked for two things: **where its data comes from** (the
rule in [DATA_POLICY.md](DATA_POLICY.md): FYERS only, never the NSE
bhavcopy, no silent fallback) and **whether its numbers are right**. The
accuracy figures are from the latest nightly run on the server (snapshot
`7e1ae57f`, cutoff 2026-11-10, FYERS candles only, leak check clean;
`altair-data/reports/2026-11-10/`).

## Fixed today

| Problem | Fix |
|---|---|
| Seven tools could still read the bhavcopy (`--source bhavcopy`, `--bhavcopy`, `--with-bhavcopy`), the snapshot could carry it, and three exchange files were committed. | Removed, with a CI scan and a snapshot-time refusal (DATA_POLICY.md). |
| Published results (variance premium 2015-2026, vol of vol, quantum smile, the curriculum's old IV tracks) were bhavcopy numbers still shown as findings, one on the desktop's verdict list. | Withdrawn from the docs and the desktop. |
| `altair_live_engine` fell back to `dataset/` (TradingView, not FYERS) for HAR, pairs, stat-arb and direction history when no FYERS history existed. Only the straddle sellers refused it. | It no longer falls back: no FYERS history means no history, and the models say why. |

## Data source of every model

| Model | Where | Data | Verdict |
|---|---|---|---|
| Forecast curriculum (about 33 models x 20 tracks, 1m to daily) | `altair_forecast_curriculum` | Nightly: FYERS snapshot only. Run by hand or by the desktop's Refresh daily call: `dataset/` by default. | Nightly is clean. The desktop path reads `dataset/` (TradingView merged with FYERS by `altair_dataset_update`): **conflict, see open item 1.** |
| altair_trader, Desk trader | `altair_trader`, `live/desk_trader.hpp` | The curriculum's logs; `dataset/` 5m bars for RV by default | Same conflict as above when run from the desktop. |
| Straddle IV>=RV NIFTY / BANKNIFTY | `live/straddle.hpp` | Live FYERS chain; FYERS history for RV; refuses stale or non-FYERS history | Clean. Rule matches (sell when forecast IV >= forecast RV, no margin). |
| Strangle 80%, books straddle | `live/models.hpp` | Live FYERS chain | Clean. Sell only. |
| Vol band (HAR), Pairs BANKNIFTY/NIFTY, Stat-arb NIFTY 50, Direction 10:15-14:15 | `live/models.hpp` | FYERS snapshot history (fixed today) | Clean now. |
| OBI A-D, OBI quoter, Order flow, Diffusion | `live/obi_*.hpp`, `live/diffusion.hpp` | Live FYERS 5/50-level books and prints | Clean. Not validated (thresholds unswept on enough sessions). |
| Cross-exchange arbitrage, OHL | `live/arbitrage.hpp`, `live/threshold.hpp` | Live FYERS NSE + BSE quotes | Clean. Their own switch plus LIVE sends real orders. Since #46 every model has a PAPER/LIVE switch, and option buys are refused for all of them. |
| Option arbitrage | `live/option_arb.hpp` | Live FYERS | Buys options to lock parity: **breaks "never buy options"**. Paper only since 2026-11-02 and the risk gate refuses opening buys, so it cannot trade. Keep it off. |
| Vol of vol | `altair_vol_of_vol` | Saved FYERS chains | Buys the ATM straddle: research only, never a trading model. |
| Variance premium (monthly straddle) | `altair_vol_premium` | Saved FYERS chains (or synthetic at VIX) | Clean. **Unvalidated**: about 2 weeks of chains so far. |
| Avellaneda-Stoikov, regimes, combos | `altair_as_mm`, `altair_regimes`, `altair_trader combos` | FYERS; non-FYERS only with `--not-fyers`, stamped "smoke test" | Clean. |
| Simulator seeds | `app/live_feed_sources.hpp` | `dataset/` closes to start a SIM day | Not trading data; allowed. |

## How good the forecasts are (FYERS, 2018/2019 to 2026-11-10)

Best model per track, return forecast against a random walk ("no change"),
out of sample, walk-forward:

| Track | Best RMSE skill vs random walk | Best direction accuracy | Pays its costs? |
|---|---|---|---|
| NIFTY / BANKNIFTY 1m | +0.13 % / +0.02 % | 51.6 % / 51.3 % | No (needs 120 % / 100 %) |
| 5m | 0.00 % / +0.01 % | 51.5 % / 52.6 % | No |
| 15m | 0.00 % / +0.02 % | 51.3 % / 51.6 % | No |
| hourly | +0.02 % / +0.12 % | 52.2 % / 54.1 % | No |
| daily | -0.03 % / +0.06 % | 53.8 % / 53.0 % | Marginal, not significant |
| INDIA VIX daily | +2.24 % | | (volatility, not price) |

Read plainly: **no model forecasts the price better than "no change"** on any
timeframe, by RMSE or MAE. That is what an efficient index looks like, and it
is not fixable by a bigger model: more depth and more parameters on these
inputs fit noise, which the walk-forward score then exposes. What does carry
skill is **volatility** (VIX, realised vol, implied vol), which is also what
the option-selling models need.

The "+ ivrv" tracks report large skill (up to +72 %) but on 1-6 days: they
start only when the FYERS chain IV starts (late October). The report already
marks them "too few forecasts to judge". Ignore them until they have 60+ days.

## Open items, in the order I would do them

1. **Desktop and manual runs read `dataset/`.** `altair_forecast_curriculum`,
   `altair_trader`, `altair_vol_premium`, `altair_band_option_demo`,
   `altair_magnitude`, `altair_threshold` and `altair_data_audit` default to
   `dataset/`. Make them default to the newest FYERS snapshot, as the live
   engine does, and refuse `dataset/` unless named.
2. **Forecast models for every timeframe, judged on RMSE/MAE** (Smit,
   2026-11-11). Forecast what can be forecast: realised volatility and the
   size of the move per timeframe, plus the 5-level depth features for the
   1m-15m tracks. Nested walk-forward (tune on the past only), Diebold-Mariano
   test against the random walk and against HAR, and a model is kept only if
   it beats both after the multiple-testing correction.
3. **The Google Drive tick data** (NIFTY / BANKNIFTY futures trades and NIFTY
   spot ticks, 2020, 2022, 2023) as a separate `drive-ticks` source: tick-level
   realised vol and trade-flow features for the years before ALTAIR's own
   recording. It has no options and no bid/ask.
4. The variance premium waits for FYERS chain history; nothing else can
   validate it.
