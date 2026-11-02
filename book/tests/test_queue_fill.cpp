// book/queue_fill.hpp: a resting order's estimated queue place and its fills.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <book/queue_fill.hpp>

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

using namespace altair::queue;

} // namespace

int main() {
    const ProbQueueConfig c{};   // power, n = 3

    // The probability that a cancel came from behind.
    check(std::fabs(prob_behind(c, 100, 100) - 0.5) < 1e-12, "equal front and back: half the cancels are behind");
    check(prob_behind(c, 10, 100) > 0.99, "a short front: a cancel is almost surely from behind");
    check(prob_behind(c, 100, 0) == 0.0, "nothing behind: every cancel is ahead");
    check(prob_behind(c, 0, 0) == 1.0, "an empty level: nothing ahead moves");
    const ProbQueueConfig lg{ProbFn::Log, 0.0};
    check(prob_behind(lg, 100, 100) == 0.5 && prob_behind(lg, 10, 100) < prob_behind(c, 10, 100),
          "log is gentler than the power");

    // A sell resting at the ask joins the back.
    QueueOrder o;
    place(o, -1, 10'000, 30, 500);
    check(o.front == 500.0 && o.remaining == 30, "placed behind the 500 shown");
    check(on_trade(o, 9'995, 100) == 0, "a trade below the ask does not reach a resting sell");
    check(on_trade(o, 10'000, 200) == 0 && o.front == 300.0, "a trade at our price eats the front first");
    on_depth(o, c, 300);   // the 200 that traded explains the whole shrink
    check(o.front == 300.0, "a shrink that trades explain is not counted again");
    on_depth(o, c, 400);
    check(o.front == 300.0, "a level that grows adds behind, the front stays");
    on_depth(o, c, 300);   // 100 cancelled, front 300 back 100: mostly from ahead (n = 3)
    check(o.front < 300.0 && o.front > 200.0, "a cancel moves the front up by the chance it was ahead");
    const double front = o.front;
    const std::int64_t got = on_trade(o, 10'000, static_cast<std::int64_t>(front) + 10);
    check(got == 10 || got == 9, "a trade past the front fills the part that reached us");
    check(o.front == 0.0, "the front is used up");
    check(on_trade(o, 10'000, 5) == 5 && o.remaining == 30 - got - 5, "at the head, the next trade at our price fills");
    const std::int64_t rest = o.remaining;
    check(on_trade(o, 10'010, 1'000) == rest, "a trade through our price fills the rest");
    check(o.remaining == 0, "filled in full");

    // A sweep through the price fills no more than printed.
    QueueOrder s;
    place(s, -1, 10'000, 50, 1'000);
    check(on_trade(s, 10'005, 20) == 20 && s.remaining == 30, "a trade through fills only what printed");

    // A crossing quote takes the rest.
    check(on_cross(s, 9'995) == 0, "a bid below the offer does not cross");
    check(on_cross(s, 10'000) == 30 && s.remaining == 0, "a bid at the offer takes the rest");

    // The level vanishing without a trade: at the head, still unfilled.
    QueueOrder v;
    place(v, -1, 10'000, 10, 200);
    on_depth(v, c, 0);
    check(v.front == 0.0 && v.remaining == 10, "the level emptied by cancels: at the head, unfilled");

    // A resting buy mirrors it.
    QueueOrder b;
    place(b, +1, 9'990, 10, 50);
    check(on_trade(b, 9'995, 100) == 0, "a trade above the bid does not reach a resting buy");
    check(on_trade(b, 9'990, 55) == 5, "a trade at the bid past the front fills a resting buy");
    check(on_cross(b, 9'990) == 5, "an offer down to the bid takes the rest of a buy");

    std::printf("%s\n", failures == 0 ? "all queue fill checks passed" : "queue fill checks did not pass");
    return failures == 0 ? 0 : 1;
}
