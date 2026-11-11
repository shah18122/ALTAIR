# Advanced signals on BANKNIFTY futures, walk-forward out of sample

Data: Drive ticks (source drive-ticks, NOT FYERS), 247 days 20220103..20221230, front-month BANKNIFTY future + 12 bank constituents, 1-s grid, rows every 5 s 09:20-15:20, entry 1 s after the signal. Ridge refitted before every day on all earlier days; first 40 days never scored (207 test days).


## Next 10 s return of the future (bp, std 2.2)

| signal(s) | OOS R2 vs no change | mean daily IC | IC t-stat | days IC>0 | trades with |forecast| > 6 bp | net bp/trade | gross bp, top 1% forecasts | gross bp, top 0.1% |
|---|---|---|---|---|---|---|---|---|
| rev10+rev60 | +0.023% | +0.0221 | +10.5 | 78% | 0 | +nan | +0.51 | +1.63 |
| misprice | +0.186% | +0.0686 | +26.5 | 97% | 0 | +nan | -0.17 | +0.57 |
| leadgap10 | +0.798% | +0.1043 | +37.2 | 100% | 0 | +nan | +0.67 | +2.51 |
| leadgap30 | +0.861% | +0.1055 | +37.2 | 100% | 0 | +nan | +1.20 | +2.58 |
| breadth | +0.035% | +0.0222 | +13.0 | 84% | 0 | +nan | -0.31 | -0.76 |
| flow60 | +0.006% | +0.0112 | +7.2 | 71% | 0 | +nan | +0.22 | +0.46 |
| flow300 | +0.007% | +0.0136 | +9.3 | 76% | 0 | +nan | +0.09 | +0.08 |
| oi | -0.001% | -0.0008 | -0.5 | 54% | 0 | +nan | -0.02 | -0.07 |
| stockflow | +0.009% | +0.0111 | +7.9 | 70% | 0 | +nan | +0.04 | +0.35 |
| rev10+rev60+misprice | +0.193% | +0.0674 | +26.2 | 97% | 0 | +nan | -0.15 | +0.63 |
| rev10+rev60+leadgap10 | +1.175% | +0.1239 | +43.4 | 100% | 0 | +nan | +0.83 | +1.29 |
| rev10+rev60+leadgap30 | +0.943% | +0.1084 | +41.0 | 100% | 0 | +nan | +1.00 | +2.10 |
| rev10+rev60+breadth | +0.286% | +0.0606 | +35.2 | 100% | 0 | +nan | +0.55 | +1.81 |
| rev10+rev60+flow60 | +0.023% | +0.0221 | +10.5 | 77% | 0 | +nan | +0.52 | +1.55 |
| rev10+rev60+flow300 | +0.025% | +0.0231 | +11.4 | 78% | 0 | +nan | +0.61 | +1.61 |
| rev10+rev60+oi | +0.026% | +0.0234 | +10.9 | 79% | 0 | +nan | +0.57 | +2.34 |
| rev10+rev60+stockflow | +0.034% | +0.0241 | +12.3 | 78% | 0 | +nan | +0.40 | +2.73 |
| ALL | +1.661% | +0.1464 | +45.4 | 100% | 0 | +nan | +1.13 | +1.95 |

## Next 60 s return of the future (bp, std 5.1)

| signal(s) | OOS R2 vs no change | mean daily IC | IC t-stat | days IC>0 | trades with |forecast| > 6 bp | net bp/trade | gross bp, top 1% forecasts | gross bp, top 0.1% |
|---|---|---|---|---|---|---|---|---|
| rev10+rev60 | +0.040% | +0.0351 | +9.6 | 75% | 0 | +nan | +0.93 | +6.75 |
| misprice | +0.037% | +0.0461 | +11.1 | 77% | 0 | +nan | -1.67 | +1.07 |
| leadgap10 | +0.177% | +0.0501 | +32.1 | 100% | 0 | +nan | +0.89 | +4.21 |
| leadgap30 | +0.177% | +0.0492 | +23.1 | 95% | 0 | +nan | +1.74 | +5.15 |
| breadth | -0.009% | +0.0075 | +2.4 | 56% | 0 | +nan | -0.32 | -1.28 |
| flow60 | +0.021% | +0.0254 | +7.9 | 72% | 0 | +nan | -0.03 | +0.45 |
| flow300 | +0.025% | +0.0305 | +8.5 | 73% | 0 | +nan | +0.20 | -0.54 |
| oi | -0.005% | +0.0028 | +0.8 | 51% | 0 | +nan | -0.16 | -1.40 |
| stockflow | -0.004% | +0.0075 | +2.8 | 53% | 0 | +nan | +0.49 | +2.85 |
| rev10+rev60+misprice | +0.060% | +0.0482 | +12.6 | 81% | 0 | +nan | +0.31 | +2.33 |
| rev10+rev60+leadgap10 | +0.222% | +0.0598 | +25.5 | 97% | 1 | +3.36 | +1.68 | +2.80 |
| rev10+rev60+leadgap30 | +0.187% | +0.0541 | +20.4 | 93% | 0 | +nan | +2.21 | +4.48 |
| rev10+rev60+breadth | +0.072% | +0.0379 | +12.7 | 87% | 0 | +nan | +1.02 | +4.97 |
| rev10+rev60+flow60 | +0.041% | +0.0355 | +9.7 | 76% | 0 | +nan | +0.91 | +6.74 |
| rev10+rev60+flow300 | +0.055% | +0.0414 | +10.9 | 78% | 0 | +nan | +0.91 | +8.16 |
| rev10+rev60+oi | +0.048% | +0.0372 | +10.0 | 76% | 0 | +nan | +1.03 | +7.48 |
| rev10+rev60+stockflow | +0.047% | +0.0383 | +10.2 | 73% | 0 | +nan | +1.08 | +7.30 |
| ALL | +0.314% | +0.0748 | +23.4 | 96% | 0 | +nan | +1.37 | +3.61 |

## Next 300 s return of the future (bp, std 11.0)

| signal(s) | OOS R2 vs no change | mean daily IC | IC t-stat | days IC>0 | trades with |forecast| > 6 bp | net bp/trade | gross bp, top 1% forecasts | gross bp, top 0.1% |
|---|---|---|---|---|---|---|---|---|
| rev10+rev60 | +0.014% | +0.0361 | +9.1 | 75% | 0 | +nan | +0.19 | +3.06 |
| misprice | -0.038% | +0.0023 | +0.3 | 52% | 0 | +nan | -0.93 | -2.55 |
| leadgap10 | +0.019% | +0.0251 | +27.3 | 99% | 0 | +nan | +0.75 | +5.34 |
| leadgap30 | +0.022% | +0.0263 | +17.6 | 87% | 1 | +23.63 | +1.54 | +9.57 |
| breadth | -0.012% | +0.0220 | +6.7 | 68% | 0 | +nan | -0.78 | +1.83 |
| flow60 | +0.008% | +0.0308 | +7.8 | 71% | 0 | +nan | +0.65 | +1.40 |
| flow300 | +0.049% | +0.0549 | +8.2 | 69% | 0 | +nan | +0.36 | -0.17 |
| oi | -0.028% | -0.0061 | -0.9 | 46% | 0 | +nan | -1.72 | -1.97 |
| stockflow | -0.022% | +0.0054 | +1.7 | 57% | 0 | +nan | -0.73 | -0.80 |
| rev10+rev60+misprice | -0.018% | +0.0306 | +7.1 | 69% | 0 | +nan | -0.46 | -2.53 |
| rev10+rev60+leadgap10 | +0.042% | +0.0394 | +15.3 | 85% | 1 | +23.63 | +1.51 | +3.01 |
| rev10+rev60+leadgap30 | +0.035% | +0.0377 | +14.1 | 86% | 2 | +22.15 | +1.63 | +7.52 |
| rev10+rev60+breadth | -0.001% | +0.0364 | +9.2 | 75% | 0 | +nan | -0.63 | -1.97 |
| rev10+rev60+flow60 | +0.016% | +0.0372 | +9.1 | 73% | 0 | +nan | -0.17 | +3.16 |
| rev10+rev60+flow300 | +0.061% | +0.0590 | +9.0 | 73% | 0 | +nan | -0.17 | +2.84 |
| rev10+rev60+oi | +0.008% | +0.0349 | +8.2 | 71% | 0 | +nan | +0.82 | +4.71 |
| rev10+rev60+stockflow | +0.017% | +0.0419 | +10.6 | 78% | 0 | +nan | +0.05 | +1.90 |
| ALL | +0.060% | +0.0667 | +11.1 | 79% | 8 | -17.48 | -1.31 | -0.98 |

## Staleness check: only rows where the future printed at t and at t+1 s

| horizon | signal | OOS R2 | mean daily IC | IC t-stat | gross bp, top 1% |
|---|---|---|---|---|---|
| 10 s | leadgap10 | -37.271% | +0.0240 | +4.0 | +0.75 |
| 10 s | rev10+rev60+leadgap10 | -20.108% | +0.0560 | +6.7 | +0.24 |
| 10 s | ALL | -13.461% | +0.0546 | +7.6 | +3.24 |
| 60 s | leadgap10 | -154.474% | +0.0177 | +3.5 | +4.26 |
| 60 s | rev10+rev60+leadgap10 | -73.716% | +0.0414 | +3.8 | +3.28 |
| 60 s | ALL | -48.237% | +0.0463 | +4.0 | +11.16 |

## Next 5 min realised vol of the future (bp, mean 10.8)

| model | OOS R2 vs HAR | mean daily IC | IC t-stat | days IC>0 |
|---|---|---|---|---|
| HAR (baseline; R2 here vs train mean) | +73.86% | +0.407 | +36.1 | 99% |
| HAR + intensity | +0.07% | +0.411 | +36.5 | 100% |
| HAR + dispersion | -0.02% | +0.408 | +36.2 | 100% |
| HAR + corr | +0.30% | +0.405 | +35.4 | 99% |
| HAR + vpin | +0.13% | +0.411 | +36.4 | 99% |
| HAR + absmis | +0.37% | +0.413 | +36.9 | 99% |
| HAR + all five | +0.89% | +0.416 | +36.9 | 100% |
