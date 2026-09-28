// P4-04 acceptance tests for oms/order_state.hpp, extended by CX02-B6.
//
// Every test here is about the broker being an unreliable narrator, because
// that is the only thing that makes an order state machine hard. A happy-path
// New -> Ack -> Fill test passes on an implementation that would lose a
// position the first time two messages crossed in flight.
//
// The three that matter:
//   * test 2 -- a stale fill report walking the position backwards
//   * test 3 -- a fill arriving AFTER the cancel was acknowledged
//   * test 4 -- duplicates being normal traffic rather than protocol errors
//
// Tests 7-12 are CX02-B6 (findings C13-001, C13-002, C13-003). Each one's
// trigger was replayed against the pre-CX02 header by
// prompts/cx02/evidence/replay_old_order_state.cpp, which shows the old table
// losing the position on exactly these sequences.
//
// No check description here may contain the substring FAIL.

#include <oms/order_state.hpp>

#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <string_view>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

} // namespace

using namespace altair;

namespace {

constexpr std::int64_t kMs = 1'000'000;
Timestamp at(std::int64_t ms) { return Timestamp{ms * kMs}; }

/// 2 NIFTY lots.
Order fresh()
{
    return *open_order(Qty{150}, at(0));
}

FillReport fill(std::int64_t cum, std::int64_t ms)
{
    return FillReport{Qty{cum}, Price{2'408'000}, at(ms)};
}

// ── 1 ────────────────────────────────────────────────────────────────────
void the_happy_path_walks_new_to_filled()
{
    std::printf("\n1 the_happy_path_walks_new_to_filled\n");
    Order o = fresh();
    check(o.state == OrderState::PendingNew, "a new order is PendingNew");
    check(o.leaves().raw() == 150 && o.cum_qty.raw() == 0,
          "with everything still working");
    check(is_live(o.state) && !is_terminal(o.state), "and it is live");

    check(apply(o, OrderEvent::Ack, at(10)) == Applied::Changed
          && o.state == OrderState::Open, "the ack opens it");

    check(apply_fill(o, FillReport{Qty{75}, Price{2'408'000}, at(20)})
          == Applied::Changed, "a partial fill applies");
    check(o.state == OrderState::PartiallyFilled && o.cum_qty.raw() == 75
          && o.leaves().raw() == 75,
          "and leaves 75 working of 150");

    check(apply_fill(o, FillReport{Qty{150}, Price{2'408'050}, at(30)})
          == Applied::Changed, "the completing fill applies");
    check(o.state == OrderState::Filled && o.leaves().raw() == 0 && o.done(),
          "the order is Filled with nothing left");
    check(is_terminal(o.state) && !is_live(o.state),
          "Filled is terminal and no longer live");
    check(o.avg_price.raw() == 2'408'050,
          "and the average price is the one the last report carried");
    check(o.refused == 0, "nothing was refused along the way");

    // A default-constructed order is not a live one.
    Order zero{};
    check(zero.state == OrderState::Unset && !is_live(zero.state),
          "a zeroed Order reads as Unset, not as a live order");
    check(!apply(zero, OrderEvent::Ack, at(1)),
          "and nothing can be applied to it");
    check(!open_order(Qty{0}, at(0)) && !open_order(Qty{-5}, at(0)),
          "a zero or negative order quantity is refused");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// Fills carry a RUNNING TOTAL. A report below what has been applied is stale.
void a_stale_fill_never_walks_the_position_backwards()
{
    std::printf("\n2 a_stale_fill_never_walks_the_position_backwards\n");
    Order o = fresh();
    (void)apply(o, OrderEvent::Ack, at(10));
    (void)apply_fill(o, FillReport{Qty{100}, Price{2'408'000}, at(20)});
    check(o.cum_qty.raw() == 100, "100 done");

    // The broker resends an earlier report. Out-of-order delivery is routine.
    const auto r = apply_fill(o, FillReport{Qty{75}, Price{2'407'000}, at(21)});
    std::printf("    cum 100, then a report of 75 arrives -> %s, cum stays %lld\n",
                r && *r == Applied::Duplicate ? "Duplicate" : "??",
                static_cast<long long>(o.cum_qty.raw()));
    check(r && *r == Applied::Duplicate,
          "a report below the running total is absorbed as a duplicate");
    check(o.cum_qty.raw() == 100,
          "and the position does NOT go backwards -- every risk limit is"
          " checked against this number");
    check(o.avg_price.raw() == 2'408'000,
          "nor is the average price overwritten by the stale one");
    check(o.duplicates == 1 && o.refused == 0,
          "it is counted as a duplicate, not as a refusal: out-of-order"
          " delivery is normal traffic, not a protocol violation");

    // An exactly-equal report is also a duplicate.
    check(apply_fill(o, FillReport{Qty{100}, Price{2'408'000}, at(22)})
          == Applied::Duplicate, "an identical repeat is a duplicate too");
    check(o.cum_qty.raw() == 100, "and changes nothing");

    // An OVERFILL is refused, not clamped.
    const auto over = apply_fill(o, FillReport{Qty{200}, Price{2'408'000},
                                               at(23)});
    check(!over && over.error() == OrderError::QuantityExceedsOrder,
          "a report of more done than was ordered is REFUSED, not clamped --"
          " clamping would hide the one fact worth escalating");
    check(o.cum_qty.raw() == 100, "and leaves the order untouched");
    check(o.refused == 1, "counted as a refusal, because this one is");

    // The OMS's own receipt sequence going backwards is refused.
    const auto back = apply_fill(o, FillReport{Qty{125}, Price{2'408'000},
                                               at(5)});
    check(!back && back.error() == OrderError::TimeWentBackwards,
          "an event stamped before the last one applied is refused");
}

// Positive cumulative fills need a real positive execution price.
// Price is integral paise, so NaN/infinity cannot be represented in this API.
void a_fill_requires_a_positive_average_price_without_mutating_the_order()
{
    std::printf("\n2b a_fill_requires_a_positive_average_price_without_mutating_the_order\n");
    Order o = fresh();
    (void)apply(o, OrderEvent::Ack, at(10));
    (void)apply_fill(o, fill(75, 20));

    const OrderState state_before = o.state;
    const Qty cum_before = o.cum_qty;
    const Price avg_before = o.avg_price;
    const Timestamp event_before = o.last_event;
    const auto zero = apply_fill(o, FillReport{Qty{100}, Price{0}, at(30)});
    check(!zero && zero.error() == OrderError::BadFillPrice,
          "a positive cumulative fill at zero paise is refused");
    check(o.state == state_before && o.cum_qty == cum_before
              && o.avg_price == avg_before && o.last_event == event_before,
          "a refused zero-price fill does not mutate order, quantity, average or event stamp");

    const auto negative = apply_fill(
        o, FillReport{Qty{100}, Price{-1}, at(31)});
    check(!negative && negative.error() == OrderError::BadFillPrice,
          "a negative average execution price is refused too");
    check(o.state == state_before && o.cum_qty == cum_before
              && o.avg_price == avg_before && o.last_event == event_before,
          "the negative-price refusal also leaves the order unchanged");
    check(o.refused == 2,
          "both malformed reports are counted as refusals");

    // The numeric validation precedes stale/duplicate handling: a bad price
    // is not laundered into a harmless duplicate merely because its quantity
    // is old.
    const auto stale_bad = apply_fill(
        o, FillReport{Qty{75}, Price{0}, at(32)});
    check(!stale_bad && stale_bad.error() == OrderError::BadFillPrice,
          "a stale cumulative quantity does not hide an invalid fill price");
    check(o.state == state_before && o.cum_qty == cum_before
              && o.avg_price == avg_before && o.last_event == event_before,
          "and its rejection still does not change the order");

    const auto price_without_fill = apply_fill(
        o, FillReport{Qty{0}, Price{2'408'000}, at(33)});
    check(!price_without_fill
              && price_without_fill.error() == OrderError::BadFillPrice,
          "a positive average price with zero cumulative quantity is malformed");
    check(o.state == state_before && o.cum_qty == cum_before
              && o.avg_price == avg_before && o.last_event == event_before
              && o.refused == 4,
          "price-without-fill is refused atomically and counted");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// The race that loses a position if you get it wrong.
void a_fill_may_arrive_after_the_cancel_was_acknowledged()
{
    std::printf("\n3 a_fill_may_arrive_after_the_cancel_was_acknowledged\n");
    Order o = fresh();
    (void)apply(o, OrderEvent::Ack, at(10));
    (void)apply(o, OrderEvent::CancelSent, at(20));
    check(o.state == OrderState::PendingCancel, "the cancel is in flight");
    (void)apply(o, OrderEvent::CancelAck, at(30));
    check(o.state == OrderState::Cancelled && is_terminal(o.state),
          "and is acknowledged -- the order is Cancelled");

    // The fill happened at the exchange BEFORE the cancel got there. The
    // messages crossed. Refusing this leaves Altair believing it is flat
    // while holding 75 units of NIFTY.
    const auto late = apply_fill(o, FillReport{Qty{75}, Price{2'408'000},
                                               at(40)});
    std::printf("    a fill for 75 arrives 10 ms after the cancel ack -> %s\n",
                late ? "ACCEPTED" : "refused");
    check(late && *late == Applied::Changed,
          "a fill after the cancel ack is ACCEPTED -- it happened before the"
          " cancel reached the exchange, and the messages crossed");
    check(o.cum_qty.raw() == 75,
          "the position is recorded, rather than Altair believing it is flat"
          " while holding 75 units");
    // CX02-B6: this used to REOPEN the order to PartiallyFilled, which made
    // it live with 75 units "working" that the exchange had already pulled.
    check(o.state == OrderState::Cancelled && !is_live(o.state),
          "and the order STAYS Cancelled -- the fill is history, not a"
          " reopening; nothing is working at the exchange");
    check(o.refused == 0, "with nothing refused -- this is legal traffic");

    // A crossed fill that completes the order makes it Filled.
    Order full = fresh();
    (void)apply(full, OrderEvent::Ack, at(10));
    (void)apply(full, OrderEvent::CancelSent, at(20));
    (void)apply(full, OrderEvent::CancelAck, at(30));
    (void)apply_fill(full, fill(150, 40));
    check(full.state == OrderState::Filled,
          "a crossed fill for the whole quantity makes it Filled: the cancel"
          " arrived after there was nothing left to cancel");

    // The exception is exactly one event wide. Everything else out of a
    // terminal state is still refused.
    // Note the reject comes from PendingNew, with NO ack first. A broker
    // rejects a REQUEST; a resting order that the exchange pulls is cancelled
    // or expires. Rejecting from Open is not in the transition table, and my
    // first draft of this test assumed it was.
    Order t = fresh();
    (void)apply(t, OrderEvent::Reject, at(20));
    check(t.state == OrderState::Rejected,
          "a broker refusing the new order leaves it Rejected");
    check(!apply(t, OrderEvent::Ack, at(30)),
          "and nothing reopens it -- not an ack");
    check(!apply_fill(t, FillReport{Qty{75}, Price{2'408'000}, at(31)}),
          "not even a fill: a REJECTED order never reached the book, so a"
          " fill against it is a broker disagreement, not a crossed message");
    check(t.refused == 2, "both refusals counted");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void duplicates_are_normal_traffic_and_illegal_moves_are_not()
{
    std::printf("\n4 duplicates_are_normal_traffic_and_illegal_moves_are_not\n");
    Order o = fresh();
    check(apply(o, OrderEvent::Ack, at(10)) == Applied::Changed,
          "the first ack changes the state");
    const auto again = apply(o, OrderEvent::Ack, at(11));
    check(again && *again == Applied::Duplicate,
          "a repeated ack is a DUPLICATE, not an error -- brokers resend, and"
          " a machine that halted on that would halt on a healthy Tuesday");
    check(o.state == OrderState::Open, "and leaves the state alone");
    check(o.duplicates == 1 && o.refused == 0,
          "counted as a duplicate rather than a refusal");

    // A resent cancel while one is already pending is likewise normal.
    (void)apply(o, OrderEvent::CancelSent, at(20));
    const auto recancel = apply(o, OrderEvent::CancelSent, at(21));
    check(recancel && *recancel == Applied::Duplicate,
          "a resent cancel while one is pending is a duplicate");

    // But an illegal move is an ERROR and is counted, never silently ignored.
    Order p = fresh();
    const auto bad = apply(p, OrderEvent::ReplaceAck, at(10));
    check(!bad && bad.error() == OrderError::IllegalTransition,
          "a ReplaceAck with no amend outstanding is refused");
    check(p.state == OrderState::PendingNew,
          "and the state is untouched");
    check(p.refused == 1,
          "and it is COUNTED -- a machine that quietly ignores what it does"
          " not understand disagrees with the broker until reconciliation"
          " finds out hours later");

    // Fill events must go through apply_fill, which carries a quantity.
    const auto wrong = apply(p, OrderEvent::Fill, at(11));
    check(!wrong,
          "a Fill cannot be applied without a quantity, so the event form is"
          " refused rather than treated as a zero fill");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void amendments_return_to_the_right_state()
{
    std::printf("\n5 amendments_return_to_the_right_state\n");
    // An amend on an untouched order returns to Open.
    Order a = fresh();
    (void)apply(a, OrderEvent::Ack, at(10));
    (void)apply(a, OrderEvent::ReplaceSent, at(20));
    check(a.state == OrderState::PendingReplace, "the amend is in flight");
    (void)apply(a, OrderEvent::ReplaceAck, at(30));
    check(a.state == OrderState::Open,
          "and an amend on an untouched order returns to Open");

    // An amend on a partly filled order must return to PartiallyFilled, NOT
    // to Open. Returning to Open would say nothing had been done, and the
    // position would then be reconciled against a lie.
    Order b = fresh();
    (void)apply(b, OrderEvent::Ack, at(10));
    (void)apply_fill(b, FillReport{Qty{75}, Price{2'408'000}, at(20)});
    (void)apply(b, OrderEvent::ReplaceSent, at(30));
    (void)apply(b, OrderEvent::ReplaceAck, at(40));
    check(b.state == OrderState::PartiallyFilled,
          "an amend on a PARTLY FILLED order returns to PartiallyFilled, not"
          " to Open -- Open would claim nothing had been done");
    check(b.cum_qty.raw() == 75 && b.leaves().raw() == 75,
          "and the done quantity survives the amend");

    // A fill can complete an order while an amend is outstanding.
    Order c = fresh();
    (void)apply(c, OrderEvent::Ack, at(10));
    (void)apply(c, OrderEvent::ReplaceSent, at(20));
    check(apply_fill(c, FillReport{Qty{150}, Price{2'408'000}, at(25)})
          == Applied::Changed,
          "a fill can complete an order while an amend is outstanding");
    check(c.state == OrderState::Filled,
          "and it goes to Filled, because it is done whatever the amend does");

    // An ack that arrives when part is already done opens to PartiallyFilled.
    Order d = fresh();
    (void)apply_fill(d, FillReport{Qty{75}, Price{2'408'000}, at(5)});
    (void)apply(d, OrderEvent::Ack, at(10));
    check(d.state == OrderState::PartiallyFilled,
          "an ack arriving after a fill opens to PartiallyFilled, not Open --"
          " the two messages crossed, and the fill is the newer fact");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void leaves_quantity_is_never_negative()
{
    std::printf("\n6 leaves_quantity_is_never_negative\n");
    Order o = fresh();
    (void)apply(o, OrderEvent::Ack, at(10));
    bool ever_negative = false;
    bool monotone = true;
    std::int64_t prev = 0;
    // The timestamp advances with the STEP, not with the quantity. Deriving it
    // from q made the trailing stale report also go backwards in time, so it
    // was refused for the wrong reason and never exercised the stale-quantity
    // path this test exists for.
    std::int64_t step = 20;
    for (std::int64_t q : {25, 50, 50, 100, 100, 150, 150, 75}) {
        (void)apply_fill(o, FillReport{Qty{q}, Price{2'408'000}, at(++step)});
        if (o.leaves().raw() < 0) { ever_negative = true; }
        if (o.cum_qty.raw() < prev) { monotone = false; }
        prev = o.cum_qty.raw();
    }
    std::printf("    after a jumbled fill sequence: cum %lld, leaves %lld,"
                " %u duplicates\n",
                static_cast<long long>(o.cum_qty.raw()),
                static_cast<long long>(o.leaves().raw()), o.duplicates);
    check(!ever_negative, "leaves quantity is never negative at any point");
    check(monotone,
          "and the cumulative quantity never decreases, through repeats and"
          " a trailing stale report");
    check(o.cum_qty.raw() == 150 && o.state == OrderState::Filled,
          "the order ends Filled at the ordered quantity");
    check(o.duplicates == 4,
          "with the three repeats and the one stale report absorbed");
    check(o.refused == 0, "and nothing refused -- all of it was legal");
}

// ── 7 ────────────────────────────────────────────────────────────────────
// C13-001. The old table: PendingCancel -> partial fill -> PartiallyFilled,
// then CancelAck ILLEGAL and refused; the dead order stayed live.
void partial_fill_during_pending_cancel_keeps_the_cancel()
{
    std::printf("\n7 partial_fill_during_pending_cancel_keeps_the_cancel\n");
    Order o = fresh();
    (void)apply(o, OrderEvent::Ack, at(10));
    (void)apply(o, OrderEvent::CancelSent, at(20));
    check(apply_fill(o, fill(75, 25)) == Applied::Changed,
          "a partial fill while the cancel is out applies");
    check(o.state == OrderState::PendingCancel && o.cum_qty.raw() == 75,
          "and the cancel is STILL pending, with 75 done");
    const auto ack = apply(o, OrderEvent::CancelAck, at(30));
    check(ack && *ack == Applied::Changed && o.state == OrderState::Cancelled,
          "so the CancelAck that follows is ACCEPTED -- it used to be refused"
          " as illegal from PartiallyFilled");
    check(!is_live(o.state) && o.cum_qty.raw() == 75 && o.refused == 0,
          "and the order is dead with 75 done and nothing refused");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void partial_fill_during_pending_replace_keeps_the_replace()
{
    std::printf("\n8 partial_fill_during_pending_replace_keeps_the_replace\n");
    Order o = fresh();
    (void)apply(o, OrderEvent::Ack, at(10));
    (void)apply(o, OrderEvent::ReplaceSent, at(20));
    (void)apply_fill(o, fill(75, 25));
    check(o.state == OrderState::PendingReplace,
          "a partial fill leaves the amend pending");
    const auto ack = apply(o, OrderEvent::ReplaceAck, at(30));
    check(ack && o.state == OrderState::PartiallyFilled && o.refused == 0,
          "and the ReplaceAck lands on a PARTLY FILLED order, refused nothing");
}

// ── 9 ────────────────────────────────────────────────────────────────────
// C13-002. The old table: a refused cancel was `Reject`, and made the ORDER
// terminal Rejected; the fill that followed was refused.
void a_refused_cancel_or_amend_is_not_a_refused_order()
{
    std::printf("\n9 a_refused_cancel_or_amend_is_not_a_refused_order\n");
    Order o = fresh();
    (void)apply(o, OrderEvent::Ack, at(10));
    (void)apply(o, OrderEvent::CancelSent, at(20));
    const auto cr = apply(o, OrderEvent::CancelReject, at(30));
    check(cr && *cr == Applied::Changed && o.state == OrderState::Open
              && is_live(o.state),
          "a refused cancel returns the order to Open -- it is still resting");
    check(apply_fill(o, fill(150, 40)) == Applied::Changed
              && o.state == OrderState::Filled && o.cum_qty.raw() == 150,
          "and the fill that follows is ACCEPTED: the position is recorded"
          " -- the old table refused it out of Rejected and showed flat");

    Order old_word = fresh();
    (void)apply(old_word, OrderEvent::Ack, at(10));
    (void)apply(old_word, OrderEvent::CancelSent, at(20));
    const auto rj = apply(old_word, OrderEvent::Reject, at(30));
    check(!rj && rj.error() == OrderError::IllegalTransition
              && old_word.state == OrderState::PendingCancel,
          "and `Reject` for an order that was already acknowledged is REFUSED"
          " and counted -- it cannot mean the new order was refused");

    Order p = fresh();
    (void)apply(p, OrderEvent::Ack, at(10));
    (void)apply_fill(p, fill(75, 15));
    (void)apply(p, OrderEvent::ReplaceSent, at(20));
    const auto rr = apply(p, OrderEvent::ReplaceReject, at(30));
    check(rr && p.state == OrderState::PartiallyFilled && p.cum_qty.raw() == 75,
          "a refused amend returns a part-filled order to PartiallyFilled");

    Order n = fresh();
    (void)apply(n, OrderEvent::CancelSent, at(5));
    (void)apply(n, OrderEvent::CancelReject, at(6));
    check(n.state == OrderState::PendingNew,
          "a refused cancel on a never-acknowledged order returns it to"
          " PendingNew, not to a resting state it never reached");

    Order q = fresh();
    (void)apply(q, OrderEvent::CancelSent, at(5));
    check(apply(q, OrderEvent::Reject, at(6)).has_value()
              && q.state == OrderState::Rejected,
          "while the NEW order being refused with a cancel chasing it is"
          " still Rejected");

    Order f = fresh();
    (void)apply(f, OrderEvent::CancelSent, at(5));
    (void)apply_fill(f, fill(75, 6));
    const auto after_fill = apply(f, OrderEvent::Reject, at(7));
    check(!after_fill,
          "but not once a fill has proved it reached the book");

    Order done = fresh();
    (void)apply(done, OrderEvent::Ack, at(10));
    (void)apply(done, OrderEvent::CancelSent, at(20));
    (void)apply_fill(done, fill(150, 25));
    const auto late_cr = apply(done, OrderEvent::CancelReject, at(30));
    check(late_cr && *late_cr == Applied::Duplicate
              && done.state == OrderState::Filled && done.refused == 0,
          "a cancel refused as too late, after the fill completed the order,"
          " is a duplicate -- that is the answer it was supposed to get");
}

// ── 10 ───────────────────────────────────────────────────────────────────
void a_cancel_over_a_pending_amend_resolves_both()
{
    std::printf("\n10 a_cancel_over_a_pending_amend_resolves_both\n");
    Order o = fresh();
    (void)apply(o, OrderEvent::Ack, at(10));
    (void)apply(o, OrderEvent::ReplaceSent, at(20));
    (void)apply(o, OrderEvent::CancelSent, at(21));
    check(o.state == OrderState::PendingCancel
              && o.resume_to == OrderState::PendingReplace,
          "a cancel sent while an amend is pending remembers the amend");
    (void)apply(o, OrderEvent::CancelReject, at(30));
    check(o.state == OrderState::PendingReplace,
          "a refused cancel returns to the amend still in flight");
    (void)apply(o, OrderEvent::ReplaceAck, at(40));
    check(o.state == OrderState::Open && o.refused == 0,
          "which then lands normally");

    Order p = fresh();
    (void)apply(p, OrderEvent::Ack, at(10));
    (void)apply(p, OrderEvent::ReplaceSent, at(20));
    (void)apply(p, OrderEvent::CancelSent, at(21));
    check(apply(p, OrderEvent::ReplaceAck, at(25)).has_value()
              && p.state == OrderState::PendingCancel,
          "the amend's ack arriving under the cancel is accepted, and the"
          " cancel stays pending");
    (void)apply(p, OrderEvent::CancelAck, at(30));
    check(p.state == OrderState::Cancelled && p.refused == 0,
          "and the cancel then completes");

    // CX02-B6b (R-AB-002): this used to be refused and counted. A broker can
    // legitimately resend the answer to an amend a cancel already overtook,
    // and an alarm keyed on `refused` must not fire on that.
    Order stray = fresh();
    (void)apply(stray, OrderEvent::Ack, at(10));
    (void)apply(stray, OrderEvent::CancelSent, at(20));
    const auto stray_ack = apply(stray, OrderEvent::ReplaceAck, at(25));
    check(stray_ack && *stray_ack == Applied::Duplicate
              && stray.state == OrderState::PendingCancel
              && stray.refused == 0,
          "a ReplaceAck under a cancel with no amend outstanding is absorbed"
          " as a duplicate, changing nothing");
}

// ── 11 ───────────────────────────────────────────────────────────────────
void a_late_ack_is_a_duplicate_not_a_refusal()
{
    std::printf("\n11 a_late_ack_is_a_duplicate_not_a_refusal\n");
    Order o = fresh();
    (void)apply_fill(o, fill(150, 5));
    check(o.state == OrderState::Filled, "a marketable order fills before its ack");
    const auto ack = apply(o, OrderEvent::Ack, at(10));
    check(ack && *ack == Applied::Duplicate && o.refused == 0,
          "and the ack that follows is a duplicate -- it used to be counted"
          " as a refusal, and a healthy order has none");

    Order c = fresh();
    (void)apply(c, OrderEvent::CancelSent, at(5));
    const auto under = apply(c, OrderEvent::Ack, at(6));
    check(under && c.state == OrderState::PendingCancel
              && c.resume_to == OrderState::Open,
          "an ack arriving under a cancel is accepted, and a refused cancel"
          " would now return to Open");
    (void)apply(c, OrderEvent::CancelAck, at(7));
    const auto after = apply(c, OrderEvent::Ack, at(8));
    check(after && *after == Applied::Duplicate && c.refused == 0,
          "an ack after the cancel completed is a duplicate too");
}

// ── 12 ───────────────────────────────────────────────────────────────────
// C13-003. `ts` is the OMS receipt stamp. Equal stamps are one batch of
// messages taken in the same instant, and are legal in arrival order.
void receipt_stamps_define_the_order_and_equal_stamps_are_legal()
{
    std::printf("\n12 receipt_stamps_define_the_order_and_equal_stamps_are_legal\n");
    Order o = fresh();
    check(apply(o, OrderEvent::Ack, at(10)).has_value()
              && apply(o, OrderEvent::CancelSent, at(10)).has_value()
              && apply_fill(o, fill(75, 10)).has_value()
              && apply(o, OrderEvent::CancelAck, at(10)).has_value(),
          "four messages received in the same instant apply in arrival order");
    check(o.state == OrderState::Cancelled && o.cum_qty.raw() == 75
              && o.refused == 0,
          "and the crossed fill is kept, whatever the EXCHANGE time on it was");
}

// ── 13 ───────────────────────────────────────────────────────────────────
// R-AB-001. The crossed-fill exception was granted to Cancelled only.
void late_fill_after_expire_is_kept()
{
    std::printf("\n13 late_fill_after_expire_is_kept\n");
    Order o = fresh();
    (void)apply(o, OrderEvent::Ack, at(10));
    (void)apply(o, OrderEvent::Expire, at(20));
    check(o.state == OrderState::Expired && is_terminal(o.state),
          "a DAY order lapses at the end of the session");
    const auto late = apply_fill(o, fill(75, 25));
    check(late && *late == Applied::Changed && o.cum_qty.raw() == 75,
          "a fill that crossed the lapse is ACCEPTED -- it was refused before,"
          " and 75 units of NIFTY went unrecorded");
    check(o.state == OrderState::Expired && !is_live(o.state)
              && o.refused == 0,
          "and the order stays terminal, exactly as it does out of Cancelled");

    Order full = fresh();
    (void)apply(full, OrderEvent::Ack, at(10));
    (void)apply(full, OrderEvent::Expire, at(20));
    (void)apply_fill(full, fill(150, 25));
    check(full.state == OrderState::Filled,
          "a crossed fill for the whole quantity ends Filled");

    Order r = fresh();
    (void)apply(r, OrderEvent::Reject, at(10));
    check(!apply_fill(r, fill(75, 20)),
          "while a REJECTED order still refuses a fill: it never reached the"
          " book, so that is a disagreement, not a crossed message");
}

// ── 14 ───────────────────────────────────────────────────────────────────
// R-AB-002. Brokers resend. A resent ANSWER to a request is not an error.
void duplicate_request_answers_are_duplicates()
{
    std::printf("\n14 duplicate_request_answers_are_duplicates\n");
    Order a = fresh();
    (void)apply(a, OrderEvent::Ack, at(10));
    (void)apply(a, OrderEvent::CancelSent, at(20));
    (void)apply(a, OrderEvent::CancelReject, at(30));
    const auto again = apply(a, OrderEvent::CancelReject, at(31));
    check(again && *again == Applied::Duplicate && a.state == OrderState::Open
              && a.refused == 0,
          "a resent CancelReject, after the order is back Open, is a duplicate");

    Order b = fresh();
    (void)apply(b, OrderEvent::Ack, at(10));
    (void)apply(b, OrderEvent::ReplaceSent, at(20));
    (void)apply(b, OrderEvent::CancelSent, at(21));
    (void)apply(b, OrderEvent::ReplaceAck, at(25));
    const auto resent = apply(b, OrderEvent::ReplaceAck, at(26));
    check(resent && *resent == Applied::Duplicate
              && b.state == OrderState::PendingCancel && b.refused == 0,
          "and so is a resent ReplaceAck for an amend already answered under"
          " a pending cancel");

    Order c = fresh();
    (void)apply(c, OrderEvent::Ack, at(10));
    (void)apply(c, OrderEvent::ReplaceSent, at(20));
    (void)apply(c, OrderEvent::ReplaceAck, at(30));
    const auto twice = apply(c, OrderEvent::ReplaceAck, at(31));
    check(twice && *twice == Applied::Duplicate && c.state == OrderState::Open
              && c.refused == 0,
          "a second ReplaceAck from Open is a duplicate -- this one predates"
          " CX02-B6 and was refused");

    Order d = fresh();
    check(!apply(d, OrderEvent::ReplaceAck, at(5)) && d.refused == 1,
          "but PendingNew still refuses one: nothing can have been answered"
          " before the order itself was acknowledged");
}

// ── 15 ───────────────────────────────────────────────────────────────────
// R-AB-003. The answer a request was SUPPOSED to get, arriving after the
// order finished, is not a protocol violation.
void terminal_states_absorb_answers_to_overtaken_requests()
{
    std::printf("\n15 terminal_states_absorb_answers_to_overtaken_requests\n");
    Order a = fresh();
    (void)apply(a, OrderEvent::Ack, at(10));
    (void)apply(a, OrderEvent::ReplaceSent, at(20));
    (void)apply(a, OrderEvent::CancelSent, at(21));
    (void)apply(a, OrderEvent::CancelAck, at(30));
    const auto rr = apply(a, OrderEvent::ReplaceReject, at(31));
    check(rr && *rr == Applied::Duplicate && a.state == OrderState::Cancelled
              && a.refused == 0,
          "\"order already cancelled\" for an amend the cancel overtook");

    Order b = fresh();
    (void)apply(b, OrderEvent::CancelSent, at(5));
    (void)apply(b, OrderEvent::Reject, at(6));
    const auto cr = apply(b, OrderEvent::CancelReject, at(7));
    check(cr && *cr == Applied::Duplicate && b.state == OrderState::Rejected
              && b.refused == 0,
          "and the refusal of a cancel that was chasing a rejected order");

    Order c = fresh();
    (void)apply(c, OrderEvent::Ack, at(10));
    (void)apply(c, OrderEvent::CancelSent, at(20));
    (void)apply(c, OrderEvent::CancelSent, at(21));
    (void)apply(c, OrderEvent::CancelAck, at(30));
    check(apply(c, OrderEvent::CancelReject, at(31)) == Applied::Duplicate
              && c.refused == 0,
          "and the refusal of the SECOND of two cancels once the first landed");

    Order d = fresh();
    (void)apply(d, OrderEvent::Ack, at(10));
    (void)apply(d, OrderEvent::ReplaceSent, at(20));
    (void)apply_fill(d, fill(150, 25));
    check(apply(d, OrderEvent::ReplaceAck, at(30)) == Applied::Duplicate
              && d.state == OrderState::Filled && d.refused == 0,
          "and an amend acknowledged after the order had already filled");

    // NOT absorbed: a CancelAck with no cancel pending would mean the
    // exchange pulled the order by itself, which is news, not an answer.
    Order e = fresh();
    (void)apply(e, OrderEvent::Ack, at(10));
    check(!apply(e, OrderEvent::CancelAck, at(20)) && e.refused == 1,
          "an unsolicited CancelAck from Open is still REFUSED and counted");
}

// ── 16 ───────────────────────────────────────────────────────────────────
// R-AB-004. Rule 9: where a refused cancel puts the order back is either
// recorded or refused. It is never guessed.
void cancel_reject_without_a_resume_state_is_refused()
{
    std::printf("\n16 cancel_reject_without_a_resume_state_is_refused\n");
    Order o{};
    o.ordered = Qty{150};
    o.state = OrderState::PendingCancel;
    o.last_event = at(0);
    o.resume_to = OrderState::Unset;      // not reachable through the API
    const auto r = apply(o, OrderEvent::CancelReject, at(10));
    check(!r && r.error() == OrderError::IllegalTransition,
          "a PendingCancel order with no recorded resume state refuses"
          " CancelReject rather than guessing the order was live");
    check(o.state == OrderState::PendingCancel && o.refused == 1,
          "and the state is untouched, with the refusal counted");
}

// ── 17 ───────────────────────────────────────────────────────────────────
void send_timeout_makes_order_unknown()
{
    std::printf("\n17 send_timeout_makes_order_unknown\n");
    static_assert(static_cast<std::uint8_t>(OrderState::Expired) == 9);
    static_assert(static_cast<std::uint8_t>(OrderState::Unknown) == 10);
    static_assert(static_cast<std::uint8_t>(OrderEvent::ReplaceReject) == 9);
    static_assert(static_cast<std::uint8_t>(OrderEvent::SendTimeout) == 10);
    static_assert(static_cast<std::uint8_t>(OrderEvent::RequestTimeout) == 11);

    Order o = fresh();
    const auto timeout = apply(o, OrderEvent::SendTimeout, at(10));
    check(timeout && *timeout == Applied::Changed
              && o.state == OrderState::Unknown,
          "a timed-out submitted order becomes Unknown, not rejected or absent");
    check(is_live(o.state) && !is_terminal(o.state)
              && o.last_event == at(10),
          "Unknown is conservatively live until the broker snapshot resolves it");
}

void cancel_and_replace_timeouts_require_snapshot_resolution()
{
    std::printf("\n17b cancel_and_replace_timeouts_require_snapshot_resolution\n");
    Order cancel = fresh();
    (void)apply(cancel, OrderEvent::Ack, at(10));
    (void)apply(cancel, OrderEvent::CancelSent, at(20));
    const auto cancel_timeout = apply(cancel, OrderEvent::RequestTimeout, at(30));
    check(cancel_timeout && cancel.state == OrderState::Unknown
              && cancel.resume_to == OrderState::Unset && is_live(cancel.state),
          "a cancel reply timeout becomes conservatively live Unknown");
    const auto cancel_snapshot = apply_reconciled(
        cancel, OrderState::Open, Qty{0}, Price{0}, at(31));
    check(cancel_snapshot && cancel.state == OrderState::Open,
          "a correlated authoritative snapshot can restore the broker's Open state");

    Order replace = fresh();
    (void)apply(replace, OrderEvent::Ack, at(10));
    (void)apply(replace, OrderEvent::ReplaceSent, at(20));
    const auto replace_timeout = apply(replace, OrderEvent::RequestTimeout, at(30));
    check(replace_timeout && replace.state == OrderState::Unknown
              && replace.resume_to == OrderState::Unset,
          "an amend reply timeout also requires reconciliation");
    const auto replace_snapshot = apply_reconciled(
        replace, OrderState::PartiallyFilled, Qty{75}, Price{2'408'000}, at(31));
    check(replace_snapshot && replace.state == OrderState::PartiallyFilled
              && replace.cum_qty == Qty{75},
          "reconciliation restores both actual state and cumulative fill quantity");
}

bool same_order_except_refused(const Order& a, const Order& b)
{
    return a.ordered == b.ordered && a.cum_qty == b.cum_qty
        && a.avg_price == b.avg_price && a.state == b.state
        && a.last_event == b.last_event && a.duplicates == b.duplicates
        && a.resume_to == b.resume_to && a.refused == b.refused + 1;
}

void unknown_refuses_ack_and_fill()
{
    std::printf("\n18 unknown_refuses_ack_and_fill\n");
    Order unknown = fresh();
    (void)apply(unknown, OrderEvent::SendTimeout, at(10));
    const Order baseline = unknown;

    // Unknown refuses every event accepted by the incremental broker-event
    // path, including late request answers and a repeated local timeout.
    constexpr OrderEvent kEvents[] = {
        OrderEvent::Ack, OrderEvent::Fill, OrderEvent::ReplaceSent,
        OrderEvent::ReplaceAck, OrderEvent::CancelSent, OrderEvent::CancelAck,
        OrderEvent::Reject, OrderEvent::Expire, OrderEvent::CancelReject,
        OrderEvent::ReplaceReject, OrderEvent::SendTimeout,
        OrderEvent::RequestTimeout};
    for (const OrderEvent event : kEvents) {
        Order candidate = baseline;
        const auto r = apply(candidate, event, at(20));
        check(!r && r.error() == OrderError::IllegalTransition
                  && same_order_except_refused(candidate, baseline),
              "Unknown refuses each non-authoritative event without state change");
    }

    Order fill_candidate = baseline;
    const auto fill_result = apply_fill(fill_candidate, fill(75, 20));
    check(!fill_result && fill_result.error() == OrderError::IllegalTransition
              && same_order_except_refused(fill_candidate, baseline),
          "Unknown refuses a fill report without changing its position fields");
}

void corrupt_state_refuses_request_answers()
{
    std::printf("\n18b corrupt_state_refuses_request_answers\n");
    Order corrupt = fresh();
    corrupt.state = static_cast<OrderState>(255);
    const Order baseline = corrupt;
    const auto result = apply(corrupt, OrderEvent::CancelReject, at(20));
    check(!result && result.error() == OrderError::IllegalTransition
              && same_order_except_refused(corrupt, baseline),
          "an out-of-range state enum cannot pass the request-answer shortcut");
}

void reconciled_snapshot_resolves_unknown()
{
    std::printf("\n19 reconciled_snapshot_resolves_unknown\n");
    Order unknown = fresh();
    (void)apply(unknown, OrderEvent::SendTimeout, at(10));
    const Order before = unknown;
    const auto unchanged = [&](const Order& candidate) {
        return candidate.ordered == before.ordered
            && candidate.cum_qty == before.cum_qty
            && candidate.avg_price == before.avg_price
            && candidate.state == before.state
            && candidate.last_event == before.last_event
            && candidate.refused == before.refused
            && candidate.duplicates == before.duplicates
            && candidate.resume_to == before.resume_to;
    };

    const auto invalid_state = apply_reconciled(
        unknown, OrderState::Unknown, Qty{0}, Price{0}, at(20));
    check(!invalid_state && invalid_state.error() == OrderError::IllegalTransition
              && unchanged(unknown),
          "an Unknown or non-authoritative snapshot state is refused atomically");
    const auto negative_qty = apply_reconciled(
        unknown, OrderState::Open, Qty{-1}, Price{0}, at(20));
    check(!negative_qty && negative_qty.error() == OrderError::QuantityWentBackwards
              && unchanged(unknown),
          "a negative broker cumulative quantity is refused atomically");
    const auto over_qty = apply_reconciled(
        unknown, OrderState::Filled, Qty{151}, Price{2'408'000}, at(20));
    check(!over_qty && over_qty.error() == OrderError::QuantityExceedsOrder
              && unchanged(unknown),
          "a broker cumulative quantity above the order is refused atomically");
    const auto no_fill_price = apply_reconciled(
        unknown, OrderState::PartiallyFilled, Qty{75}, Price{0}, at(20));
    check(!no_fill_price && no_fill_price.error() == OrderError::BadFillPrice
              && unchanged(unknown),
          "a positive broker fill with no average price is refused atomically");
    const auto price_without_fill = apply_reconciled(
        unknown, OrderState::Open, Qty{0}, Price{1}, at(20));
    check(!price_without_fill
              && price_without_fill.error() == OrderError::BadFillPrice
              && unchanged(unknown),
          "an average execution price without a fill is refused atomically");
    const auto inconsistent = apply_reconciled(
        unknown, OrderState::Filled, Qty{75}, Price{2'408'000}, at(20));
    check(!inconsistent && inconsistent.error() == OrderError::MalformedOrder
              && unchanged(unknown),
          "a broker state inconsistent with cumulative quantity is refused atomically");
    const auto old_time = apply_reconciled(
        unknown, OrderState::Open, Qty{0}, Price{0}, at(9));
    check(!old_time && old_time.error() == OrderError::TimeWentBackwards
              && unchanged(unknown),
          "a broker snapshot older than the timeout is refused atomically");

    const auto resolved = apply_reconciled(
        unknown, OrderState::PartiallyFilled, Qty{75}, Price{2'408'000}, at(20));
    check(resolved && *resolved == Applied::Changed
              && unknown.state == OrderState::PartiallyFilled
              && unknown.cum_qty == Qty{75}
              && unknown.avg_price == Price{2'408'000}
              && unknown.last_event == at(20)
              && unknown.resume_to == OrderState::Unset,
          "only a valid authoritative snapshot resolves Unknown to its broker state");
}

void client_order_ids_are_stable_fixed_width_and_alphanumeric()
{
    std::printf("\n20 client_order_ids_are_stable_fixed_width_and_alphanumeric\n");
    constexpr ClientOrderId expected = make_client_order_id(7, 42);
    constexpr ClientOrderId repeat = make_client_order_id(7, 42);
    constexpr ClientOrderId other_session = make_client_order_id(8, 42);
    constexpr ClientOrderId other_sequence = make_client_order_id(7, 43);
    check(expected.size() == 20
              && std::string_view(expected.data(), expected.size())
                     == "00000000070000000042",
          "session and sequence are encoded into exactly 20 stable characters");
    bool alphanumeric = true;
    for (const char c : expected) {
        alphanumeric = alphanumeric
            && ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
    }
    check(alphanumeric, "the client id alphabet is exactly uppercase letters and digits");
    check(expected == repeat && expected != other_session
              && expected != other_sequence,
          "the encoding is deterministic and distinguishes both uint32 inputs");
    constexpr auto max_component = std::numeric_limits<std::uint32_t>::max();
    constexpr ClientOrderId maximum =
        make_client_order_id(max_component, max_component);
    check(std::string_view(maximum.data(), maximum.size())
              == "42949672954294967295",
          "both maximum uint32 components fit without truncation or allocation");
}

} // namespace

int main()
{
    std::printf("altair oms order state tests\n");
    the_happy_path_walks_new_to_filled();
    a_stale_fill_never_walks_the_position_backwards();
    a_fill_requires_a_positive_average_price_without_mutating_the_order();
    a_fill_may_arrive_after_the_cancel_was_acknowledged();
    duplicates_are_normal_traffic_and_illegal_moves_are_not();
    amendments_return_to_the_right_state();
    leaves_quantity_is_never_negative();
    partial_fill_during_pending_cancel_keeps_the_cancel();
    partial_fill_during_pending_replace_keeps_the_replace();
    a_refused_cancel_or_amend_is_not_a_refused_order();
    a_cancel_over_a_pending_amend_resolves_both();
    a_late_ack_is_a_duplicate_not_a_refusal();
    receipt_stamps_define_the_order_and_equal_stamps_are_legal();
    late_fill_after_expire_is_kept();
    duplicate_request_answers_are_duplicates();
    terminal_states_absorb_answers_to_overtaken_requests();
    cancel_reject_without_a_resume_state_is_refused();
    send_timeout_makes_order_unknown();
    cancel_and_replace_timeouts_require_snapshot_resolution();
    unknown_refuses_ack_and_fill();
    corrupt_state_refuses_request_answers();
    reconciled_snapshot_resolves_unknown();
    client_order_ids_are_stable_fixed_width_and_alphanumeric();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
