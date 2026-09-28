// oms/exit_ladder.hpp -- stops, targets, and the square-off clock.
//
// P4-09. Decides when a position leaves, and at which level.
//
// RULE 8: THE TIGHTER STOP IS CHECKED BEFORE THE ORIGINAL STOP.
//
// CLAUDE.md records that this exact ordering cost the predecessor about
// Rs 41,000 in one replay, and the mechanism is worth stating precisely
// because the bug is invisible on almost every tick.
//
// A long position has an initial stop at 23,800 and, after the trade moved in
// its favour, a tighter stop at 24,000. On any ordinary tick only one of the
// two is breached and the order of the checks makes no difference at all. Then
// the price GAPS through both -- an opening print at 23,700, a news candle,
// a circuit move -- and both are true at once. Whichever check runs first is
// the one that reports, because the evaluation returns on the first match.
//
// Check the initial stop first and the exit is ATTRIBUTED to 23,800 when it
// was the 24,000 stop that should have taken the position out. So the order
// here is fixed, and the test asserts it by constructing exactly that gap.
//
// CX02-B8. WHAT THE ORDERING IS WORTH, CORRECTED (finding C13-010).
//
// The first version of this header said the ordering was worth money on the
// gap, and its test "measured" Rs 1,50,000 on one 10-lot position: the P&L
// booked at 24,000 minus the P&L booked at 23,800. Both numbers were fiction.
// After a gap to 23,700 nobody can sell at 24,000 OR at 23,800; the position
// leaves at the gap print or worse, whichever check ran first. Priced at the
// stop, the exit booked Rs 0 on a trade that actually lost Rs 2,25,000. That
// is the defect the ordering bug hid behind -- and very likely how a
// backtest showed a Rs 41,000 difference at all.
//
// So a decision now carries two prices, and they are not interchangeable:
//
//   `level`     the rule that FIRED: the stop or target crossed, or the tick
//               for kill-switch and clock exits. Attribution and diagnostics.
//   `fill_ref`  the most a P&L, a simulated fill, or a square-off order may
//               assume on THIS tick. For a stop that is the tick itself: a
//               breach means the tick is at or through the stop, so it is the
//               worse of the two by construction. For a target it is the
//               target: a resting limit fills at its price or better.
//
// A square-off order is priced from `fill_ref`, never from `level`. After a
// gap, a SELL limit at either stop sits above the market and does not fill,
// which leaves the position on at exactly the moment it had to come off.
//
// What rule 8 still buys, stated honestly: the right REASON and the right
// trigger level -- which stop fired, which a trailing ladder, a post-trade
// review and a strategy's own exit statistics all depend on. It does not buy
// a better price on a gap. Nothing does.
//
// CX02-B8b, from the independent review (review/REVIEW_AB.md).
//
//   R-AB-015  `fill_ref` was the TICK, and a tick is a last-traded print. A
//             market exit crosses the spread: a long sells at the BID, a short
//             buys at the ASK. Valuing it at the print is better than
//             attainable by half a spread before impact -- gate 7. So
//             evaluate_exit now takes the TOUCH, and uses the side that gets
//             hit. With no touch supplied it falls back to the tick, and that
//             is an ASSUMPTION, stated here rather than hidden.
//   R-AB-016  "Price the square-off from fill_ref" would repeat C13-010 one
//             print later: a SELL LIMIT at the gap print rests unfilled if the
//             next print is lower. `square_off_limit` prices a protective exit
//             THROUGH the touch by a band, and `fill_ref` is a valuation
//             bound only.
//   R-AB-017  A target touched EXACTLY is a queue-position question, not a
//             fill. The decision carries `fill_certain = false` there, so a
//             backtest does not book it as filled at the target.
//
// A STOP CAN ONLY EVER TIGHTEN. `tighten_stop` refuses a level that would
// loosen the protection, rather than accepting it. A trailing stop that can
// move both ways is not a stop; it is a suggestion, and one bad tick widens it
// permanently.
//
// TIME COMES OFF THE TICK (rule 7). The square-off clock is compared against
// the tick's own timestamp, never a wall clock, so a replay squares off at the
// same tick the live path did (rule 6).

#pragma once

#include <core/types/units.hpp>
#include <core/time/timestamp.hpp>

#include <cstdint>
#include <expected>
#include <limits>

namespace altair {

/// Which rule fired. Ordinal 0 is None so a zeroed decision does not read as
/// an exit instruction.
enum class ExitReason : std::uint8_t {
    None = 0,
    /// The tightened stop. CHECKED FIRST -- see the header.
    TighterStop,
    /// The stop the position was opened with.
    InitialStop,
    /// A profit target rung.
    Target,
    /// The intraday square-off clock.
    TimeSquareOff,
    /// The kill switch is tripped; everything comes off.
    KillSwitch
};

enum class ExitError : std::uint8_t {
    /// A stop or target on the wrong side of the entry for this direction.
    NonsensicalLevel,
    /// A non-positive price or quantity.
    BadPosition,
    /// A proposed stop that would LOOSEN the existing one.
    WouldLoosen,
    /// A protective-through band is zero, too large, or too small to move
    /// the integer-paise limit even one paisa.
    BadBand,
    /// A computed protective limit cannot be represented as a positive paise
    /// price (for example, a short exit above INT64_MAX).
    PriceOverflow
};

/// Direction. Long exits on a fall, short on a rise, and every comparison in
/// this file flips with it -- which is why it is a type and not a bool.
enum class Direction : std::uint8_t { Long, Short };

/// The two prices an exit can actually trade against. UNIT: paise.
///
/// A default-constructed ExitTouch is ABSENT, not a zero market: callers that have
/// no book pass nothing and get the tick-based assumption.
struct ExitTouch {
    Price bid{0};
    Price ask{0};

    [[nodiscard]] constexpr bool known() const noexcept {
        return bid.raw() > 0 && ask.raw() > 0 && ask.raw() >= bid.raw();
    }
};

/// The side an exit of `dir` gets filled on when it crosses: a long SELLS into
/// the bid, a short BUYS from the ask. Falls back to `fallback` -- the tick --
/// when the touch is absent.
[[nodiscard]] constexpr Price
exit_side_price(Direction dir, ExitTouch t, Price fallback) noexcept {
    if (!t.known()) { return fallback; }
    return dir == Direction::Long ? t.bid : t.ask;
}

/// A position's exit plan.
struct ExitPlan {
    Direction dir = Direction::Long;
    /// Entry price, for sanity checks. UNIT: paise.
    Price entry{0};
    /// The stop the position opened with. UNIT: paise.
    Price initial_stop{0};
    /// The tightened stop, once one exists. Zero means none yet.
    Price tighter_stop{0};
    /// Profit target. Zero means none.
    Price target{0};
    /// Quantity to take off at the target. Zero means the whole position --
    /// a partial target that defaults to "all" would silently close a book.
    Qty target_qty{0};
    /// Total position size, units, always positive.
    Qty qty{0};
    /// When intraday positions must be flat. Compared against the TICK.
    Timestamp square_off_at{};
};

/// What to do.
struct ExitDecision {
    ExitReason reason = ExitReason::None;
    /// The position's direction, carried so an order can be priced from the
    /// decision alone. Meaningless while `reason` is None.
    Direction dir = Direction::Long;
    /// The level that FIRED: the stop or target crossed, or the tick price for
    /// kill-switch and clock exits. Attribution only. UNIT: paise.
    Price level{0};
    /// CX02-B8. The price an exit may be VALUED at on this tick -- see the
    /// header. Never better than the side that gets hit: the bid for a long,
    /// the ask for a short, or the tick when no touch was supplied. It is not
    /// a price to send an order at; use `square_off_limit`. UNIT: paise.
    Price fill_ref{0};
    /// CX02-B8b (R-AB-017). False when the exit depends on a resting limit
    /// being reached rather than on crossing the spread -- a target touched
    /// exactly. A backtest must not book an uncertain fill as filled.
    bool fill_certain = false;
    /// How much to take off. Equal to the whole position except at a target
    /// rung that specifies less.
    Qty qty{0};
};

/// Is `proposed` a tightening of `current` for this direction?
///
/// For a long, tighter means HIGHER. For a short, LOWER. A zero current means
/// there is no tighter stop yet and anything is a tightening.
[[nodiscard]] constexpr bool
is_tightening(Direction dir, Price current, Price proposed) noexcept {
    if (current.raw() <= 0) { return proposed.raw() > 0; }
    return (dir == Direction::Long) ? proposed.raw() > current.raw()
                                    : proposed.raw() < current.raw();
}

/// Move the tighter stop, refusing anything that would loosen it.
///
/// Refuses rather than clamps. A caller trying to loosen a stop has a bug, and
/// silently ignoring the request would leave that bug in place while the code
/// appears to work.
[[nodiscard]] inline std::expected<void, ExitError>
tighten_stop(ExitPlan& p, Price proposed) noexcept {
    if (proposed.raw() <= 0) {
        return std::unexpected(ExitError::BadPosition);
    }
    if (p.entry.raw() <= 0) {
        return std::unexpected(ExitError::BadPosition);
    }
    const bool protective = p.dir == Direction::Long
                          ? proposed.raw() < p.entry.raw()
                          : proposed.raw() > p.entry.raw();
    if (!protective) {
        return std::unexpected(ExitError::NonsensicalLevel);
    }
    // It must also be a tightening relative to the INITIAL stop, or a
    // "tighter" stop could sit further away than the one it replaces.
    if (!is_tightening(p.dir, p.initial_stop, proposed)
        || !is_tightening(p.dir, p.tighter_stop, proposed)) {
        return std::unexpected(ExitError::WouldLoosen);
    }
    p.tighter_stop = proposed;
    return {};
}

/// Has `price` breached `stop` for this direction?
///
/// Breach is at-or-through, not strictly through: a print exactly AT the stop
/// is a stop hit. Treating it as untouched leaves a position on at the level
/// it was supposed to leave.
[[nodiscard]] constexpr bool
stop_breached(Direction dir, Price stop, Price price) noexcept {
    if (stop.raw() <= 0) { return false; }
    return (dir == Direction::Long) ? price.raw() <= stop.raw()
                                    : price.raw() >= stop.raw();
}

[[nodiscard]] constexpr bool
target_reached(Direction dir, Price target, Price price) noexcept {
    if (target.raw() <= 0) { return false; }
    return (dir == Direction::Long) ? price.raw() >= target.raw()
                                    : price.raw() <= target.raw();
}

/// Validate a plan. Catches a stop on the wrong side of the entry, which is
/// the shape of a direction bug and would otherwise fire instantly.
[[nodiscard]] inline std::expected<void, ExitError>
validate(const ExitPlan& p) noexcept {
    if (p.entry.raw() <= 0 || p.qty.raw() <= 0) {
        return std::unexpected(ExitError::BadPosition);
    }
    if (p.target_qty.raw() < 0 || p.target_qty.raw() > p.qty.raw()) {
        return std::unexpected(ExitError::BadPosition);
    }
    if (p.initial_stop.raw() < 0 || p.tighter_stop.raw() < 0
        || p.target.raw() < 0) {
        return std::unexpected(ExitError::NonsensicalLevel);
    }
    // Public aggregate construction/loading can bypass tighten_stop(). Keep
    // the same strict monotonicity invariant at the plan validation boundary:
    // zero means absent; any supplied tighter stop must genuinely tighten the
    // initial stop, if one exists.
    const bool lng = (p.dir == Direction::Long);
    if (p.tighter_stop.raw() > 0) {
        const bool protective = lng ? p.tighter_stop.raw() < p.entry.raw()
                                    : p.tighter_stop.raw() > p.entry.raw();
        if (!protective) {
            return std::unexpected(ExitError::NonsensicalLevel);
        }
    }
    if (p.tighter_stop.raw() > 0 && p.initial_stop.raw() > 0
        && !is_tightening(p.dir, p.initial_stop, p.tighter_stop)) {
        return std::unexpected(ExitError::NonsensicalLevel);
    }
    if (p.initial_stop.raw() > 0) {
        const bool ok = lng ? p.initial_stop.raw() < p.entry.raw()
                            : p.initial_stop.raw() > p.entry.raw();
        if (!ok) { return std::unexpected(ExitError::NonsensicalLevel); }
    }
    if (p.target.raw() > 0) {
        const bool ok = lng ? p.target.raw() > p.entry.raw()
                            : p.target.raw() < p.entry.raw();
        if (!ok) { return std::unexpected(ExitError::NonsensicalLevel); }
    }
    return {};
}

/// Decide whether this tick takes the position off, and at which level.
///
/// THE ORDER OF THESE CHECKS IS THE CARD. Kill switch, then the TIGHTER stop,
/// then the initial stop, then time, then target. Every one of them returns on
/// the first match, so the order is the answer whenever two are true at once.
///
/// `now` and `price` both come off the tick. `touch` comes off the book; when
/// it is absent every crossing exit is valued at the tick instead, which
/// assumes a fill at the last print (R-AB-015).
[[nodiscard]] ALTAIR_HOT inline ExitDecision
evaluate_exit(const ExitPlan& p, Price price, Timestamp now,
              bool kill_tripped, ExitTouch touch = ExitTouch{}) noexcept {
    ExitDecision d{};
    d.dir = p.dir;
    // Every exit below except the target CROSSES: it takes whatever the other
    // side is showing.
    const Price cross = exit_side_price(p.dir, touch, price);

    // 1. The kill switch takes everything off, whatever else is true.
    if (kill_tripped) {
        d.reason = ExitReason::KillSwitch;
        d.level = price;
        d.fill_ref = cross;
        d.fill_certain = true;
        d.qty = p.qty;
        return d;
    }

    // 2. THE TIGHTER STOP, BEFORE THE INITIAL ONE. Rule 8. On a gap through
    //    both, this is the stop that should be REPORTED as firing.
    //
    //    fill_ref is the TICK. A breach means the tick is at or through the
    //    stop, so it is already the worse of the two; the stop itself is a
    //    price nobody can trade at after a gap (C13-010).
    if (stop_breached(p.dir, p.tighter_stop, price)) {
        d.reason = ExitReason::TighterStop;
        d.level = p.tighter_stop;
        d.fill_ref = cross;
        d.fill_certain = true;
        d.qty = p.qty;
        return d;
    }

    // 3. Then the original stop. Same pricing, same reason.
    if (stop_breached(p.dir, p.initial_stop, price)) {
        d.reason = ExitReason::InitialStop;
        d.level = p.initial_stop;
        d.fill_ref = cross;
        d.fill_certain = true;
        d.qty = p.qty;
        return d;
    }

    // 4. The square-off clock, BEFORE the target. A position at its target one
    //    second after the square-off time is still a position that has to be
    //    flat -- taking the target instead would leave the remainder on past
    //    the deadline whenever the target is partial.
    if (p.square_off_at != Timestamp{} && now >= p.square_off_at) {
        d.reason = ExitReason::TimeSquareOff;
        d.level = price;
        d.fill_ref = cross;
        d.fill_certain = true;
        d.qty = p.qty;
        return d;
    }

    // 5. Targets last: they are the only rule here that is optional to act on,
    //    and the only one that does NOT cross. A resting limit at the target
    //    fills at the target or better, so the target is the conservative
    //    value even when the tick is beyond it -- but a tick that only TOUCHED
    //    the target says nothing about queue position (R-AB-017).
    if (target_reached(p.dir, p.target, price)) {
        d.reason = ExitReason::Target;
        d.level = p.target;
        d.fill_ref = p.target;
        d.fill_certain = price.raw() != p.target.raw();
        d.qty = (p.target_qty.raw() > 0) ? p.target_qty : p.qty;
        return d;
    }
    return d;
}

/// The LIMIT price a protective square-off goes out at. UNIT: paise.
///
/// R-AB-016. A stop, a kill switch or the square-off clock must LEAVE, so the
/// order is priced THROUGH the touch by `band_bps`: a long sells below the
/// bid, a short buys above the ask. Pricing it at `fill_ref` -- the bid itself
/// -- rests unfilled the moment the next print is lower, which is C13-010's
/// live failure moved one print. A target is the opposite: it is a resting
/// limit AT its level, and crossing through it would give away the profit it
/// exists to take.
///
/// Refuses (BadPosition) an unusable touch or a negative band, rather than
/// pricing an order off a tick that may be minutes old.
[[nodiscard]] inline std::expected<Price, ExitError>
square_off_limit(const ExitDecision& d, ExitTouch t, std::int64_t band_bps) noexcept {
    if (d.reason == ExitReason::None || d.qty.raw() <= 0) {
        return std::unexpected(ExitError::BadPosition);
    }
    if (d.reason == ExitReason::Target) {
        if (d.level.raw() <= 0) {
            return std::unexpected(ExitError::NonsensicalLevel);
        }
        return d.level;
    }
    if (!t.known()) {
        return std::unexpected(ExitError::BadPosition);
    }
    // The protective band must move at least one paise through the touch and
    // stay below 100%; at/above 100% no positive sell limit can be produced.
    // Refuse zero/tiny bands explicitly instead of quietly returning the touch
    // and claiming a marketable protective limit.
    if (band_bps <= 0 || band_bps >= 10'000) {
        return std::unexpected(ExitError::BadBand);
    }
    // The decision carries its own direction. Inferring it from `fill_ref`
    // would be wrong for a decision made with no touch, where fill_ref is the
    // tick and matches neither side.
    const Price touch = d.dir == Direction::Long ? t.bid : t.ask;
    // Compute floor(price * bp / 10,000) without the overflowing intermediate
    // product. Since 0 < band_bps < 10,000, each term and their sum fit in
    // int64 for every positive int64 touch.
    const std::int64_t whole = touch.raw() / 10'000;
    const std::int64_t rem = touch.raw() % 10'000;
    const std::int64_t give = whole * band_bps + (rem * band_bps) / 10'000;
    if (give == 0) {
        return std::unexpected(ExitError::BadBand);
    }
    std::int64_t limit = 0;
    if (d.dir == Direction::Long) {
        limit = touch.raw() - give;
    } else {
        if (give > std::numeric_limits<std::int64_t>::max() - touch.raw()) {
            return std::unexpected(ExitError::PriceOverflow);
        }
        limit = touch.raw() + give;
    }
    if (limit <= 0) {
        return std::unexpected(ExitError::NonsensicalLevel);
    }
    return Price{limit};
}

/// Realised P&L of an exit, in paise, before costs.
///
/// Pass `ExitDecision::fill_ref`, not `level` -- see the header.
///
/// Before costs deliberately: P3-09 owns costs, and a P&L helper that also
/// deducted would double-count against it (rule 5 is about the SIGNAL being
/// net, not about every arithmetic helper deducting again).
[[nodiscard]] inline std::expected<Notional, ExitError>
exit_pnl(Direction dir, Price entry, Price exit_level, Qty qty) noexcept {
    if (entry.raw() <= 0 || exit_level.raw() <= 0 || qty.raw() <= 0) {
        return std::unexpected(ExitError::BadPosition);
    }
    const std::int64_t per_unit = (dir == Direction::Long)
                                ? exit_level.raw() - entry.raw()
                                : entry.raw() - exit_level.raw();
    const auto n = notional_of(Price{per_unit < 0 ? -per_unit : per_unit}, qty);
    if (!n) { return std::unexpected(ExitError::BadPosition); }
    return Notional{per_unit < 0 ? -n->raw() : n->raw()};
}

} // namespace altair
