#include <backtest/agent_market.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* text) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    if (!ok) ++failures;
}

std::vector<altair::SimAgent> agents() {
    using altair::SimAgentKind;
    return {
        {SimAgentKind::Fundamental, 1'000'000, 0, 200.0, 20},
        {SimAgentKind::Momentum, 1'000'000, 0, 300.0, 20},
        {SimAgentKind::Noise, 1'000'000, 0, 8.0, 8},
        {SimAgentKind::LiquidityProvider, 5'000'000, 0, 0.0, 100}
    };
}
}

int main() {
    using namespace altair;
    const AgentMarketConfig config{100.0, 105.0, 0.0005, 42};
    auto a = AgentMarket::create(config, agents());
    auto b = AgentMarket::create(config, agents());
    check(a && b, "agent market accepts one explicitly-capacitated liquidity provider");
    bool identical = a && b;
    bool conserved = a && b;
    if (a && b) for (int i = 0; i < 100; ++i) {
        const auto sa = a->step(), sb = b->step();
        identical = identical && sa && sb && sa->price == sb->price
                  && sa->volume == sb->volume;
        if (!sa || !sb) break;
        conserved = conserved && a->conserved() && b->conserved();
    }
    check(conserved, "every clearing step conserves total cash and inventory");
    check(identical, "identical seeds produce identical agent-market trajectories");
    check(a && std::isfinite(a->price()) && a->price() > 0.0,
          "agent market retains a finite positive clearing price");
    auto no_provider = agents();
    no_provider.back().kind = SimAgentKind::Noise;
    check(!AgentMarket::create(config, no_provider),
          "agent simulation refuses to invent an external liquidity source");
    std::printf("Agent market: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
