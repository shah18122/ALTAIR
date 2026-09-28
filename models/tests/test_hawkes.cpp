#include <models/hawkes.hpp>

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
    // Ogata thinning from a seeded generator.
    std::uint64_t state = 0x123456789ABCDEF0ull;
    auto uniform = [&]() {
        state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
        return (static_cast<double>((state * 2685821657736338717ull) >> 11) + 1.0)
             / (static_cast<double>(1ull << 53) + 2.0);
    };
    constexpr double mu = 0.8, alpha = 0.6, beta = 1.5, end = 600.0;
    std::vector<double> events;
    double time = 0.0, excitation = 0.0;
    while (time < end) {
        const double upper = mu + excitation;
        const double dt = -std::log(uniform()) / upper;
        time += dt;
        if (time >= end) break;
        excitation *= std::exp(-beta * dt);
        if (uniform() * upper <= mu + excitation) {
            events.push_back(time);
            excitation += alpha;
        }
    }
    const auto fitted = fit_hawkes(events, end, 48, 60);
    check(fitted && fitted->branching_ratio() < 1.0,
          "Hawkes fit enforces a stable branching ratio");
    if (fitted) {
        std::printf("Hawkes true n=%.3f fitted n=%.3f beta=%.3f events=%zu\n",
                    alpha / beta, fitted->branching_ratio(), fitted->beta,
                    events.size());
        check(std::fabs(fitted->branching_ratio() - alpha / beta) < 0.18,
              "Hawkes likelihood recovers the seeded branching ratio");
        const auto residual = fitted->residuals(events);
        double mean = 0.0;
        for (const double value : residual) mean += value;
        mean /= static_cast<double>(residual.size());
        check(mean > 0.75 && mean < 1.25,
              "time-rescaling residuals have approximately unit mean");
    }
    HawkesModel unstable{1.0, 2.0, 1.0, 0.0};
    check(unstable.branching_ratio() > 1.0,
          "branching ratio makes an unstable parameter set explicit");
    std::printf("Hawkes: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
