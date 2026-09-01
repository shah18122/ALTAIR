# P3-05a — `analytics/svi`: the slice, and its arbitrage conditions

> Phase 3 · Status: **DONE** 2026-09-01 · Depends on: P3-01, P3-04
> Feeds: P3-05b (calibration), P3-08 (India VIX), P6 (vol strategies)

## 1. UNITS, BECAUSE THEY ARE CONSTANTLY CONFUSED

* **w is TOTAL implied variance**, `sigma^2 * T`. Not variance, not vol. SVI is
  parameterised in w because that is the quantity in which no-calendar is
  simply "increasing in T"; in vol it is not.
* **k is log-moneyness `ln(K/F)` against the FORWARD**, matching `black76`.
  Using spot shifts the whole smile by the carry — 0.44% on a 30-day NIFTY —
  which reads as a skew that is not there.

## 2. DECISIONS

**D1 — g(k) < 0 is a NEGATIVE BUTTERFLY PRICE, and the test proves it in
money.** Real butterflies priced through `black76` off each slice, and the
sign of Gatheral-Jacquier g agrees with the sign of the butterfly on **42 of
42 strike-slice pairs**. A surface a little too curved implies negative
probability, prices a butterfly negative, and a scanning strategy will find it
and trade against a number the engine invented.

**D2 — butterflies are spaced in STRIKE, not log-moneyness.** A first draft
spaced them in k, and the CLEAN slice priced one negative at k=-0.10. `K e^-dk,
K, K e^+dk` are not equally spaced in K, so that structure is not a butterfly.
Convexity is in K. As written it would have condemned a good surface.

**D3 — Lee's slope bound is NECESSARY, NOT SUFFICIENT.** The violating
reference slice passes it (`b(1+|rho|) = 0.4975 <= 2`) and still implies
negative density. A fit validated only by its asymptotic slopes is not
validated.

**D4 — a scan is evidence about its range and nothing else.** The same
violating slice scans CLEAN over [0.10, 0.50] because the violation sits at
k=-0.068. The range is an argument; the result carries WHERE the worst point
was. Resolution matters too: 5 points find -0.196 where 4001 find -0.664.

**D5 — calendar is on TOTAL VARIANCE, not vol.** Implied vol routinely falls
with maturity in a stressed market and that is not arbitrage; total variance
falling is. The crossing case here is -4.3e-5 — far too small to see by eye.

**D6 — domain violations are refused, not clamped.** `sigma = 0` gives a kink
and a delta in w''; `|rho| = 1` a corner; negative minimum variance has no
square root at the money strike.

## 3. MANIFEST

`analytics/svi.hpp` · `analytics/tests/test_svi.cpp` ·
`analytics/CMakeLists.txt`

## 4. GATES — all pass

0 warnings · 46 checks. Independent validation: the implied density integrates
to **1.000000000000** over [-4,4] with no negative points on the clean slice.
All 13 strikes on a ladder round-trip slice vol -> black76 -> P3-04 solver to
7.5e-14.

Reference slice: a realistic NIFTY 30-day smile, 15.06% ATM, 29.73% at
k=-0.20, 12.53% at k=+0.10.
