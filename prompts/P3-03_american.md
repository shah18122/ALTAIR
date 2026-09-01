# P3-03 — `analytics/american`: Bjerksund–Stensland

> Phase 3 · Status: **DONE** 2026-09-01 · Depends on: P3-01
> Feeds: nothing on an NSE path today — see the scope note

## SCOPE — READ FIRST

**Nothing Altair currently trades needs this.** NIFTY and BANKNIFTY index
options have always been European; NSE moved single stock options to European
exercise years ago; MCX commodity options are European on futures. Every option
Altair will see is priced exactly by `black76` / `black_scholes`.

Built because ROADMAP §674 lists it, and because the premise is a
market-structure fact rather than a mathematical one — if Altair reaches a
venue with American exercise, this is the right tool. **Verify the exercise
style against a current exchange circular before relying on either statement.**

## DECISIONS

**D1 — verified against a BINOMIAL, not a published constant.** The test builds
a 1500-step CRR tree: no shared algebra, converges to truth, makes the exercise
decision at every node. This mattered — I recalled 5.2704 for r=0.08 and got
5.1803; the published case is r=0.04, where this returns 5.27040 exactly.

**D2 — IT IS A LOWER BOUND.** A flat exercise boundary is sub-optimal, so it
can only under-price. Below the tree in all 7 cases and across 792 sampled
parameter combinations. **Safe when buying, dangerous when selling** — quoting
it for a short American option quotes below what the option is worth.

**D3 — where the error is worst matters more than its size.** The worst
relative error (−2.62%) lands on the cheapest option in the set, 52.92 paise
deep OTM at low vol. In absolute terms that is 1.38 paise — **0.28 of a
5-paise tick**, below quotable resolution.

**D4 — two exact facts, not approximations.** A call with carry ≥ rate is never
exercised early, so the header returns the European price bit for bit. And an
American put on (S,K,r,b) is exactly an American call on (K,S,r−b,−b), so the
put is the transformed call: one boundary formula in the file, and the put
inherits both the accuracy and the lower-bound direction.

## MANIFEST · GATES

`analytics/american.hpp` · `analytics/tests/test_american.cpp` · CMakeLists.
0 warnings · 26 checks.
