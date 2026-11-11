# Pre-registered: pairs reversion across many same-sector stock pairs

Follow-up to [SINGLE_STOCK_RESULTS.md](SINGLE_STOCK_RESULTS.md): the HDFCBANK/ICICIBANK pair rule
(P6) earned +120 bp a trade in dev but on only 19 trades. This test asks whether the same rule,
unchanged, makes money across many pairs, so there are enough trades to tell edge from luck.
Written and committed before any run.

## Pairs (fixed now; HDFCBANK left out because its pairs were already looked at)

Every pair within each group:
- Banks: ICICIBANK, KOTAKBANK, AXISBANK, SBIN, INDUSINDBK (10 pairs)
- IT: INFY, TCS, HCLTECH, WIPRO, TECHM (10)
- Autos: MARUTI, M&M, TATAMOTORS, BAJAJ-AUTO, HEROMOTOCO, EICHERMOT (15)
- Metals: TATASTEEL, JSWSTEEL, HINDALCO, VEDL (6)
- FMCG: HINDUNILVR, ITC, NESTLEIND, BRITANNIA, DABUR (10)
- Pharma: SUNPHARMA, CIPLA, DRREDDY, DIVISLAB (6)
- Energy: RELIANCE, ONGC, BPCL, IOC (6)

63 pairs.

## Rule (P6, unchanged)

s = log(close A / close B) at 15:25; z = (s - mean of the last 20 closes) / their sd. When
|z| > 2, short the rich leg and long the cheap leg (0.5 each) at the next 09:20 open; exit at the
next 09:20 open after a close with |z| < 0.5, or after 10 sessions. One trade per pair at a time.
A pair is skipped across any corporate-action night (|overnight log move| > 0.20 in either leg).
Costs: stock futures, 10 bp round trip per unit of gross. Data and split as before: Drive 5-minute
bars, dev 2020 + 2022, holdout 2023 run once after dev is committed.

## Pass bar (one test, all must hold)

1. Dev: mean net per trade > 0 with t >= 2, where t is computed on **entry-month averages**
   (trades open at the same time are correlated; a month is one draw).
2. Mean net > 0 in 2020 and in 2022, and in at least 5 of the 7 groups.
3. Above the 95th percentile of 2,000 placebos that keep each trade's dates but pick its
   direction at random.
4. Entering one day later keeps the sign; a leaked signal is refused.
5. Holdout 2023: mean net > 0 with t >= 2 (same monthly t).
