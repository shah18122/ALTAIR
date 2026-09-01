# P4-01 — `risk/sizing`: how many lots, and which constraint decided

> Phase 4 · Status: **DONE** 2026-09-01 · Depends on: P3-09, P3-10, P1 (spec store)
> Feeds: P4-02, P4-07

## DECISIONS

**D1 — the MINIMUM binds, never the average.** Each method answers from a
different premise; averaging produces a number no premise supports.
`SizeDecision` keeps all four answers and names the binding one.

**D2 — quarter Kelly, and the factor of four is the point.** Computed in the
test, not quoted. In units of μ²/σ²:

| bet at | true edge | edge 2× too big |
|---|---|---|
| full Kelly | 0.500 (optimal) | **0.000** |
| quarter Kelly | 0.219 | 0.375 |

Full Kelly tolerates a 2× overestimate before growth reaches zero. Quarter
tolerates **8×**. 56% of the theoretical growth buys a 4× increase in how wrong
the edge may be — and 10-minute edges are wrong by 2× routinely.

**D3 — everything rounds DOWN.** A NIFTY lot is ₹18.06 lakh of notional;
rounding to nearest can exceed the risk budget by most of a lakh.

**D4 — the edge is taken at its LOWER confidence bound** (ROADMAP §3). 8 bps
± 5 gives −2, so zero lots, binding = `NoEdge` — not a generic Kelly bind,
because the cause was *no signal*. The same edge at ±1 bps sizes 5 lots.

**D5 — zero lots is a VALUE, not an error.** "Do not trade this" is the most
common correct answer; making it an exception tempts a caller into routing
around it.

**D6 — the edge must already be net of cost** (rule 5). P3-09 and P3-10 deduct;
a sizer that also deducted would double-count.

## CORRECTED DURING REVIEW

*"Halving volatility doubles the size"* is false with whole contracts —
flooring does not commute with doubling (2.768 → 2, 5.537 → 5, not 4).
`floor(2x) ≥ 2·floor(x)` is the exact statement. Even a lot size of one NIFTY
**unit** does not show the underlying scaling (a unit is ₹24,080); on a ₹1
share the ratio is 2.000000000.

## MANIFEST · GATES
`risk/sizing.hpp` · `risk/tests/test_sizing.cpp` · CMakeLists.
0 warnings · 56 checks.
