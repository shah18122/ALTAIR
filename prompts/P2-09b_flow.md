# P2-09b — `book/flow`: VPIN and Kyle's lambda

> Phase 2 · Card 9b of 14 · Status: **DONE** 2026-08-31
> Depends on: P2-08 (`BookState`) · P2-09a (`mid`)

## 1. ROADMAP §3 GOVERNS THIS FILE

**Measurements carry error.** Neither estimator returns a bare number: each
reports the sample it was computed from, and each returns **empty** until it
has enough. An estimate from three observations is not a small number with a
wide error bar — it is noise wearing the costume of a signal. "Size on the
lower confidence bound of edge, never the point estimate" only means anything
if the point estimate arrives with something to bound it.

## 2. THE SEVEN DECISIONS

**D1 — VPIN buckets by VOLUME, not time.** That is the whole point of it.
Clock-time buckets sample a process whose activity varies by orders of
magnitude across a session: a fixed interval oversamples the quiet middle and
undersamples the open. Volume buckets make every observation carry the same
amount of trading.

**D2 — Trade classification needs the PRIOR book.** Lee-Ready against the book
that already absorbed the trade is look-ahead: the very tick being classified
has moved the quote, and the answer becomes near-tautological. A print exactly
at the mid is `Unknown`, not a coin flip — a tick-rule fallback would
manufacture a side from nothing, and at-mid prints are common enough that the
invention would matter.

**D3 — A trade larger than one bucket is SPLIT across buckets.** A block print
is exactly when VPIN should move; assigning it wholly to one bucket would spike
that bucket and starve the next.

**D4 — Kyle's lambda is fitted THROUGH THE ORIGIN, with no intercept.**
An intercept is a price change that happens with zero order flow, which is not
something this model claims exists. Fitting one would quietly absorb drift and
flatter the slope. Test 6 constructs exactly that trap — price drifting up
regardless of flow direction — and asserts the estimator reports a near-zero
slope with a near-zero R².

**D5 — Lambda is DIMENSIONED: paise per unit.** The unit is the content. λ
times a size in units gives an expected move in paise, which is what a sizing
rule needs.

**D6 — Unknown-side volume fills a bucket but takes no side.** It is volume
that happened; pretending otherwise would stretch each bucket over a longer
stretch of tape. It contributes to neither numerator side, so it dilutes rather
than being silently credited to one.

**D7 — Nothing is reported below `min_samples` / `min_buckets`**, and every
reading carries its own sample size.

## 3. FILE MANIFEST

```
CREATE   book/flow.hpp
CREATE   book/tests/test_flow.cpp
MODIFY   book/CMakeLists.txt
```

## 4. REVIEW RECORD

Reviewed 2026-08-31. MSVC, `/W4`, zero warnings, 29/29 ctest, 45 checks.

**The test caught its own author.** Test 6 was meant to show a poor fit, but
the "noise" I first wrote had its signs tracking the flow signs exactly — so
the data was *highly* correlated and R² came back 0.74 against an asserted
< 0.5. The estimator was right and the test data was wrong. Rewritten as price
drifting up **regardless** of flow direction, which is both genuinely
uncorrelated and a better story: it is precisely the case D4 exists for. A
contrast case was added alongside, so the low R² is shown to be meaningful
rather than something the estimator always returns.

Gate 7: doubles throughout, within rule 3's analytics carve-out. VPIN is
dimensionless in [0, 1]; λ is paise per unit and dimensioned. Neither ever
reaches the ledger without going back through integer paise.
