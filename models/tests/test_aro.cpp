// models/tests/test_aro.cpp -- artificial rabbits optimisation on standard
// test functions against random search with the same budget, then Gülmez's
// set-up (20 past closes -> next close, 2018-2023, 80/20) re-run on NIFTY
// with ARO choosing the hyperparameters.
//
// Optional argument: a daily CSV (time,open,high,low,close,...), normally
// dataset/spot/nifty/1d/all.csv. Without it the real-data section is skipped.

#include <models/aro.hpp>
#include <analytics/forecast_metrics.hpp>

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

double random_search(const std::function<double(const std::vector<double>&)>& f,
                     const std::vector<double>& lo, const std::vector<double>& hi,
                     std::size_t budget, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    double best = 1e300;
    std::vector<double> x(lo.size());
    for (std::size_t e = 0; e < budget; ++e) {
        for (std::size_t k = 0; k < x.size(); ++k) x[k] = lo[k] + U(rng) * (hi[k] - lo[k]);
        best = std::min(best, f(x));
    }
    return best;
}

/// Ridge on rows (x, y), solved by Gaussian elimination. Returns weights
/// with the intercept last.
std::vector<double> ridge(const std::vector<std::vector<double>>& x, const std::vector<double>& y,
                          double lambda) {
    const std::size_t p = x.empty() ? 1 : x[0].size() + 1;
    std::vector<double> a(p * p, 0.0), b(p, 0.0), row(p);
    for (std::size_t i = 0; i < x.size(); ++i) {
        for (std::size_t k = 0; k + 1 < p; ++k) row[k] = x[i][k];
        row[p - 1] = 1.0;
        for (std::size_t r = 0; r < p; ++r) {
            b[r] += row[r] * y[i];
            for (std::size_t c = 0; c < p; ++c) a[r * p + c] += row[r] * row[c];
        }
    }
    for (std::size_t k = 0; k + 1 < p; ++k) a[k * p + k] += lambda * static_cast<double>(x.size());
    for (std::size_t c = 0; c < p; ++c) {
        std::size_t piv = c;
        for (std::size_t r = c + 1; r < p; ++r) if (std::fabs(a[r * p + c]) > std::fabs(a[piv * p + c])) piv = r;
        for (std::size_t k = 0; k < p; ++k) std::swap(a[c * p + k], a[piv * p + k]);
        std::swap(b[c], b[piv]);
        if (std::fabs(a[c * p + c]) < 1e-300) return std::vector<double>(p, 0.0);
        for (std::size_t r = c + 1; r < p; ++r) {
            const double m = a[r * p + c] / a[c * p + c];
            for (std::size_t k = c; k < p; ++k) a[r * p + k] -= m * a[c * p + k];
            b[r] -= m * b[c];
        }
    }
    for (std::size_t c = p; c-- > 0;) {
        double s = b[c];
        for (std::size_t k = c + 1; k < p; ++k) s -= a[c * p + k] * b[k];
        b[c] = s / a[c * p + c];
    }
    return b;
}

} // namespace

int main(int argc, char** argv) {
    using namespace altair;
    std::printf("Artificial rabbits optimisation (Wang et al. 2022, as in Gulmez 2023)\n");

    // ---- standard test functions, equal budgets -----------------------------
    const std::size_t d = 10;
    const auto sphere = [](const std::vector<double>& x) {
        double s = 0; for (double v : x) s += v * v; return s;
    };
    const auto rastrigin = [](const std::vector<double>& x) {
        double s = 10.0 * static_cast<double>(x.size());
        for (double v : x) s += v * v - 10.0 * std::cos(2.0 * std::numbers::pi * v);
        return s;
    };
    // The same sphere with its minimum moved to 2.5 in every coordinate,
    // inside a box that excludes 0: what a hyperparameter search looks like.
    const auto shifted = [](const std::vector<double>& x) {
        double s = 0; for (double v : x) s += (v - 2.5) * (v - 2.5); return s;
    };
    {
        const std::vector<double> lo(d, -5.12), hi(d, 5.12);
        AroConfig cfg{30, 100, 7};
        const auto a1 = aro_minimise(sphere, lo, hi, cfg);
        const auto a2 = aro_minimise(rastrigin, lo, hi, cfg);
        const double r1 = random_search(sphere, lo, hi, a1.evaluations, 7);
        const double r2 = random_search(rastrigin, lo, hi, a2.evaluations, 7);
        std::printf("    d=%zu, %zu evaluations each\n", d, a1.evaluations);
        std::printf("    sphere:    ARO %.3g, random search %.3g\n", a1.value, r1);
        std::printf("    rastrigin: ARO %.3g, random search %.3g\n", a2.value, r2);
        check(a1.evaluations == 30 + 30 * 100, "n + n*T evaluations");
        check(a1.value < 1e-6 && a1.value < 1e-4 * r1, "sphere: ARO converges, random search does not");
        check(a2.value < 0.5 * r2, "rastrigin: ARO at least halves random search's best");
        const std::vector<double> lo2(d, 1.0), hi2(d, 4.0);
        const auto a3 = aro_minimise(shifted, lo2, hi2, cfg);
        const double r3 = random_search(shifted, lo2, hi2, a3.evaluations, 7);
        const auto shifted_rastrigin = [&](const std::vector<double>& x) {
            std::vector<double> z(x.size());
            for (std::size_t k = 0; k < x.size(); ++k) z[k] = x[k] - 2.5;
            return rastrigin(z);
        };
        const auto a4 = aro_minimise(shifted_rastrigin, lo2, hi2, cfg);
        const double r4 = random_search(shifted_rastrigin, lo2, hi2, a4.evaluations, 7);
        std::printf("    same functions, optimum moved to 2.5 in [1, 4]:\n");
        std::printf("    sphere:    ARO %.3g, random search %.3g\n", a3.value, r3);
        std::printf("    rastrigin: ARO %.3g, random search %.3g\n", a4.value, r4);
        check(a3.value < 0.05 * r3 && a4.value < 0.5 * r4,
              "away from 0 ARO still beats random search");
        check(a3.value > 1e6 * a1.value,
              "but the exact convergence at 0 is the burrow's origin bias: moved, "
              "the same sphere is solved a million times less precisely");
    }

    // ---- Gulmez's set-up on NIFTY, ARO choosing the hyperparameters ---------
    if (argc > 1) {
        std::ifstream in(argv[1]);
        std::vector<double> px;
        std::vector<std::string> dates;
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            std::stringstream ss(line);
            std::string f[5];
            for (auto& v : f) std::getline(ss, v, ',');
            const std::string dt = f[0].substr(0, 10);
            if (dt < "2018-01-01" || dt >= "2023-01-01" || f[4].empty()) continue;
            dates.push_back(dt);
            px.push_back(std::stod(f[4]));
        }
        if (px.size() < 1000) {
            std::printf("    %s: not enough rows, skipped\n", argv[1]);
        } else {
            // Target index t+1 predicted from closes up to t. 80/20 split of
            // the targets; the last 20 % of the training part validates.
            const std::size_t first = 20, n = px.size();
            const std::size_t split = first + (n - first) * 8 / 10;
            const std::size_t val = first + (split - first) * 8 / 10;
            // Hyperparameters on boxes that exclude 0 (see aro.hpp):
            // h[0] in [1, 2] -> window 1..20 closes; h[1] in [1, 2] -> log10 lambda -6..2.
            const auto decode = [](const std::vector<double>& h, std::size_t& w, double& lam) {
                w = 1 + static_cast<std::size_t>(std::lround((h[0] - 1.0) * 19.0));
                lam = std::pow(10.0, -6.0 + 8.0 * (h[1] - 1.0));
            };
            // Features: past returns relative to the last close; target: next return.
            const auto rows = [&](std::size_t w, std::size_t a, std::size_t b,
                                  std::vector<std::vector<double>>& x, std::vector<double>& y) {
                x.clear(); y.clear();
                for (std::size_t t = a; t < b; ++t) {   // predicts px[t] from px[..t-1]
                    std::vector<double> r(w);
                    for (std::size_t k = 0; k < w; ++k) r[k] = px[t - 1 - k] / px[t - 2 - k] - 1.0;
                    x.push_back(std::move(r));
                    y.push_back(px[t] / px[t - 1] - 1.0);
                }
            };
            const auto mape_of = [&](std::size_t w, double lam, std::size_t fit_a, std::size_t fit_b,
                                     std::size_t a, std::size_t b, std::vector<double>* pred) {
                std::vector<std::vector<double>> xf, xs; std::vector<double> yf, ys;
                rows(w, fit_a, fit_b, xf, yf);
                rows(w, a, b, xs, ys);
                const auto beta = ridge(xf, yf, lam);
                std::vector<double> act, p;
                for (std::size_t i = 0; i < xs.size(); ++i) {
                    double r = beta.back();
                    for (std::size_t k = 0; k < w; ++k) r += beta[k] * xs[i][k];
                    act.push_back(px[a + i]);
                    p.push_back(px[a + i - 1] * (1.0 + r));
                }
                if (pred) *pred = p;
                return forecast_mape(act, p);
            };
            const auto fitness = [&](const std::vector<double>& h) {
                std::size_t w; double lam; decode(h, w, lam);
                return mape_of(w, lam, first + 1, val, val, split, nullptr);
            };
            const auto best = aro_minimise(fitness, {1.0, 1.0}, {2.0, 2.0}, AroConfig{20, 20, 2023});
            std::size_t w; double lam; decode(best.best, w, lam);
            std::vector<double> pred;
            const double m_model = mape_of(w, lam, first + 1, split, split, n, &pred);
            std::vector<double> act(px.begin() + static_cast<long>(split), px.end());
            std::vector<double> rw(px.begin() + static_cast<long>(split) - 1, px.end() - 1);
            std::vector<double> e_rw, e_m;
            for (std::size_t i = 0; i < act.size(); ++i) {
                e_rw.push_back(rw[i] - act[i]); e_m.push_back(pred[i] - act[i]);
            }
            const auto dm = diebold_mariano(e_rw, e_m);
            const auto dir = forecast_direction(act, pred);   // vs act[i-1]
            std::printf("    NIFTY %s..%s, test from %s (%zu days)\n", dates.front().c_str(),
                        dates.back().c_str(), dates[split].c_str(), act.size());
            std::printf("    ARO picked window %zu, lambda %.2g (validation MAPE %.3f %%, %zu fits)\n",
                        w, lam, best.value, best.evaluations);
            std::printf("    test MAPE: tuned model %.3f %%, random walk %.3f %%; DM %.2f (p %.2f, > 0 favours the model)\n",
                        m_model, forecast_mape(act, rw), dm.stat, dm.p_value);
            std::printf("    direction %.1f %% of %zu calls\n", 100.0 * dir.accuracy(), dir.calls);
            check(best.evaluations == 20 + 20 * 20 && std::isfinite(m_model) && std::isfinite(dm.stat),
                  "the set-up runs end to end on NIFTY (numbers above are the result; none assumed)");
        }
    } else {
        std::printf("    (no NIFTY file given: real-data run skipped)\n");
    }

    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
