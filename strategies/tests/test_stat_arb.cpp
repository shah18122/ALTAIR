#include <strategies/stat_arb.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* text) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    if (!ok) ++failures;
}
}

int main() {
    using namespace altair;
    const std::vector<StatArbCandidateLeg> c{
        {1, 0.020, 100, 1.0, 100'000, 5},
        {2, 0.010, 200, 1.0, 100'000, 5},
        {3,-0.015,  80, 0.5, 100'000, 5},
        {4,-0.012, 120, 1.5, 100'000, 5}
    };
    const auto p = build_stat_arb(c, 120'000.0, 1e-8);
    check(p.has_value(), "costed stat-arb basket accepts a feasible hedge");
    check(p && std::fabs(p->gross_notional - 120'000.0) < 1e-8
            && std::fabs(p->net_notional) < 1e-8,
          "long and short cash notionals conserve exactly");
    check(p && std::fabs(p->factor_exposure) < 1e-8,
          "multi-leg hedge neutralises the declared factor exposure");
    check(p && p->expected_cost > 0.0
            && p->expected_net == p->expected_gross - p->expected_cost,
          "every candidate edge is reported after explicit round-trip cost");
    auto capped = c;
    capped[0].max_notional = 1.0;
    check(!build_stat_arb(capped, 120'000.0),
          "portfolio refuses capacity it cannot fill without breaking constraints");
    auto costly = c;
    for (auto& x : costly) x.round_trip_cost_bps = 10'000.0;
    check(!build_stat_arb(costly, 120'000.0),
          "portfolio refuses a gross signal whose cost removes the edge");
    std::printf("Stat arb: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
