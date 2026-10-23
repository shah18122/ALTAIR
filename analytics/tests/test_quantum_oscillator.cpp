// analytics/tests/test_quantum_oscillator.cpp -- Orrell's quantum oscillator:
// the formulas, then the paper's two checkable claims on NIFTY's daily closes.

#include <analytics/quantum_oscillator.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

double gauss_ll(double x, double v) { return -0.5 * std::log(2.0 * std::numbers::pi * v) - x * x / (2.0 * v); }

double student_ll(double x, double v, double nu) {
    const double s = std::sqrt(v * (nu - 2.0) / nu);
    return std::lgamma((nu + 1.0) / 2.0) - std::lgamma(nu / 2.0) - 0.5 * std::log(nu * std::numbers::pi) - std::log(s)
         - (nu + 1.0) / 2.0 * std::log(1.0 + (x / s) * (x / s) / nu);
}

} // namespace

int main(int argc, char** argv) {
    using namespace altair::qosc;
    std::printf("Orrell quantum oscillator\n");

    // ---- the formulas -------------------------------------------------------
    {
        double s = 0.0, m = 0.0;
        for (std::size_t n = 0; n < 40; ++n) { s += level_prob(n, 0.25); m += static_cast<double>(n) * level_prob(n, 0.25); }
        check(std::fabs(s - 1.0) < 1e-12 && std::fabs(m - 0.25) < 1e-12, "energy levels: Poisson, mean lambda = <n>");
        check(std::fabs(rms_vol(1.0, 0.25) - std::sqrt(1.5)) < 1e-12 && std::fabs(rms_vol(1.0, 0.5) - std::sqrt(2.0)) < 1e-12,
              "eq. 1: RMS vol sqrt(3/2) sigma at lambda 1/4, sqrt(2) sigma at 1/2");
        check(std::fabs(kurtosis(0.25) - 13.0 / 3.0) < 1e-12 && kurtosis(0.0) == 3.0,
              "kurtosis 4.33 at lambda 1/4; a normal at lambda 0");
        // the density integrates to one and has the stated variance and kurtosis
        double i0 = 0, i2 = 0, i4 = 0;
        const double h = 0.001;
        for (double x = -30.0; x <= 30.0; x += h) {
            const double f = density(x, 1.0, 0.25) * h;
            i0 += f; i2 += x * x * f; i4 += x * x * x * x * f;
        }
        check(std::fabs(i0 - 1.0) < 1e-6 && std::fabs(i2 - 1.5) < 1e-6 && std::fabs(i4 / (i2 * i2) - kurtosis(0.25)) < 1e-6,
              "the mix of normals integrates to 1, variance 1.5 sigma^2, kurtosis as above");
        check(std::fabs(impact_constant(0.5) - 1.0) < 1e-12 && std::fabs(impact_constant(0.25) - 1.1547) < 1e-4,
              "eq. 5: Y = 1 at lambda 1/2, 1.15 at 1/4 (\"of order unity\", Toth et al. 2011)");
        check(std::fabs(impact(0.01 * 1e6, 1e6, 0.02, 0.5) - 0.002) < 1e-15 && impact(-1e4, 1e6, 0.02, 0.5) < 0.0,
              "a 1 % of volume order at 2 % daily vol moves the price 0.2 %, signed");
        check(std::fabs(impact_variance(0.02, 0.5, 1e4, 1e6) - 0.0004 * (0.5 + 0.02)) < 1e-15,
              "eq. 9: Var = sigma^2 T + sigma^2 Q / (V T)");
        check(std::fabs(smile_vol(0.15, 0.0, 0.1) - 0.15) < 1e-15 && std::fabs(smile_vol(0.15, 0.1, 0.1) - std::sqrt(0.0225 + 0.05)) < 1e-15,
              "eq. 8: the smile's floor is sigma; a 10 % move over 0.1 y adds 0.05 to the variance");

        // a smile drawn from (8) is fitted back exactly
        std::vector<double> k, iv;
        for (double x = -0.08; x <= 0.0801; x += 0.01) { k.push_back(x); iv.push_back(smile_vol(0.14, x, 30.0 / 365.0)); }
        const auto f = fit_smile(k, iv, 30.0 / 365.0);
        check(std::fabs(f.sigma - 0.14) < 1e-9 && f.rmse_orrell < 1e-9 && std::fabs(f.b - 365.0 / 60.0) < 1e-6,
              "fit_smile recovers sigma and the free slope b = 1/(2T) from an Orrell smile");
    }

    // ---- NIFTY daily closes -------------------------------------------------
    if (argc > 1) {
        std::ifstream in(argv[1]);
        std::vector<double> lp;
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            std::stringstream ss(line);
            std::string f[5];
            for (auto& v : f) std::getline(ss, v, ',');
            if (f[4].empty() || f[0].substr(0, 10) < "2000-01-01") continue;
            lp.push_back(std::log(std::stod(f[4])));
        }
        if (lp.size() < 2000) {
            std::printf("    (NIFTY file too short: skipped)\n");
        } else {
            // Figure 1: the mix against a normal and a Student-t, horizons 1-40 sessions.
            bool beats_normal = true, t_beats_mix = true, tails_thin = true;
            for (const std::size_t T : {1u, 5u, 10u, 20u, 40u}) {
                std::vector<double> x;
                for (std::size_t i = 0; i + T < lp.size(); i += T) x.push_back(lp[i + T] - lp[i]);
                double m = 0;
                for (double a : x) m += a;
                m /= static_cast<double>(x.size());
                double v = 0, k4 = 0;
                for (double& a : x) { a -= m; v += a * a; k4 += a * a * a * a; }
                v /= static_cast<double>(x.size());
                k4 = k4 / static_cast<double>(x.size()) / (v * v);
                const double sg = ground_vol(std::sqrt(v), 0.25);
                double lg = 0, lq = 0, lt = -1e300;
                for (double a : x) { lg += gauss_ll(a, v); lq += std::log(density(a, sg, 0.25)); }
                for (const double nu : {3.0, 3.5, 4.0, 5.0, 6.0, 8.0, 10.0}) {
                    double l = 0;
                    for (double a : x) l += student_ll(a, v, nu);
                    lt = std::max(lt, l);
                }
                const double dn = static_cast<double>(x.size());
                std::printf("    %2zu sessions: n %4zu kurtosis %5.2f  log-lik/obs normal %.4f  Orrell %.4f  Student-t %.4f\n", T,
                            x.size(), k4, lg / dn, lq / dn, lt / dn);
                beats_normal = beats_normal && lq > lg;
                t_beats_mix = t_beats_mix && lt > lq;
                tails_thin = tails_thin && k4 > kurtosis(0.25);
            }
            check(beats_normal, "NIFTY: the mix fits better than a normal at every horizon");
            check(tails_thin && t_beats_mix,
                  "but its tails are too thin: NIFTY's kurtosis is above 4.33 everywhere and a Student-t fits better");

            // Eq. 8: realised vol in 20-session windows against the window's move.
            const std::size_t T = 20;
            std::vector<std::pair<double, double>> w;   // |move| / sqrt(T years), realised variance (annual)
            for (std::size_t i = 0; i + T < lp.size(); i += T) {
                double rv = 0;
                for (std::size_t j = i; j < i + T; ++j) rv += (lp[j + 1] - lp[j]) * (lp[j + 1] - lp[j]);
                w.push_back({std::fabs(lp[i + T] - lp[i]) / std::sqrt(T / 252.0), rv / T * 252.0});
            }
            std::sort(w.begin(), w.end());
            const std::size_t q = w.size() / 5;
            double floor_var = 0;
            for (std::size_t i = 0; i < q; ++i) floor_var += w[i].second;
            floor_var /= static_cast<double>(q);
            double worst = 0;
            for (std::size_t b = 1; b < 5; ++b) {
                double z = 0, v = 0;
                for (std::size_t i = b * q; i < (b + 1) * q; ++i) { z += w[i].first; v += w[i].second; }
                z /= static_cast<double>(q);
                v /= static_cast<double>(q);
                const double pred = std::sqrt(floor_var + z * z / 2.0);   // (8) with T = 1 in units of sqrt(T)
                std::printf("    quintile %zu of |move|: realised vol %.3f, eq. 8 %.3f\n", b + 1, std::sqrt(v), pred);
                worst = std::max(worst, std::fabs(pred / std::sqrt(v) - 1.0));
            }
            check(worst < 0.15, "eq. 8 on NIFTY: from the calmest fifth's vol, every other fifth within 15 %");
        }
    } else {
        std::printf("    (no NIFTY file given: real-data check skipped)\n");
    }

    std::printf("Orrell quantum oscillator: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
