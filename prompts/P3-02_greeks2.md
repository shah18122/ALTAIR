# P3-02 — `analytics/greeks2`: second- and third-order greeks

> Phase 3 · Status: **DONE** 2026-09-01 · Depends on: P3-01
> Feeds: P10 (hedge book), P6 (vol strategies)

Vanna, volga, charm, veta, speed, zomma, colour, ultima on Black-76.

## DECISIONS

**D1 — every greek verified TWO independent ways.** A Python reference catches
a transcription slip; a central difference of the P3-01 greeks catches a wrong
formula, which a reference computed the same wrong way would confirm. Vanna
gets a third: it is a mixed second partial, so `d(vega)/dF` and
`d(delta)/dsigma` must be one number (Clairaut). They are.

**D2 — CHARM IS NOT RIGHT-INDEPENDENT; everything else is.** Call and put delta
differ by the discount factor, which moves with time, so
`charm_call - charm_put = r*df` exactly — 0.1691 against 0.1045. The other
seven are identical for both rights because a call minus a put is a forward,
which has no convexity and no vol sensitivity.

**D3 — charm, veta and colour are per CALENDAR time**, hence minus the dT
derivative, per year. The finite differences assert the sign flip explicitly:
getting it backwards is invisible in the magnitude and wrong in every hedge.

**D4 — `detail::black76_higher_unchecked`**, mirroring greeks.hpp. Not
cosmetic: at the strike where d2 = 0, volga is −6.9e-11 on the exact forward
and −0.155 on the same forward rounded to paise, because a fifth of a paise on
2.4 million moves d2 by 2e-6 and volga is proportional to it.

## MEASURED

| | |
|---|---|
| 3-vol move breaks the delta hedge by | 0.0034 |
| vanna correction cuts it to | 0.0010 (3.3×) |
| overnight delta drift, same price and vol | 0.000475 |
| charm/day predicts | 0.000463; halving the step quarters the residual (4.06) |
| gamma change on a 2% move | 16% |

## MANIFEST · GATES

`analytics/greeks2.hpp` · `analytics/tests/test_greeks2.cpp` · CMakeLists.
0 warnings · 50 checks · all eight gates pass.
