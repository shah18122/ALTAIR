# Pre-registered hypotheses: rule-based BANKNIFTY / NIFTY futures strategies

Written and committed **before** any of these rules was run on the data, so the
git history shows the tests were fixed in advance. Results go in
`RESULTS.md`; nothing in this file changes after the first run.

## Why these and not more forecasting

`docs/MODEL_AUDIT.md`: no model forecasts the next bar's price better than "no
change" at any timeframe, and the tick study found only the bid-ask bounce.
Those were *linear forecasts on every bar*. A different, still open question
is whether a few **conditional, event-type rules from the literature** pay on
the days or moments they fire. Each rule below has a published economic story,
a fixed specification and a fixed pass bar.

## Data, split, costs

- Data: Drive tick files (source `drive-ticks`, NOT FYERS), BANKNIFTY and
  NIFTY front-month futures, 1-minute bars from `tools/drive_ticks_to_bars.py`
  (Nov 2019 - Dec 2023, about 823 sessions). BANKNIFTY is the main
  instrument, NIFTY is the replication.
- **Development set:** every session up to 2022-12-31.
  **Holdout:** every 2023 session, sealed; run once, after the dev verdicts
  are written.
- Roll days (the stitched series changes contract) are dropped from any rule
  that uses the previous session's close.
- Timing: a signal uses bars that have closed; the trade enters at the
  **next bar's open** (its first trade) and exits at a bar's open, or at the
  15:29 bar's close for "hold to close".
- Costs per round trip: **7 bp** base (2026 futures STT 0.05 % on the sell
  side, exchange, stamp, GST, plus about 1 bp slippage; `config/charges.toml`
  schedule 2). Also shown: gross, 3.5 bp (pre-2026 STT) and break-even cost.

## Hypotheses

**H1 Intraday momentum** (Gao, Han, Li & Zhou, *JFE* 2018). The return from the
previous close to 09:45 predicts the last half hour's return: late traders and
hedgers trade in the direction of the day. Rule: at 15:00 open go
sign(close of 09:44 bar / previous close - 1); exit at the 15:29 close.

**H2 Shock reversal** (liquidity provision; Nagel *RFS* 2012). A 5-minute
return larger than 3 times its usual size is partly a liquidity demand and
reverses. Usual size = root mean square of the same 5-minute slot over the
previous 20 sessions. Rule: on each 5-minute boundary after 09:20 and before
15:10, if |r_5m| > 3 x usual, fade it at the next open, exit 15 minutes later;
no new trade while one is open.

**H3a Opening range breakout** (Crabel 1990; Zarattini & Aziz 2023). Rule:
range = high/low of 09:15-09:29. From the 09:30 bar on, the first 1-minute
close above the high goes long, below the low goes short, at the next open;
hold to the 15:29 close; one trade a day.

**H3b Opening range breakout on high-volatility days only.** Same rule, only on
days whose forecast session range (mean of the last 5 and last 22 sessions'
high-low range, past days only) is in the top third of the last 60 sessions'
forecasts. Story: breakouts follow through when the day is going to move.

**H4 Opening gap fade.** Overnight gaps in index futures partly overshoot and
fill in the first hour. Rule: gap = 09:15 open / previous close - 1; if |gap|
is above the 75th percentile of the previous 60 sessions' |gap|, fade it at the
09:20 open, exit at the 10:15 open.

**H5 Overnight drift** (Cliff, Cooper & Gulen 2008; Lou, Polk & Skouras *JFE*
2019). Index returns accrue mostly overnight. Rule: long at the 15:29 close,
exit at the next 09:15 open, every non-roll day.

## Pass bar (all must hold)

1. Dev net mean P&L per trade > 0 at 7 bp, with t >= 2.87 (two-sided 5 %
   Bonferroni over the 12 tests: 6 rules x 2 instruments).
2. Net mean > 0 in each third of the dev period.
3. Beats the placebo: above the 95th percentile of 2,000 runs with the same
   entry times and random signs (H5: random entry days of the same count is
   not meaningful, so H5's placebo is the drift of the 15:29-09:15 window
   against an equal-length intraday window).
4. Look-ahead checks: entering one bar later keeps the sign; a deliberately
   leaked version (signal uses the trade's own return) is caught by the
   harness's timestamp assertion.
5. Holdout 2023 net mean > 0 with t >= 2 on BANKNIFTY.

A rule that fails any step is reported as failed, with its numbers. Anything
looked at after this file (variants, thresholds) is labelled exploratory and
cannot pass.

## Volatility overlay (descriptive, not a pass/fail test)

For each rule, the same trades sized by 1 / forecast session range (past days
only, normalised to mean size 1). Reported: Sharpe with and without.
