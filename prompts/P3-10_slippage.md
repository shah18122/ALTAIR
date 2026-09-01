# P3-10 — `risk/slippage`: execution cost learned from fill history

> Phase 3 · Status: **DONE** 2026-09-01 · Depends on: P3-09 (`cost`)
> Feeds: P5 (sizing), P8 (OMS routing)

Distinct from `book/flow.hpp`'s KyleLambda, which measures impact from order
flow in the BOOK. This measures what Altair's own orders cost, which is the
number rule 5 needs.

## DECISIONS

**D1 — the square-root law**, `impact_bps = eta * vol_bps * sqrt(Q/ADV)`,
fitted through the origin. No intercept: an order of zero size moves nothing,
and a fixed cost already has a name (the spread) and a home (P3-09). Two models
charging for the same thing is how a backtest double-counts to profitability.

**D2 — FILLS ARE THE WRONG SAMPLE.** While a passive order rests the market
moves by m, symmetric about zero. If m < 0 it came to you and the order fills
cheap; if m > 0 it ran away and the order misses, leaving you needing the
position at a worse price. Realised cost is c(p) + m either way. Over 800
orders, 48% filled:

| fitted on | eta |
|---|---|
| FILLS ONLY | 0.6348 |
| EVERY ORDER | 0.9026 |
| **truth** | **0.9000** |

Fitting on fills understates cost by **30%**, structurally. So `add` takes the
whole order and prices the unfilled remainder as opportunity cost.

**D3 — a linear model is wrong on both sides at once.** A through-origin line
crosses a square root exactly once — here at 2.1177% of ADV — so it understates
every size below and overstates every size above. Small orders are most of the
count, large ones most of the cost.

**D4 — size on the PESSIMISTIC bound.** ROADMAP §3 says size on the lower bound
of edge; applied to cost that means the upper bound. The premium shrinks as the
model learns — +15.9% at 12 orders, +9.0% at 60, +2.8% at 600 — so it is an
error bar, not a tax. `max_participation` inverts the law, and the inversion is
a SQUARE: halving the cost budget quarters the tradable size.

**D5 — Perold's shortfall decomposed, not blended.** Delay is a latency
problem, impact a sizing problem, opportunity a limit-price problem. Each is
weighted by the quantity it applies to so the three sum exactly; the sign
convention is checked on both sides, because getting it backwards makes every
short look profitable.

## MANIFEST · GATES

`risk/slippage.hpp` · `risk/tests/test_slippage.cpp` · `risk/CMakeLists.txt`.
0 warnings · 43 checks.
