# Pre-registered: fund flows, bank-stock dislocations and pair reversion

Paper: *Who Pays for Liquidity? Fund Flows, Bank-Stock Dislocations and Pair Reversion in India*
(Smit, 2026-11-12). Written and committed **before** any volume, flow or new-years data is
looked at. What was already seen: the P&L of every P6 trade in
[PAIRS_RESULTS.md](PAIRS_RESULTS.md) (dev 2020+2022, holdout 2023), by group. No trade has been
split by volume, flow or calendar yet.

Idea: a pair diverges either because one firm had news (the gap is information and should
**not** close) or because funds moved money in or out of the stocks for reasons unrelated to
the firm (index changes, foreign-investor flows, sector rotation). Those gaps should close, and
the pair trade earns a fee for absorbing them (Coval-Stafford 2007, Greenwood 2005, Lou 2012).

## Trades (unchanged)

Rule P6 from [PAIRS_HYPOTHESES.md](PAIRS_HYPOTHESES.md), 63 same-sector pairs, 10 bp round trip.
Trade lists: `results/pairs_dev_trades.csv`, `results/pairs_holdout_trades.csv`. For each trade
the **trigger day** d is the session before entry (the close where |z| > 2); the **window** W is
d and the two sessions before it.

## Definitions (fixed now)

- Daily volume of a stock = sum of its 5-minute bar volumes (Drive bars) or the FYERS daily
  volume (new years). **Abnormal volume** AV(day) = volume / median volume of the 20 sessions
  before W.
- **NEWS** divergence: on some day of W, one leg has AV >= 3 while the other leg has AV < 1.5
  (a stock-specific volume spike: results, ratings, firm news).
- **Calendar flow day**: the last trading session of February, May, August or November (MSCI
  index reviews take effect) or of March or September (NSE index reconstitution takes effect),
  and the session before it.
- **FPI flow day**: |FPI net equity investment| on that day in the top 10 % of the previous 250
  sessions' absolute values (NSDL / NSE provisional figures, Smit OK'd 2026-11-12 for this paper).
- **FLOW** divergence: not NEWS, and W contains a calendar flow day or an FPI flow day.
- **SECTOR** divergence (secondary): not NEWS, not FLOW, and on some day of W both legs have
  AV >= 1.5.
- **OTHER**: everything else.

## Hypotheses and pass bars

**F1, out-of-sample bank pairs (primary for the bank result).** The 10 bank pairs (ICICIBANK,
KOTAKBANK, AXISBANK, SBIN, INDUSINDBK), rule P6 unchanged except that it runs on FYERS daily
candles (z on the daily close, enter and exit at the next session's open), on years never used:
2015-01 to 2019-12 and 2024-01 to the latest close. Pass: mean net > 0 with t >= 2 on
entry-month means, and mean net > 0 in each of the two periods. All 63 pairs are reported too
(not a pass test).

**F2, flow gaps close and news gaps don't (primary for the mechanism).** All 63 pairs, dev and
2023 trades pooled. Pass: mean net(FLOW) - mean net(NEWS) > 0 with t >= 2 (difference of means,
standard errors clustered by entry month), and mean net(NEWS) below mean net(all trades).
Run twice: **F2a** with calendar flow days only (runs now), **F2b** adding FPI flow days (when
the FPI series arrives). F2b is the headline; F2a is reported whatever it shows.

**F3, continuous version (secondary).** Regress each trade's net on the volume asymmetry
max over W of |log AV_A - log AV_B|. Predicted slope < 0 with t >= 2 (month-clustered).

**F4, banks (secondary).** F2b restricted to the 10 bank pairs, and to the new years once F1 runs.
Reported, no pass bar (too few trades to promise one).

Every result is reported, including failures. The FPI and new-years runs happen once each.

## Amendment (2026-11-12, data alignment only, committed before F1/F2b were run)

The NSDL file (altair-data b9a0b13, `research/flows/fpi_daily.csv`) dates each figure by its
**reporting** date, about one trading day after the trades. FPI flow day d is therefore the
session *before* the reporting date: each row's value is moved to the previous row's date. The
top-10 % threshold and everything else are unchanged. FYERS's TATAMOTORS file is the TMPV
listing; F1 is banks only and does not use it.
