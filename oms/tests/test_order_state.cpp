// P4-04 acceptance tests for oms/order_state.hpp.
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
// No check description here may contain the substring FAIL.

#include <oms/order_state.hpp>

#include <cstdio>
#include <cstdint>
#include <cstddef>

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

    // Time going backwards within one order's history is refused.
    const auto back = apply_fill(o, FillReport{Qty{125}, Price{2'408'000},
                                               at(5)});
    check(!back && back.error() == OrderError::TimeWentBackwards,
          "an event stamped before the last one applied is refused");
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
    // while holding 75 lots of NIFTY.
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
    check(o.state == OrderState::PartiallyFilled,
          "and the order reopens to PartiallyFilled, because it is no longer"
          " true that nothing was done");
    check(o.refused == 0, "with nothing refused -- this is legal traffic");

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

} // namespace

int main()
{
    std::printf("altair oms order state tests\n");
    the_happy_path_walks_new_to_filled();
    a_stale_fill_never_walks_the_position_backwards();
    a_fill_may_arrive_after_the_cancel_was_acknowledged();
    duplicates_are_normal_traffic_and_illegal_moves_are_not();
    amendments_return_to_the_right_state();
    leaves_quantity_is_never_negative();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
