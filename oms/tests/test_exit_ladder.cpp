// P4-09 acceptance tests for oms/exit_ladder.hpp, corrected by CX02-B8.
//
// Test 1 is the card. It builds the exact situation CLAUDE.md's rule 8
// describes -- a gap through both stops at once -- and runs the checks in the
// WRONG order alongside the right one. On every other tick the two orders
// agree, which is precisely why the bug survived.
//
// CX02-B8 (finding C13-010): this test used to "measure" the ordering as worth
// Rs 1,50,000 on one gap by valuing each exit AT ITS STOP. After a gap to
// 23,700 neither stop is a price anyone can trade at, so both orderings leave
// at the gap print and realise the same loss. The test now asserts that, and
// asserts what the ordering really changes: which stop is reported as firing.
// prompts/cx02/evidence/replay_old_exit_ladder.cpp shows the old decision
// booking Rs 0 on a trade that lost Rs 2,25,000.
//
// No check description here may contain the substring FAIL.

#include <oms/exit_ladder.hpp>

#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <limits>

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
// Initial stop 23,800; tightened to one paisa below entry after the trade moved.
ExitPlan long_plan()
{
    ExitPlan p{};
    p.dir = Direction::Long;
    p.entry = Price{2'400'000};
    p.initial_stop = Price{2'380'000};
    p.tighter_stop = Price{2'399'999};
    p.target = Price{2'450'000};
    p.qty = Qty{750};
    p.square_off_at = at(1'000'000);
    return p;
}

/// The WRONG order: initial stop checked before the tighter one. Present only
/// so what it changes can be shown. It prices its exit the way the real one
/// does, off the tick -- the ordering does not change what the tick allows.
ExitDecision evaluate_wrong_order(const ExitPlan& p, Price price)
{
    ExitDecision d{};
    if (stop_breached(p.dir, p.initial_stop, price)) {
        d.reason = ExitReason::InitialStop;
        d.level = p.initial_stop;
        d.fill_ref = price;
        d.qty = p.qty;
        return d;
    }
    if (stop_breached(p.dir, p.tighter_stop, price)) {
        d.reason = ExitReason::TighterStop;
        d.level = p.tighter_stop;
        d.fill_ref = price;
        d.qty = p.qty;
        return d;
    }
    return d;
}

// ── 1 ────────────────────────────────────────────────────────────────────
// RULE 8, and what it is actually worth.
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
    std::printf("      tighter-first : fires %s at level %lld, fill_ref %lld\n",
                right.reason == ExitReason::TighterStop ? "TighterStop"
                                                        : "other",
                static_cast<long long>(right.level.raw()),
                static_cast<long long>(right.fill_ref.raw()));
    std::printf("      initial-first : fires %s at level %lld, fill_ref %lld\n",
                wrong.reason == ExitReason::InitialStop ? "InitialStop"
                                                        : "other",
                static_cast<long long>(wrong.level.raw()),
                static_cast<long long>(wrong.fill_ref.raw()));

    // ordering_changes_attribution
    check(right.reason == ExitReason::TighterStop,
          "the tighter stop is the one that fires");
    check(wrong.reason == ExitReason::InitialStop,
          "while the wrong ordering attributes the exit to the initial stop");
    check(right.level.raw() == 2'399'999 && wrong.level.raw() == 2'380'000,
          "so the two report different trigger levels -- that, and the"
          " reason, is what rule 8 decides");

    // gap_stop_fill_ref_is_the_gap_price_not_the_stop
    check(right.fill_ref.raw() == gap.raw(),
          "the exit is valued at the GAP PRINT, 23,700 -- nobody can sell at"
          " 24,000 after the market opened at 23,700");
    check(right.level.raw() > gap.raw(),
          "and a SELL limit at the trigger level would sit above the market"
          " and not fill: square-offs are priced from fill_ref");

    // both_orderings_realise_the_same_gap_pnl
    const auto pnl_right = exit_pnl(p.dir, p.entry, right.fill_ref, p.qty);
    const auto pnl_wrong = exit_pnl(p.dir, p.entry, wrong.fill_ref, p.qty);
    check(pnl_right && pnl_wrong, "both P&Ls compute");
    if (!pnl_right || !pnl_wrong) { return; }
    std::printf("      realised P&L: tighter-first %+.2f, initial-first %+.2f\n",
                static_cast<double>(pnl_right->raw()) / 100.0,
                static_cast<double>(pnl_wrong->raw()) / 100.0);
    check(pnl_right->raw() == -22'500'000,
          "the realised loss is Rs 2,25,000: 300 points on 750 units");
    check(pnl_right->raw() == pnl_wrong->raw(),
          "and it is the SAME for both orderings -- the gap, not the check"
          " order, set the price");

    // What the old test measured instead, kept so the fiction is on record.
    const auto at_level_right = exit_pnl(p.dir, p.entry, right.level, p.qty);
    const auto at_level_wrong = exit_pnl(p.dir, p.entry, wrong.level, p.qty);
    if (at_level_right && at_level_wrong) {
        std::printf("      priced AT THE STOP instead: %+.2f and %+.2f -- a"
                    " Rs %.0f 'difference' between two fills that could not"
                    " happen\n",
                    static_cast<double>(at_level_right->raw()) / 100.0,
                    static_cast<double>(at_level_wrong->raw()) / 100.0,
                    static_cast<double>(at_level_right->raw()
                                        - at_level_wrong->raw()) / 100.0);
        check(at_level_right->raw() == -750,
              "valued one paisa below entry, the stop books only Rs 7.50 loss"
              " against the Rs 2,25,000 gap loss -- still not an attainable fill");
    }

    // And the same thing for a SHORT, where every comparison flips.
    ExitPlan s{};
    s.dir = Direction::Short;
    s.entry = Price{2'400'000};
    s.initial_stop = Price{2'420'000};     // above, for a short
    s.tighter_stop = Price{2'400'001};
    s.qty = Qty{750};
    check(validate(s).has_value(), "the short plan is well formed");
    const ExitDecision sd = evaluate_exit(s, Price{2'430'000}, at(0), false);
    check(sd.reason == ExitReason::TighterStop,
          "a short gapping UP through both stops also exits on the tighter"
          " one -- every comparison flips with the direction");
    check(sd.fill_ref.raw() == 2'430'000,
          "and is valued at the gap print ABOVE both stops, where a short"
          " actually buys back");

    // A target is a resting limit: conservative at the target, even past it.
    ExitPlan t = long_plan();
    t.tighter_stop = Price{0};
    const ExitDecision td = evaluate_exit(t, Price{2'460'000}, at(0), false);
    check(td.reason == ExitReason::Target && td.fill_ref.raw() == 2'450'000,
          "a target reached by a gap is valued at the target, not the better"
          " print -- a resting limit is only promised its own price");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void a_stop_can_only_ever_tighten()
{
    std::printf("\n2 a_stop_can_only_ever_tighten\n");
    ExitPlan p = long_plan();
    p.tighter_stop = Price{2'390'000};
    check(p.tighter_stop.raw() == 2'390'000, "starts between the initial stop and entry");

    check(tighten_stop(p, Price{2'395'000}).has_value()
          && p.tighter_stop.raw() == 2'395'000,
          "a higher protective-side stop tightens a long and is accepted");
    const auto loosen = tighten_stop(p, Price{2'392'000});
    check(!loosen && loosen.error() == ExitError::WouldLoosen,
          "a LOWER one is refused, not silently applied");
    check(p.tighter_stop.raw() == 2'395'000,
          "and the existing stop is untouched");
    check(!tighten_stop(p, Price{2'395'000}),
          "an equal level is refused too -- it is not a tightening");
    check(!tighten_stop(p, Price{2'370'000}),
          "and neither is one below the INITIAL stop, which would be looser"
          " than the stop it claims to replace");

    ExitPlan no_initial = long_plan();
    no_initial.initial_stop = Price{0};
    no_initial.tighter_stop = Price{0};
    const auto wrong_long = tighten_stop(no_initial, Price{2'400'001});
    check(!wrong_long && wrong_long.error() == ExitError::NonsensicalLevel,
          "without an initial stop, a long trailing stop above entry is refused");
    check(!tighten_stop(no_initial, no_initial.entry),
          "and one exactly at long entry is refused");
    check(tighten_stop(no_initial, Price{2'399'999}).has_value(),
          "while the adjacent protective-side long level is accepted");

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

    s.initial_stop = Price{0};
    s.tighter_stop = Price{0};
    check(!tighten_stop(s, Price{2'399'999}),
          "without an initial stop, a short trailing stop below entry is refused");
    check(!tighten_stop(s, s.entry),
          "and one exactly at short entry is refused");
    check(tighten_stop(s, Price{2'400'001}).has_value(),
          "while the adjacent protective-side short level is accepted");

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
    check(evaluate_exit(p, Price{2'399'999}, at(0), false).reason
          == ExitReason::TighterStop,
          "a print exactly AT the stop is a breach -- treating it as untouched"
          " leaves a position on at the level it was meant to leave");
    check(evaluate_exit(p, Price{2'400'000}, at(0), false).reason
          == ExitReason::None,
          "one paisa above it is not");

    // A stop on the wrong side of the entry is the shape of a direction bug.
    ExitPlan bad = p;
    bad.initial_stop = Price{2'450'000};    // ABOVE entry, for a long
    const auto v = validate(bad);
    check(!v && v.error() == ExitError::NonsensicalLevel,
          "a long stop ABOVE the entry is refused -- it would fire instantly,"
          " and it is the shape of a direction bug rather than a level bug");
    bad = p; bad.tighter_stop = Price{2'370'000}; // looser than initial
    check(!validate(bad) && validate(bad).error() == ExitError::NonsensicalLevel,
          "a long plan with a tighter stop below its initial stop is refused");
    bad = p; bad.tighter_stop = Price{-1};
    check(!validate(bad) && validate(bad).error() == ExitError::NonsensicalLevel,
          "a negative tighter stop is refused");
    bad = p; bad.initial_stop = Price{0}; bad.tighter_stop = bad.entry;
    check(!validate(bad),
          "without an initial stop, a long tighter stop exactly at entry is refused");
    bad.tighter_stop = Price{bad.entry.raw() + 1};
    check(!validate(bad),
          "and a long tighter stop one paisa above entry is refused");
    bad.tighter_stop = Price{bad.entry.raw() - 1};
    check(validate(bad).has_value(),
          "a long tighter stop one paisa below entry is accepted");
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

    ExitPlan bad_short{};
    bad_short.dir = Direction::Short;
    bad_short.entry = Price{2'400'000};
    bad_short.initial_stop = Price{2'420'000};
    bad_short.tighter_stop = Price{2'430'000};
    bad_short.qty = Qty{1};
    check(!validate(bad_short)
              && validate(bad_short).error() == ExitError::NonsensicalLevel,
          "a short plan with a tighter stop above its initial stop is refused");
    bad_short.initial_stop = Price{0};
    bad_short.tighter_stop = bad_short.entry;
    check(!validate(bad_short),
          "without an initial stop, a short tighter stop exactly at entry is refused");
    bad_short.tighter_stop = Price{bad_short.entry.raw() - 1};
    check(!validate(bad_short),
          "and a short tighter stop one paisa below entry is refused");
    bad_short.tighter_stop = Price{bad_short.entry.raw() + 1};
    check(validate(bad_short).has_value(),
          "a short tighter stop one paisa above entry is accepted");
}

// ── 6 ────────────────────────────────────────────────────────────────────
// CX02-B8b (R-AB-015). A tick is a last-traded print; an exit that crosses
// takes the side that gets hit.
void a_crossing_exit_is_valued_at_the_touch()
{
    std::printf("\n6 a_crossing_exit_is_valued_at_the_touch\n");
    const ExitPlan p = long_plan();
    const ExitTouch gap{Price{2'369'800}, Price{2'370'200}};
    const ExitDecision d = evaluate_exit(p, Price{2'370'000}, at(0), false, gap);
    std::printf("    gap print 2,370,000 with bid 2,369,800 / ask 2,370,200 ->"
                " fill_ref %lld\n",
                static_cast<long long>(d.fill_ref.raw()));
    check(d.reason == ExitReason::TighterStop
              && d.fill_ref.raw() == 2'369'800,
          "a long stop is valued at the BID -- the print is half a spread"
          " better than anyone can actually sell at");
    check(d.fill_certain && d.dir == Direction::Long,
          "a crossing exit is a certain fill, and carries its direction");

    ExitPlan s{};
    s.dir = Direction::Short;
    s.entry = Price{2'400'000};
    s.initial_stop = Price{2'420'000};
    s.tighter_stop = Price{2'400'001};
    s.qty = Qty{750};
    const ExitTouch up{Price{2'429'800}, Price{2'430'200}};
    const ExitDecision sd = evaluate_exit(s, Price{2'430'000}, at(0), false, up);
    check(sd.fill_ref.raw() == 2'430'200,
          "a short stop is valued at the ASK, which is what it buys back at");

    const ExitDecision none = evaluate_exit(p, Price{2'370'000}, at(0), false);
    check(none.fill_ref.raw() == 2'370'000,
          "with no touch supplied it falls back to the tick -- an assumption"
          " the header states rather than hides");
    const ExitDecision crossed =
        evaluate_exit(p, Price{2'370'000}, at(0), false,
                      ExitTouch{Price{2'371'000}, Price{2'370'000}});
    check(crossed.fill_ref.raw() == 2'370'000,
          "and a crossed or empty touch is not trusted");

    const ExitDecision k =
        evaluate_exit(p, Price{2'450'000}, at(0), true,
                      ExitTouch{Price{2'449'900}, Price{2'450'100}});
    check(k.reason == ExitReason::KillSwitch && k.fill_ref.raw() == 2'449'900,
          "the kill switch sells into the bid, not at the print");

    ExitPlan c = long_plan();
    c.tighter_stop = Price{0};
    const ExitDecision cl =
        evaluate_exit(c, Price{2'400'000}, at(1'000'000), false,
                      ExitTouch{Price{2'399'900}, Price{2'400'100}});
    check(cl.reason == ExitReason::TimeSquareOff
              && cl.fill_ref.raw() == 2'399'900,
          "and so does the square-off clock");

    ExitPlan i = long_plan();
    i.tighter_stop = Price{0};
    const ExitDecision id =
        evaluate_exit(i, Price{2'379'000}, at(0), false,
                      ExitTouch{Price{2'378'900}, Price{2'379'100}});
    check(id.reason == ExitReason::InitialStop
              && id.fill_ref.raw() == 2'378'900,
          "an initial-stop-only breach is valued the same way");
}

// ── 7 ────────────────────────────────────────────────────────────────────
// R-AB-017. A resting limit AT the print is a queue-position question.
void a_target_touched_exactly_is_not_a_certain_fill()
{
    std::printf("\n7 a_target_touched_exactly_is_not_a_certain_fill\n");
    ExitPlan p = long_plan();
    p.tighter_stop = Price{0};
    const ExitDecision exact = evaluate_exit(p, Price{2'450'000}, at(0), false);
    check(exact.reason == ExitReason::Target
              && exact.fill_ref.raw() == 2'450'000 && !exact.fill_certain,
          "a tick that only TOUCHES the target is not booked as filled");
    const ExitDecision through =
        evaluate_exit(p, Price{2'460'000}, at(0), false);
    check(through.fill_certain && through.fill_ref.raw() == 2'450'000,
          "a tick THROUGH it is a fill, still valued at the target and not at"
          " the better print");

    ExitPlan s{};
    s.dir = Direction::Short;
    s.entry = Price{2'400'000};
    s.initial_stop = Price{2'420'000};
    s.target = Price{2'350'000};
    s.qty = Qty{750};
    const ExitDecision st = evaluate_exit(s, Price{2'340'000}, at(0), false);
    check(st.reason == ExitReason::Target && st.fill_ref.raw() == 2'350'000
              && st.fill_certain,
          "a short target is valued at the target too");
}

// ── 8 ────────────────────────────────────────────────────────────────────
// R-AB-016. A protective exit must LEAVE.
void a_square_off_is_priced_through_the_touch()
{
    std::printf("\n8 a_square_off_is_priced_through_the_touch\n");
    const ExitPlan p = long_plan();
    const ExitTouch t{Price{2'369'800}, Price{2'370'200}};
    const ExitDecision d = evaluate_exit(p, Price{2'370'000}, at(0), false, t);
    const auto lim = square_off_limit(d, t, 20);          // 20 bp
    const std::int64_t want = 2'369'800 - (2'369'800 * 20) / 10'000;
    std::printf("    long stop: bid %lld -> limit %lld (20 bp through)\n",
                static_cast<long long>(t.bid.raw()),
                lim ? static_cast<long long>(lim->raw()) : -1);
    check(lim && lim->raw() == want && lim->raw() < t.bid.raw(),
          "a long square-off is priced BELOW the bid by the band, so it still"
          " fills when the next print is lower");
    check(lim && lim->raw() != d.fill_ref.raw(),
          "never AT fill_ref -- a limit there rests unfilled as the price"
          " falls, which is C13-010's live failure one print later");

    ExitPlan s{};
    s.dir = Direction::Short;
    s.entry = Price{2'400'000};
    s.initial_stop = Price{2'420'000};
    s.tighter_stop = Price{2'400'001};
    s.qty = Qty{750};
    const ExitTouch up{Price{2'429'800}, Price{2'430'200}};
    const ExitDecision sd = evaluate_exit(s, Price{2'430'000}, at(0), false, up);
    const auto slim = square_off_limit(sd, up, 20);
    check(slim && slim->raw() == 2'430'200 + (2'430'200 * 20) / 10'000,
          "a short square-off is priced ABOVE the ask by the band");

    ExitPlan tp = long_plan();
    tp.tighter_stop = Price{0};
    const ExitDecision td =
        evaluate_exit(tp, Price{2'460'000}, at(0), false,
                      ExitTouch{Price{2'459'900}, Price{2'460'100}});
    const auto tl = square_off_limit(td, ExitTouch{Price{2'459'900},
                                               Price{2'460'100}}, 20);
    check(tl && tl->raw() == 2'450'000,
          "a TARGET rests at its level: crossing through would give away the"
          " profit it exists to take");

    check(!square_off_limit(d, ExitTouch{}, 20),
          "an absent touch refuses -- an order must not be priced off a print"
          " that may be minutes old");
    check(!square_off_limit(ExitDecision{}, t, 20),
          "a decision that is not an exit refuses");
    check(!square_off_limit(d, t, -1), "and so does a negative band");

    const ExitDecision tiny_decision{ExitReason::TighterStop,
                                     Direction::Long,
                                     Price{10}, Price{9}, true, Qty{1}};
    const ExitTouch tiny_touch{Price{1}, Price{2}};
    const auto zero_band = square_off_limit(tiny_decision, tiny_touch, 0);
    check(!zero_band && zero_band.error() == ExitError::BadBand,
          "a zero protective band is refused, not returned at the touch");
    const auto tiny_band = square_off_limit(tiny_decision, tiny_touch, 1);
    check(!tiny_band && tiny_band.error() == ExitError::BadBand,
          "a positive band that rounds to zero paise is refused explicitly");
    const auto full_band = square_off_limit(d, t, 10'000);
    check(!full_band && full_band.error() == ExitError::BadBand,
          "a band of 100 percent or more is refused");

    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    const ExitTouch max_touch{Price{kMax - 1}, Price{kMax}};
    const ExitDecision max_long{ExitReason::TighterStop, Direction::Long,
                                Price{1}, Price{1}, true, Qty{1}};
    constexpr std::int64_t kLongTouch = kMax - 1;
    const std::int64_t long_give = (kLongTouch / 10'000) * 9'999
                                 + ((kLongTouch % 10'000) * 9'999) / 10'000;
    const auto max_long_limit = square_off_limit(max_long, max_touch, 9'999);
    check(max_long_limit && max_long_limit->raw() == kLongTouch - long_give,
          "near-INT64_MAX long arithmetic stays representable without intermediate overflow");

    const ExitDecision max_short{ExitReason::TighterStop, Direction::Short,
                                 Price{1}, Price{1}, true, Qty{1}};
    const auto max_short_limit = square_off_limit(max_short, max_touch, 1);
    check(!max_short_limit && max_short_limit.error() == ExitError::PriceOverflow,
          "a short limit above INT64_MAX is rejected instead of overflowing");
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
    a_crossing_exit_is_valued_at_the_touch();
    a_target_touched_exactly_is_not_a_certain_fill();
    a_square_off_is_priced_through_the_touch();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
