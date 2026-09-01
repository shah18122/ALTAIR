# P4-02 — `risk/limits`: pre-trade checks and the kill switch

> Phase 4 · Status: **DONE** 2026-09-01 · Depends on: P4-01, P1 (spec store)
> Feeds: P4-04, P4-07, P4-08

## DECISIONS

**D1 — every check runs; the order is not rejected on the first.** One
violation is a sizing bug, four is a broken upstream, and only the full mask
says which. Result is a bitmask; the cost is a few branches per ORDER.

**D2 — conservation is EXACT integer paise, no tolerance.** On a one-paisa
breach at ₹4 lakh crore of notional, where a double's ulp is 8 paise:

| | residual |
|---|---|
| integer paise | **−1 → BREACH** |
| same in double | **0.0 → looks balanced** |

A conservation check in double is not a weaker version of the integer one; it
answers yes when the answer is no. Only checkable exactly because rule 3 keeps
the ledger in integers.

**D3 — the kill switch is STICKY.** It refuses the order that passed a moment
earlier, still refuses an hour later, and clears only on an explicit
acknowledgement token — `reset(0)` and `reset(1)` are no-ops, because `reset()`
is too easy to reach for in a retry loop. Checked FIRST, short-circuits to one
bit. The FIRST trip reason is kept; later trips are consequences.

**D4 — two zeros that mean opposite things.** A zero lot or tick size means the
spec store was read before it was loaded → **BLOCKS**. A zero price band means
the exchange publishes none → **passes**. Conflating them is the
phantom-default defect.

**D5 — caps are checked against the RESULTING position.** A cap checked against
what you already hold approves the order that breaches it. A reduction landing
under the cap always passes, so an oversized book can be brought back.

**D6 — staleness rejects FUTURE quotes too.** Accepting one would be look-ahead
(rule 7).

## MANIFEST · GATES
`risk/limits.hpp` · `risk/tests/test_limits.cpp` · CMakeLists.
0 warnings · 49 checks.
