#include <models/event_study.hpp>

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
    std::vector<EntityReturn> returns;
    for (std::uint64_t entity : {11ull, 22ull}) {
        for (std::uint64_t i = 0; i < 20; ++i) {
            double asset = 0.001, benchmark = 0.001;
            if (i == 8) asset += entity == 11 ? 0.03 : 0.01;
            returns.push_back({(i + 1) * 100, entity, asset, benchmark});
        }
    }
    // Published after bar 7 closes: first usable bar is bar 8 (timestamp 800).
    const std::vector<EventObservation> events{{750, 11}, {760, 11}, {750, 22}};
    const auto result = event_study(events, returns, 1, 1, 1);
    check(result && result->windows.size() == 2 && result->excluded_overlap == 1,
          "event study embargoes overlapping same-entity windows");
    if (result) {
        check(result->windows[0].first_bar == 7,
              "event alignment uses the first bar close after publication");
        check(std::fabs(result->mean_cumulative_abnormal_return - 0.02) < 1e-12,
              "event CAR subtracts the benchmark and averages usable events");
        check(result->standard_error > 0.0,
              "event study reports cross-event uncertainty");
    }
    std::printf("Event study: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
