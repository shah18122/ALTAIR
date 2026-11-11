# Results: pre-registered futures strategies on the Drive ticks

Hypotheses, rules and pass bar: [HYPOTHESES.md](HYPOTHESES.md), committed to main
(97835d8) before the first run. Code: `study.py`. Data: Drive tick files
(source `drive-ticks`, NOT FYERS), front-month BANKNIFTY and NIFTY futures,
1-minute bars. Dev = Nov 2019 to Dec 2022 (~580 sessions), holdout = 2023
(~240 sessions), run once after the dev verdicts below were fixed.

    python research/strategy/study.py --bars data/drive_ticks --split dev
    python research/strategy/study.py --bars data/drive_ticks --split holdout

## Verdict

**All six rules fail, on both instruments, in dev and in the 2023 holdout.**
None clears the 7 bp round-trip cost, none has a positive net in every third,
and none beats the random-sign placebo at 5 %. Two rules that looked
promising gross in dev (opening range breakout, H3a/H3b) reversed sign in
2023. This agrees with `docs/MODEL_AUDIT.md`: on index futures, taken as a
taker on 1-minute bars, there is no direction edge here, rule-based or
model-based.

| Rule | BANKNIFTY dev | BANKNIFTY 2023 | Why it fails |
|---|---|---|---|
| H1 Intraday momentum (first 30 min -> last 30 min) | +1.5 bp gross, -5.5 net | +0.7 gross, -6.3 net | Real but tiny: ~1 bp a trade, a fifth of the cost |
| H2 Shock reversal (fade 3-sigma 5-min moves) | -4.4 gross | -0.2 gross | Shocks do **not** reverse; in dev they continued (see below) |
| H3a Opening range breakout | +5.7 gross, -1.3 net | -7.4 gross | Positive in 2020 only, then negative; flips in holdout |
| H3b ORB on high-vol days | +11.1 gross, +4.1 net, t 0.22 | -8.6 gross | One good third (2020) carries it; holdout -15.6 net |
| H4 Opening gap fade | +3.4 gross, -3.6 net | +4.5 gross, -2.5 net | Below cost; thirds swing +11 / -13 |
| H5 Overnight drift (long 15:29 -> 09:15) | +2.3 gross | +3.9 gross | Positive gross in every split, but it is index beta and pays the cost every day |

Look-ahead checks passed: the harness refused the deliberately leaked H1
(signal minute 374 after entry 345) on both instruments, and entering one bar
later moves every result by under 1.5 bp except H4 (gap fade is sensitive to
the 09:20 vs 09:21 entry, another sign it is noise).

The volatility overlay (size by 1 / forecast range) does not rescue any rule:
it changes Sharpe by -1.6 to +0.5 and never makes a negative rule positive.

## Full tables

Net at 7 bp round trip; t on per-trade net; thirds of the period in time
order; placebo p = share of 2,000 random-sign runs on the same entries with
gross mean at least as high (H5 is always long: n/a); Sharpe is daily,
annualised, plain / vol-sized.

### Dev (Nov 2019 - Dec 2022)

| Inst | Rule | Trades | Gross bp | Net bp @7 | t | Net @3.5 | Thirds (net) | Placebo p | Delay +1 net | Sharpe |
|---|---|---|---|---|---|---|---|---|---|---|
| BANKNIFTY | H1 | 508 | +1.5 | -5.5 | -2.89 | -2.0 | -5 / -4 / -7 | 0.19 | -4.7 | -2.03 / -1.67 |
| BANKNIFTY | H2 | 689 | -4.4 | -11.4 | -5.39 | -7.9 | -14 / -11 / -9 | 0.97 | -10.9 | -6.23 / -6.96 |
| BANKNIFTY | H3a | 536 | +5.7 | -1.3 | -0.20 | +2.2 | +6 / -5 / -5 | 0.18 | -1.3 | -0.14 / -0.19 |
| BANKNIFTY | H3b | 132 | +11.1 | +4.1 | +0.22 | +7.6 | +26 / -4 / -9 | 0.26 | +4.1 | +0.31 / +0.03 |
| BANKNIFTY | H4 | 104 | +3.4 | -3.6 | -0.26 | -0.1 | +11 / -13 / -9 | 0.40 | +1.3 | -0.40 / -0.20 |
| BANKNIFTY | H5 | 508 | +2.3 | -4.7 | -0.89 | -1.2 | -7 / -1 / -6 | n/a | -4.7 | -0.63 / -0.50 |
| NIFTY | H1 | 516 | +1.1 | -5.9 | -5.01 | -2.4 | -7 / -6 / -5 | 0.18 | -5.3 | -3.50 / -3.01 |
| NIFTY | H2 | 662 | -4.2 | -11.2 | -7.65 | -7.7 | -14 / -11 / -9 | 1.00 | -11.1 | -6.53 / -8.13 |
| NIFTY | H3a | 547 | +7.0 | +0.0 | +0.00 | +3.5 | +1 / +3 / -4 | 0.08 | +0.5 | +0.00 / -0.28 |
| NIFTY | H3b | 126 | +15.4 | +8.4 | +0.55 | +11.9 | +22 / -0 / +4 | 0.17 | +9.4 | +0.77 / +0.94 |
| NIFTY | H4 | 114 | -2.2 | -9.2 | -1.24 | -5.7 | -5 / -12 / -11 | 0.60 | -5.5 | -1.85 / -1.77 |
| NIFTY | H5 | 518 | +0.2 | -6.8 | -1.52 | -3.3 | -12 / -3 / -5 | n/a | -6.8 | -1.06 / -0.77 |

### Holdout (2023, run once)

| Inst | Rule | Trades | Gross bp | Net bp @7 | t | Net @3.5 | Thirds (net) | Placebo p | Delay +1 net | Sharpe |
|---|---|---|---|---|---|---|---|---|---|---|
| BANKNIFTY | H1 | 232 | +0.7 | -6.3 | -4.36 | -2.8 | -7 / -8 / -4 | 0.31 | -6.1 | -4.54 / -5.02 |
| BANKNIFTY | H2 | 378 | -0.2 | -7.2 | -7.21 | -3.7 | -10 / -7 / -5 | 0.58 | -7.5 | -8.04 / -7.97 |
| BANKNIFTY | H3a | 236 | -7.4 | -14.4 | -3.48 | -10.9 | -14 / -17 / -12 | 0.97 | -14.5 | -3.59 / -3.47 |
| BANKNIFTY | H3b | 85 | -8.6 | -15.6 | -2.19 | -12.1 | -14 / -20 / -12 | 0.88 | -14.7 | -3.77 / -3.85 |
| BANKNIFTY | H4 | 55 | +4.5 | -2.5 | -0.46 | +1.0 | +11 / -15 / -5 | 0.21 | -3.5 | -0.98 / -1.06 |
| BANKNIFTY | H5 | 233 | +3.9 | -3.1 | -1.34 | +0.4 | -1 / -6 / -2 | n/a | -3.1 | -1.39 / -1.60 |
| NIFTY | H1 | 231 | +0.9 | -6.1 | -6.05 | -2.6 | -7 / -7 / -5 | 0.18 | -6.8 | -6.32 / -6.50 |
| NIFTY | H2 | 293 | -1.1 | -8.1 | -10.98 | -4.6 | -9 / -7 / -8 | 0.93 | -7.8 | -11.86 / -11.96 |
| NIFTY | H3a | 239 | -5.3 | -12.3 | -4.09 | -8.8 | -14 / -12 / -11 | 0.96 | -11.9 | -4.20 / -4.15 |
| NIFTY | H3b | 71 | -11.3 | -18.3 | -2.96 | -14.8 | -29 / -15 / -10 | 0.96 | -18.0 | -5.58 / -5.77 |
| NIFTY | H4 | 59 | -2.9 | -9.9 | -3.30 | -6.4 | -9 / -9 / -12 | 0.83 | -9.3 | -6.82 / -6.91 |
| NIFTY | H5 | 232 | +5.8 | -1.2 | -0.61 | +2.3 | -4 / -4 / +4 | n/a | -1.2 | -0.64 / -0.78 |

## Exploratory (seen after the pre-registration, so it cannot pass)

- **Shock continuation.** H2's placebo p of 0.97-1.00 in dev means 3-sigma
  5-minute moves kept going for 15 minutes: following them earned +4.4 bp
  gross on both instruments, decaying across thirds (about +7, +4, +2) and
  ~0 in 2023. Below cost as a taker. It could only matter for a maker that
  pulls quotes on the shock side, which needs the order book (FYERS tapes).
- **Intraday momentum is real but small.** H1's gross is positive in every
  split on both instruments (+0.7 to +1.5 bp), the same sign as Gao et al.,
  but about a fifth of the cost. Useful at most as a tilt on execution timing
  (when a hedge or a straddle leg has to trade in the last half hour anyway).

## What this leaves as a strategy

Direction on futures is closed on this data. The one candidate left is the
one the audit already pointed to, where the forecastable thing (volatility)
is the thing being sold: **S1, the hedged BANKNIFTY short straddle,
"Straddle IV>=RV BANKNIFTY"** (`live/straddle.hpp`, paper). Its specification
and the forward test that can validate it are in [README.md](README.md#s1).
It cannot be backtested on the Drive data (no options) or on FYERS history
(FYERS serves no expired contracts), so its test is forward, on the chains
the recorder saves each day.

## Options: the first five recorded FYERS sessions (4-11 Nov 2026)

Smit, 2026-11-11: "at least do 5 sessions, will get to know how it works".
`option_sessions.py` replays sell-only variants on the chains the recorder
saved (`altair-data/chain_iv`, source `fyers_tape`), one lot, sold at the
minute's mid and bought back at 15:20, every fill charged an assumed
half-spread (Rs 1 BANKNIFTY, Rs 0.25 NIFTY) plus schedule-2 option costs.
Only 6 Nov and 11 Nov were recorded in full; the others start late or end
early (10 Nov has no chain), so a blank means the variant had no clean quote
in the recorded window. **Five sessions prove nothing either way**; this
shows how the trade behaves, not whether it pays.

Net Rs per lot:

| Session | Index | Recorded | Straddle 09:20 | Same, hedged (4 lots) | Straddle 13:00 | 25-delta strangle | 0DTE straddle |
|---|---|---|---|---|---|---|---|
| 04 Nov | BANKNIFTY | 11:58-15:18 |  |  | +1,046 | +865 |  |
| 04 Nov | NIFTY | 11:46-15:18 |  |  | +335 |  |  |
| 05 Nov | NIFTY | 10:22-12:47 | +1,217 | +1,217 |  | +776 |  |
| 06 Nov | BANKNIFTY | full | -16,110 | -8,819 | -5,405 | -11,740 |  |
| 06 Nov | NIFTY | full | -16,500 | -4,350 | -7,187 | -11,420 |  |
| 09 Nov | BANKNIFTY | 09:55-15:30 |  |  | -107 |  |  |
| 09 Nov | NIFTY | 10:36-15:30 |  |  | +208 |  | -113 |
| 11 Nov | BANKNIFTY | full | -2,473 | -3,894 | +1,849 | -2,862 |  |
| 11 Nov | NIFTY | full | -1,845 | -1,325 | -478 | -1,477 |  |
| **Total** | | | **-35,711** (1 win of 5) | **-17,171** (1/5) | **-9,739** (4/8) | **-25,859** (2/6) | -113 (0/1) |

What it shows:

- **The shape of short vol.** Quiet sessions earn Rs 300-1,800 a lot; one
  trend day (6 Nov, BANKNIFTY +2.4 % in a straight line from 57,520 to
  58,900) costs Rs 16,000, ten quiet days' worth. A strategy that sells
  premium lives or dies on skipping or limiting those days.
- **Both full sessions realised more than the IV sold.** BANKNIFTY ATM IV at
  09:20 was 15.8 % on 6 Nov (realised 18.6 %) and 17.3 % on 11 Nov
  (realised 34.6 %). That is exactly the case the IV >= forecast-RV gate is
  for; whether HAR saw it coming is what the paper model's own log will show.
- **Selling later loses less.** The 13:00 straddle lost a third as much on
  6 Nov and won on 11 Nov: less time left for a trend, at nearly the same
  premium (intraday theta is small on a 12-17 day option).
- **A 1-lot BANKNIFTY straddle cannot be delta-hedged.** The futures lot is
  the same size as the option lot, so the straddle's delta (it reached 0.49
  on 6 Nov) never rounds to one futures lot and the hedge never trades
  ("hedged" = unhedged for BANKNIFTY). With 4 straddle lots per hedge it
  halves the trend-day loss on 6 Nov but costs more on the choppy 11 Nov.
- **The strangle is not safer intraday**: it collects a third of the
  straddle's premium and still loses about 70 % as much on the trend day.

Rerun `option_sessions.py` on the latest altair-data to extend this table.
Not built yet: a nightly scorer that does it automatically, so the
60-session verdict in README.md builds up day by day.
