// P4-09 acceptance tests for oms/exit_ladder.hpp.
//
// Test 1 is the card. It builds the exact situation CLAUDE.md's rule 8
// describes -- a gap through both stops at once -- runs the checks in the
// WRONG order alongside the right one, and measures the difference in rupees.
// On every other tick the two orders agree, which is precisely why the bug
// survived long enough to cost the predecessor about Rs 41,000.
//
// No check description here may contain the substring FAIL.

#include <oms/exit_ladder.hpp>

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

// A long NIFTY position: 10 lots of 75 = 750 units, entered at 24,000.
// Initial stop 23,800; tightened to 24,000 after the trade moved.
ExitPlan long_plan()
{
    ExitPlan p{};
    p.dir = Direction::Long;
    p.entry = Price{2'400'000};
    p.initial_stop = Price{2'380'000};
    p.tighter_stop = Price{2'400'000};
    p.target = Price{2'450'000};
    p.qty = Qty{750};
    p.square_off_at = at(1'000'000);
    return p;
}

/// The WRONG order: initial stop checked before the tighter one. Present only
/// so the cost of it can be measured.
ExitDecision evaluate_wrong_order(const ExitPlan& p, Price price)
{
    ExitDecision d{};
    if (stop_breached(p.dir, p.initial_stop, price)) {
        d.reason = ExitReason::InitialStop;
        d.level = p.initial_stop;
        d.qty = p.qty;
        return d;
    }
    if (stop_breached(p.dir, p.tighter_stop, price)) {
        d.reason = ExitReason::TighterStop;
        d.level = p.tighter_stop;
        d.qty = p.qty;
        return d;
    }
    return d;
}

// ── 1 ────────────────────────────────────────────────────────────────────
// RULE 8, measured.
void the_tighter_stop_is_checked_first_and_it_matters()
{
    std::printf("\n1 the_tighter_stop_is_checked_first_and_it_matters\n");
    const ExitPlan p = long_plan();
    check(validate(p).has_value(), "the plan is well formed");

    // An ordinary tick: price between the two stops. Only ONE is breached, so
    // both orderings agree -- which is why the bug is invisible day to day.
    const Price between{2'390'000};        // 23,900
    const ExitDecision a = evaluate_exit(p, between, at(0), false);
    const ExitDecision b = evaluate_wrong_order(p, between);
    check(a.reason == ExitReason::TighterStop
          && b.reason == ExitReason::TighterStop,
          "on an ordinary tick between the stops, BOTH orderings agree --"
          " this is why the ordering bug survives");

    // Now the gap. An opening print at 23,700 is through BOTH stops at once.
    const Price gap{2'370'000};            // 23,700
    const ExitDecision right = evaluate_exit(p, gap, at(0), false);
    const ExitDecision wrong = evaluate_wrong_order(p, gap);
    std::printf("    gap to 23,700 (through both stops):\n");
    std::printf("      tighter-first : exits at %lld  (%s)\n",
                static_cast<long long>(right.level.raw()),
                right.reason == ExitReason::TighterStop ? "TighterStop"
                                                        : "other");
    std::printf("      initial-first : exits at %lld  (%s)\n",
                static_cast<long long>(wrong.level.raw()),
                wrong.reason == ExitReason::InitialStop ? "InitialStop"
                                                        : "other");
    check(right.reason == ExitReason::TighterStop,
          "the tighter stop is the one that fires");
    check(wrong.reason == ExitReason::InitialStop,
          "while the wrong ordering attributes the exit to the initial stop");
    check(right.level.raw() > wrong.level.raw(),
          "and does so at a WORSE level for a long");

    const auto pnl_right = exit_pnl(p.dir, p.entry, right.level, p.qty);
    const auto pnl_wrong = exit_pnl(p.dir, p.entry, wrong.level, p.qty);
    check(pnl_right && pnl_wrong, "both P&Ls compute");
    if (!pnl_right || !pnl_wrong) { return; }
    const double diff = static_cast<double>(pnl_right->raw()
                                          - pnl_wrong->raw());
    std::printf("      P&L tighter-first %+.2f, initial-first %+.2f\n",
                static_cast<double>(pnl_right->raw()) / 100.0,
                static_cast<double>(pnl_wrong->raw()) / 100.0);
    std::printf("      -> the ordering is worth Rs %.0f on ONE gap of ONE"
                " position\n", diff / 100.0);
    check(diff > 0.0,
          "checking the tighter stop first is worth real money on this gap");
    // Rs 1,50,000 on ONE gap of ONE 10-lot position -- which is more than the
    // Rs 41,000 CLAUDE.md records for a whole replay. That is not a
    // contradiction: the recorded figure came from smaller positions and
    // smaller gaps. The point is the ORDER OF MAGNITUDE. A 300-point gap
    // through a 200-point stop band is a single bad morning, and this ordering
    // decides whether it costs anything at all.
    check(diff > 1'000'000.0,
          "over Rs 10,000 on a single 10-lot position -- one bad gap here"
          " exceeds the whole Rs 41,000 the predecessor lost over a replay");

    // And the same thing for a SHORT, where every comparison flips.
    ExitPlan s{};
    s.dir = Direction::Short;
    s.entry = Price{2'400'000};
    s.initial_stop = Price{2'420'000};     // above, for a short
    s.tighter_stop = Price{2'400'000};
    s.qty = Qty{750};
    check(validate(s).has_value(), "the short plan is well formed");
    const ExitDecision sd = evaluate_exit(s, Price{2'430'000}, at(0), false);
    check(sd.reason == ExitReason::TighterStop,
          "a short gapping UP through both stops also exits on the tighter"
          " one -- every comparison flips with the direction");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void a_stop_can_only_ever_tighten()
{
    std::printf("\n2 a_stop_can_only_ever_tighten\n");
    ExitPlan p = long_plan();
    check(p.tighter_stop.raw() == 2'400'000, "starts at 24,000");

    check(tighten_stop(p, Price{2'410'000}).has_value()
          && p.tighter_stop.raw() == 2'410'000,
          "a higher stop tightens a long and is accepted");
    const auto loosen = tighten_stop(p, Price{2'405'000});
    check(!loosen && loosen.error() == ExitError::WouldLoosen,
          "a LOWER one is refused, not silently applied");
    check(p.tighter_stop.raw() == 2'410'000,
          "and the existing stop is untouched");
    check(!tighten_stop(p, Price{2'410'000}),
          "an equal level is refused too -- it is not a tightening");
    check(!tighten_stop(p, Price{2'370'000}),
          "and neither is one below the INITIAL stop, which would be looser"
          " than the stop it claims to replace");

    // A short tightens downward.
    ExitPlan s{};
    s.dir = Direction::Short;
    s.entry = Price{2'400'000};
    s.initial_stop = Price{2'420'000};
    s.qty = Qty{750};
    check(tighten_stop(s, Price{2'410'000}).has_value(),
          "a short tightens DOWNWARD");
    check(!tighten_stop(s, Price{2'415'000}),
          "and upward is refused");

    check(is_tightening(Direction::Long, Price{0}, Price{2'400'000}),
          "with no tighter stop yet, any positive level is a tightening");
    check(!is_tightening(Direction::Long, Price{0}, Price{0}),
          "but zero is not a stop");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void the_square_off_clock_beats_the_target()
{
    std::printf("\n3 the_square_off_clock_beats_the_target\n");
    ExitPlan p = long_plan();
    p.tighter_stop = Price{0};              // isolate the clock and the target
    p.target_qty = Qty{375};                // a PARTIAL target: half the book

    // Before the deadline, at the target: take the partial.
    const ExitDecision early = evaluate_exit(p, Price{2'450'000},
                                             at(999'999), false);
    check(early.reason == ExitReason::Target && early.qty.raw() == 375,
          "before the deadline the target takes off its partial quantity");

    // At the deadline, still at the target: the clock wins, and takes it ALL.
    const ExitDecision late = evaluate_exit(p, Price{2'450'000},
                                            at(1'000'000), false);
    std::printf("    at the square-off instant, sitting on the target:"
                " reason %s, qty %lld of %lld\n",
                late.reason == ExitReason::TimeSquareOff ? "TimeSquareOff"
                                                         : "Target",
                static_cast<long long>(late.qty.raw()),
                static_cast<long long>(p.qty.raw()));
    check(late.reason == ExitReason::TimeSquareOff,
          "at the deadline the CLOCK wins over the target");
    check(late.qty.raw() == p.qty.raw(),
          "and takes the WHOLE position -- letting the partial target win"
          " would leave the remainder on past the deadline, which is exactly"
          " what the broker charges to square off for you");

    // The deadline is at-or-after, not strictly after.
    check(evaluate_exit(p, Price{2'400'000}, at(1'000'000), false).reason
          == ExitReason::TimeSquareOff,
          "the deadline fires AT the instant, not one tick later");
    check(evaluate_exit(p, Price{2'400'000}, at(999'999), false).reason
          == ExitReason::None,
          "and not before it");

    // A plan with no square-off time never squares off on time.
    ExitPlan overnight = p;
    overnight.square_off_at = Timestamp{};
    check(evaluate_exit(overnight, Price{2'400'000}, at(9'000'000),
                        false).reason == ExitReason::None,
          "a positional trade with no deadline is never squared off by the"
          " clock -- an unset time means overnight, not midnight");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void the_kill_switch_outranks_everything()
{
    std::printf("\n4 the_kill_switch_outranks_everything\n");
    ExitPlan p = long_plan();
    // A price sitting happily at the target, well before any deadline.
    const ExitDecision normal = evaluate_exit(p, Price{2'450'000}, at(0),
                                              false);
    check(normal.reason == ExitReason::Target,
          "without the kill switch this tick takes the target");

    const ExitDecision killed = evaluate_exit(p, Price{2'450'000}, at(0),
                                              true);
    check(killed.reason == ExitReason::KillSwitch,
          "with it tripped, the same tick comes off on the kill switch");
    check(killed.qty.raw() == p.qty.raw(),
          "and takes the whole position");
    check(killed.level.raw() == 2'450'000,
          "at the MARKET price, not at a stop level -- a kill switch exit is"
          " an unwind, not a limit, and pricing it at a stop would leave the"
          " order unfilled exactly when it must not be");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void breach_is_at_or_through_and_levels_are_validated()
{
    std::printf("\n5 breach_is_at_or_through_and_levels_are_validated\n");
    ExitPlan p = long_plan();
    check(evaluate_exit(p, Price{2'400'000}, at(0), false).reason
          == ExitReason::TighterStop,
          "a print exactly AT the stop is a breach -- treating it as untouched"
          " leaves a position on at the level it was meant to leave");
    check(evaluate_exit(p, Price{2'400'001}, at(0), false).reason
          == ExitReason::None,
          "one paisa above it is not");

    // A stop on the wrong side of the entry is the shape of a direction bug.
    ExitPlan bad = p;
    bad.initial_stop = Price{2'450'000};    // ABOVE entry, for a long
    const auto v = validate(bad);
    check(!v && v.error() == ExitError::NonsensicalLevel,
          "a long stop ABOVE the entry is refused -- it would fire instantly,"
          " and it is the shape of a direction bug rather than a level bug");
    bad = p; bad.target = Price{2'350'000}; // BELOW entry, for a long
    check(!validate(bad), "and so is a long target below the entry");
    bad = p; bad.qty = Qty{0};
    check(!validate(bad), "a zero position is refused");
    bad = p; bad.target_qty = Qty{1000};    // more than the position
    check(!validate(bad),
          "and a target quantity larger than the position");

    check(ExitDecision{}.reason == ExitReason::None,
          "a default-constructed decision does not read as an exit"
          " instruction");
    check(!exit_pnl(Direction::Long, Price{0}, Price{100}, Qty{1}),
          "P&L on a zero entry is refused");
}

} // namespace

int main()
{
    std::printf("altair oms exit ladder tests\n");
    the_tighter_stop_is_checked_first_and_it_matters();
    a_stop_can_only_ever_tighten();
    the_square_off_clock_beats_the_target();
    the_kill_switch_outranks_everything();
    breach_is_at_or_through_and_levels_are_validated();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
