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
//      fiction. So a fill is accepted out of Cancelled, and reopens the order.
//
// TERMINAL MEANS TERMINAL, with that one exception, and the exception is
// explicit rather than a hole. Filled, Rejected and Expired admit nothing.
//
// EVERY REFUSAL IS AN ERROR, NEVER A SILENT NO-OP. An illegal transition
// returns and is counted. A state machine that quietly ignores what it does
// not understand is a state machine whose disagreement with the broker is
// discovered by reconciliation hours later (rule 9).

#pragma once

#include <core/types/units.hpp>
#include <core/time/timestamp.hpp>

#include <cstdint>
#include <expected>

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
    /// reopen it -- see the header.
    Cancelled,
    /// Terminal: the broker or exchange refused it.
    Rejected,
    /// Terminal: it lapsed at the end of the session.
    Expired
};

[[nodiscard]] constexpr bool is_terminal(OrderState s) noexcept {
    return s == OrderState::Filled || s == OrderState::Cancelled
        || s == OrderState::Rejected || s == OrderState::Expired;
}

/// Is this order still capable of trading?
[[nodiscard]] constexpr bool is_live(OrderState s) noexcept {
    return s == OrderState::PendingNew || s == OrderState::Open
        || s == OrderState::PartiallyFilled
        || s == OrderState::PendingReplace || s == OrderState::PendingCancel;
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
    /// The broker refused the order.
    Reject,
    /// End of session.
    Expire
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
    /// An event stamped before the last one applied. Time comes off the tick
    /// and must not go backwards within one order's history.
    TimeWentBackwards
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
    /// Timestamp of the last event applied. From the tick, never a clock.
    Timestamp last_event{};
    /// Events refused as illegal, and duplicates absorbed. Both counted,
    /// because a healthy order has duplicates and no refusals, and telling
    /// the two apart is the whole diagnostic value.
    std::uint32_t refused = 0;
    std::uint32_t duplicates = 0;

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
    Timestamp ts{};
};

namespace detail {

/// Is `ev` legal from `s`? The whole transition table, in one place, so that
/// adding a state cannot leave a hole somewhere else in the file.
[[nodiscard]] constexpr bool legal(OrderState s, OrderEvent ev) noexcept {
    switch (s) {
    case OrderState::PendingNew:
        return ev == OrderEvent::Ack || ev == OrderEvent::Fill
            || ev == OrderEvent::Reject || ev == OrderEvent::CancelSent
            || ev == OrderEvent::Expire;
    case OrderState::Open:
    case OrderState::PartiallyFilled:
        return ev == OrderEvent::Fill || ev == OrderEvent::ReplaceSent
            || ev == OrderEvent::CancelSent || ev == OrderEvent::Expire
            || ev == OrderEvent::Ack;          // a duplicate ack is normal
    case OrderState::PendingReplace:
        return ev == OrderEvent::ReplaceAck || ev == OrderEvent::Fill
            || ev == OrderEvent::Reject || ev == OrderEvent::CancelSent
            || ev == OrderEvent::Expire;
    case OrderState::PendingCancel:
        return ev == OrderEvent::CancelAck || ev == OrderEvent::Fill
            || ev == OrderEvent::Reject || ev == OrderEvent::Expire
            || ev == OrderEvent::CancelSent;   // a resent cancel is normal
    case OrderState::Cancelled:
        // THE ONE EXCEPTION, and it is deliberate. A fill that crossed the
        // cancel in flight must be accepted, or Altair believes it is flat
        // while holding a position. A duplicate CancelAck is also normal.
        return ev == OrderEvent::Fill || ev == OrderEvent::CancelAck;
    case OrderState::Filled:
        // A duplicate final fill is normal traffic; anything else is not.
        return ev == OrderEvent::Fill;
    case OrderState::Rejected:
    case OrderState::Expired:
    case OrderState::Unset:
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

/// Apply a non-fill event.
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
    switch (ev) {
    case OrderEvent::Ack:
        o.state = (o.cum_qty.raw() > 0) ? OrderState::PartiallyFilled
                                        : OrderState::Open;
        break;
    case OrderEvent::ReplaceSent:  o.state = OrderState::PendingReplace; break;
    case OrderEvent::ReplaceAck:
        o.state = (o.cum_qty.raw() > 0) ? OrderState::PartiallyFilled
                                        : OrderState::Open;
        break;
    case OrderEvent::CancelSent:   o.state = OrderState::PendingCancel; break;
    case OrderEvent::CancelAck:    o.state = OrderState::Cancelled; break;
    case OrderEvent::Reject:       o.state = OrderState::Rejected; break;
    case OrderEvent::Expire:       o.state = OrderState::Expired; break;
    case OrderEvent::Fill:
        // Fills carry a quantity and go through `apply_fill`.
        ++o.refused;
        return std::unexpected(OrderError::IllegalTransition);
    }
    o.last_event = ts;
    if (o.state == before) {
        ++o.duplicates;
        return Applied::Duplicate;
    }
    return Applied::Changed;
}

/// Apply a fill report.
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

    if (f.cum_qty.raw() <= o.cum_qty.raw()) {
        // A stale or repeated report. Normal traffic, absorbed, counted.
        ++o.duplicates;
        o.last_event = f.ts;
        return Applied::Duplicate;
    }

    o.cum_qty = f.cum_qty;
    o.avg_price = f.avg_price;
    o.last_event = f.ts;
    o.state = o.done() ? OrderState::Filled : OrderState::PartiallyFilled;
    return Applied::Changed;
}

} // namespace altair
