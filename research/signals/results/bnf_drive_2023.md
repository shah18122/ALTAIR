# Advanced signals on BANKNIFTY futures, walk-forward out of sample

Data: Drive ticks (source drive-ticks, NOT FYERS), 241 days 20230102..20231228, front-month BANKNIFTY future + 12 bank constituents, 1-s grid, rows every 5 s 09:20-15:20, entry 1 s after the signal. Ridge refitted before every day on all earlier days; first 40 days never scored (201 test days).


## Next 10 s return of the future (bp, std 1.5)

| signal(s) | OOS R2 vs no change | mean daily IC | IC t-stat | days IC>0 | trades with |forecast| > 6 bp | net bp/trade | gross bp, top 1% forecasts | gross bp, top 0.1% |
|---|---|---|---|---|---|---|---|---|
| rev10+rev60 | +0.091% | +0.0428 | +16.1 | 89% | 0 | +nan | +0.14 | +0.55 |
| misprice | +0.044% | +0.0476 | +20.0 | 96% | 0 | +nan | +0.12 | +0.09 |
| leadgap10 | +1.196% | +0.1209 | +38.3 | 99% | 0 | +nan | +0.40 | +0.63 |
| leadgap30 | +1.025% | +0.1126 | +38.9 | 100% | 0 | +nan | +0.49 | +1.35 |
| breadth | +0.063% | +0.0267 | +16.6 | 91% | 0 | +nan | +0.03 | -0.05 |
| flow60 | -0.001% | +0.0106 | +6.7 | 68% | 0 | +nan | -0.05 | -0.45 |
| flow300 | -0.004% | +0.0077 | +5.0 | 64% | 0 | +nan | +0.06 | -0.07 |
| oi | -0.026% | -0.0006 | -0.5 | 49% | 0 | +nan | -0.04 | +0.03 |
| stockflow | +0.024% | +0.0157 | +9.7 | 77% | 0 | +nan | +0.03 | +0.03 |
| rev10+rev60+misprice | +0.079% | +0.0534 | +20.1 | 95% | 0 | +nan | +0.31 | +0.09 |
| rev10+rev60+leadgap10 | +1.500% | +0.1329 | +43.1 | 100% | 0 | +nan | +0.56 | +0.52 |
| rev10+rev60+leadgap30 | +0.965% | +0.1093 | +40.7 | 100% | 0 | +nan | +0.52 | +1.33 |
| rev10+rev60+breadth | +0.383% | +0.0676 | +33.9 | 98% | 0 | +nan | +0.27 | +0.89 |
| rev10+rev60+flow60 | +0.086% | +0.0408 | +15.5 | 86% | 0 | +nan | +0.14 | +0.44 |
| rev10+rev60+flow300 | +0.088% | +0.0419 | +15.9 | 86% | 0 | +nan | +0.14 | +0.56 |
| rev10+rev60+oi | +0.078% | +0.0455 | +17.0 | 89% | 0 | +nan | +0.15 | +0.45 |
| rev10+rev60+stockflow | +0.108% | +0.0394 | +16.0 | 86% | 0 | +nan | +0.15 | +0.66 |
| ALL | +1.933% | +0.1509 | +44.9 | 100% | 0 | +nan | +0.68 | +1.08 |

## Next 60 s return of the future (bp, std 3.3)

| signal(s) | OOS R2 vs no change | mean daily IC | IC t-stat | days IC>0 | trades with |forecast| > 6 bp | net bp/trade | gross bp, top 1% forecasts | gross bp, top 0.1% |
|---|---|---|---|---|---|---|---|---|
| rev10+rev60 | +0.073% | +0.0399 | +9.4 | 74% | 0 | +nan | +0.69 | +2.42 |
| misprice | -0.014% | +0.0240 | +5.5 | 68% | 0 | +nan | +0.01 | +0.88 |
| leadgap10 | +0.242% | +0.0587 | +29.0 | 96% | 0 | +nan | +0.52 | +0.47 |
| leadgap30 | +0.118% | +0.0494 | +16.5 | 90% | 0 | +nan | +0.38 | +1.69 |
| breadth | +0.001% | +0.0126 | +3.7 | 58% | 0 | +nan | -0.28 | +0.31 |
| flow60 | -0.022% | +0.0175 | +5.2 | 64% | 0 | +nan | +0.18 | -0.07 |
| flow300 | -0.032% | +0.0142 | +3.7 | 56% | 0 | +nan | +0.02 | -0.65 |
| oi | -0.280% | -0.0008 | -0.2 | 46% | 27 | -7.10 | -0.33 | +0.12 |
| stockflow | +0.038% | +0.0207 | +6.9 | 70% | 0 | +nan | +0.10 | -0.60 |
| rev10+rev60+misprice | +0.050% | +0.0373 | +8.7 | 72% | 0 | +nan | +0.77 | +1.85 |
| rev10+rev60+leadgap10 | +0.305% | +0.0686 | +21.7 | 94% | 0 | +nan | +0.64 | +1.33 |
| rev10+rev60+leadgap30 | +0.099% | +0.0545 | +14.3 | 84% | 0 | +nan | +0.60 | +2.31 |
| rev10+rev60+breadth | +0.068% | +0.0420 | +9.9 | 75% | 0 | +nan | +0.68 | +2.70 |
| rev10+rev60+flow60 | +0.059% | +0.0378 | +9.1 | 73% | 0 | +nan | +0.73 | +1.80 |
| rev10+rev60+flow300 | +0.052% | +0.0394 | +9.2 | 75% | 0 | +nan | +0.63 | +1.82 |
| rev10+rev60+oi | -0.209% | +0.0413 | +9.8 | 74% | 27 | -7.10 | +0.56 | +1.15 |
| rev10+rev60+stockflow | +0.094% | +0.0437 | +9.9 | 75% | 0 | +nan | +0.74 | +2.94 |
| ALL | +0.304% | +0.0728 | +19.2 | 92% | 0 | +nan | +0.84 | +3.13 |

## Next 300 s return of the future (bp, std 7.1)

| signal(s) | OOS R2 vs no change | mean daily IC | IC t-stat | days IC>0 | trades with |forecast| > 6 bp | net bp/trade | gross bp, top 1% forecasts | gross bp, top 0.1% |
|---|---|---|---|---|---|---|---|---|
| rev10+rev60 | -0.003% | +0.0361 | +8.6 | 75% | 0 | +nan | +0.18 | -4.05 |
| misprice | -0.051% | +0.0300 | +3.4 | 58% | 0 | +nan | +2.44 | +6.93 |
| leadgap10 | +0.022% | +0.0258 | +20.4 | 94% | 0 | +nan | +0.58 | +0.57 |
| leadgap30 | -0.014% | +0.0168 | +7.6 | 75% | 0 | +nan | -0.62 | -2.14 |
| breadth | +0.012% | +0.0321 | +9.6 | 77% | 0 | +nan | -1.16 | -0.44 |
| flow60 | -0.054% | +0.0165 | +3.7 | 59% | 0 | +nan | -0.32 | -1.69 |
| flow300 | -0.135% | +0.0157 | +2.2 | 57% | 0 | +nan | -1.55 | -6.35 |
| oi | -1.667% | +0.0086 | +1.3 | 54% | 243 | -5.05 | +0.18 | +0.22 |
| stockflow | -0.012% | +0.0157 | +4.6 | 64% | 0 | +nan | +0.01 | +0.06 |
| rev10+rev60+misprice | -0.042% | +0.0320 | +6.7 | 74% | 0 | +nan | +1.24 | -1.00 |
| rev10+rev60+leadgap10 | +0.010% | +0.0386 | +11.3 | 78% | 0 | +nan | +0.24 | -5.86 |
| rev10+rev60+leadgap30 | -0.023% | +0.0358 | +8.5 | 76% | 0 | +nan | +0.30 | -9.21 |
| rev10+rev60+breadth | -0.000% | +0.0369 | +9.4 | 74% | 0 | +nan | +0.59 | -8.84 |
| rev10+rev60+flow60 | -0.030% | +0.0295 | +6.8 | 70% | 0 | +nan | -0.45 | -3.29 |
| rev10+rev60+flow300 | -0.106% | +0.0308 | +4.9 | 63% | 0 | +nan | -1.76 | -2.74 |
| rev10+rev60+oi | -1.685% | +0.0368 | +7.3 | 69% | 243 | -5.05 | +0.49 | +0.47 |
| rev10+rev60+stockflow | -0.003% | +0.0371 | +8.4 | 74% | 0 | +nan | +0.65 | +2.86 |
| ALL | -0.024% | +0.0427 | +11.8 | 80% | 0 | +nan | +0.05 | +2.23 |

## Staleness check: only rows where the future printed at t and at t+1 s

| horizon | signal | OOS R2 | mean daily IC | IC t-stat | gross bp, top 1% |
|---|---|---|---|---|---|
| 10 s | leadgap10 | +0.027% | +0.0242 | +6.0 | +0.02 |
| 10 s | rev10+rev60+leadgap10 | +0.135% | +0.0529 | +12.8 | +0.03 |
| 10 s | ALL | +0.185% | +0.0568 | +13.4 | +0.55 |
| 60 s | leadgap10 | -0.002% | +0.0162 | +5.0 | +0.08 |
| 60 s | rev10+rev60+leadgap10 | +0.049% | +0.0378 | +6.5 | +0.33 |
| 60 s | ALL | -0.050% | +0.0402 | +6.1 | +1.38 |

## Next 5 min realised vol of the future (bp, mean 7.4)

| model | OOS R2 vs HAR | mean daily IC | IC t-stat | days IC>0 |
|---|---|---|---|---|
| HAR (baseline; R2 here vs train mean) | +75.85% | +0.378 | +35.2 | 100% |
| HAR + intensity | +0.21% | +0.381 | +35.5 | 100% |
| HAR + dispersion | -0.97% | +0.377 | +34.9 | 100% |
| HAR + corr | -0.01% | +0.376 | +34.8 | 100% |
| HAR + vpin | +0.11% | +0.379 | +35.4 | 100% |
| HAR + absmis | -0.61% | +0.382 | +35.8 | 100% |
| HAR + all five | -1.31% | +0.382 | +35.4 | 100% |
