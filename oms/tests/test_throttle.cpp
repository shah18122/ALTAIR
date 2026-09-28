// P4-07 acceptance tests for oms/throttle.hpp.
//
// Test 2 is the one that matters: a strategy comfortably under the per-second
// limit all day still exhausts the daily budget, and only the daily window
// sees it coming. Checking the fastest window alone is what gets an account
// rate-limited at 14:30 with positions open.
//
// No check description here may contain the substring FAIL.

#include <oms/throttle.hpp>

#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cstring>

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

Timestamp at_ms(std::int64_t ms) { return Timestamp{ms * 1'000'000}; }

// ── 1 ────────────────────────────────────────────────────────────────────
void a_burst_is_allowed_then_throttled_and_the_window_rolls()
{
    std::printf("\n1 a_burst_is_allowed_then_throttled_and_the_window_rolls\n");
    BrokerThrottle t{kKiteOrderLimits};        // 10/s, 200/min, 3000/day

    int allowed = 0;
    for (int i = 0; i < 10; ++i) {
        if (t.acquire(at_ms(0)).verdict == ThrottleVerdict::Allowed) {
            ++allowed;
        }
    }
    check(allowed == 10, "ten orders in the same instant are allowed");

    const ThrottleDecision eleventh = t.acquire(at_ms(0));
    std::printf("    the eleventh in the same second: %s, wait %lld ms\n",
                describe(eleventh.verdict),
                static_cast<long long>(eleventh.wait_for.raw() / 1'000'000));
    check(eleventh.verdict == ThrottleVerdict::PerSecond,
          "the eleventh is refused on the per-second limit");
    check(eleventh.wait_for.raw() > 0 && !eleventh.never_refills,
          "with a real wait, because a second does pass on its own");
    check(t.refused() == 1 && t.granted() == 10,
          "grants and refusals are counted separately");

    // Wait out the window and it rolls.
    check(t.acquire(at_ms(1000)).verdict == ThrottleVerdict::Allowed,
          "one second later the window has rolled and orders flow again");

    // A refusal must NOT consume budget. Eleven attempts, ten grants.
    check(t.used_today() == 11,
          "and only the granted orders consumed the daily budget -- consuming"
          " a token for an order that was refused would leak budget on every"
          " rejection");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// THE test. Under the fast limit all day, and still out of budget.
void the_daily_budget_runs_out_while_the_fast_limits_never_bind()
{
    std::printf("\n2 the_daily_budget_runs_out_while_the_fast_limits_never"
                "_bind\n");
    BrokerThrottle t{kKiteOrderLimits};

    // A polite strategy: 2 orders per second, well under 10 and well under
    // 200 a minute. Run it for an hour of market time.
    int sent = 0;
    ThrottleDecision last{};
    std::int64_t ms = 0;
    for (; ms < 3'600'000; ms += 500) {        // 2 per second
        last = t.acquire(at_ms(ms));
        if (last.verdict != ThrottleVerdict::Allowed) { break; }
        ++sent;
    }
    std::printf("    2 orders/second, never above 120/minute:\n");
    std::printf("      stopped after %d orders at t = %.1f minutes\n",
                sent, static_cast<double>(ms) / 60'000.0);
    std::printf("      refused by: %s\n", describe(last.verdict));
    check(sent == 3000, "exactly the daily budget was spent");
    check(last.verdict == ThrottleVerdict::PerDay,
          "and the refusal is the DAILY limit -- the per-second and"
          " per-minute windows never came close");
    check(last.never_refills,
          "reported as never refilling, so a caller does not sleep on a wait"
          " that cannot help within the session");
    check(ms < 3'600'000,
          "and it ran out inside the hour, mid-session, with positions"
          " potentially open");

    // Waiting genuinely does not help.
    check(t.acquire(at_ms(ms + 60'000)).verdict == ThrottleVerdict::PerDay,
          "a minute later it is still refused");
    check(t.acquire(at_ms(ms + 3'600'000)).verdict == ThrottleVerdict::PerDay,
          "and an hour later too");

    // Only an explicit new session clears it.
    t.new_session(at_ms(ms + 3'600'000));
    check(t.acquire(at_ms(ms + 3'600'000)).verdict == ThrottleVerdict::Allowed,
          "only an explicit new_session clears the daily window");
    check(t.used_today() == 1, "which restarts the count");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void the_tightest_window_binds_and_says_which()
{
    std::printf("\n3 the_tightest_window_binds_and_says_which\n");
    // Per-minute deliberately tighter than 60x the per-second, so it can bind
    // without the per-second one ever doing so.
    BrokerThrottle t{BrokerLimits{10, 20, 3000}};

    int sent = 0;
    ThrottleDecision last{};
    // 30 steps, not 20: at 500 ms apart, twenty iterations is exactly the
    // twenty-order cap, so the loop would end before the limit could bind and
    // the test would pass on having run out of iterations.
    for (std::int64_t ms = 0; ms < 15'000; ms += 500) {  // 2/s, well under 10
        last = t.acquire(at_ms(ms));
        if (last.verdict != ThrottleVerdict::Allowed) { break; }
        ++sent;
    }
    std::printf("    at 2 orders/second with a 20/minute cap: stopped after"
                " %d, refused by %s\n", sent, describe(last.verdict));
    check(sent == 20 && last.verdict == ThrottleVerdict::PerMinute,
          "the per-MINUTE limit binds while the per-second one never does --"
          " the windows are not redundant");
    check(!last.never_refills && last.wait_for.raw() > 0,
          "and a minute does pass on its own, so the wait is real");

    // The DAY is reported ahead of the minute when both are exhausted,
    // because it is the one that changes what a caller should do.
    BrokerThrottle d{BrokerLimits{10, 5, 5}};
    for (int i = 0; i < 5; ++i) { (void)d.acquire(at_ms(0)); }
    const ThrottleDecision both = d.acquire(at_ms(0));
    check(both.verdict == ThrottleVerdict::PerDay,
          "with the minute AND the day both exhausted, the DAY is reported --"
          " a caller told 'per minute' retries in a moment, told 'per day' it"
          " stops");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void an_unset_limit_does_not_block_everything()
{
    std::printf("\n4 an_unset_limit_does_not_block_everything\n");
    // Zero means "no limit published", not "zero allowed". Getting this
    // backwards makes an unconfigured venue refuse every order and look
    // exactly like a broken engine.
    BrokerThrottle none{BrokerLimits{}};
    int allowed = 0;
    for (int i = 0; i < 5000; ++i) {
        if (none.acquire(at_ms(0)).verdict == ThrottleVerdict::Allowed) {
            ++allowed;
        }
    }
    check(allowed == 5000,
          "with no limits published, nothing is throttled -- a zero limit"
          " means unset, not zero allowed");

    // A partially configured venue enforces only what it was given.
    BrokerThrottle only_day{BrokerLimits{0, 0, 3}};
    int n = 0;
    while (only_day.acquire(at_ms(0)).verdict == ThrottleVerdict::Allowed) {
        ++n;
        if (n > 10) { break; }
    }
    check(n == 3,
          "and a venue with only a daily limit enforces exactly that one");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void the_router_refuses_an_unavailable_venue_rather_than_falling_back()
{
    std::printf("\n5 the_router_refuses_an_unavailable_venue_rather_than"
                "_falling_back\n");
    BrokerThrottle kite{kKiteOrderLimits};

    const auto k = route(Venue::Kite, kite, at_ms(0));
    check(k && k->venue == Venue::Kite, "a Kite order routes to Kite");

    const auto x = route(Venue::Xts, kite, at_ms(0));
    check(!x && x.error() == RouteError::VenueUnavailable,
          "an XTS order is REFUSED while P4-06 is unbuilt -- a silent"
          " fallback would send the order to a broker the caller did not"
          " choose, on an account they may not have meant to use");
    check(kite.used_today() == 1,
          "and the refused XTS order did not consume Kite's budget");

    const auto n = route(Venue::None, kite, at_ms(0));
    check(!n && n.error() == RouteError::NoVenue,
          "an unset venue is refused -- Venue::None is ordinal 0 so a zeroed"
          " request cannot route anywhere by default");

    BrokerThrottle fyers{kFyersOrderLimits};
    BrokerThrottle kite_secondary{kKiteOrderLimits};
    const auto f = route(Venue::Fyers, fyers, kite_secondary, at_ms(0));
    check(f && f->venue == Venue::Fyers,
          "the explicit FYERS route uses the FYERS throttle");
    check(fyers.used_today() == 1 && kite_secondary.used_today() == 0,
          "a FYERS route never consumes the Kite secondary budget");
    const auto f_legacy = route(Venue::Fyers, kite, at_ms(0));
    check(!f_legacy && f_legacy.error() == RouteError::VenueUnavailable,
          "the legacy Kite-only route refuses FYERS instead of silently"
          " routing a primary order to the secondary account");
    check(std::strcmp(describe(Venue::Fyers), "FYERS") == 0,
          "venue descriptions expose the primary broker clearly");

    // A throttled venue surfaces as a route failure carrying the reason.
    BrokerThrottle tiny{BrokerLimits{1, 0, 0}};
    (void)route(Venue::Kite, tiny, at_ms(0));
    const auto blocked = route(Venue::Kite, tiny, at_ms(0));
    check(!blocked && blocked.error() == RouteError::AllThrottled,
          "and a throttled venue fails the route rather than sending anyway");

    check(RouteDecision{}.venue == Venue::None
          && ThrottleDecision{}.verdict == ThrottleVerdict::Allowed,
          "a zeroed decision reads as no-venue and as permission-not-refusal,"
          " so neither can be mistaken for an unexplained block");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void time_comes_off_the_tick()
{
    std::printf("\n6 time_comes_off_the_tick\n");
    BrokerThrottle t{BrokerLimits{2, 0, 0}};
    check(t.acquire(at_ms(0)).verdict == ThrottleVerdict::Allowed, "one");
    check(t.acquire(at_ms(0)).verdict == ThrottleVerdict::Allowed, "two");
    check(t.acquire(at_ms(999)).verdict == ThrottleVerdict::PerSecond,
          "at 999 ms the window has not rolled");
    check(t.acquire(at_ms(1000)).verdict == ThrottleVerdict::Allowed,
          "at exactly 1000 ms it has -- the boundary is at-or-after");

    // Nothing here reads a clock, so the same sequence of timestamps gives
    // the same decisions every time. That is what makes a replay reproduce
    // the live path (rules 6 and 7).
    BrokerThrottle a{kKiteOrderLimits};
    BrokerThrottle b{kKiteOrderLimits};
    bool identical = true;
    for (std::int64_t ms = 0; ms < 5000; ms += 37) {
        if (a.acquire(at_ms(ms)).verdict != b.acquire(at_ms(ms)).verdict) {
            identical = false;
        }
    }
    check(identical,
          "two throttles fed the same timestamps make identical decisions --"
          " nothing reads a wall clock, so a replay throttles at exactly the"
          " ticks the live path did");

    // A fixed window admits a boundary burst, which is a stated cost rather
    // than an oversight.
    BrokerThrottle w{BrokerLimits{5, 0, 0}};
    int burst = 0;
    for (int i = 0; i < 5; ++i) {
        if (w.acquire(at_ms(999)).verdict == ThrottleVerdict::Allowed) {
            ++burst;
        }
    }
    for (int i = 0; i < 5; ++i) {
        if (w.acquire(at_ms(1000)).verdict == ThrottleVerdict::Allowed) {
            ++burst;
        }
    }
    std::printf("    fixed window: %d orders across a window boundary against"
                " a limit of 5\n", burst);
    check(burst == 10,
          "a FIXED window admits up to 2x the limit across a boundary -- a"
          " documented cost of not carrying a timestamp per event, and Kite's"
          " own limiter is not documented to be sliding either");
}

} // namespace

int main()
{
    std::printf("altair oms throttle tests\n");
    a_burst_is_allowed_then_throttled_and_the_window_rolls();
    the_daily_budget_runs_out_while_the_fast_limits_never_bind();
    the_tightest_window_binds_and_says_which();
    an_unset_limit_does_not_block_everything();
    the_router_refuses_an_unavailable_venue_rather_than_falling_back();
    time_comes_off_the_tick();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
