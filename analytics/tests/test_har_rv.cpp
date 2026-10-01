// Tests for analytics/har_rv.hpp -- realised variance and the HAR forecast.

#include <analytics/har_rv.hpp>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

using namespace altair;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

/// Daily variance whose log follows a persistent AR(1): today's level says a
/// lot about the next weeks', which is what HAR is for.
std::vector<double> persistent(std::size_t n, unsigned seed) {
    std::mt19937 g(seed);
    std::normal_distribution<double> z(0.0, 1.0);
    std::vector<double> rv(n);
    double x = std::log(1e-4);
    for (std::size_t i = 0; i < n; ++i) {
        x = std::log(1e-4) + 0.97 * (x - std::log(1e-4)) + 0.2 * z(g);
        const double e = z(g);
        rv[i] = std::exp(x) * (0.5 + 0.5 * e * e);   // measurement noise around the true level
    }
    return rv;
}

void test_forecast_tracks_the_level() {
    const auto rv = persistent(1500, 7);
    double err_har = 0.0, err_flat = 0.0, all_mean = 0.0;
    for (std::size_t i = 0; i < 500; ++i) { all_mean += rv[i]; }
    all_mean /= 500.0;
    std::size_t n = 0;
    for (std::size_t t = 500; t + 10 < rv.size(); t += 5) {
        const auto f = har_forecast(rv, t, 10, 1000);
        if (!f) { continue; }
        double real = 0.0;
        for (std::size_t k = t + 1; k <= t + 10; ++k) { real += rv[k]; }
        real /= 10.0;
        err_har += std::pow(std::log(f->variance / real), 2);
        err_flat += std::pow(std::log(all_mean / real), 2);
        ++n;
    }
    std::printf("    log error: HAR %.3f, flat %.3f over %zu forecasts\n", err_har / static_cast<double>(n),
                err_flat / static_cast<double>(n), n);
    check(n > 150 && err_har < 0.6 * err_flat, "on persistent variance, HAR forecasts far better than a constant");
}

void test_no_look_ahead() {
    auto rv = persistent(400, 3);
    const auto a = har_forecast(rv, 300, 15);
    for (std::size_t k = 301; k < rv.size(); ++k) { rv[k] *= 50.0; }   // the future, changed beyond recognition
    const auto b = har_forecast(rv, 300, 15);
    check(a && b && a->variance == b->variance && a->rows == b->rows,
          "a forecast at day t is unchanged by anything after day t");
    rv[300] *= 4.0;
    const auto c = har_forecast(rv, 300, 15);
    check(c && c->variance > a->variance, "and does use day t itself");
}

void test_refusals_and_rv() {
    const auto rv = persistent(100, 1);
    check(!har_forecast(rv, 40, 20).has_value(), "too little history: refused");
    auto bad = rv;
    bad[10] = 0.0;
    check(!har_forecast(bad, 90, 5).has_value(), "a zero variance is refused, not logged as minus infinity");
    check(!har_forecast(rv, 90, 0).has_value(), "a zero horizon is refused");
    const double closes[] = {101.0, 99.0};
    const double r = session_rv(closes, 100.0, 98.0);
    const double want = std::pow(std::log(101.0 / 100.0), 2) + std::pow(std::log(99.0 / 101.0), 2)
                      + std::pow(std::log(100.0 / 98.0), 2);
    check(std::fabs(r - want) < 1e-15, "session RV: squared bar returns from the open, plus the overnight gap");
}

} // namespace

int main() {
    std::printf("HAR realised variance\n");
    test_forecast_tracks_the_level();
    test_no_look_ahead();
    test_refusals_and_rv();
    std::printf("HAR realised variance: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
