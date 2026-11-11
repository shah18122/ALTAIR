# Model audit, 2026-11-11

Every ALTAIR model checked for two things: **where its data comes from** (the
rule in [DATA_POLICY.md](DATA_POLICY.md): FYERS only, never the banned exchange
file, no silent fallback) and **whether its numbers are right**. The
accuracy figures are from the latest nightly run on the server (snapshot
`7e1ae57f`, cutoff 2026-11-10, FYERS candles only, leak check clean;
`altair-data/reports/2026-11-10/`).

## Fixed today

| Problem | Fix |
|---|---|
| Seven tools could still read the banned exchange file (an opt-in source option each), the snapshot could carry it, and three exchange files were committed. | Removed, with a CI scan and a snapshot-time refusal (DATA_POLICY.md). |
| Published results (variance premium 2015-2026, vol of vol, quantum smile, the curriculum's old IV tracks) were numbers from that file still shown as findings, one on the desktop's verdict list. | Withdrawn from the docs and the desktop. |
| `altair_live_engine` fell back to `dataset/` (TradingView, not FYERS) for HAR, pairs, stat-arb and direction history when no FYERS history existed. Only the straddle sellers refused it. | It no longer falls back: no FYERS history means no history, and the models say why. |

## Data source of every model

| Model | Where | Data | Verdict |
|---|---|---|---|
| Forecast curriculum (about 33 models x 20 tracks, 1m to daily) | `altair_forecast_curriculum` | Nightly: FYERS snapshot only. Run by hand or by the desktop's Refresh daily call: `dataset/` by default. | Clean: the newest FYERS snapshot by default (fixed today; it used to default to `dataset/`). |
| altair_trader, Desk trader | `altair_trader`, `live/desk_trader.hpp` | The curriculum's logs; `dataset/` 5m bars for RV by default | Clean: FYERS snapshot by default (fixed today). |
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

1. ~~Desktop and manual runs read `dataset/`.~~ **Fixed 2026-11-11:**
   `altair_forecast_curriculum`, `altair_trader`, `altair_vol_premium`,
   `altair_band_option_demo`, `altair_magnitude`, `altair_threshold` and
   `altair_resid_reversion` now default to the newest FYERS snapshot, else
   `data/store/fyers`, and stop when there is neither
   (`app/fyers_source.hpp` `pick_history`). `dataset/` is read only when
   named with `--dataset`, and is then printed as NOT FYERS.
   (`altair_data_audit` still audits `dataset/`: that is its job.)
2. **Forecast models for every timeframe, judged on RMSE/MAE** (Smit,
   2026-11-11). **Built:** `altair_vol_forecast` (`models/vol_forecast.hpp`)
   forecasts the size of the next move (|r| in bp) on 1m, 5m, 15m, 60m and
   1d for NIFTY and BANKNIFTY spot and futures, over the whole history:
   random walk, 22-session mean, EWMA, time-of-day seasonal, HAR-seasonal
   ridge (with INDIA VIX) and an error-weighted combination. All walk
   forward; Diebold-Mariano against the random walk and the 22-session mean,
   Bonferroni over every test, skill required in each third of the period,
   and the HAR fit's in-sample error compared with its real forecasts' error
   on the same rows (equal means it learned no noise). The nightly runs it on
   the FYERS snapshot (`runs/<day>/vol_forecast.txt`, pushed to altair-data).
   A smoke run on `dataset/` (not FYERS, so not evidence) gave the shape:
   HAR-seasonal has the lowest RMSE on every timeframe, about 35 % below the
   random walk and 11-18 % below the plain average intraday (significant),
   but only about 1.5 % below the average on daily bars (not significant);
   in-sample and out-of-sample errors agree within 1-2 %.
   Next: the 5-level depth features on 1m-15m, and feeding the forecast to
   the straddle sellers' RV side.
3. **The Google Drive tick data** (NIFTY / BANKNIFTY futures trades and NIFTY
   spot ticks, 2020, 2022, 2023) as a separate `drive-ticks` source, NOT
   FYERS. **Built 2026-11-11:** `altair_tick_forecast --ticks DIR`
   (`models/tick_forecast.hpp`) forecasts trade by trade on the raw ticks, no
   bars: the next trade's return, the direction of the next price change, the
   seconds to the next trade, and the realised vol of the next 60 s and 300 s
   from every tick. Walk-forward, ridge fits on past rows only, batch-means
   Diebold-Mariano, Bonferroni, thirds, in-sample vs out-of-sample on the same
   rows. The simulator replays a tick file: `altair_price_service
   --replay-ticks FILE [--date D] [--speed N]`. `tools/drive_ticks_to_bars.py`
   also makes bars for the bar models. No options and no bid/ask in it, so
   no depth features.

   **First run, 2026-11-11** (on Smit's PC with `tools/tick_forecast.py`, the
   Python twin; 72 NIFTY/BANKNIFTY futures contract files 2020/2022/2023
   stitched front-month, 823 sessions; NIFTY spot 740 sessions; 39 M ticks;
   full table `docs/research/drive_ticks_tick_forecast_2026-11-11.csv`).
   Bonferroni bar |t| >= 3.24 over 42 tests, plus better in every third:

   | Stream | Next-trade return: AR vs RW RMSE | Next change reverses | Gap: best vs mean | Vol next 60 s: HAR vs last | Vol next 300 s |
   |---|---|---|---|---|---|
   | NIFTY FUT (12.6 M ticks) | +4.3 % (t -7.4) | 59.9 % | EWMA +14.4 % (t -5.8) | +19.0 % (t -1.9) | +20.7 % (t -1.5) |
   | BANKNIFTY FUT (9.9 M) | +2.4 % (t -8.4) | 58.2 % | EWMA +16.2 % (t -4.6) | +17.6 % (t -4.5, significant) | +16.0 % (t -2.2) |
   | NIFTY spot (16.7 M) | +1.1 % (t -23.7) | 58.0 % | none | +19.1 % (t -2.2) | +19.6 % (t -1.7) |

   In-sample and out-of-sample RMSE are equal to 3-4 digits for every ridge:
   nothing is overfitted. Read plainly: the trade-by-trade price "edge" is the
   bid-ask bounce (a print at the ask tends to be followed by one at the bid),
   which a taker cannot capture; it is not a trading signal. What is usable is
   the volatility of the next minute to five minutes (HAR lowest RMSE on every
   stream, ~16-21 % below "same as last window" in every third; the block DM
   t-stats fall short of the strict bar outside BANKNIFTY 60 s because of the
   fat tails), and the arrival-rate model for execution timing.
4. The variance premium waits for FYERS chain history; nothing else can
   validate it.
4. **Every Model Atlas row against the new data** (Smit, 2026-11-11: "update
   all the model based on new data, every single model in model atlas").
   Done for all 108 rows: `docs/research/atlas_retrain_2026-11-11/README.md`
   (before = FYERS nightly 2026-11-10, after = Drive ticks). 37 retrained,
   4 are the test harness itself, 1 half (straddle: RV side only), 66 cannot
   be retrained on that data and each says why (no options, no order book,
   no stocks, no news, or nothing to fit). Result: still no direction edge
   on price at any timeframe; HAR size forecasts hold (32-35 % below the
   random walk intraday). Each row's ⓘ in the Atlas shows its line
   (`desktop/atlas_retrain.hpp`, checked by `altair_atlas_test`).
   `app/fyers_source.hpp` no longer labels a SOURCE.txt saying "NOT FYERS"
   as FYERS.
