# Results, 2026-11-27 (run on Smit's PC, 9 min)

Data: 12 bank stocks + front BANKNIFTY future, 2020 (251 days), 2022 (247), 2023 (241, 29 Dec missing:
no Jan-2024 contract in the Drive folder). After the 60 warm-up days: dev 438 scored days, holdout 241.
Selftest (random walk) passed nothing first. Full table: `results/summary.csv` (amounts in bp).

| Split | Horizon | Variant | Trades | Hit | Avg win | Avg loss | Gross bp | Net bp | t (daily) |
|---|---|---|---|---|---|---|---|---|---|
| dev | 60 s | primary | 2,230 | 46% | 16.2 | -21.2 | +1.9 | -4.1 | -3.2 |
| dev | 60 s | + meta | 290 | 49% | 12.7 | -19.4 | +2.3 | -3.7 | -3.0 |
| holdout | 60 s | both | 0 | | | | | | |
| dev | 300 s | primary | 3,571 | 43% | 21.4 | -26.3 | +0.3 | -5.7 | -6.9 |
| dev | 300 s | + meta | 1,044 | 43% | 17.2 | -23.2 | +0.4 | -5.6 | -5.6 |
| holdout | 300 s | primary | 22 | 50% | 24.9 | -25.5 | +5.7 | -0.3 | -0.05 |
| holdout | 300 s | + meta | 15 | 47% | 35.4 | -31.6 | +5.7 | -0.3 | -0.04 |

**Verdict: nothing passes; "no change" (no trade) wins.** The 300 s forecast carries information
(rank IC 0.05-0.06 in dev and holdout), but the gross edge is 0.3-2.3 bp a trade against a 6 bp round
trip, so net is -3.7 to -5.7 bp with average losses larger than average wins at a ~45% hit rate. The
meta-label cuts trades 70-87% and nudges net per trade up, but never above zero, so it is not kept.
In 2023 the model almost never finds a trade it expects to clear costs. Same conclusion as the ridge
study in research/signals/ and the CNN: real but tiny microstructure signal, not tradable as a taker.
Per the pre-registration, DeepLOB/TFT are not tried. OFI, queue imbalance and microprice still need
~60 recorded FYERS book sessions (`research/signals/obi_study.py`).
