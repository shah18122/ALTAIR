# Can ALTAIR's CNN make money? (2026-11-11)

Model: the curriculum CNN (`models/curriculum.hpp` CurriculumCnn, `models/trainable_cnn.hpp`
TrainableTcn<4,8,3,3,16>): 3 dilated causal convolutions over the last 16 bars, logistic
p(up) for the next bar. Re-implemented in numpy in `cnn_test.py` (gradient-checked).

Data: Drive tick bars (source drive-ticks, NOT FYERS), NIFTY fut, BANKNIFTY fut, NIFTY spot,
2020/2022/2023, 1m 5m 15m 60m 1d. Walk-forward: each month refit from scratch on the past only.
Costs: 6 bp round trip on turnover, intraday flat at the close. Full table: `results/summary.csv`.

## Verdict: no. The CNN has no tradable direction edge.

- Return RMSE vs "no change": worse on 14 of 15 series/timeframes (best +0.07%, NIFTY spot 1m, not tradable).
- Gross edge per bar traded: 0.0 to 0.3 bp intraday, against 6 bp cost. Loses after costs on every
  intraday series/timeframe, including only its most confident 10% of calls (about -6 bp per trade,
  daily t -2 to -80). It would lose even at 2 bp cost.
- Daily top-10% calls show +8 to +25 bp per trade on 41-46 trades, t 0.5-1.5: noise after 30 tests.
- AR(5) on returns is no better. Neither beats "no change", matching docs/MODEL_AUDIT.md.

## Where the CNN does something: move size

Predicting "next |return| above median" (AUC, 0.5 = coin): CNN 0.67-0.69 at 1m/5m, a tie with or slightly
better than HAR + time of day (0.64-0.70); HAR wins at 15m, 60m and 1d (0.58-0.70 vs 0.51-0.67).
So the CNN is at best a size/volatility input for the option-selling gate, not a direction trader.

    python research/cnn/cnn_test.py --bars data/drive_ticks --out research/cnn/results

# Combining the CNN with every other signal (`combo.py`, 2026-11-12)

Smit: "finish the CNN work by finding a combination of models and understanding how the market
works to generate the best alpha". 5-minute bars, NIFTY and BANKNIFTY front futures, 2020/2022/2023.
29 features: CNN p(up) and p(big move) (walk-forward), own and other-index returns over 5-60 min and
since the open, overnight gap, NIFTY futures-minus-spot basis and its changes, HAR move size, bar range
and close location, volume, open-interest change and OI x trend, time of day, weekday, days to the
monthly expiry. Combined by ridge, gradient-boosted trees and their average, refit monthly on the past
only. Horizons 5, 15, 30, 60 min and to the close. 6 bp round trip. Tables: `results/combo_summary.csv`,
`results/combo_ic.csv`.

## Verdict: no combination makes money after costs

All 90 tests (2 futures x 5 horizons x 3 models x 3 trading rules) lose after 6 bp. The best gross
edges are 2-5 bp per trade, never steady across years; out-of-sample IC is at most 0.04.
Most confident 10%: -1.4 to -11 bp net per trade. Trading only when HAR expects a big move does
not help. Adding the CNN changes nothing: its p(up) has IC under 0.01 and p(big) carries no direction.

## How the market works (signals with the same sign in 2020, 2022 and 2023)

| Effect | IC range by year | What it means |
|---|---|---|
| Move since the open continues to the close (both indices) | +0.035 to +0.09 | intraday trend days persist |
| NIFTY trend confirmed by rising OI continues to the close | +0.035 to +0.13 | new positions behind a move = trend day |
| BANKNIFTY follows NIFTY's day move to the close | +0.037 to +0.074 | the market leads the bank index |
| Last 5-15 min move and close location reverse next 5 min | -0.015 to -0.07 | short-term noise mean reverts |
| NIFTY futures-vs-spot basis jump reverses next 5 min | -0.018 to -0.031 | futures overshoot, spot catches up |
| Later in the day, less left to the close | time of day | most of the day's move is done by midday |

These are real but small: each is worth about 1-5 bp a trade, below the 6 bp a futures round trip
costs. They are more useful as risk filters for the option-selling strategy (S1, `research/strategy/`):
a day with a big move since the open and rising OI is a trend day, the day a short straddle loses most.

# Rerun on the merged dataset: 11.8 years (2026-11-12)

Data: altair-data `merged/` first build (c72aa19, `tools/merge_bars.py`): NIFTY and BANKNIFTY index
plus India VIX, 5-minute bars 2015-01 to 2026-11 (TradingView `dataset` + FYERS since 2026-11-04;
flagged bars dropped). Index, not futures: this measures whether price can be forecast, not tradable P&L.
Added VIX features (level, move since open, 5/30-min change, VIX minus HAR realised). Refit every
quarter on the past only. `python research/cnn/combo.py --bars <altair-data>/merged --segment spot --fold quarter`.
Tables: `results/combo_summary_spot_quarter.csv`, `results/combo_ic_spot_quarter.csv`.

## Price forecast: still no usable skill

- Best RMSE skill vs "no change": +0.1% (NIFTY, 15 min, ridge+GBM average). Most are negative.
- No trading rule passes after 6 bp. The best, NIFTY 30-min ridge on high-vol bars: +6.7 bp/trade net,
  215 trades in 11 years, t 1.2, positive in 9 of 11 years. That is not significant after 90 tests
  (needs t >= 3.2), and it trades once every ~3 weeks.

## What 12 years confirm (same sign every year, 12/12)

| Effect | median IC |
|---|---|
| NIFTY move since the open continues to the close | +0.076 |
| BANKNIFTY follows NIFTY's day move to the close (and NIFTY follows BANKNIFTY) | +0.056 / +0.075 |
| Last 5-15 min BANKNIFTY move and close location reverse next 5-15 min | -0.024 to -0.035 |
| The CNN's p(up) for BANKNIFTY next 5 min | +0.026 |
| VIX rising since the open -> NIFTY lower into the close (10 of 12 years) | -0.057 |

The CNN does carry a real but tiny next-5-minute signal (12 of 12 years), worth well under 1 bp.
