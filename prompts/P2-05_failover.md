# P2-05 — `feed/failover`: the stall watchdog and primary switch

> Phase 2 · Card 5 of 14 · Status: **DONE** 2026-08-31
> Depends on: P2-04 (`Normaliser::set_active`) · P2-02 (the recv_ts finding)

## 1. IT KEYS ON `recv_ts`, NEVER `exchange_ts`

This is the direct consequence of P2-02's finding. Kite's exchange timestamp is
**one-second resolution**, and two of its five modes carry none at all. A 250 ms
staleness threshold measured against a clock that only moves once a second
would fire constantly on a perfectly healthy feed. `recv_ts` comes off the TSC,
is locally monotonic, and is exactly what "have we heard anything lately" needs.

## 2. THE SIX DECISIONS

**D1 — Nothing here reads a clock.** `now` is a parameter, so a replay of the
same arrival pattern produces the same switches (rule 6).

**D2 — A source never heard from is `Unknown`, not `Live` and not `Stale`.**
Treating silence-so-far as healthy would let the watchdog fail over to a feed
that has never delivered a byte. Treating it as stale would trip `NoneHealthy`
before the session has begun. Same phantom-default-state shape as P1-06 and
P2-08, in its feed-health costume.

**D3 — Leaving a dead primary is IMMEDIATE. Returning is not.**
There is no stability wait on the way out: every tick spent on a silent feed is
lost. Coming back is gated on `failback_stable_s`, because a feed flapping
either side of the threshold would otherwise drag the primary with it, and
every switch costs a subscription round trip.

**D4 — Both sources stale is `NoneHealthy`, and the primary does NOT move.**
Switching to an equally dead feed would look like a fix and change nothing. It
is counted so it can be alarmed on.

**D5 — Disabled still reports.** With `enabled = false` the health is still
computed and the suppressed switch is counted, so an operator can see what the
watchdog *would* have done before trusting it with the decision.

**D6 — An out-of-order arrival never lowers the high-water mark.**
One late straggler must not make a live source look stale.

## 3. FILE MANIFEST

```
CREATE   feed/failover.hpp
CREATE   feed/tests/test_failover.cpp
MODIFY   feed/CMakeLists.txt
```

## 4. TESTS

Eight, one per decision plus the composition: test 8 drives a real `Normaliser`
through a switch and asserts the sequence runs **unbroken across it** — no
repeat, no gap — because `seq` never came from the feed.

## 5. REVIEW RECORD

Reviewed 2026-08-31. MSVC, `/W4`, zero warnings, 28/28 ctest.

Gate 7 note: no arithmetic on money here, but the failure mode is financial in
both directions. Failing over too eagerly costs a subscription round trip and a
gap in the book; failing over too late means trading on a feed that stopped
telling the truth. The asymmetry in D3 is the deliberate answer, and the
stability window is config rather than a literal.
