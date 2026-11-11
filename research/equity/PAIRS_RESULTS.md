# Results: pairs reversion on 63 same-sector pairs

Rules: [PAIRS_HYPOTHESES.md](PAIRS_HYPOTHESES.md) (5809298, before any run). Code `pairs.py`;
numbers `results/pairs_*.csv` (every trade listed). Dev committed before the holdout ran.
Net = after 10 bp round trip per unit of gross (stock futures), per trade.

| | Dev 2020 + 2022 | Holdout 2023 |
|---|---|---|
| Trades (pairs) | 1,119 (63) | 634 (63) |
| Gross / net per trade | +38.1 / **+28.1 bp** | +20.2 / **+10.2 bp** |
| Win rate | 63 % | 61 % |
| t on entry-month means (bar: 2) | **1.30** | **0.85** |
| t on trades (not the bar; trades overlap) | 3.07 | 1.32 |
| By year | 2020 +50.0, 2022 +3.5 | 2023 +10.2 |
| Groups net positive (bar: 5 of 7) | 6 | 4 |
| Random-direction placebo 95th pct | +4.9 | +2.7 |
| Entered a day later | +30.7 | +7.3 |
| Leaked signal refused | yes | yes |
| Median hold | 12 days | 14 days |

**Verdict: FAIL.** The average trade is positive and beats the placebo, but the profit comes in
lumps: month to month it is not distinguishable from zero (t 1.3 dev, 0.85 holdout), 2022
barely made money (+3.5 bp), and 2023 fell to +10 bp. Most of the dev profit is the 2020 crash
year, when stocks in a sector diverged and snapped back.

By group (net bp per trade, t on trades):

| Group | Dev | 2023 |
|---|---|---|
| Banks | +61.5 (2.9) | +51.2 (4.0) |
| Autos | +46.0 (2.3) | +36.2 (2.0) |
| IT | +25.3 (1.7) | +16.1 (1.1) |
| Metals | +19.8 (0.5) | +3.7 (0.1) |
| Energy | +44.5 (1.3) | -25.9 (-0.8) |
| FMCG | +8.5 (0.4) | -31.0 (-1.6) |
| Pharma | -32.2 (-1.1) | -32.0 (-1.1) |

Exploratory only (picked after seeing the table, so it cannot pass): **bank pairs** were
positive in dev and in 2023, as were autos. Together with the HDFCBANK/ICICIBANK result, banks
are the one place stock-pair reversion has held up on every split. The honest next step is a
forward paper test on bank pairs with live FYERS prices (the recorder already streams the
BANKNIFTY banks), judged on rules fixed before it starts.
