# Avellaneda-Stoikov and the HMM regime: results so far (2026-10-28)

All runs use local `dataset/` data (TradingView exports). None of it is FYERS, so these are smoke tests only, not results.
Index spot is used as a stand-in for the future. Lot sizes are NIFTY 65 and BANKNIFTY 30. Every fill pays 3 bp.
gamma is 1e-4, the cap is 5 lots, and the book is flat by 15:20. All 9 runs passed the leak check.

| Series | Sessions | Fills (lots) | Net Rs | Net with no costs | Quoted edge (pts/fill) | 5-min markout (pts/fill) |
|---|---|---|---|---|---|---|
| BANKNIFTY spot 1m, 2015-2026 | 2,901 | 611,026 | -21.49 cr | -2.72 cr (895 of 2,901 sessions up) | 10.84 | -1.44 |
| ... with HMM regime | 2,901 | 599,432 | -21.02 cr | | 10.74 | -1.41 |
| NIFTY spot 1m, 2015-2026 | 2,901 | 630,050 | -22.29 cr | -3.58 cr (636 of 2,901 up) | 3.38 | -0.85 |
| ... with HMM regime | 2,901 | 627,282 | -22.17 cr | | 3.37 | -0.85 |
| NIFTY futures 1m (50 sessions) | 50 | 7,778 | -37.8 L | -0.81 L (27 of 50 up) | 5.61 | -0.13 |
| ... with HMM regime | 50 | 7,774 | -37.6 L | | 5.61 | -0.11 |

What it says:
- **Even with zero costs it loses on every series.** A quote that bars fill is a quote the price moved through (trade-through). By five minutes after the fill, the move has taken back the whole edge and more. This is adverse selection, and 1-minute bars cannot avoid it.
- With costs, it loses every single session.
- The HMM regime widens quotes the day after stress, which cuts the loss by about 2%. That doesn't change the verdict.

HMM regime walk-forward, latest rows (leak check passed on every series):
- NIFTY 1d: calm (P(stress) 0.05 on 2026-09-24).
- BANKNIFTY 1d: calm (0.09).
- NIFTY futures 1d: calm (0.01 on 2026-09-08).
- INDIA VIX 1d: stress (1.00). This is VIX's own regime, i.e. high vol of vol.

Still open: the same runs on FYERS 1-minute continuous futures (from 2019), which need a FYERS login on Smit's PC.
