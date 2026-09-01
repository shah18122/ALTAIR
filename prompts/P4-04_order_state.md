# P4-04 — `oms/order_state`: the order lifecycle

> Phase 4 · Status: **DONE** 2026-09-01 · Opens `oms/`
> Feeds: P4-05, P4-06, P4-07, P4-08

`oms/` is THE TRADE HANDLER: nothing outside it places or amends an order, and
this is the only file where an order changes state.

## THE PREMISE

**The broker is not a reliable narrator.** Messages arrive duplicated, out of
order, and after the event they describe. A happy-path New → Ack → Fill test
passes on an implementation that loses a position the first time two messages
cross.

## DECISIONS

**D1 — cumulative quantity only rises.** Fills report a RUNNING TOTAL, not an
increment, precisely so a lost message cannot double-count. A report below what
has been applied is a stale duplicate and is DISCARDED — applying it walks the
position backwards, and the position is what every P4-02 limit checks against.

**D2 — a fill may arrive AFTER the cancel ack, and is accepted.** The fill
happened before the cancel reached the exchange; the messages crossed. Refusing
it leaves Altair believing it is flat while holding 75 units — the worst
failure here, because every downstream check is then computed against a
fiction. The order reopens to `PartiallyFilled`.

**D3 — duplicates are traffic, not errors.** A repeated ack, a resent cancel, a
re-sent final fill all return `Duplicate`. A machine that halted on a resend
would halt on a healthy Tuesday. Refusals and duplicates are counted
SEPARATELY: a healthy order has duplicates and no refusals.

**D4 — an overfill is REFUSED, not clamped.** A broker reporting more done than
was asked is a disagreement about reality; clamping hides the one fact worth
escalating.

**D5 — an amend on a partly filled order returns to PartiallyFilled**, not
Open. Open would claim nothing had been done.

## CORRECTED DURING REVIEW

* I asserted a `Reject` from `Open`. Not in the table, and correctly so: a
  broker rejects a **request**; a resting order the exchange pulls is cancelled
  or expires.
* Test 6 derived timestamps from the fill quantity, so the trailing stale
  report also went backwards in TIME and was refused for the wrong reason,
  never exercising the path it exists for.

## MANIFEST · GATES
`oms/order_state.hpp` · `oms/tests/test_order_state.cpp` ·
`oms/CMakeLists.txt` · root CMakeLists. 0 warnings · 56 checks.
