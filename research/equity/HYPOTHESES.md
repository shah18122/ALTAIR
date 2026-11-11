# Pre-registered hypotheses: what the equity (stock) data can do for us

Smit, 2026-11-11: "we have data for equity also, how about that?"

Written and committed **before** any of these was run, so git history shows
the tests were fixed in advance. Results go in `RESULTS.md`; nothing in this
file changes after the first run. Same method as `research/strategy/`.

## What we already know

- Index direction: no model beats "no change" after costs at any timeframe
  (`docs/MODEL_AUDIT.md`, `research/cnn/`, `research/strategy/`).
- Bank basket leading BANKNIFTY futures at 10 s: IC 0.07-0.17 but ~1 bp,
  mostly stale futures prints, never clears ~6 bp (`research/signals/`).
- Move size is forecastable: HAR 16-35 % below a random walk.

So the questions for stock data are: (a) is there a **cross-sectional** edge
(long some stocks, short others) that index-only work could never see,
(b) does the basket lead the index at a slower, tradable horizon (5 min, not
10 s), and (c) do stocks make the **volatility** forecast better.

## Data, split, costs

- Data: Drive files on Smit's PC, `S:\tick_data\NSE Equities (5min)\<year>\
  <SYM>_5min_OHLCV.csv` (source `drive-ticks`, NOT FYERS; never merged into
  FYERS series). Bars are labelled by their start time, 09:15 .. 15:25
  (75 bars); rows outside 09:15-15:29 are dropped. Years: 2020, 2022, 2023
  (there is no 2021 stock file). Index side: the Drive BANKNIFTY and NIFTY
  front-month futures 5-minute bars (`tools/drive_ticks_to_bars.py`).
- **Development set:** 2020 and 2022. **Holdout:** 2023, sealed; staged and
  run once, after the dev verdicts are written in `RESULTS.md`.
- **Universe** (`universe.txt`, 149 names, fixed now): large caps that were in
  NIFTY 50 / Next 50 / NIFTY Bank at some point in 2020-2023 plus a few liquid
  F&O mid caps. It is a list from today's knowledge, so it carries some
  survivorship bias (names that dropped out of the indices are under-
  represented); the bias favours momentum and is noted in the results.
  On each day, the tradable set is the top 100 of the list by median traded
  value (close x volume) over the previous 20 sessions, among names with bars
  on at least 18 of those 20 sessions. Past days only.
- Prices are not adjusted for splits/bonuses. Any overnight move with
  |log return| > 0.20 is treated as a corporate action: that stock is out of
  every signal that spans it, and out of any holding that spans it.
- Timing: a signal uses bars that have closed; a trade enters at the **next
  bar's open** and exits at a bar's open. The harness checks every signal
  time is strictly before its entry time and refuses the run otherwise.
- Costs, per unit of gross exposure traded one way (half a round trip):
  - Cash intraday (MIS): **4 bp** (STT 0.025 % sell side, exchange, stamp,
    GST, ~1.5 bp slippage a side) -> 8 bp round trip.
  - Stock futures (anything held overnight; shorts cannot be carried in
    cash): **5 bp** -> 10 bp round trip (2026 futures STT 0.05 % sell side,
    wider spreads than index futures). Stock-futures returns are proxied by
    the stock's own price (basis change ignored).
  - Index futures: **7 bp round trip**, as in `research/strategy/`.
  Cross-sectional costs are charged on actual turnover (|change in weight|).
  Gross and break-even cost are reported too.

## Hypotheses

Cross-sectional rules are long-short, equal weight, dollar neutral: long the
bottom/top decile, short the other, gross exposure 1 (0.5 a side). Returns
used for ranking are market-adjusted (minus the tradable set's equal-weight
mean).

**E1 Daily reversal** (Lehmann 1990; Jegadeesh 1990; Nagel 2012: liquidity
provision). Yesterday's losers beat yesterday's winners today. Signal:
close-to-close return of day d-1 (15:25 bar close to 15:25 bar close).
Day d: long the bottom decile, short the top decile at the 09:20 bar's open,
exit at the 15:20 bar's open. Cash intraday costs.

**E2 Intraday 30-minute reversal** (Heston, Korajczyk & Sadka 2010; liquidity
provision at short horizons). At each half-hour boundary 09:45, 10:15, ...,
14:45, rank by the last 30 minutes' return (six bars); long the bottom decile,
short the top decile at the next bar's open, hold 30 minutes; flat at 15:15.
A stock needs volume > 0 in the last signal bar to be ranked. Cash intraday
costs.

**E3 One-month momentum** (Jegadeesh & Titman 1993 at short horizon; Indian
evidence of short-horizon momentum). Signal: return from close of d-22 to
close of d-2 (skip the last day, which is E1's reversal). Rebalance on the
first session of each week at the 09:20 open, hold to the next rebalance.
Long the top decile, short the bottom decile. Stock-futures costs. Needs 22
sessions of history, so each year's first month is warm-up.

**E4 Basket leads the index, 5-minute** (Lo & MacKinlay 1990; Chordia &
Swaminathan 2000). Basket return per 5-minute bar = ridge fit of the future's
5-minute return on the constituents' 5-minute returns over the previous 60
sessions, refit at each month start (ridge lambda = 0.1 x mean diagonal of
X'X; BANKNIFTY on the bank names, NIFTY on the NIFTY 50 names of the list).
Gap g_t = basket_t - future_t. If |g_t| > 2 x sd of g over the previous 20
sessions, trade the future in the sign of g at the next bar's open, exit one
bar (5 minutes) later. Bars 09:20 .. 15:15 only. One position at a time.
Index futures costs. BANKNIFTY main, NIFTY replication.

**E5 Index vs constituent mispricing, level** (index arbitrage: the future
tracks the basket, so a gap that opened closes). G_t = sum of g from the
09:20 bar to bar t. z_t = G_t / sd(G over all bars of the previous 20
sessions). From 09:45 to 14:45, when |z_t| > 2, trade the future in the sign of
G (toward the basket) at the next bar's open, exit 30 minutes (6 bars) later.
One position at a time. Index futures costs. BANKNIFTY main, NIFTY
replication.

**E6 Stocks improve the index volatility forecast** (dispersion and average
correlation carry information about future index variance: Driessen, Maenhout
& Vilkov 2009; Pollet & Wilson 2010). Two targets, BANKNIFTY and NIFTY futures:
 - (a) next hour's realised variance (sum of twelve squared 5-minute log
   returns), forecast at 10:15, 11:15, 12:15, 13:15 and 14:15 (the last is the
   70 minutes to the close);
 - (b) next session's realised variance (09:20 .. 15:25 bars).
Baseline HAR on log RV: (a) last hour, today so far, last session, last 5
sessions, plus hour dummies; (b) last 1, 5 and 22 sessions. Augmented =
baseline + the same windows of: log mean constituent RV, log cross-sectional
dispersion (std of market-adjusted constituent returns), and log average
correlation proxy (basket variance / mean constituent variance). OLS on log RV,
walk-forward: refit at each month start on all earlier dev data, first
forecast after 60 sessions. Score: mean squared error of log RV
(primary) and QLIKE.

## Pass bar (all must hold)

Trading rules E1-E5 (7 tests: E1, E2, E3, E4 x 2, E5 x 2):
1. Dev net mean P&L per trade (per rebalance for E1-E3) > 0 with
   t >= 2.69 (two-sided 5 % Bonferroni over 7 tests). E3 overlapping
   holdings: none (weekly rebalance, non-overlapping).
2. Net mean > 0 in each dev year (2020 and 2022).
3. Placebo: above the 95th percentile of 2,000 runs. E1-E3: the signal is
   shuffled across stocks on each date (same days, same decile sizes).
   E4-E5: same entry times, random signs.
4. Look-ahead: entering one bar later (E3: one day later) keeps the sign; a
   deliberately leaked signal (the trade's own return) is refused by the
   harness's timestamp check.
5. Holdout 2023: net mean > 0 with t >= 2 (E4, E5 on BANKNIFTY).

Volatility E6 (4 tests: 2 targets x 2 instruments):
1. Dev: augmented log-MSE at least 2 % below HAR's, Diebold-Mariano t >= 2.50
   (Bonferroni over 4), lower in each dev year.
2. Holdout 2023: lower than HAR with DM t >= 2.

A rule that fails any step is reported as failed with its numbers. Anything
looked at after this file (other deciles, horizons, thresholds) is labelled
exploratory and cannot pass.
