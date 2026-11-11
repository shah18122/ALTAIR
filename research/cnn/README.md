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
