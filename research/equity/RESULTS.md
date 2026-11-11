# Results: equity (stock) data study

Hypotheses and pass bars: [HYPOTHESES.md](HYPOTHESES.md) (committed in 5aadd9a before
anything ran). Code: `study.py`, `load.py`. Numbers: `results/*.csv`.
Data: Drive 5-minute stock bars (source drive-ticks, NOT FYERS), 148 of the 149 listed
names found, top 100 by trailing traded value each day; Drive BANKNIFTY / NIFTY futures.

## Dev (2020 + 2022), written before the holdout was staged

Per trade (E1-E3: per rebalance), bp of gross exposure. t uses each trade as one draw.

| Rule | n | Gross | Net | t | Each year (net) | Placebo 95th | One bar later | Verdict |
|---|---|---|---|---|---|---|---|---|
| E1 daily reversal | 458 | -2.3 | -10.3 | -3.3 | 2020 -7.8, 2022 -12.8 | -4.7 | -8.2 | FAIL |
| E2 30-min reversal | 5,034 | -1.1 | -8.1 | -32.4 | 2020 -8.3, 2022 -7.8 | -7.0 | -8.2 | FAIL |
| E3 1-month momentum | 93 | -4.8 | -9.8 | -0.5 | 2020 -16.8, 2022 -2.6 | +9.3 | -4.6 | FAIL |
| E4 basket lead 5m BANKNIFTY | 1,041 | +0.7 | -6.3 | -9.2 | 2020 -5.7, 2022 -6.9 | -5.8 | -6.4 | FAIL |
| E4 basket lead 5m NIFTY | 1,143 | +0.4 | -6.6 | -18.4 | 2020 -6.2, 2022 -7.0 | -6.4 | -7.3 | FAIL |
| E5 mispricing level BANKNIFTY | 338 | +2.2 | -4.8 | -1.5 | 2020 -3.3, 2022 -6.6 | -1.9 | -4.8 | FAIL |
| E5 mispricing level NIFTY | 398 | -1.8 | -8.8 | -5.9 | 2020 -9.4, 2022 -8.4 | -4.5 | -7.8 | FAIL |

The leak check (a signal equal to the trade's own return) was refused by the timestamp
assertion in every rule.

Volatility (E6), out-of-sample log-RV mean squared error, augmented vs HAR:

| Test | n | HAR | HAR + stocks | Gain | DM t | QLIKE gain | Each year | Verdict |
|---|---|---|---|---|---|---|---|---|
| E6a next hour BANKNIFTY | 2,060 | 0.481 | 0.496 | -3.1 % | -2.6 | -3.9 % | 2020 -4.3 %, 2022 -2.0 % | FAIL |
| E6b next day BANKNIFTY | 353 | 0.319 | 0.344 | -7.8 % | -1.8 | -9.2 % | 2020 -8.0 %, 2022 -7.7 % | FAIL |
| E6a next hour NIFTY | 2,060 | 0.411 | 0.408 | +0.7 % | +0.5 | -3.0 % | 2020 -1.9 %, 2022 +3.3 % | FAIL |
| E6b next day NIFTY | 353 | 0.284 | 0.355 | -25.0 % | -3.2 | -23.6 % | 2020 -42.8 %, 2022 -5.1 % | FAIL |

Dev verdict: nothing passes. Every trading rule loses after costs in both years; the
gross edge is never above +2.2 bp against 7-10 bp of costs. Daily and 30-minute
reversal do not exist in this large-cap universe (both are slightly *continuation*
gross). Adding stock dispersion / correlation / constituent RV to HAR makes the index
vol forecast worse out of sample, not better: the extra regressors add noise that HAR's
own index RV already contains.

## Holdout 2023 (run once, after the dev section above was committed in bb200f3)

| Rule | n | Gross | Net | t | Verdict |
|---|---|---|---|---|---|
| E1 daily reversal | 242 | +0.7 | -7.3 | -1.9 | FAIL |
| E2 30-min reversal | 2,662 | -0.6 | -7.6 | -26.5 | FAIL |
| E3 1-month momentum | 51 | -14.7 | -19.5 | -1.0 | FAIL |
| E4 basket lead 5m BANKNIFTY | 915 | +0.2 | -6.8 | -17.5 | FAIL |
| E4 basket lead 5m NIFTY | 820 | +0.2 | -6.8 | -23.8 | FAIL |
| E5 mispricing level BANKNIFTY | 224 | +0.6 | -6.4 | -4.5 | FAIL |
| E5 mispricing level NIFTY | 202 | +0.5 | -6.5 | -5.3 | FAIL |

| Test | n | Gain vs HAR | DM t | Verdict |
|---|---|---|---|---|
| E6a next hour BANKNIFTY | 1,205 | +1.1 % | +1.2 | FAIL |
| E6b next day BANKNIFTY | 241 | -0.5 % | -0.2 | FAIL |
| E6a next hour NIFTY | 1,205 | +1.0 % | +0.7 | FAIL |
| E6b next day NIFTY | 241 | -5.3 % | -0.9 | FAIL |

## Verdict

The stock data adds no tradable edge and no volatility skill on top of what we have:

- **Cross-sectional** (long some stocks, short others): no daily or 30-minute reversal,
  no one-month momentum in the 100 most liquid names. Gross edges are -15 to +1 bp;
  costs are 7-10 bp.
- **Index vs constituents at 5 minutes**: the basket's lead and the cumulative
  mispricing are worth +0.2 to +2.2 bp gross a trade, a third of the 7 bp round trip.
  This matches the 10-second study (`research/signals/`): the gap is real but tiny,
  and it closes faster than a taker can trade it.
- **Volatility**: adding constituent RV, dispersion and correlation to HAR does not
  beat HAR (dev -3 % to -25 %, holdout -5 % to +1 %, never significant). HAR on the
  index's own RV stays the volatility model.

Caveats: the universe is today's large-cap list (some survivorship bias, which would
favour momentum, and momentum still failed); stock-futures returns are proxied by cash
prices; 5-minute bar closes are last trades, not mid-quotes.

Exploratory only (not part of any pass/fail): E1 and E2 are slightly *continuation*
gross in dev; E5 BANKNIFTY had the largest gross (+2.2 bp dev, +0.6 bp 2023), which
shrank out of sample. Nothing here is worth a forward test.
