// oms/tests/test_capacity.cpp -- P13-07.
//
// QUANTLAB's Q7: a size-blind cost model literally cannot express a capacity
// ceiling. This test builds both models and shows the linear one allocating
// without limit on the same inputs where the square-root law produces a bound.

#include <algorithm>
#include <oms/capacity.hpp>

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

} // namespace

int main() {
    using altair::ImpactParams;
    using altair::capacity_from_edge;
    using altair::linear_impact;
    using altair::square_root_impact;

    std::printf("P13-07 market impact and the capacity ceiling\n");

    // NIFTY futures: roughly 300,000 contracts a day, 1% daily vol.
    ImpactParams p;
    p.daily_volume = 300'000.0;
    p.daily_vol = 0.010;
    p.eta = 0.7;

    // A 15 bps edge -- small, and about what survives cost in this market.
    const double edge = 0.0015;

    // ---- 1. COST PER SHARE RISES WITH SIZE. THAT IS THE WHOLE POINT -------
    {
        std::printf("\n  %12s %16s %16s\n", "size", "sqrt impact",
                    "linear impact");
        double prev = -1.0;
        bool rising = true;
        for (const double q : {1'000.0, 10'000.0, 100'000.0, 1'000'000.0}) {
            const auto s = square_root_impact(q, p);
            const double l = linear_impact(q, 0.0008);
            if (!s) { continue; }
            std::printf("    %10.0f %15.2f%% %15.2f%%\n",
                        q, 100.0 * *s, 100.0 * l);
            if (*s <= prev) { rising = false; }
            prev = *s;
        }
        check(rising,
              "square-root impact RISES with order size, so total cost grows "
              "as Q^1.5 while revenue grows at best linearly -- the two must "
              "cross, and that crossing is the capacity");
        check(std::fabs(linear_impact(1'000.0, 0.0008)
                        - linear_impact(1'000'000.0, 0.0008)) < 1e-15,
              "the linear model charges the SAME per share at a thousand and "
              "at a million: profit is then linear in size, unbounded, and an "
              "optimiser handed it allocates infinite capital -- not a "
              "calibration error, the wrong functional form");
    }

    // ---- 2. THE CEILING, AND WHERE TO SIT UNDER IT ------------------------
    {
        const auto c = capacity_from_edge(edge, p);
        check(c.has_value(), "the capacity solves");
        if (c) {
            std::printf("\n  edge %.2f bps against %.0f contracts/day at "
                        "%.1f%% daily vol\n",
                        10000.0 * edge, p.daily_volume, 100.0 * p.daily_vol);
            std::printf("    break-even size : %10.0f contracts "
                        "(%.2f%% of daily volume)\n",
                        c->break_even_qty,
                        100.0 * c->break_even_qty / p.daily_volume);
            std::printf("    OPTIMAL size    : %10.0f contracts "
                        "(%.2f%% of daily volume)\n",
                        c->optimal_qty,
                        100.0 * c->optimal_qty / p.daily_volume);

            check(c->optimal_qty < c->break_even_qty,
                  "the profit-maximising size is SMALLER than break-even, and "
                  "by a factor of 9/4 -- sizing at break-even is the instinct "
                  "and it earns exactly zero");
            const auto at_be = square_root_impact(c->break_even_qty, p);
            check(at_be && std::fabs(*at_be - edge) < 1e-9,
                  "at break-even the impact exactly equals the edge, which is "
                  "what break-even means and is worth asserting rather than "
                  "trusting the algebra");
            check(c->max_profit_fraction > 0.0,
                  "and there is real profit at the optimal size");
        }
    }

    // ---- 3. THE COEFFICIENT IS SWEPT, NOT FITTED --------------------------
    //
    // QUANTLAB rejected its own plan to calibrate eta from an order-book
    // simulator as CIRCULAR: the simulator's depth profile is something you
    // wrote, so it hands back the number you put in. Literature range, swept,
    // reported as an interval.
    {
        std::printf("\n  capacity across the literature range for eta\n");
        std::printf("    %8s %18s %18s\n", "eta", "optimal size",
                    "% daily volume");
        double lo = 1e300, hi = 0.0;
        for (const double eta : {0.4, 0.7, 1.0, 1.5}) {
            ImpactParams q = p;
            q.eta = eta;
            const auto c = capacity_from_edge(edge, q);
            if (!c) { continue; }
            std::printf("    %8.1f %18.0f %17.2f%%\n",
                        eta, c->optimal_qty,
                        100.0 * c->optimal_qty / p.daily_volume);
            lo = std::min(lo, c->optimal_qty);
            hi = std::max(hi, c->optimal_qty);
        }
        std::printf("    -> capacity is a RANGE of %.0f to %.0f contracts, "
                    "a %.1fx spread\n", lo, hi, hi / lo);
        check(hi / lo > 3.0,
              "the plausible range of eta spans the capacity by more than 3x, "
              "so a single capacity number implies a precision nobody has -- "
              "it is reported as an interval for the same reason a Monte "
              "Carlo price is reported with a standard error");
    }

    // ---- 4. REFUSALS ------------------------------------------------------
    {
        check(!capacity_from_edge(0.0, p).has_value(),
              "a strategy with NO edge has no capacity question -- the answer "
              "is zero at every size, and returning a number would suggest "
              "otherwise");
        check(!capacity_from_edge(-0.001, p).has_value(),
              "and a negative edge is refused rather than producing a "
              "capacity for losing money faster");
        ImpactParams bad = p;
        bad.daily_volume = 0.0;
        check(!square_root_impact(1000.0, bad).has_value(),
              "an instrument with no volume divides by zero and is refused -- "
              "which is also the honest answer for an index, that does not "
              "trade at all");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
