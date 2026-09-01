# P3-06b — `analytics/ewma`: exponential averages with a TIME constant

> Phase 3 · Status: **DONE** 2026-09-01 · Depends on: P0-01, P0-03 (`timestamp`)
> Feeds: P4 (features), P3-08, P6

## 1. DECISIONS

**D1 — alpha is derived per update from elapsed time on the tick:**
`alpha = 1 - exp(-dt/tau)`. A fixed alpha measures memory in TICKS, so its
time constant is really the tick rate — which on NSE varies by orders of
magnitude between 09:15 and 13:00. Same path, same wall-clock second:

| tick spacing | time-decayed | fixed-alpha |
|---|---|---|
| 100 us | 199.326205 | 200.000 |
| 1 ms | 199.326205 | 199.326 |
| 10 ms | 199.326205 | 139.347 |
| 50 ms | 199.326205 | 109.516 |

A 90.5-point spread on identical data. A model trained on that learns the
exchange message rate. The time-decayed step response is analytic and
sampling-independent, so the test asserts it exactly.

**D2 — Seeded by the first observation, not by zero.** No Adam-style bias
correction needed: the first tick is a fine estimate of the mean when it
arrives.

**D3 — The variance recursion is BIAS CORRECTED.** The standard form is low by
5% at alpha 0.095, 46% at 0.63, 76% at 0.86 — the exponential-weights analogue
of dividing by n rather than n-1, and much larger because the weights are
concentrated. A 46%-low vol does not look broken; it looks like a quiet market
and oversizes the book. Corrected to within 0.2% at every alpha via the
weighted-sample reliability factor, carried recursively since alpha varies.
(The post-update-mean variant, the other common slip, is 92.7% low.)

**D4 — The denominator is carried, never subtracted.** `1 - V/W^2` loses eight
significant digits after a long gap, when alpha approaches 1 — on an illiquid
counter or the first ticks after a halt. Carried as
`D <- (1-a)^2 D + 2a(1-a)W`, every term positive.

**D5 — Out-of-order refused and counted; coincident ticks carry zero weight.**
P2-04 deliberately does not require monotonic timestamps. A negative dt gives
alpha > 1, which overshoots rather than averages.

## 2. MANIFEST

`analytics/ewma.hpp` · `analytics/tests/test_ewma.cpp` ·
`analytics/CMakeLists.txt`

## 3. GATES — all pass

0 warnings · 45 checks · 14.1 ns mean update, 17.1 ns variance update.
Gate 8: D1 is the Nyquist/stationarity argument made concrete.

## 4. CORRECTED DURING REVIEW

I asserted two observations give an effective sample size of exactly 2. It is
1.87 — unequal weights always give an effective n below their count. The
invariant that IS exact is weight-independence: two points give
`(x2-x1)^2/2` at every spacing from 1 ms to 20 taus.
