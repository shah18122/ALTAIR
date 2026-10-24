// models/tests/test_anfis.cpp -- the neuro-fuzzy model, then Atsalakis &
// Valavanis's set-up re-run on NIFTY.
//
// Optional argument: a daily CSV (time,open,high,low,close,...), normally
// dataset/spot/nifty/1d/all.csv. Without it the real-data section is skipped.

#include <research/models/anfis.hpp>
#include <analytics/forecast_metrics.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
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

} // namespace

int main(int argc, char** argv) {
    using namespace altair;
    std::printf("ANFIS (first-order Sugeno)\n");

    // ---- it learns a smooth nonlinear surface ------------------------------
    {
        std::mt19937_64 rng(36);
        std::uniform_real_distribution<double> U(-2.0, 2.0);
        std::normal_distribution<double> N(0.0, 0.05);
        const auto f = [](double a, double b) { return std::sin(a) + 0.5 * b * b; };
        std::vector<std::vector<double>> x, xt;
        std::vector<double> y, yt;
        for (int i = 0; i < 600; ++i) {
            const double a = U(rng), b = U(rng);
            x.push_back({a, b}); y.push_back(f(a, b) + N(rng));
        }
        for (int i = 0; i < 400; ++i) {
            const double a = U(rng), b = U(rng);
            xt.push_back({a, b}); yt.push_back(f(a, b));
        }
        Anfis m(AnfisConfig{3, 50, 0.1, 1e-6});
        const double train = m.fit(x, y);
        // Linear least squares on the same rows, for scale.
        double sxx[3][3] = {}, sxy[3] = {};
        for (std::size_t i = 0; i < x.size(); ++i) {
            const double r[3] = {x[i][0], x[i][1], 1.0};
            for (int a = 0; a < 3; ++a) { sxy[a] += r[a] * y[i]; for (int b = 0; b < 3; ++b) sxx[a][b] += r[a] * r[b]; }
        }
        // x0, x1 are independent and centred, so the normal equations are
        // close to diagonal; solve them exactly by Cramer's rule.
        const auto det3 = [](double m3[3][3]) {
            return m3[0][0] * (m3[1][1] * m3[2][2] - m3[1][2] * m3[2][1])
                 - m3[0][1] * (m3[1][0] * m3[2][2] - m3[1][2] * m3[2][0])
                 + m3[0][2] * (m3[1][0] * m3[2][1] - m3[1][1] * m3[2][0]);
        };
        double beta[3];
        const double D = det3(sxx);
        for (int k = 0; k < 3; ++k) {
            double t[3][3];
            for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) t[a][b] = b == k ? sxy[a] : sxx[a][b];
            beta[k] = det3(t) / D;
        }
        double se_a = 0, se_l = 0;
        for (std::size_t i = 0; i < xt.size(); ++i) {
            const double ea = m.predict(xt[i]) - yt[i];
            const double el = beta[0] * xt[i][0] + beta[1] * xt[i][1] + beta[2] - yt[i];
            se_a += ea * ea; se_l += el * el;
        }
        const double rm_a = std::sqrt(se_a / 400.0), rm_l = std::sqrt(se_l / 400.0);
        std::printf("    sin(a) + b^2/2: train RMSE %.3f, test RMSE %.3f (linear %.3f), %zu rules\n",
                    train, rm_a, rm_l, m.rules());
        check(m.fitted() && m.rules() == 9, "two inputs x three functions = 9 rules");
        check(rm_a < 0.1 && rm_a < 0.2 * rm_l,
              "held-out error under 0.1, and a fifth of a linear fit's");
    }
    {
        Anfis m;
        check(m.fit({{1.0}, {2.0}}, {1.0, 2.0}) < 0.0 && !m.fitted(),
              "refuses fewer rows than parameters");
    }

    // ---- Atsalakis & Valavanis on NIFTY ------------------------------------
    //
    // Inputs at day k: the last two daily returns and the rate of change of
    // the 3-day average of the close (all known at the close of day k).
    // Target: day k+1's return. Their 27-rule, 100-epoch, step-0.1 model,
    // trained on 2009-2021, scored on 2022 - June 2025 (about 870 sessions;
    // the paper scored three windows of about 60). Their trading rule: long
    // when the forecast is up, flat otherwise, against buy-and-hold, here
    // with 3 bp a side for NIFTY futures.
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
            const std::string d = f[0].substr(0, 10);
            if (d < "2009-01-01" || d > "2025-06-30" || f[4].empty()) continue;
            dates.push_back(d);
            px.push_back(std::stod(f[4]));
        }
        if (px.size() < 2000) {
            std::printf("    %s: not enough rows, skipped\n", argv[1]);
        } else {
            std::vector<std::vector<double>> xtr, xte;
            std::vector<double> ytr, yte;
            std::vector<std::size_t> day_te;
            for (std::size_t k = 4; k + 1 < px.size(); ++k) {
                const double r0 = px[k] / px[k - 1] - 1.0, r1 = px[k - 1] / px[k - 2] - 1.0;
                const double sma = (px[k] + px[k - 1] + px[k - 2]) / 3.0;
                const double sma1 = (px[k - 1] + px[k - 2] + px[k - 3]) / 3.0;
                const std::vector<double> row{r0, r1, sma / sma1 - 1.0};
                const double target = px[k + 1] / px[k] - 1.0;
                if (dates[k + 1] < "2022-01-01") { xtr.push_back(row); ytr.push_back(target); }
                else { xte.push_back(row); yte.push_back(target); day_te.push_back(k); }
            }
            Anfis m(AnfisConfig{3, 100, 0.1, 1e-6});
            const double rmse = m.fit(xtr, ytr);
            std::vector<double> actual, pred, rw;
            double strat = 0.0, hold = 0.0, cost = 0.0;
            bool in_mkt = false;
            std::size_t longs = 0;
            for (std::size_t i = 0; i < xte.size(); ++i) {
                const std::size_t k = day_te[i];
                const double fc = m.predict(xte[i]);
                actual.push_back(px[k + 1]);
                pred.push_back(px[k] * (1.0 + fc));
                rw.push_back(px[k]);
                const bool want = fc > 0.0;
                if (want != in_mkt) { cost += 0.0003; in_mkt = want; }
                if (in_mkt) { strat += std::log1p(yte[i]); ++longs; }
                hold += std::log1p(yte[i]);
            }
            // Direction against the close the forecast was made at.
            DirectionTally t;
            for (std::size_t i = 0; i < pred.size(); ++i) {
                const double p = pred[i] - rw[i], a = actual[i] - rw[i];
                if (p == 0.0 || a == 0.0) continue;
                ++t.calls; t.hits += (p > 0) == (a > 0);
                t.actual_up += a > 0; t.predicted_up += p > 0;
            }
            const auto pt = pesaran_timmermann(t);
            std::vector<double> e_rw, e_m;
            for (std::size_t i = 0; i < actual.size(); ++i) {
                e_rw.push_back(rw[i] - actual[i]); e_m.push_back(pred[i] - actual[i]);
            }
            const auto dm = diebold_mariano(e_rw, e_m);
            std::printf("    trained on %zu days (RMSE %.4f), scored %zu days from %s\n",
                        xtr.size(), rmse, xte.size(), dates[day_te.front() + 1].c_str());
            std::printf("    hit rate %.1f %% (%zu/%zu; up-days %.1f %%), PT z %.2f p %.3f\n",
                        100.0 * t.accuracy(), t.hits, t.calls,
                        100.0 * static_cast<double>(t.actual_up) / static_cast<double>(t.calls),
                        pt.stat, pt.p_value);
            std::printf("    MAPE %.3f %% vs random walk %.3f %%, DM %.2f (p %.2f; > 0 favours ANFIS)\n",
                        forecast_mape(actual, pred), forecast_mape(actual, rw), dm.stat, dm.p_value);
            std::printf("    long/flat %+.1f %% after %.1f %% costs (long %zu of %zu days), "
                        "buy-and-hold %+.1f %%\n",
                        100.0 * (std::exp(strat - cost) - 1.0), 100.0 * cost, longs, xte.size(),
                        100.0 * (std::exp(hold) - 1.0));
            check(rmse > 0.0 && std::isfinite(pt.stat) && std::isfinite(dm.stat),
                  "the paper's set-up runs end to end on NIFTY (the numbers above are "
                  "the result; none is assumed)");
        }
    } else {
        std::printf("    (no NIFTY file given: real-data run skipped)\n");
    }

    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
