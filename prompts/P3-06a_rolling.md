# P3-06a — `analytics/rolling`: streaming moments and fixed windows

> Phase 3 · Status: **DONE** 2026-09-01 · Depends on: P0-01 (`units`)
> Feeds: P3-07, P4 (feature registry), P5-P6

## 1. SCOPE

`RunningMoments` (session-long, O(1), Welford + Pebay) and `RollingWindow<N>`
(fixed capacity, O(1) push, O(N) exact query). Mean, population and sample
variance, skewness, EXCESS kurtosis, z-score, min, max.

## 2. DECISIONS

**D1 — Welford, not the textbook formula.** `sum(x^2)/n - mean^2` cancels
catastrophically on paise magnitudes. Measured on 50,000 quotes:

| quote | moves by | naive error |
|---|---|---|
| Rs 24,080 | +/- 5 paise | 9.4% low |
| Rs 24,080 | +/- 1 paise | 97% low |
| Rs 240,800 | +/- 1 paise | **2300% HIGH — a vol 4.9x too big** |
| Rs 1 crore | +/- 5 paise | NEGATIVE |

Row three is the dangerous one: positive, finite, plausible, and silent.

**D2 — Every statistic returns `std::expected`.** A variance needs 2 points, a
skewness 3, a kurtosis 4, a z-score a non-zero spread. Returning 0.0 for "not
yet" puts a value that passes every finite check into a feature vector.

**D3 — `RollingWindow` queries are O(N), deliberately.** The O(1) subtractive
update reintroduces the cancellation Welford avoids. It drifts to 4.7e-3 after
a volatility regime change — but the real objection is that its accumulator
carries residue from every point that ever passed through, so two windows
holding identical data report different variances. That is a **rule 10**
violation, not a precision one.

**D4 — NO Kahan compensation, measured not assumed.** CLAUDE.md requires it on
session-long accumulators, so it was implemented and benchmarked over 2M
ticks: identical on the mean, marginally worse on the variance. Removed.
Welford is not a summation — its increments shrink as 1/n, so nothing
accumulates for a compensator to recover, and its residual error is
conditioning, not accumulation.

Where the rule DOES bite is a raw sum past 2^53 paise = **Rs 90.07 lakh
crore**. Daily NSE cash turnover (1e14 paise) sums exactly and needs nothing;
daily F&O NOTIONAL turnover (4e16 paise) loses Rs 30,673 a day, all of which
compensation recovers. That is the accumulator P3-09 debt is really about.

## 3. MANIFEST

`analytics/rolling.hpp` · `analytics/tests/test_rolling.cpp` ·
`analytics/CMakeLists.txt`

## 4. GATES — all pass

0 warnings · 55 checks · `add` 7.1 ns, 256-window variance 650 ns.
Gate 7: D1, D4. Gate 8: D3 (reproducibility).

## 5. CARRIED FORWARD

The O(N) query is 650 ns at N=256. A feature registry evaluating hundreds of
windows per tick must read that number before assuming it is free.
