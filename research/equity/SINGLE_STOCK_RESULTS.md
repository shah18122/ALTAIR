# Results: one stock on its own (HDFCBANK)

Rules and pass bars: [SINGLE_STOCK_HYPOTHESES.md](SINGLE_STOCK_HYPOTHESES.md) (be75d00, before any
run). Code `single.py`; numbers `results/single_*.csv`. Dev results were committed before the
2023 holdout ran. bp per trade; P6 is per pair trade (0.5 a leg, held up to 10 days).

## HDFCBANK

| Rule | Dev n | Dev gross | Dev net | Dev t | 2020 / 2022 net | 2023 n | 2023 net | 2023 t | Verdict |
|---|---|---|---|---|---|---|---|---|---|
| P1 intraday momentum | 495 | -3.6 | -11.6 | -6.2 | -14.0 / -9.2 | 241 | -9.9 | -6.0 | FAIL |
| P2 opening range breakout | 495 | +0.8 | -7.2 | -1.0 | -13.5 / -0.8 | 241 | -0.5 | -0.1 | FAIL |
| P3 gap fade | 77 | -7.0 | -15.0 | -1.2 | -2.7 / -29.7 | 63 | -7.6 | -1.2 | FAIL |
| P4 overnight drift | 496 | +4.6 | -5.4 | -0.9 | +7.5 / -18.5 | 242 | -2.3 | -0.5 | FAIL |
| P5 residual reversion vs BANKNIFTY | 249 | -5.8 | -13.8 | -3.6 | -14.7 / -13.0 | 220 | -8.6 | -4.6 | FAIL |
| P6 pair HDFCBANK / ICICIBANK | 19 | +129.5 | **+119.5** | 1.86 | +121.4 / +117.8 | 8 | +14.1 | 0.29 | FAIL (too few trades) |

Every rule's leaked version was refused by the timestamp check.

## Same rules on ten other liquid names (descriptive only)

Names net positive, out of 10 (P6 pairs each name with HDFCBANK):

| Rule | Dev | 2023 | Dev median net | 2023 median net |
|---|---|---|---|---|
| P1 | 0 | 0 | -9.9 | -8.1 |
| P2 | 5 | 0 | -0.7 | -9.5 |
| P3 | 5 | 2 | -2.8 | -11.7 |
| P4 | 5 | 5 | +0.7 | +0.1 |
| P5 | 0 | 0 | -7.2 | -6.7 |
| P6 | 8 | 8 | **+75.0** | **+40.3** |

## Verdict

No single-stock rule passes. Intraday timing rules on one stock (momentum, breakout, gap fade,
reversion against the index) lose after the 8 bp round trip, like the same rules on the index.
Overnight drift is about break-even after futures costs.

The one thing worth following up is **pairs reversion (P6)**: HDFCBANK/ICICIBANK earned
+120 bp a trade in dev, positive in both years, above the placebo and still positive entered a
day late, but with only 19 trades (t 1.86, bar 2.64). In 2023 it made +14 bp on 8 trades
(HDFC merger year). The same rule paired against HDFCBANK was net positive for 8 of 10 other
names in both dev and 2023. That pattern is exploratory, so it cannot pass here; it needs its
own pre-registered test with many pairs (more trades) before it can be called an edge.
