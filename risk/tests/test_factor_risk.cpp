#include <risk/factor_risk.hpp>

#include <array>
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
    // Two assets, market/value factors. Exposures are B' holdings.
    const std::array<double,2> holdings{100.0, -40.0};
    const std::array<double,4> beta{1.0, 0.5, 0.8, -0.25};
    const std::array<double,4> covariance{0.04, 0.006, 0.006, 0.01};
    const std::array<double,2> idio{0.02, 0.03};
    const auto result = factor_risk(holdings, beta, 2, covariance, idio);
    check(result && std::fabs(result->exposure[0] - 68.0) < 1e-12
                 && std::fabs(result->exposure[1] - 60.0) < 1e-12,
          "factor exposures reconcile exactly to B-prime times holdings");
    if (result) {
        const double contributions = result->factor_contribution[0]
                                   + result->factor_contribution[1];
        check(std::fabs(contributions - result->factor_variance) < 1e-10,
              "factor contributions add exactly to common-factor variance");
        check(std::fabs(result->total_variance
                        - result->factor_variance
                        - result->idiosyncratic_variance) < 1e-10,
              "common and idiosyncratic components reconcile to total variance");
    }
    auto asymmetric = covariance;
    asymmetric[1] = 0.02;
    check(!factor_risk(holdings, beta, 2, asymmetric, idio),
          "factor risk refuses a non-symmetric covariance matrix");
    std::printf("Factor risk: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
