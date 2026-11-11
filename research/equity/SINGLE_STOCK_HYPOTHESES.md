# Pre-registered: can one stock make money on its own? (HDFCBANK)

Smit, 2026-11-12: "apart from short straddle is there anything we can do in stocks? Take one
instrument and check if we can make profit."

Written and committed **before** any run. Same data, split, timing and leak checks as
[HYPOTHESES.md](HYPOTHESES.md) (Drive 5-minute bars, source drive-ticks, NOT FYERS;
dev 2020 + 2022, holdout 2023 run once after the dev verdicts are committed).

## Instrument

**HDFCBANK**: the biggest BANKNIFTY weight and one of the two most traded NSE stocks, so its
spread is about as tight as a single stock gets. Chosen for liquidity, not for past results.

Replication (descriptive only, cannot pass or fail anything): the same rules on ten other
very liquid names, fixed now: RELIANCE, ICICIBANK, INFY, TCS, SBIN, AXISBANK, KOTAKBANK, LT,
ITC, BHARTIARTL. Reported: how many of the ten are net positive in dev and in 2023.
Picking the best of these afterwards would be data mining, so nothing is promoted from them.

## Costs (round trip, per unit traded)

- Cash intraday (MIS): **8 bp** (STT 0.025 % sell, exchange, stamp, GST, ~3 bp slippage).
- Held overnight (stock futures, needed for shorts and cheaper than delivery): **10 bp**.
- Gross and break-even are reported too.

## Rules (one position at a time, long or short 1 unit)

Bar s starts at 09:15 + 5s minutes; a signal uses closed bars; entry at the next bar's open.

**P1 Intraday momentum** (Gao, Han, Li & Zhou 2018). sign(close of the 09:40 bar / previous
day's 15:25 close - 1). Enter at the 15:00 open, exit at the 15:20 open. Cash costs.

**P2 Opening range breakout** (Crabel; Zarattini & Aziz 2023). Range = high/low of the closes
and opens of 09:15-09:25 bars. From the 09:30 bar on, first close above the range goes long,
below goes short, at the next open; exit at the 15:20 open. One trade a day. Cash costs.

**P3 Gap fade.** gap = 09:15 open / previous 15:25 close - 1. If |gap| > the 75th percentile of
the previous 60 sessions' |gap|, fade it at the 09:20 open, exit at the 10:15 open. Cash costs.

**P4 Overnight drift** (Lou, Polk & Skouras 2019). Long at the 15:25 bar's close, sell at the
next 09:15 open, every night with consecutive sessions. Futures costs.

**P5 Residual reversion against BANKNIFTY** (idiosyncratic moves partly revert; Avellaneda &
Lee 2010). Per 5-minute bar, residual e = r_stock - beta x r_BANKNIFTY-future, beta = OLS on
the previous 60 sessions' 5-minute returns, refit each month start. E = sum of e from the
09:20 bar. z = E / sd(E over all bars of the previous 20 sessions). From 09:45 to 14:45, when
|z| > 2, trade the stock against E (short if E > 0) at the next open, exit 30 minutes later.
Stock only, no index hedge. Cash costs.

**P6 Pair HDFCBANK / ICICIBANK** (Gatev, Goetzmann & Rouwenhorst 2006). s = log(HDFCBANK close
/ ICICIBANK close) at 15:25; z = (s - mean of the last 20 closes) / their sd. When |z| > 2 at a
close, go short the rich leg and long the cheap leg (0.5 each) at the next 09:20 open; exit at
the next 09:20 open after a close with |z| < 0.5, or after 10 sessions. Futures costs on both
legs (10 bp round trip per unit of gross).

## Pass bar (all must hold)

1. Dev net mean per trade > 0 with t >= 2.64 (two-sided 5 % Bonferroni over 6 rules).
2. Net mean > 0 in 2020 and in 2022.
3. Above the 95th percentile of 2,000 random-sign placebos with the same entry times
   (P4: long-overnight vs the same-length intraday window, as in research/strategy).
4. Entering one bar later keeps the sign; a leaked signal is refused by the timestamp check.
5. Holdout 2023: net mean > 0 with t >= 2.
