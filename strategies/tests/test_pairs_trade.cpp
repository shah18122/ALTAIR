#include <strategies/pairs_trade.hpp>
#include <cmath>
#include <cstdio>
#include <vector>

int main() {
    using namespace altair;
    int failures = 0;
    const auto check = [&](bool ok, const char* text) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };
    const std::vector<double> x{100,101,102,103,104,105};
    const std::vector<double> y{100, 98, 99,102,104,105};
    const std::vector<double> z{0,-2.2,-1.5,-0.2,0,0};
    PairTradePolicy p; p.round_trip_cost_bps = 4.0;
    const auto trades = replay_pair_trades(y, x, z, 1.0, p);
    check(trades && trades->size() == 1 && (*trades)[0].spread_side == 1,
          "negative spread enters long-residual pair and exits at mean");
    check(trades && (*trades)[0].entry_bar == 1 && (*trades)[0].exit_bar == 3,
          "entry and exit bars obey the declared z thresholds");
    check(trades && (*trades)[0].cost == 40.0
          && (*trades)[0].net_pnl == (*trades)[0].gross_pnl - 40.0,
          "pair P&L includes explicit round-trip cost");
    auto stop_z = z; stop_z[2] = -4.2;
    const auto stopped = replay_pair_trades(y, x, stop_z, 1.0, p);
    check(stopped && stopped->size() == 1 && (*stopped)[0].stopped,
          "structural divergence reaches a deterministic stop exit");
    std::printf("Pairs trade: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
