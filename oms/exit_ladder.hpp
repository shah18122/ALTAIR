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
// Check the initial stop first and the exit is attributed to 23,800, when the
// position should have been out at 24,000. In a backtest that overstates the
// loss on every gap and quietly makes a strategy look worse than it is. Live
// it is worse than that: the exit level is what the square-off order is
// priced against, so the order goes out at a level the position should never
// have reached.
//
// So the order here is fixed, and the test asserts it by constructing exactly
// that gap and measuring the difference in rupees.
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
    WouldLoosen
};

/// Direction. Long exits on a fall, short on a rise, and every comparison in
/// this file flips with it -- which is why it is a type and not a bool.
enum class Direction : std::uint8_t { Long, Short };

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
    /// The level the exit is attributed to. UNIT: paise.
    Price level{0};
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
    const bool lng = (p.dir == Direction::Long);
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
/// `now` and `price` both come off the tick.
[[nodiscard]] ALTAIR_HOT inline ExitDecision
evaluate_exit(const ExitPlan& p, Price price, Timestamp now,
              bool kill_tripped) noexcept {
    ExitDecision d{};

    // 1. The kill switch takes everything off, whatever else is true.
    if (kill_tripped) {
        d.reason = ExitReason::KillSwitch;
        d.level = price;
        d.qty = p.qty;
        return d;
    }

    // 2. THE TIGHTER STOP, BEFORE THE INITIAL ONE. Rule 8. On a gap through
    //    both, this is the level the position should have left at, and the
    //    only reason the order is visible at all is that both are true.
    if (stop_breached(p.dir, p.tighter_stop, price)) {
        d.reason = ExitReason::TighterStop;
        d.level = p.tighter_stop;
        d.qty = p.qty;
        return d;
    }

    // 3. Then the original stop.
    if (stop_breached(p.dir, p.initial_stop, price)) {
        d.reason = ExitReason::InitialStop;
        d.level = p.initial_stop;
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
        d.qty = p.qty;
        return d;
    }

    // 5. Targets last: they are the only rule here that is optional to act on.
    if (target_reached(p.dir, p.target, price)) {
        d.reason = ExitReason::Target;
        d.level = p.target;
        d.qty = (p.target_qty.raw() > 0) ? p.target_qty : p.qty;
        return d;
    }
    return d;
}

/// Realised P&L of an exit, in paise, before costs.
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
