# P3-05b — `analytics/svi_fit`: calibration, weighted by P3-04

> Phase 3 · Status: **DONE** 2026-09-01 · Depends on: P3-05a, P3-04
> Feeds: P3-08, P6

## 1. DECISIONS

**D1 — the five-parameter fit is two problems, one linear.** Holding
(m, sigma) and substituting `y = (k-m)/sigma, z = sqrt(y^2+1)` gives
`w = a + c*y + d*z`, linear in (a, c, d), solved exactly by a ridged 3x3
Cholesky. Only (m, log sigma) are searched, by 2-D Nelder-Mead. `b = d/sigma`,
`rho = c/d`. Exact data recovers all five parameters to 1e-13 in 89 iterations.

The ridge is not decoration: with every quote at nearly the same moneyness the
columns [1, y, z] go collinear and an unridged solve returns something
enormous rather than something wrong-looking.

**D2 — WEIGHTS COME FROM P3-04. This is the point of the card.** Across one
21-strike NIFTY chain the IV error bars span a factor of **2.7e7** — tightest
0.000018 vol, loosest 485. Equal weighting therefore lets the least reliable
quotes on the board pull the surface hardest. Inverse-variance weighting fits
the money strikes **97x better** on identical quotes.

**D3 — the weight converts vol error to VARIANCE error.** The fit is in total
variance, so `dw = 2*sigma*T*dsigma`. Weighting by `1/dsigma^2` directly tilts
the fit by `sigma^2` — four to one across a normal equity smile, and invisible
in the output.

**D4 — a fit is not returned without its arbitrage scan.** `SviFitResult`
carries the P3-05a butterfly scan of its own output, over a range padded 25%
beyond the fitted strikes, because a surface is invariably asked for a vol just
outside what it saw and that is where an over-curved fit breaks first.

**D5 — the inner solve projects into the domain and says so.** When the
unconstrained answer leaves it (d<0, |c|>d, Lee, negative min variance) it
clamps, re-solves `a` exactly against the clamped (c,d), and sets
`projections`. A flat slice returns `rho = 0` rather than a stale or clamped
value: flat data has no identifiable skew and reporting one invites reading a
signal off nothing.

## 2. MANIFEST

`analytics/svi_fit.hpp` · `analytics/tests/test_svi_fit.cpp` ·
`analytics/CMakeLists.txt`

## 3. GATES — all pass

0 warnings · 43 checks. End to end: 21/21 fitted strikes price through
`black76` and invert back through the P3-04 solver to their own vol, within
0.11 vol points of the generating slice despite noise on every quote.

## 4. CORRECTED DURING REVIEW

My first noise model perturbed each quote by its own IV error bar — 485 vol
points at k=+0.25 — and "proved" weighting was 7,000,000x better.
`iv_uncertainty` is a local linearisation `dsigma = dPrice/vega` and stops
meaning anything once one tick spans much of the option value. The
perturbation is now capped at five vol points, while the WEIGHT still uses the
true uncorrected error bar, which is what a caller actually has. **97x is the
honest number.**

## 5. CARRIED FORWARD

* Nelder-Mead is a local search. Seeded at the moneyness of the cheapest
  observed variance, which is close to m, but a pathological chain could still
  find a local optimum. A multi-start would harden it if P6 needs it.
* Certainty across a chain is NOT symmetric: skew leaves the puts with vega and
  the calls without, so the reliable half of a chain is the expensive half.
