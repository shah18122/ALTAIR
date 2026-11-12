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

## Pending

- **F2b** (FPI flow days): waits for `fpi_daily.csv` from the server.
- **F1** (bank pairs on unseen FYERS years 2015-2019, 2024-2026): waits for FYERS daily candles.
