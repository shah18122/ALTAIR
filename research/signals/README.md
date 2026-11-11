# Advanced signals: index vs constituents, order flow, OI, volatility

Asked 2026-11-11: "add order-book imbalance, index mispricing from stock constituents and 5-10 of the
most advanced signals". Every signal below is tested walk-forward, out of sample, after costs.

## Verdict

**No price-direction signal clears trading costs as a taker.** A BANKNIFTY futures round trip costs
about 6 bp today (0.05% STT on the sell side, plus exchange, stamp, GST and the spread). The best
forecasts here move about 1 bp on a 10-second horizon. They are real (positive information
coefficient on 95-100% of days in all three years), but 5 to 10 times too small to trade alone.
Possible uses: timing entries and hedges for the option seller, and skewing a market maker's quotes.

**Volatility is predictable, but none of the new vol signals beat HAR.** HAR (realised vol over 5
minutes, 30 minutes and the day so far, plus the time-of-day mean) explains about 75% of the next
5 minutes' realised vol. Adding intensity, dispersion, correlation, VPIN or |mispricing| changes
that by -1.3% to +1.1%.

This matches docs/MODEL_AUDIT.md: no edge on price, skill on volatility.

## Data and method

- **Drive ticks** (source `drive-ticks`, NOT FYERS; kept separate): front-month BANKNIFTY future
  and the 12 bank constituents. These are 1-second trade snapshots with no bid/ask:
  2020 (251 days), 2022 (247), 2023 (241).
- `load.py` builds a 1-second grid. `study.py` samples a row every 5 s from 09:20 to 15:20 and
  enters **1 s after** the signal.
- A ridge model is refitted before each day on all earlier days. The first 40 days are never
  scored. Basket betas come from the 20 prior days.
- Results: `results/bnf_drive_<year>.md`.
- `obi_study.py` runs the same test on the recorder's FYERS books (mid prices, actual spread). See
  "Order book" below.

## Results, next 10 s, mean daily IC (days with IC > 0)

| # | Signal | 2020 | 2022 | 2023 | Verdict |
|---|---|---|---|---|---|
| 1 | **Constituent lead** (basket moved, future not yet: `leadgap10/30`) | 0.024-0.035 (78-87%) | 0.10 (100%) | 0.12 (99%) | Strongest. About 2/3 is the Drive future's stale prints (see check below). |
| 2 | **Index mispricing** vs beta-weighted basket (`misprice`, level vs 30-min mean) | 0.025 (81%) | 0.069 (97%) | 0.048 (96%) | Real, small. Mean-reverts in seconds, not minutes. |
| 3 | **Breadth** (beta-weighted share of banks up minus down, 60 s) | 0.015 | 0.022 | 0.027 | Weak alone. Helps with own-reversal. |
| 4 | **Signed trade flow** (tick-rule, 60 s / 300 s) | 0.007 | 0.011 | 0.011 | Negligible. |
| 5 | **OI build-up** (sign of price change x OI change, 15 min) | -0.001 | -0.001 | -0.001 | **None** at any horizon. |
| 6 | **Constituent volume surge** x direction | 0.014 | 0.011 | 0.016 | Negligible. |
| 7 | Everything combined (with own reversal) | 0.061 | 0.146 | 0.151 | Gross ~1 bp on the top 1% of forecasts. Never above 6 bp. |

Staleness check: score only rows where the future itself printed at t and t+1. The constituent
lead's IC then falls to 0.008-0.024, and the combined model's to 0.05-0.057. Part of the lead is
real; most of it is the futures print arriving late in this data. The FYERS book test (mid
quotes, every update) settles it.

Volatility, next 5 min (vs HAR): intensity +0.1-0.3%, dispersion -1.0 to -0.0%, constituent
correlation -0.0 to +0.6%, VPIN +0.1%, |mispricing| -0.6 to +0.4%.

## Order book (OBI, OFI, microprice) on FYERS books

The OBI engine (book/flow.hpp) already computes OBI, microprice and OFI live, and
book/depth_study.hpp tests them. `obi_study.py` adds:

- a walk-forward test, by day, on the recorder's own books
  (`altair_store decode --day D --ticks` -> `data/store/ticks/<day>/books.csv`);
- a crossing test against the actual spread;
- the constituent-lead feature computed on mid quotes.

    python3 research/signals/obi_study.py data/store/ticks research/signals/results/obi_fyers.md

It needs the server's recorded days. Results go in `results/obi_fyers.md`.

## Constituents the recorder streams

The recorder streams the NIFTY 50 stocks with 5-level books. Of BANKNIFTY's banks, only HDFCBANK,
ICICIBANK, SBIN, KOTAKBANK and AXISBANK are in the NIFTY 50. `config/universe_banknifty.csv` now
adds the rest (INDUSINDBK, BANKBARODA, PNB, CANBK, FEDERALBNK, IDFCFIRSTB, AUBANK, UNIONBANK,
YESBANK), with books and futures and no BSE twin. The price service picks the file up at its next
start. Basket betas are fitted, so a name that has left the index just gets a beta near 0.

## Run it

    python3 research/signals/load.py "<Drive>/NSE Contract Futures 2022-23" "<Drive>/NSE Equities (Tick)/2023" bnf2023.npz \
        HDFCBANK,ICICIBANK,SBIN,KOTAKBANK,AXISBANK,INDUSINDBK,BANKBARODA,AUBANK,FEDERALBNK,IDFCFIRSTB,PNB,BANDHANBNK
    python3 research/signals/study.py bnf2023.npz report.md
