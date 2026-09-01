# P3-08 — `analytics/vix`: India VIX by variance-swap replication

> Phase 3 · Status: **DONE** 2026-09-01 · Depends on: P3-01, P3-05a
> Feeds: P6 (vol strategies), P4 (regime features)

## WHY IT IS WORTH HAVING

It is **model-free**. Nothing in the header prices an option: the formula is
the static replication of a log contract by a 1/K^2-weighted strip of OTM
options. So it is a check on P3-05's SVI fit that shares no code and no
assumptions with it — and the two agree to **0.0025 vol points** (16.6969
continuous, 16.6995 discrete).

Validation: a flat 15-vol surface must return exactly 15, with nothing to tune.
It returns **15.0028** on NIFTY's real Rs 50 ladder.

## THE THREE BIASES, pointing different ways so they do not cancel

**Coarse strikes bias UPWARD.** Rs 50 → 15.0028 · Rs 100 → 15.0116 ·
Rs 500 → 15.2923 · Rs 1000 → 16.1373. Monotone in spacing, so discretisation
and not noise. A thin chain reports more fear than there is.

**Truncation biases DOWNWARD**, and what matters is the range in STANDARD
DEVIATIONS. At 30 days and 15 vol one sigma is 0.043 in log-moneyness, so a
NIFTY chain listed to ±0.5 is eleven sigma wide and loses nothing. At 2.8σ:
−0.0025. At 1.4σ: −0.42. At 0.9σ: −1.42.

**ITM options double count.** An ITM call is an OTM put plus a forward, so the
strip is OTM only. Nothing errors if you get it wrong; the number is just too
big.

## THE THING PEOPLE READ WRONG

**VIX is not the ATM vol.** On the P3-05 slice — 15.06 ATM — this returns
**16.70**. Flattening the same surface at the same ATM vol returns 15.07, which
identifies the 1.64-point gap as the skew rather than a signal.

## DETAILS ASSERTED

Strip walks OUTWARD from K0 so truncation counts gaps where liquidity dies ·
one hole is a gap, two consecutive is an end · ladder endpoints take the FULL
distance to their neighbour, not half · the 30-day interpolation is in TOTAL
variance (doing it in vol is off by 0.83 on a 16d/44d pair).

## MANIFEST · GATES

`analytics/vix.hpp` · `analytics/tests/test_vix.cpp` · CMakeLists.
0 warnings · 47 checks.
