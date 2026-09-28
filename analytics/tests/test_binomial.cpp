#include <analytics/binomial.hpp>

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* text) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    if (!ok) ++failures;
}
}

int main() {
    using namespace altair;
    const Price spot{10'000}, strike{10'000};
    const Years t{0.5};
    const Vol vol{0.20};
    const auto lattice = binomial_price(OptionRight::Call, spot, strike, t, vol,
                                        0.05, 512, false);
    const auto black = black_scholes(OptionRight::Call, spot, strike, t, vol, 0.05, 0.0);
    check(lattice && black && std::fabs(lattice->price - black->price) < 3.0,
          "European CRR lattice converges toward Black-Scholes");
    const auto american = binomial_price(OptionRight::Put, spot, strike, t, vol,
                                         0.10, 512, true);
    const auto european = binomial_price(OptionRight::Put, spot, strike, t, vol,
                                         0.10, 512, false);
    check(american && european && american->price >= european->price,
          "American lattice never prices below its European continuation value");
    check(!binomial_price(OptionRight::Call, spot, strike, t, vol, 0.05, 0),
          "zero step lattice is refused");
    check(!binomial_price(OptionRight::Call, spot, strike, t, vol, 0.05, 4097),
          "unbounded step request is refused");
    std::printf("Binomial: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
