#include <models/named_factors.hpp>

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
    std::vector<FactorInput> rows;
    for (std::uint64_t i = 1; i <= 10; ++i)
        rows.push_back({i, 100, 100.0, 10.0 * static_cast<double>(i),
                        2.0 * static_cast<double>(i), 100.0,
                        0.01 * static_cast<double>(i),
                        0.005 * static_cast<double>(i)});
    rows.back().information_available_ns = 950; // after the lagged cutoff
    const auto value = named_factor_portfolio(rows, NamedFactor::Value,
                                               1000, 100, 0.3);
    check(value && value->coverage == 0.9,
          "named factor excludes fundamentals unavailable at the lagged cutoff");
    check(value && value->long_instruments.front() == 8
                && value->short_instruments.front() == 1,
          "value factor ranks explicit book-to-market with deterministic ties");
    check(value && value->long_short_return > 0.0,
          "toy value portfolio reproduces its published top-minus-bottom direction");
    const auto momentum = named_factor_portfolio(rows, NamedFactor::Momentum,
                                                  1000, 100, 0.3);
    check(momentum && momentum->long_short_return > 0.0,
          "momentum definition uses the supplied lagged trailing return");
    std::printf("Named factors: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
