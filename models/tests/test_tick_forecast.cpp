// models/tests/test_tick_forecast.cpp -- models/tick_forecast.hpp finds the
// bid-ask bounce where there is one, nothing where there is none, and its
// realised-vol and gap forecasts beat the naive ones on clustered activity.

#include <models/tick_forecast.hpp>

#include <cmath>
#include <cstdio>
#include <random>

namespace {

namespace tf = altair::tick_forecast;
int g_fail = 0;

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

/// `days` sessions of trades. bounce: trades print at bid or ask around a
/// random-walk mid (negative lag-1 autocorrelation). Otherwise trades print at
/// the mid itself. Activity clusters (busy and quiet spells) either way.
tf::TkEngine run(int days, bool bounce, unsigned seed) {
    std::mt19937_64 g(seed);
    std::normal_distribution<double> z(0.0, 1.0);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    tf::TkEngine e;
    double mid = 20000.0, act = 1.0;
    for (int d = 0; d < days; ++d) {
        double t = tf::kTkOpen;
        while (t < tf::kTkClose - 1) {
            act = std::exp(0.98 * std::log(act) + 0.15 * z(g));
            t += -std::log(1.0 - u(g)) * 2.0 / act;
            mid += 0.05 * std::sqrt(act) * z(g) * 4.0;
            const double px = std::round((bounce ? mid + (u(g) < 0.5 ? -0.5 : 0.5) : mid) / 0.05) * 0.05;
            e.add({20220101 + d, static_cast<std::int32_t>(t), px, 25.0 * (1 + static_cast<int>(3 * u(g)))});
        }
    }
    e.finish();
    return e;
}

} // namespace

int main() {
    const auto b = run(60, true, 3);
    const auto& r = b.ret();
    std::printf("  bounce: %zu ticks, RW %.3f AR %.3f bp RMSE, DM %+.1f; reversal %.1f%%\n", b.ticks(), r.rmse(0), r.rmse(2), r.dm(2, 0),
                100 * b.reversal().rate());
    check(r.skill(2, 0) > 0.05 && r.dm(2, 0) < -4.0, "bid-ask bounce: the AR ridge beats the random walk on the next trade");
    check(b.reversal().rate() > 0.6 && b.reversal().z() > 5.0, "and the next change reverses the last one");
    double in, oos;
    b.overfit_ret(in, oos);
    check(std::isfinite(in) && in > 0.95 * oos, "in-sample error close to out-of-sample (no overfit)");

    const auto n = run(60, false, 5);
    std::printf("  no bounce: AR vs RW skill %+.3f%%, DM %+.2f\n", 100 * n.ret().skill(2, 0), n.ret().dm(2, 0));
    check(n.ret().skill(2, 0) < 0.01 && std::fabs(n.ret().dm(2, 0)) < 3.5, "a pure random walk: no false edge on the next trade");

    const auto& rv = b.rv(0);
    std::printf("  rv 60s: last %.3f mean %.3f tod %.3f HAR %.3f\n", rv.rmse(0), rv.rmse(1), rv.rmse(2), rv.rmse(3));
    check(rv.n() > 1000 && rv.skill(3, 0) > 0.05, "clustered activity: HAR beats last window for the next minute's vol");
    std::printf("  gap: last %.3f mean %.3f ewma %.3f ridge %.3f\n", b.gap().rmse(0), b.gap().rmse(1), b.gap().rmse(2), b.gap().rmse(3));
    check(b.gap().skill(3, 1) > 0.0, "and the gap ridge beats the running mean");

    std::printf(g_fail ? "tick_forecast: %d FAILED\n" : "tick_forecast: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
