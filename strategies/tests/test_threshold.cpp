// Tests for strategies/threshold.hpp -- the prev-2-day breakout and the
// BANKNIFTY/NIFTY ratio z-score, on scripted bars.

#include <strategies/threshold.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using namespace altair::threshold;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

struct B { double o, h, l, c; };
std::vector<DayBar> bars(std::initializer_list<B> in) {
    std::vector<DayBar> out;
    std::int64_t day = 20'000;
    for (const B& b : in) out.push_back(DayBar{day++, b.o, b.h, b.l, b.c});
    return out;
}

} // namespace

int main() {
    std::printf("[1] entries\n");
    {
        // Previous two highs 105 and 104: a day reaching 107 buys at 105, the
        // stop fixed at the previous day's low (96).
        const auto t = two_day_breakout(bars({{100, 105, 95, 100}, {100, 104, 96, 101}, {100, 107, 99, 106}}));
        check(t.size() == 1 && t[0].side == 1 && t[0].entry == 105.0 && t[0].stop == 96.0,
              "a break above the higher of the previous two highs buys at that level, stop at the previous low");
        check(t[0].why == ExitWhy::OpenEod && t[0].exit == 106.0, "what is open on the last day closes at its close");
        const auto g = two_day_breakout(bars({{100, 105, 95, 100}, {100, 104, 96, 101}, {108, 110, 107, 109}}));
        check(g.size() == 1 && g[0].entry == 108.0, "a gap above the level fills at the open");
        const auto s = two_day_breakout(bars({{100, 105, 95, 100}, {100, 104, 96, 101}, {97, 98, 90, 92}}));
        check(s.size() == 1 && s[0].side == -1 && s[0].entry == 95.0 && s[0].stop == 104.0,
              "a break below the lower of the previous two lows sells at that level, stop at the previous high");
        const auto both = two_day_breakout(bars({{100, 105, 95, 100}, {100, 104, 96, 101}, {100, 108, 90, 93}}));
        check(both.size() == 1 && both[0].side == -1, "from flat, both sides on one day: a close below the open picks short");
        const auto up = two_day_breakout(bars({{100, 105, 95, 100}, {100, 104, 96, 101}, {100, 108, 90, 106}}));
        check(up.size() == 1 && up[0].side == 1, "and a close above the open picks long");
    }

    std::printf("[2] exits\n");
    {
        // Long at 105 (stop 96). Day 3 stays above day 2's low (99); day 4
        // trades below day 3's low (103): out at 103, the trailing stop.
        const auto t = two_day_breakout(bars({{100, 105, 95, 100}, {100, 104, 96, 101}, {100, 107, 99, 106},
                                              {106, 110, 103, 109}, {108, 109, 101, 102}}));
        check(t.size() == 1 && t[0].exit_i == 4 && t[0].exit == 103.0 && t[0].why == ExitWhy::Reversal && t[0].days() == 2,
              "a long leaves below the previous day's low, at that level: Reversal");
        check(t[0].mfe == 5.0 && t[0].mae == 4.0, "MFE and MAE are measured over the days after entry, exit day included");
        // Entry day's low (90) under the fixed stop (96): the next day's
        // nearer stop is the fixed one.
        const auto s = two_day_breakout(bars({{100, 105, 95, 100}, {100, 104, 96, 101}, {100, 107, 90, 106},
                                              {100, 101, 94, 95}}));
        check(s.size() >= 1 && s[0].exit == 96.0 && s[0].why == ExitWhy::Stop, "when the fixed stop is nearer it fills: Stop");
        const auto gap = two_day_breakout(bars({{100, 105, 95, 100}, {100, 104, 96, 101}, {100, 107, 99, 106},
                                                {95, 97, 93, 94}}));
        check(gap.size() >= 1 && gap[0].exit == 95.0, "a gap through the stop fills at the open");
        const auto hold = two_day_breakout(bars({{100, 105, 95, 100}, {100, 104, 96, 101}, {100, 107, 80, 106}}));
        check(hold.size() == 1 && hold[0].why == ExitWhy::OpenEod, "no exit on the entry day: one day's minimum hold");
    }

    std::printf("[3] same-day flip\n");
    {
        // Long at 105 (stop 96). Day 3 opens at 98, under day 2's low (99):
        // out at the open. Its low (90) is also under the lower of the
        // previous two lows (96): short at 96 the same day.
        const auto t = two_day_breakout(bars({{100, 105, 95, 100}, {100, 104, 96, 101}, {100, 107, 99, 106},
                                              {98, 99, 90, 92}}));
        check(t.size() == 2 && t[0].side == 1 && t[0].exit == 98.0 && t[1].side == -1 && t[1].flip && t[1].entry_i == 3,
              "the exit day's opposite breakout reverses the position the same day (Flip)");
        check(t[1].entry == 96.0, "the reversed side fills at its own level when the open is not beyond it");
        check(t[0].points() == -7.0, "points are side x (exit - entry)");
    }

    std::printf("[4] ratio z-score\n");
    {
        RatioConfig cfg;
        cfg.window = 5;
        // B flat at 100; A at 200 with a little noise, then 230 (z > 1),
        // back to 200 (z reaches 0), then 170 (z < -1).
        std::vector<double> a{200, 201, 199, 200, 201, 230, 231, 200, 201, 199, 200, 200, 170, 171};
        std::vector<double> b(a.size(), 100.0);
        const auto d = ratio_z(a, b, cfg);
        check(std::isnan(d[3].z) && !std::isnan(d[4].z), "no z before a full window");
        check(d[5].pos == -1, "z above +1 shorts the ratio (short A, long B)");
        check(d[6].pos == -1, "and it is held while z stays between the bands");
        check(d[10].pos == -1 && d[11].pos == 0, "|z| under 0.25 goes flat");
        check(d[12].pos == +1, "z below -1 buys the ratio");
        const auto trips = ratio_trips(d);
        check(trips.size() == 2 && trips[0].side == -1 && trips[0].entry_i == 5 && trips[0].exit_i == 11 && trips[1].side == 1
                  && trips[1].entry_i == 12 && trips[1].open_at_end,
              "trips: one short of the ratio, then a long still open at the end");
    }

    std::printf("\nthreshold: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
