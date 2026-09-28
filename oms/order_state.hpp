// oms/order_state.hpp -- the order lifecycle, and what may follow what.
//
// P4-04. `oms/` is THE TRADE HANDLER: nothing outside this directory places or
// amends an order, and this file is the only place an order changes state.
//
// THE BROKER IS NOT A RELIABLE NARRATOR.
//
// Everything here exists because exchange and broker messages arrive
// duplicated, out of order, and after the event they describe. The three
// consequences drive the whole design:
//
//   1. DUPLICATES ARE NORMAL. Every transition is idempotent -- applying the
//      same ack twice leaves the order exactly where the first one did, and
//      says so rather than erroring. A state machine that treats a resend as a
//      protocol violation will halt on a healthy Tuesday.
//
//   2. CUMULATIVE QUANTITY ONLY EVER RISES. Fills report a RUNNING TOTAL, not
//      an increment, precisely so a lost or reordered message cannot
//      double-count. A report whose cum_qty is below what has already been
//      seen is a stale duplicate and is DISCARDED. Applying it would walk the
//      position backwards, and the position is what the risk limits are
//      checked against.
//
//   3. A FILL MAY ARRIVE AFTER A CANCEL ACK. That is not a bug in the broker:
//      the fill happened before the cancel reached the exchange, and the
//      messages crossed. Refusing it would leave Altair believing it is flat
//      while holding a position -- the worst of all the failure modes here,
//      because every other check downstream would then be computed against a
//      fiction. So a fill is accepted out of Cancelled. It raises the done
//      quantity; it does NOT reopen the order (CX02-B6, below).
//
// TERMINAL MEANS TERMINAL, with that one exception, and the exception is
// explicit rather than a hole. Rejected and Expired admit nothing; Filled
// admits only duplicates.
//
// EVERY REFUSAL IS AN ERROR, NEVER A SILENT NO-OP. An illegal transition
// returns and is counted. A state machine that quietly ignores what it does
// not understand is a state machine whose disagreement with the broker is
// discovered by reconciliation hours later (rule 9).
//
// CX02-B6. THREE WAYS THE FIRST TABLE LOST THE POSITION.
//
//   C13-001  A PARTIAL fill while a cancel or amend was in flight set the
//            state to PartiallyFilled, erasing the pending request. The
//            CancelAck that followed was then ILLEGAL from PartiallyFilled and
//            refused, and the dead order stayed "live" with 75 units working.
//            A partial fill now leaves PendingCancel/PendingReplace in place.
//
//   C13-002  `Reject` meant two different things -- "the broker refused the
//            ORDER" and "the broker refused my CANCEL/AMEND REQUEST" -- and
//            both made the order terminal Rejected. A cancel refused because
//            the order had already filled left Altair recording nothing done,
//            and the fill that followed was refused out of Rejected: flat on
//            paper, long in the market. Refused requests are now their own
//            events, CancelReject and ReplaceReject, and they return the order
//            to where it was. Reject is legal only while the NEW order could
//            still be the thing refused.
//
//   C13-003  `ts` was never defined, and the time check refuses anything
//            earlier than the last event. If adapters passed EXCHANGE time, a
//            fill that crossed a cancel (filled at 25 ms, cancel acked at
//            30 ms) was refused as TimeWentBackwards. It is now defined: `ts`
//            is the OMS RECEIPT stamp, non-decreasing in arrival order, and
//            equal stamps are legal. Exchange time belongs in the audit
//            record, not in the ordering check.
//
// A late ack -- after the fill completed the order, or after a cancel went
// out -- is absorbed as a duplicate rather than counted as a refusal. The
// header's own promise is that a healthy order has no refusals.
//
// CX02-B6b, from the independent review of B6 (review/REVIEW_AB.md).
//
//   R-AB-001  The crossed-fill exception was granted to Cancelled only, so a
//             fill that crossed an EXPIRY was refused and the position lost --
//             a DAY order filled at 15:29:59.9 whose lapse message overtakes
//             its fill report. The argument in point 3 above does not stop at
//             one state: Expired accepts a late fill on the same terms.
//   R-AB-002  An answer to a cancel or amend REQUEST that arrives when no such
//             request is outstanding -- a broker resending CancelReject, or a
//             ReplaceAck for an amend a cancel already overtook -- was refused
//             and counted, so an alarm on `refused > 0` fired on healthy
//   R-AB-003  traffic. Those answers are now DUPLICATES from every state that
//             could have sent the request, and refused only from Unset and
//             PendingNew, where no request can have been answered yet.
//             CancelAck is deliberately NOT in that set: out of a state with
//             no cancel pending it would be an unsolicited exchange cancel,
//             which must be escalated rather than absorbed.
//   R-AB-004  CancelReject with no recorded resume state now REFUSES instead
//             of guessing a live state for an order that may never have been
//             acknowledged (rule 9).

#pragma once

#include <core/types/units.hpp>
#include <core/time/timestamp.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

namespace altair {

/// Where an order is. Ordinal 0 is Unset so a zeroed struct does not read as a
/// live order -- the defect class this repo keeps meeting.
enum class OrderState : std::uint8_t {
    Unset = 0,
    /// Sent to the broker, no acknowledgement yet. The dangerous state: the
    /// order may or may not be live and only the broker knows.
    PendingNew,
    /// Acknowledged and resting, nothing done.
    Open,
    /// Acknowledged and resting, partly done.
    PartiallyFilled,
    /// An amend has been sent and not yet acknowledged.
    PendingReplace,
    /// A cancel has been sent and not yet acknowledged.
    PendingCancel,
    /// Terminal: fully done.
    Filled,
    /// Terminal: cancelled with quantity remaining. A late fill may still
    /// raise `cum_qty` -- see the header -- but does not reopen it.
    Cancelled,
    /// Terminal: the broker or exchange refused the NEW order.
    Rejected,
    /// Terminal: it lapsed at the end of the session.
    Expired,
    /// The send may have reached the broker, but no authoritative answer was
    /// received. It may still be live until an explicit reconciliation resolves it.
    Unknown
};

[[nodiscard]] constexpr bool is_terminal(OrderState s) noexcept {
    return s == OrderState::Filled || s == OrderState::Cancelled
        || s == OrderState::Rejected || s == OrderState::Expired;
}

/// Is this order still capable of trading?
[[nodiscard]] constexpr bool is_live(OrderState s) noexcept {
    return s == OrderState::PendingNew || s == OrderState::Open
        || s == OrderState::PartiallyFilled
        || s == OrderState::PendingReplace || s == OrderState::PendingCancel
        || s == OrderState::Unknown;
}

enum class OrderEvent : std::uint8_t {
    /// The broker acknowledged the new order.
    Ack,
    /// A fill report. Carries a CUMULATIVE quantity, never an increment.
    Fill,
    /// An amend was sent.
    ReplaceSent,
    /// The amend was acknowledged.
    ReplaceAck,
    /// A cancel was sent.
    CancelSent,
    /// The cancel was acknowledged.
    CancelAck,
    /// The broker refused the NEW order. Not a refused cancel or amend --
    /// those are CancelReject and ReplaceReject.
    Reject,
    /// End of session.
    Expire,
    // CX02-B6. Appended so the ordinals above keep their meaning.
    /// The broker refused a CANCEL REQUEST. The order is whatever it was.
    CancelReject,
    /// The broker refused an AMEND REQUEST. The order is unchanged and live.
    ReplaceReject,
    /// A NEW-order submit timed out after submission; the order may be live.
    /// Cancel/amend timeouts are not modeled by this event.
    SendTimeout,
    /// A cancel/amend reply timed out while its outcome is uncertain; only an
    /// authoritative broker snapshot may then resolve the order.
    RequestTimeout
};

enum class OrderError : std::uint8_t {
    /// The event is not legal from the current state.
    IllegalTransition,
    /// A fill reported a cumulative quantity below one already applied, or
    /// above the order quantity.
    QuantityWentBackwards,
    QuantityExceedsOrder,
    /// The order has not been given a quantity, or it is not positive.
    MalformedOrder,
    /// An event stamped before the last one applied. `ts` is the OMS receipt
    /// stamp (see the header), so this means the OMS's own sequence went
    /// backwards -- a caller bug, never crossed broker messages.
    TimeWentBackwards,
    /// A positive cumulative fill cannot have a zero or negative average
    /// execution price. `Price` is integral paise, so NaN/infinity cannot be
    /// represented at this boundary.
    BadFillPrice
};

/// What applying an event did. Distinguishing "applied" from "already in that
/// state" matters: the second is normal traffic, and counting it as a change
/// would make every duplicate look like a real event downstream.
enum class Applied : std::uint8_t {
    Changed,
    /// The event was a duplicate and the order was already where it put it.
    Duplicate
};

/// One order, and everything known about it.
struct Order {
    /// Quantity sent, always POSITIVE. Direction lives in `side`, so that a
    /// signed quantity cannot silently flip a sell into a buy.
    Qty ordered{0};
    /// Cumulative quantity done. Monotonically non-decreasing, by construction.
    Qty cum_qty{0};
    /// Volume-weighted average price of what has been done. UNIT: paise.
    Price avg_price{0};
    OrderState state = OrderState::Unset;
    /// The OMS RECEIPT stamp of the last event applied: when the OMS took the
    /// message, from its own non-decreasing sequence -- not the exchange's
    /// event time (C13-003). In replay that sequence comes off the tick.
    Timestamp last_event{};
    /// Events refused as illegal, and duplicates absorbed. Both counted,
    /// because a healthy order has duplicates and no refusals, and telling
    /// the two apart is the whole diagnostic value.
    std::uint32_t refused = 0;
    std::uint32_t duplicates = 0;
    /// CX02-B6. While a CANCEL is pending: where the order goes back to if
    /// that request is refused. One of PendingNew, Open, PartiallyFilled or
    /// PendingReplace. Open and PartiallyFilled are re-derived from `cum_qty`
    /// on the way back, because a fill may have landed in between.
    ///
    /// Unset whenever no cancel is pending -- including in PendingReplace,
    /// where a refused amend returns to a live state derived from `cum_qty`
    /// and needs no record (R-AB-004). A PendingCancel order whose resume_to
    /// is Unset did not get there through this API, and CancelReject refuses
    /// rather than guessing.
    OrderState resume_to = OrderState::Unset;

    /// Quantity still working. Never negative by construction.
    [[nodiscard]] Qty leaves() const noexcept {
        const std::int64_t l = ordered.raw() - cum_qty.raw();
        return Qty{l > 0 ? l : 0};
    }
    [[nodiscard]] bool done() const noexcept {
        return cum_qty.raw() >= ordered.raw();
    }
};

/// A fill report from the broker.
struct FillReport {
    /// CUMULATIVE quantity done, not an increment.
    Qty cum_qty{0};
    /// Volume-weighted average price across everything done so far.
    Price avg_price{0};
    /// OMS receipt stamp -- see `Order::last_event`.
    Timestamp ts{};
};

/// A stable, fixed-width client id. Each uint32 input is encoded as ten
/// zero-padded decimal digits, yielding exactly 20 characters in [0-9].
using ClientOrderId = std::array<char, 20>;

/// Deterministic, injective encoding of (session, sequence), with no allocation.
[[nodiscard]] constexpr ClientOrderId
make_client_order_id(std::uint32_t session, std::uint32_t seq) noexcept {
    ClientOrderId id{};
    const auto encode = [](std::uint32_t value, char* out) constexpr noexcept {
        for (std::size_t i = 10; i > 0; --i) {
            out[i - 1] = static_cast<char>('0' + value % 10u);
            value /= 10u;
        }
    };
    encode(session, id.data());
    encode(seq, id.data() + 10);
    return id;
}

namespace detail {

/// An answer to a cancel or amend REQUEST, as opposed to news about the order
/// itself. Brokers resend these, and one can arrive after whatever it answers
/// stopped mattering.
///
/// CancelAck is NOT one of them: out of a state with no cancel pending it
/// would mean the exchange pulled the order by itself, which is news about the
/// order and must not be absorbed as a duplicate.
[[nodiscard]] constexpr bool is_request_answer(OrderEvent ev) noexcept {
    return ev == OrderEvent::CancelReject || ev == OrderEvent::ReplaceAck
        || ev == OrderEvent::ReplaceReject;
}

/// Is `ev` legal from `s`? The whole transition table, in one place, so that
/// adding a state cannot leave a hole somewhere else in the file.
///
/// `Reject` from PendingCancel is further conditioned on `resume_to` inside
/// `apply`, as is `CancelReject`.
[[nodiscard]] constexpr bool legal(OrderState s, OrderEvent ev) noexcept {
    // Unknown may only be resolved by an authoritative snapshot, never by an
    // individual broker event (or another timeout).
    if (s == OrderState::Unknown) { return false; }
    // A request answer is legal wherever such a request could have been sent.
    // It changes the state only in the matching pending state; everywhere else
    // `apply` leaves the order alone and reports Duplicate (R-AB-002/003).
    if (is_request_answer(ev)) {
        switch (s) {
        case OrderState::Open:
        case OrderState::PartiallyFilled:
        case OrderState::PendingReplace:
        case OrderState::PendingCancel:
        case OrderState::Filled:
        case OrderState::Cancelled:
        case OrderState::Rejected:
        case OrderState::Expired:
            return true;
        case OrderState::Unset:
        case OrderState::PendingNew:
        case OrderState::Unknown:
            return false;
        }
        return false; // corrupted/out-of-range enum value
    }
    switch (s) {
    case OrderState::PendingNew:
        return ev == OrderEvent::Ack || ev == OrderEvent::Fill
            || ev == OrderEvent::Reject || ev == OrderEvent::CancelSent
            || ev == OrderEvent::Expire || ev == OrderEvent::SendTimeout;
    case OrderState::Open:
    case OrderState::PartiallyFilled:
        return ev == OrderEvent::Fill || ev == OrderEvent::ReplaceSent
            || ev == OrderEvent::CancelSent || ev == OrderEvent::Expire
            || ev == OrderEvent::Ack;          // a duplicate ack is normal
    case OrderState::PendingReplace:
        return ev == OrderEvent::Fill || ev == OrderEvent::CancelSent
            || ev == OrderEvent::Expire
            || ev == OrderEvent::Ack           // the original ack, late
            || ev == OrderEvent::RequestTimeout;
    case OrderState::PendingCancel:
        return ev == OrderEvent::CancelAck
            || ev == OrderEvent::Fill || ev == OrderEvent::Expire
            || ev == OrderEvent::CancelSent    // a resent cancel is normal
            || ev == OrderEvent::Ack           // the original ack, late
            || ev == OrderEvent::Reject        // only if never acknowledged
            || ev == OrderEvent::RequestTimeout;
    case OrderState::Cancelled:
        // THE EXCEPTION, and it is deliberate. A fill that crossed the cancel
        // in flight must be accepted, or Altair believes it is flat while
        // holding a position. A duplicate CancelAck, and an ack that arrives
        // after the cancel it preceded, are also normal.
        return ev == OrderEvent::Fill || ev == OrderEvent::CancelAck
            || ev == OrderEvent::Ack;
    case OrderState::Expired:
        // R-AB-001. The same exception, for the same reason: a fill can cross
        // an end-of-session lapse exactly as it crosses a cancel.
        return ev == OrderEvent::Fill || ev == OrderEvent::Ack;
    case OrderState::Filled:
        // Duplicates only.
        return ev == OrderEvent::Fill || ev == OrderEvent::Ack;
    case OrderState::Rejected:
        // A rejected order never reached the book, so a fill against it is a
        // broker disagreement, not a crossed message. Only the answers to
        // requests that chased it are absorbed, above.
    case OrderState::Unset:
    case OrderState::Unknown:
        return false;
    }
    return false;
}

} // namespace detail

/// Start an order. The only way to leave Unset.
[[nodiscard]] inline std::expected<Order, OrderError>
open_order(Qty ordered, Timestamp sent_at) noexcept {
    if (ordered.raw() <= 0) {
        return std::unexpected(OrderError::MalformedOrder);
    }
    Order o{};
    o.ordered = ordered;
    o.state = OrderState::PendingNew;
    o.last_event = sent_at;
    return o;
}

/// Apply a non-fill event. `ts` is the OMS receipt stamp.
[[nodiscard]] ALTAIR_HOT inline std::expected<Applied, OrderError>
apply(Order& o, OrderEvent ev, Timestamp ts) noexcept {
    if (o.state == OrderState::Unset) {
        ++o.refused;
        return std::unexpected(OrderError::IllegalTransition);
    }
    if (ts < o.last_event) {
        ++o.refused;
        return std::unexpected(OrderError::TimeWentBackwards);
    }
    if (!detail::legal(o.state, ev)) {
        ++o.refused;
        return std::unexpected(OrderError::IllegalTransition);
    }

    const OrderState before = o.state;
    const OrderState resume_before = o.resume_to;
    const OrderState live = (o.cum_qty.raw() > 0) ? OrderState::PartiallyFilled
                                                  : OrderState::Open;
    const bool cancel_pending = (before == OrderState::PendingCancel);

    // The conditioned entries of the table. From PendingCancel, Reject is the
    // NEW order refused, which is possible only if it was never acknowledged;
    // and a CancelReject must know where to put the order back (R-AB-004).
    if (cancel_pending) {
        const bool refuse =
            (ev == OrderEvent::Reject && o.resume_to != OrderState::PendingNew)
            || (ev == OrderEvent::CancelReject
                && o.resume_to == OrderState::Unset);
        if (refuse) {
            ++o.refused;
            return std::unexpected(OrderError::IllegalTransition);
        }
    }

    switch (ev) {
    case OrderEvent::Ack:
        if (before == OrderState::PendingNew) {
            o.state = live;
        } else if (before == OrderState::Open
                   || before == OrderState::PartiallyFilled) {
            o.state = live;                        // duplicate
        } else if (cancel_pending && o.resume_to == OrderState::PendingNew) {
            // The ack for the order the cancel is chasing. It is on the book,
            // so a refused cancel now returns it to live, not to PendingNew.
            o.resume_to = live;
        }
        // PendingReplace, Filled, Cancelled: a late ack changes nothing.
        break;
    case OrderEvent::ReplaceSent:
        o.resume_to = before;                      // Open or PartiallyFilled
        o.state = OrderState::PendingReplace;
        break;
    case OrderEvent::ReplaceAck:
    case OrderEvent::ReplaceReject:
        if (before == OrderState::PendingReplace) {
            o.state = live;
        } else if (cancel_pending
                   && o.resume_to == OrderState::PendingReplace) {
            // A cancel sent over an amend: the amend has been answered, so a
            // refused cancel now returns to the live order rather than to a
            // request that is finished.
            o.resume_to = live;
        }
        // Anywhere else -- Open, terminal, or a cancel with no amend out --
        // the request it answers stopped mattering: absorbed as a duplicate
        // (R-AB-002/003).
        break;
    case OrderEvent::CancelSent:
        if (!cancel_pending) {
            o.resume_to = before;       // PendingNew, Open, Partial, PendingReplace
            o.state = OrderState::PendingCancel;
        }
        break;
    case OrderEvent::CancelAck:
        o.state = OrderState::Cancelled;
        o.resume_to = OrderState::Unset;
        break;
    case OrderEvent::CancelReject:
        if (cancel_pending) {
            // resume_to is never Unset here: that case was refused above.
            OrderState back = o.resume_to;
            if (back == OrderState::Open
                || back == OrderState::PartiallyFilled) {
                back = live;                   // a fill may have landed since
            }
            o.state = back;
            o.resume_to = OrderState::Unset;
        }
        // Anywhere else: "too late to cancel" is the answer the request was
        // supposed to get. Duplicate.
        break;
    case OrderEvent::Reject:
        o.state = OrderState::Rejected;
        o.resume_to = OrderState::Unset;
        break;
    case OrderEvent::Expire:
        o.state = OrderState::Expired;
        o.resume_to = OrderState::Unset;
        break;
    case OrderEvent::SendTimeout:
        o.state = OrderState::Unknown;
        o.resume_to = OrderState::Unset;
        break;
    case OrderEvent::RequestTimeout:
        o.state = OrderState::Unknown;
        o.resume_to = OrderState::Unset;
        break;
    case OrderEvent::Fill:
        // Fills carry a quantity and go through `apply_fill`.
        ++o.refused;
        return std::unexpected(OrderError::IllegalTransition);
    }
    o.last_event = ts;
    if (o.state == before && o.resume_to == resume_before) {
        ++o.duplicates;
        return Applied::Duplicate;
    }
    return Applied::Changed;
}

/// Apply a fill report. `f.ts` is the OMS receipt stamp.
///
/// `cum_qty` is a RUNNING TOTAL. A report at or below what has already been
/// applied is a stale duplicate and is absorbed without moving anything --
/// applying it would walk the position backwards, and the position is what
/// every risk limit is checked against.
[[nodiscard]] ALTAIR_HOT inline std::expected<Applied, OrderError>
apply_fill(Order& o, const FillReport& f) noexcept {
    if (o.state == OrderState::Unset) {
        ++o.refused;
        return std::unexpected(OrderError::IllegalTransition);
    }
    if (f.ts < o.last_event) {
        ++o.refused;
        return std::unexpected(OrderError::TimeWentBackwards);
    }
    if (!detail::legal(o.state, OrderEvent::Fill)) {
        ++o.refused;
        return std::unexpected(OrderError::IllegalTransition);
    }
    // Validate every numeric component before changing the order. Price is
    // integral paise, so non-finite values are unrepresentable; zero and
    // negative execution prices are still invalid. In particular, do this
    // before the duplicate-cumulative fast path so a malformed broker report
    // is not silently absorbed as ordinary traffic.
    if (f.avg_price.raw() <= 0) {
        ++o.refused;
        return std::unexpected(OrderError::BadFillPrice);
    }
    if (f.cum_qty.raw() < 0) {
        ++o.refused;
        return std::unexpected(OrderError::QuantityWentBackwards);
    }
    if (f.cum_qty.raw() > o.ordered.raw()) {
        // Overfill. Refused rather than clamped: a broker reporting more done
        // than was asked for is a disagreement about reality, and clamping it
        // would hide the one fact worth escalating.
        ++o.refused;
        return std::unexpected(OrderError::QuantityExceedsOrder);
    }
    if (f.cum_qty.raw() == 0 && f.avg_price.raw() != 0) {
        ++o.refused;
        return std::unexpected(OrderError::BadFillPrice);
    }

    if (f.cum_qty.raw() <= o.cum_qty.raw()) {
        // A stale or repeated report. Normal traffic, absorbed, counted.
        ++o.duplicates;
        o.last_event = f.ts;
        return Applied::Duplicate;
    }

    o.cum_qty = f.cum_qty;
    o.avg_price = f.avg_price;
    o.last_event = f.ts;
    if (o.done()) {
        // Done whatever any pending request says.
        o.state = OrderState::Filled;
        o.resume_to = OrderState::Unset;
        return Applied::Changed;
    }
    switch (o.state) {
    case OrderState::PendingNew:
    case OrderState::Open:
        o.state = OrderState::PartiallyFilled;
        break;
    case OrderState::PendingCancel:
    case OrderState::PendingReplace:
        // C13-001. The request is still out and its answer is still coming.
        // A fill proves the order reached the book, so a refused cancel can
        // no longer return it to PendingNew.
        if (o.resume_to == OrderState::PendingNew
            || o.resume_to == OrderState::Open) {
            o.resume_to = OrderState::PartiallyFilled;
        }
        break;
    case OrderState::Cancelled:
    case OrderState::Expired:
        // The crossed fill: recorded, and the order stays dead. Reopening it
        // would put `leaves()` units back to work that the exchange pulled
        // (or that lapsed with the session).
        break;
    case OrderState::PartiallyFilled:
    case OrderState::Filled:
    case OrderState::Rejected:
    case OrderState::Unset:
    case OrderState::Unknown:
        break;
    }
    return Applied::Changed;
}

namespace detail {

[[nodiscard]] constexpr bool authoritative_snapshot_state(OrderState state) noexcept {
    switch (state) {
    case OrderState::Open:
    case OrderState::PartiallyFilled:
    case OrderState::Filled:
    case OrderState::Cancelled:
    case OrderState::Rejected:
    case OrderState::Expired:
        return true;
    case OrderState::Unset:
    case OrderState::PendingNew:
    case OrderState::PendingReplace:
    case OrderState::PendingCancel:
    case OrderState::Unknown:
        return false;
    }
    return false;
}

[[nodiscard]] constexpr bool snapshot_state_matches(OrderState state,
                                                     std::int64_t ordered,
                                                     std::int64_t cum) noexcept {
    switch (state) {
    case OrderState::Open:
    case OrderState::Rejected:
        return cum == 0;
    case OrderState::PartiallyFilled:
        return cum > 0 && cum < ordered;
    case OrderState::Filled:
        return cum == ordered;
    case OrderState::Cancelled:
    case OrderState::Expired:
        return cum >= 0 && cum < ordered;
    case OrderState::Unset:
    case OrderState::PendingNew:
    case OrderState::PendingReplace:
    case OrderState::PendingCancel:
    case OrderState::Unknown:
        return false;
    }
    return false;
}

} // namespace detail

/// Resolve an Unknown order using one authoritative broker snapshot. The
/// caller MUST authenticate and correlate that snapshot to this exact order,
/// account, product and instrument, and establish that it is fresh; this
/// value-level function cannot prove broker identity or snapshot provenance.
/// Refusals are deliberately non-mutating, including diagnostic counters.
[[nodiscard]] inline std::expected<Applied, OrderError>
apply_reconciled(Order& o, OrderState broker, Qty cum, Price avg,
                 Timestamp ts) noexcept {
    if (o.state != OrderState::Unknown) {
        return std::unexpected(OrderError::IllegalTransition);
    }
    if (ts < o.last_event) {
        return std::unexpected(OrderError::TimeWentBackwards);
    }
    if (o.ordered.raw() <= 0) {
        return std::unexpected(OrderError::MalformedOrder);
    }
    if (!detail::authoritative_snapshot_state(broker)) {
        return std::unexpected(OrderError::IllegalTransition);
    }
    if (cum.raw() < 0) {
        return std::unexpected(OrderError::QuantityWentBackwards);
    }
    if (cum.raw() > o.ordered.raw()) {
        return std::unexpected(OrderError::QuantityExceedsOrder);
    }
    if ((cum.raw() == 0 && avg.raw() != 0)
        || (cum.raw() > 0 && avg.raw() <= 0)) {
        return std::unexpected(OrderError::BadFillPrice);
    }
    if (!detail::snapshot_state_matches(broker, o.ordered.raw(), cum.raw())) {
        return std::unexpected(OrderError::MalformedOrder);
    }

    o.state = broker;
    o.cum_qty = cum;
    o.avg_price = avg;
    o.last_event = ts;
    o.resume_to = OrderState::Unset;
    return Applied::Changed;
}

} // namespace altair
