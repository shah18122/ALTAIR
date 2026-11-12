# Results: fund flows and pair reversion

Hypotheses: [FLOWS_HYPOTHESES.md](FLOWS_HYPOTHESES.md) (e9427f2, committed before this run).
Code `flows.py`; numbers `results/flows_*.csv`. Volume from Drive 5-minute bars (drive-ticks,
NOT FYERS).

## F2a and F3: calendar flow days (run 2026-11-12) - FAIL, and the sign is reversed

    python research/equity/flows.py --eq "<Drive>/NSE Equities (5min)"

1,741 of 1,753 P6 trades classified (12 lacked 22 sessions of volume history).

| Divergence | Trades | Net bp/trade | Dev | 2023 | Banks |
|---|---|---|---|---|---|
| FLOW (index-review days) | 171 | **-31.1** | -62.6 (104) | +17.7 (67) | +83.5 (17) |
| NEWS (one-stock volume spike) | 338 | **+37.6** | +26.1 (217) | +58.2 (121) | +15.1 (44) |
| SECTOR (both legs heavy) | 295 | +24.7 | +34.0 | +12.0 | +57.4 (47) |
| OTHER | 937 | +26.2 | +45.2 | -10.2 | +65.7 (162) |

- **F2a: FAIL.** FLOW minus NEWS = **-68.7 bp** (month-clustered t -1.59). The prediction was
  positive. News-driven gaps closed *better* than index-review gaps.
- **F3: FAIL.** Net on volume asymmetry: slope +5.3 bp, t +0.43 (predicted negative).
- Not a pass test (seen after the run): dropping 15 Feb to 30 Apr 2020 (COVID crash, which
  falls on the March review) moves FLOW to -2.7 bp and NEWS to +41.1 bp. The reversal is
  not only the crash. In 2023 alone FLOW is positive (+17.7) but still below NEWS (+58.2).

Read plainly: index-review days do not mark the gaps that close. If anything, gaps opened on
one-stock volume spikes (results, ratings) revert more, which looks like overreaction to firm
news rather than a fee for absorbing fund flows. Index-review days are a crude flow proxy;
F2b (daily FPI flows) is the direct test and still runs once, as registered.

## F2b: daily FPI flows (run once, 2026-11-12) - FAIL

    python research/equity/flows.py --eq "<Drive>/NSE Equities (5min)" --fpi <altair-data>/research/flows/fpi_daily.csv

FPI from NSDL (altair-data b9a0b13), shifted to trade date (amendment 22ef325).

| Divergence | Trades | Net bp | Dev | 2023 | Banks (F4) |
|---|---|---|---|---|---|
| FLOW (index review or top-10 % FPI day) | 566 | +34.1 | +34.0 (394) | +34.3 (172) | **+118.8** (82) |
| NEWS | 338 | +37.6 | +26.1 | +58.2 | +15.1 (44) |
| SECTOR | 163 | +1.5 | +9.3 | -7.3 | +27.2 (25) |
| OTHER | 674 | +10.3 | +31.4 | -22.5 | +36.5 (119) |

FLOW minus NEWS = **-3.5 bp, t -0.17**. News-gap trades are still above the average
(+37.6 vs +22.5). **F2b fails.** F3 is unchanged (FPI does not enter it).

## F1: bank pairs on unseen FYERS years (run once, 2026-11-12) - FAIL

    python research/equity/flows_oos.py --daily <altair-data>/research/flows/fyers_daily --fpi <...>/fpi_daily.csv

Source FYERS daily candles only. TATAMOTORS left out (the FYERS file is the TMPV listing).

| Set | Trades | Months | Net bp/trade | t (monthly) | Win |
|---|---|---|---|---|---|
| **Banks, both periods (F1)** | 770 | 93 | **+13.4** | **0.97** | 64 % |
| Banks 2015-2019 | 502 | 59 | +39.5 | 2.87 | 68 % |
| Banks 2024-2026 | 268 | 34 | **-35.5** | -1.87 | 56 % |
| All 62 pairs 2015-2019 | 2,916 | 59 | +23.0 | 4.19 | 64 % |
| All 62 pairs 2024-2026 | 1,661 | 35 | -3.8 | -0.67 | 60 % |

Net by group: 2015-2019 every group positive (+15 to +39); 2024-2026 banks -35, autos -27,
pharma -11, FMCG -5, IT +16, energy +22, metals +33.

Flow split in the new years (F4, reported, no bar): all pairs FLOW +18.7 (1,182) vs NEWS -12.7
(903), difference +31.4 bp, t 2.35; banks FLOW -12.2 (204) vs NEWS +36.8 (141). The sign of the
flow-vs-news gap changes between samples and between banks and the rest, so it is not a stable
effect.

## Verdict

- The flow story (**F2a, F2b, F3**) is not supported: gaps that open on fund-flow days do not
  close better than gaps opened by firm news.
- The bank-pairs edge (**F1**) does not survive unseen years. It was strong in 2015-2019
  (+39 bp, t 2.9), and pair reversion then worked in every sector (+23 bp, t 4.2), but it fell
  over time: 2020 +50 (crash), 2022 +3.5, 2023 +10, 2024-2026 -4 overall and -35 for banks.
- What the data does show, consistently, is **a same-sector pairs premium that was large and
  broad in 2015-2019 and has since disappeared**, the pattern Do and Faff (2010) report for US
  pairs trading. That, not fund flows, is the defensible paper, and it can rest entirely on
  FYERS daily data plus NSDL flows.
- The live "Pair trading" paper model trades exactly the bank rule that lost 35 bp a trade in
  2024-2026. It should stay paper.
