#include <models/fill_probability.hpp>

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
    std::vector<FillFeatures> x;
    std::vector<std::uint8_t> y;
    for (std::size_t i = 0; i < 600; ++i) {
        FillFeatures f;
        f.queue_ahead = static_cast<double>((i * 37) % 300);
        f.depth_at_price = 50.0 + static_cast<double>((i * 17) % 250);
        f.imbalance = -1.0 + 2.0 * static_cast<double>((i * 13) % 101) / 100.0;
        f.horizon_seconds = 1.0 + static_cast<double>((i * 7) % 60);
        const double score = -0.025 * f.queue_ahead + 0.07 * f.horizon_seconds
                           + 1.5 * f.imbalance;
        x.push_back(f);
        y.push_back(score > 0.0 ? 1 : 0);
    }
    const auto model = FillProbabilityModel::fit(x, y);
    check(model.has_value(), "fill-probability model fits labelled queue outcomes");
    if (model) {
        const double easy = model->probability({5, 100, 0.8, 45});
        const double hard = model->probability({250, 100, -0.8, 2});
        check(easy > 0.9 && hard < 0.1,
              "fill probability moves in the expected queue/horizon direction");
        std::vector<double> p; p.reserve(x.size());
        for (const auto& row : x) p.push_back(model->probability(row));
        const auto bins = FillProbabilityModel::reliability(p, y, 10);
        std::size_t represented = 0;
        if (bins) for (const auto& bin : *bins) represented += bin.count;
        check(bins && represented == x.size(),
              "reliability bins account for every labelled outcome exactly once");
    }
    check(!FillProbabilityModel::fit(
              std::span<const FillFeatures>(x.data(), 10),
              std::span<const std::uint8_t>(y.data(), 10)),
          "fill model refuses an underpowered sample");
    std::printf("Fill probability: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
