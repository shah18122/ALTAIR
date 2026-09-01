# P3-07 — `analytics/derivatives`: multi-scale velocity, acceleration, jerk

> Phase 3 · Status: **DONE** 2026-09-01 · Depends on: P0-01, P0-03
> Feeds: P4 (feature registry), P6

## THE PROBLEM

Differentiation multiplies each Fourier component by its frequency, and price
noise lives at the top of the band. On a CONSTANT quote plus a one-tick bid-ask
bounce — true velocity exactly zero — the two-point difference at 10 ms returns
**+500 paise/second**. It is not a noisy trend estimate; it is an accurate
measurement of the spread.

## DECISIONS

**D1 — local polynomial, not smooth-then-difference.** Fit in a time window,
differentiate the polynomial. Smoothing first then differencing commits to one
filter and then applies the amplifying operator to what it left.

**D2 — the scale is an argument and there is no default.** Same series:

| scale | velocity |
|---|---|
| 0.20 s | −14,246 |
| 0.50 s | +7,079 |
| 2.00 s | +18,718 |
| 5.00 s | +19,775 |

All true. Every result carries its own scale so two cannot be compared across
scales by accident.

**D3 — centred and normalised abscissa AND ordinate.** Found by test, not
reasoning. A derivative window lies entirely on ONE side of `t_eval`, giving a
one-sided Vandermonde whose condition number the normal equations then square:
a cubic through exact cubic data returned 299.99794. Normalising u to [−1,1]
took it to 1e-7; centring the ordinate (a 2.4e6 price beside a 238 slope) took
it to **exact**.

**D4 — rule 7 enforced structurally.** Points stamped after `t_eval` are
excluded, so a future outlier of 9,999,999 paise leaves the answer at 100.0000.

## MEASURED TRADEOFF

It reverses, which is why both halves are tested: on a pure drift the 4 s
window is 73× quieter AND closer; on a 0.5 Hz sine the 0.2 s window is accurate
and the 4 s window has the **sign wrong**.

## MANIFEST · GATES

`analytics/derivatives.hpp` · `analytics/tests/test_derivatives.cpp` ·
CMakeLists. 0 warnings · 34 checks.
