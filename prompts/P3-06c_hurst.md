# P3-06c — `analytics/hurst`: rescaled range, with its error bar

> Phase 3 · Status: **DONE** 2026-09-01 · Depends on: P0-01
> Feeds: P4 (regime features), P6

## 1. THE TWO TRAPS

**T1 — feed it RETURNS, not prices.** R/S is defined on increments. A price
series returns ~1.0 every time, because a price is the running sum of its
returns. Measured on one dataset both ways: **0.9609 as prices, 0.5032 as
returns.** Nothing looks wrong — 0.96 reads as an exceptionally clean signal —
and this is the standard way a backtest claims a trending regime that is not
there.

**T2 — the uncorrected estimator is biased high, worse on short samples:**

| n | corrected | uncorrected | bias |
|---|---|---|---|
| 512 | 0.4701 | 0.5272 | +0.057 |
| 1024 | 0.4949 | 0.5448 | +0.050 |
| 8192 | 0.5032 | 0.5374 | +0.034 |

That bias is the size of the effects people claim to find, on pure noise.
E[R/S] for a finite random walk is not n^0.5 — the implied exponent from
n=64 to 4096 is **0.5234** — so this regresses against the Anis-Lloyd
finite-sample expectation instead.

## 2. DECISIONS

**D1 — one exact formula at every n.** Anis and Lloyd switch to an asymptotic
form above n=340, a 1976 workaround for Gamma overflow that `std::lgamma`
makes unnecessary. It is not free: the asymptotic form is 0.22% BELOW the
exact one across the boundary, so blocks straddling 340 saw a step in their
own null — biasing the null down and H up.

**D2 — the standard error is floored at 1/sqrt(n).** The OLS regression error
measures only scatter about the fitted line, and R/S values at different block
sizes share their data, so they move together. On 256 points of INDEPENDENT
noise the raw version returned 0.387 +/- 0.047 and reported it SIGNIFICANT.
Floored, that becomes 0.0625 and it correctly does not.

**D3 — `hurst_departs_from_random_walk` is the only sanctioned way to act on
the number.** An H of 0.58 with a standard error of 0.06 is a random walk
(ROADMAP section 3). At n=256 the error bar is 0.062, wider than most claimed
effects — which is the fact that decides whether a short-window Hurst is
usable at all.

## 3. MANIFEST

`analytics/hurst.hpp` · `analytics/tests/test_hurst.cpp` ·
`analytics/CMakeLists.txt`

## 4. GATES — all pass

0 warnings · 29 checks. Gate 8: D2 and D3 are error propagation.
