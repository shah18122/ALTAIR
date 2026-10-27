// Tests for analytics/vol_cone.hpp -- realised-vol windows, percentile rank,
// point-in-time cone rank, and the cone's quantiles.

#include <analytics/vol_cone.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using namespace altair;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// Closes whose log return alternates +/- r: every window's realised vol is
// exactly r * sqrt(252).
std::vector<double> zigzag(std::size_t n, double r) {
    std::vector<double> c{100.0};
    for (std::size_t i = 1; i < n; ++i) { c.push_back(c.back() * std::exp(i % 2 ? r : -r)); }
    return c;
}

void test_windows() {
    const auto c = zigzag(50, 0.01);
    const auto w = realised_vol_windows(c, c.size() - 1, 10);
    check(w.size() == 40, "one window per end day from h to the last");
    bool flat = true;
    for (const double v : w) { flat = flat && near(v, 0.01 * std::sqrt(252.0), 1e-12); }
    check(flat, "constant |return| gives r * sqrt(252) in every window");
    check(realised_vol_windows(c, 5, 10).empty(), "too short for the horizon: no windows");
    auto bad = c;
    bad[20] = 0.0;
    const auto wb = realised_vol_windows(bad, bad.size() - 1, 10);
    check(wb.size() == 40 - 11, "windows spanning a bad close are dropped");
}

void test_rank_and_point_in_time() {
    const std::vector<double> s{0.10, 0.12, 0.14, 0.16, 0.18};
    check(near(percentile_rank(s, 0.14), 0.6, 1e-12), "rank counts at-or-below");
    check(percentile_rank(s, 0.05) == 0.0 && percentile_rank(s, 0.30) == 1.0, "rank is bounded");
    check(std::isnan(percentile_rank({}, 0.1)), "no samples: NaN");

    // A calm first half then a wild second half: the rank at the midpoint must
    // not see the wild half.
    auto c = zigzag(200, 0.005);
    const auto wild = zigzag(201, 0.03);
    for (std::size_t i = 1; i < wild.size(); ++i) { c.push_back(c.back() * wild[i] / wild[i - 1]); }
    const double iv = 0.20;   // above the calm 7.9 vol, below the wild 47.6 vol
    const double early = cone_rank(c, 199, 10, iv);
    const double late = cone_rank(c, c.size() - 1, 10, iv);
    check(near(early, 1.0, 1e-12), "at the midpoint IV is above every calm window");
    check(late < 0.6, "once the wild half is history, the same IV ranks lower");
    check(std::isnan(cone_rank(c, 30, 10, iv)), "fewer than 60 windows: NaN");
}

void test_cone_slices() {
    const auto c = zigzag(300, 0.01);
    const std::vector<std::size_t> hs{5, 20};
    const auto cone = vol_cone(c, hs);
    check(cone.size() == 2 && cone[0].horizon == 5 && cone[1].windows == 280, "one slice per horizon");
    check(near(cone[1].min, cone[1].max, 1e-12) && near(cone[1].median, 0.01 * std::sqrt(252.0), 1e-12),
          "a constant series has a flat cone");
}

} // namespace

int main() {
    test_windows();
    test_rank_and_point_in_time();
    test_cone_slices();
    if (failures) { std::printf("%d check(s) FAILED\n", failures); return 1; }
    std::printf("all vol_cone checks passed\n");
    return 0;
}
