// models/tests/test_vol_forecast.cpp -- models/vol_forecast.hpp: it finds
// clustering and the clock where they exist, finds nothing where there is
// nothing, and never reads a bar it is scored on.

#include <models/vol_forecast.hpp>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

namespace vf = altair::vol_forecast;
int g_fail = 0;

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

/// 5-minute bars, 75 a session from 09:15, for `days` sessions. `clustered`:
/// a GARCH-like volatility and a U-shaped clock; otherwise iid at one vol.
std::vector<vf::Bar> make(int days, bool clustered, unsigned seed) {
    std::mt19937_64 g(seed);
    std::normal_distribution<double> z(0.0, 1.0);
    std::vector<vf::Bar> out;
    double p = 20000.0, h = 1.0;
    for (int d = 0; d < days; ++d) {
        const std::int64_t day0 = (20000 + d) * vf::kVfDaySec;
        for (int k = 0; k < 75; ++k) {
            double sd = 8e-4;
            if (clustered) {
                const double clock = 1.0 + 1.5 * std::exp(-k / 6.0) + 0.8 * std::exp(-(74 - k) / 6.0);
                sd = 6e-4 * clock * std::sqrt(h);
            }
            const double r = sd * z(g);
            if (clustered) h = 0.05 + 0.90 * h + 0.05 * (r * r) / (sd * sd / h);
            p *= std::exp(r);
            out.push_back({day0 + (9 * 3600 + 15 * 60) + k * 300, p});
        }
    }
    return out;
}

} // namespace

int main() {
    vf::Options o;
    o.burn_sessions = 60;

    const auto c = make(400, true, 7);
    const auto rc = vf::run(c, {}, o);
    const auto har = vf::score(rc.rows, vf::kHar), seas = vf::score(rc.rows, vf::kSeas), rw = vf::score(rc.rows, vf::kRw);
    std::printf("  clustered: RW %.2f  Seasonal %.2f  HAR %.2f bp RMSE (n %zu); HAR skill vs RW %+.1f %%, DM %+.1f\n",
                rw.rmse, seas.rmse, har.rmse, har.n, 100 * har.skill_rw, har.dm_rw);
    check(har.n > 0 && har.skill_rw > 0.15 && har.dm_rw < -3.0, "clustered data with a clock: HAR beats the random walk, significantly");
    check(seas.skill_rw > 0.10, "the time-of-day average alone beats the random walk");
    check(har.skill_rw_thirds[0] > 0 && har.skill_rw_thirds[1] > 0 && har.skill_rw_thirds[2] > 0,
          "and it does so in every third of the scored period");
    check(std::isfinite(rc.har_in_sample_rmse) && rc.har_in_sample_rmse > 0.9 * rc.har_oos_same_weights,
          "in-sample error is close to out-of-sample: no sign of overfitting");

    const auto n = make(400, false, 11);
    const auto rn = vf::run(n, {}, o);
    const auto hn = vf::score(rn.rows, vf::kHar);
    std::printf("  iid: HAR vs Mean 22 skill %+.2f %%, DM %+.2f\n", 100 * hn.skill_mean, hn.dm_mean);
    check(hn.n > 0 && std::fabs(hn.dm_mean) < 3.0 && hn.skill_mean < 0.02,
          "iid data: HAR finds nothing beyond the plain average (no false edge)");

    // No look-ahead: change every bar after a cut; every forecast for a bar at
    // or before the cut must be identical.
    auto moved = c;
    const std::size_t cut = c.size() * 2 / 3;
    for (std::size_t i = cut + 1; i < moved.size(); ++i) moved[i].c *= 1.0 + 0.01 * std::sin(static_cast<double>(i));
    const auto rm = vf::run(moved, {}, o);
    bool same = true;
    std::size_t compared = 0;
    for (std::size_t i = 0; i < rc.rows.size() && i < rm.rows.size(); ++i) {
        if (rc.rows[i].t > c[cut].t) break;
        for (std::size_t m = 0; m < vf::kModels; ++m) same = same && rc.rows[i].f[m] == rm.rows[i].f[m];
        ++compared;
    }
    check(same && compared > 1000, "a different future changes no forecast made before it");

    // A daily series runs too (no clock, one bar a session).
    std::vector<vf::Bar> daily;
    for (std::size_t i = 74; i < c.size(); i += 75) daily.push_back({(c[i].t / vf::kVfDaySec) * vf::kVfDaySec, c[i].c});
    vf::Options od = o;
    od.intraday = false;
    const auto rd = vf::run(daily, {}, od);
    check(!rd.rows.empty() && std::isfinite(vf::score(rd.rows, vf::kComb).rmse), "daily bars: every model scores");

    std::printf(g_fail ? "vol_forecast: %d FAILED\n" : "vol_forecast: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
