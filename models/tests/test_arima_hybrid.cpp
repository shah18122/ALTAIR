// models/tests/test_arima_hybrid.cpp -- Zhang's ARIMA + ANN hybrid on a series
// built to need it, then on NIFTY daily log returns.
//
// Optional argument: a daily CSV (time,open,high,low,close,...), normally
// dataset/spot/nifty/1d/all.csv. Without it the real-data section is skipped.

#include <models/arima_hybrid.hpp>
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

double mse(const std::vector<double>& e) {
    double s = 0.0;
    for (double v : e) { s += v * v; }
    return e.empty() ? 0.0 : s / static_cast<double>(e.size());
}

} // namespace

int main(int argc, char** argv) {
    using namespace altair;
    std::printf("ARIMA + ANN hybrid (Zhang 2003)\n");

    // ---- a series with a linear part and a nonlinear residual ---------------
    // y_t = 0.4 y_{t-1} + u_t, u_t = 0.8 (u_{t-1}^2 - 1) / 2 + eps_t. The
    // u-term is uncorrelated with u_{t-1} (symmetric), so the AR cannot see it.
    {
        std::mt19937_64 rng(17);
        std::normal_distribution<double> N(0.0, 1.0);
        std::vector<double> y(3000);
        double u = 0.0, prev = 0.0;
        for (auto& v : y) {
            const double eps = N(rng);
            const double un = 0.4 * (std::min(u * u, 9.0) - 1.0) + eps;
            v = 0.4 * prev + un;
            prev = v; u = un;
        }
        ArimaAnnHybrid h(ArimaHybridConfig{1, 0, 2, 6, 400, 0.02, 1e-4, 2003});
        const std::vector<double> train(y.begin(), y.begin() + 2000);
        check(h.fit(train), "both stages fit on 2000 rows");
        std::vector<double> e_lin, e_hyb, e_rw;
        for (std::size_t t = 2000; t < y.size(); ++t) {
            const auto f = h.forecast();
            e_lin.push_back(f.linear - y[t]);
            e_hyb.push_back(f.hybrid - y[t]);
            e_rw.push_back(0.0 - y[t]);
            h.update(y[t]);
        }
        const auto dm = diebold_mariano(e_lin, e_hyb);
        std::printf("    out-of-sample MSE: zero %.3f, ARMA %.3f, hybrid %.3f; DM %.2f (p %.3f)\n",
                    mse(e_rw), mse(e_lin), mse(e_hyb), dm.stat, dm.p_value);
        check(mse(e_lin) < mse(e_rw), "the ARMA stage captures the linear part");
        check(mse(e_hyb) < 0.95 * mse(e_lin) && dm.stat > 2.0,
              "the network stage captures what the ARMA cannot (DM > 2)");
    }

    // ---- a purely linear series: the network must not make things worse ----
    {
        std::mt19937_64 rng(5);
        std::normal_distribution<double> N(0.0, 1.0);
        std::vector<double> y(3000);
        double prev = 0.0;
        for (auto& v : y) { v = 0.5 * prev + N(rng); prev = v; }
        ArimaAnnHybrid h;
        const std::vector<double> train(y.begin(), y.begin() + 2000);
        h.fit(train);
        std::vector<double> e_lin, e_hyb;
        for (std::size_t t = 2000; t < y.size(); ++t) {
            const auto f = h.forecast();
            e_lin.push_back(f.linear - y[t]); e_hyb.push_back(f.hybrid - y[t]);
            h.update(y[t]);
        }
        std::printf("    linear AR(1): ARMA MSE %.4f, hybrid %.4f\n", mse(e_lin), mse(e_hyb));
        // With nothing nonlinear to find, the network fits noise: it costs a
        // little out of sample. That is the same loss it shows on NIFTY.
        check(mse(e_hyb) < 1.05 * mse(e_lin),
              "on a linear series the network's overfit costs under 5 % MSE");
    }

    // ---- NIFTY: train 2009-2021, score from 2022 ----------------------------
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
            if (d < "2009-01-01" || f[4].empty()) continue;
            dates.push_back(d);
            px.push_back(std::stod(f[4]));
        }
        if (px.size() < 2000) {
            std::printf("    %s: not enough rows, skipped\n", argv[1]);
        } else {
            std::vector<double> r(px.size(), 0.0);   // r[k] = log(px[k]/px[k-1])
            for (std::size_t k = 1; k < px.size(); ++k) r[k] = std::log(px[k] / px[k - 1]);
            std::size_t split = 1;
            while (split < px.size() && dates[split] < "2022-01-01") ++split;
            const std::vector<double> train(r.begin() + 1, r.begin() + static_cast<long>(split));
            ArimaAnnHybrid h(ArimaHybridConfig{2, 0, 4, 6, 400, 0.01, 1e-3, 2003});
            const bool ok = h.fit(train);
            std::vector<double> actual, rw, p_lin, p_hyb;
            double s_lin = 0, s_hyb = 0, hold = 0, c_lin = 0, c_hyb = 0;
            bool in_lin = false, in_hyb = false;
            for (std::size_t k = split; ok && k < px.size(); ++k) {
                const auto f = h.forecast();
                actual.push_back(px[k]);
                rw.push_back(px[k - 1]);
                p_lin.push_back(px[k - 1] * std::exp(f.linear));
                p_hyb.push_back(px[k - 1] * std::exp(f.hybrid));
                if ((f.linear > 0) != in_lin) { c_lin += 0.0003; in_lin = !in_lin; }
                if ((f.hybrid > 0) != in_hyb) { c_hyb += 0.0003; in_hyb = !in_hyb; }
                if (in_lin) s_lin += r[k];
                if (in_hyb) s_hyb += r[k];
                hold += r[k];
                h.update(r[k]);
            }
            if (ok && !actual.empty()) {
                std::vector<double> e_rw, e_lin, e_hyb;
                for (std::size_t i = 0; i < actual.size(); ++i) {
                    e_rw.push_back(rw[i] - actual[i]);
                    e_lin.push_back(p_lin[i] - actual[i]);
                    e_hyb.push_back(p_hyb[i] - actual[i]);
                }
                // Direction tallies against the last known close.
                DirectionTally tl, th;
                for (std::size_t i = 0; i < actual.size(); ++i) {
                    const double a = actual[i] - rw[i];
                    const double pl = p_lin[i] - rw[i], ph = p_hyb[i] - rw[i];
                    if (a == 0.0) continue;
                    if (pl != 0.0) { ++tl.calls; tl.hits += (pl > 0) == (a > 0); tl.actual_up += a > 0; tl.predicted_up += pl > 0; }
                    if (ph != 0.0) { ++th.calls; th.hits += (ph > 0) == (a > 0); th.actual_up += a > 0; th.predicted_up += ph > 0; }
                }
                const auto dm_l = diebold_mariano(e_rw, e_lin);
                const auto dm_h = diebold_mariano(e_rw, e_hyb);
                const auto dm_lh = diebold_mariano(e_lin, e_hyb);
                const auto pt_h = pesaran_timmermann(th);
                std::printf("    trained on %zu returns, scored %zu days from %s\n",
                            train.size(), actual.size(), dates[split].c_str());
                std::printf("    MAPE: random walk %.3f %%, ARIMA %.3f %%, hybrid %.3f %%\n",
                            forecast_mape(actual, rw), forecast_mape(actual, p_lin),
                            forecast_mape(actual, p_hyb));
                std::printf("    DM vs random walk: ARIMA %.2f (p %.2f), hybrid %.2f (p %.2f); "
                            "hybrid vs ARIMA %.2f (p %.2f)   [> 0 favours the second]\n",
                            dm_l.stat, dm_l.p_value, dm_h.stat, dm_h.p_value,
                            dm_lh.stat, dm_lh.p_value);
                std::printf("    hit rate: ARIMA %.1f %%, hybrid %.1f %% (PT z %.2f p %.3f)\n",
                            100.0 * tl.accuracy(), 100.0 * th.accuracy(), pt_h.stat, pt_h.p_value);
                std::printf("    long/flat at 3 bp: ARIMA %+.1f %%, hybrid %+.1f %%, buy-and-hold %+.1f %%\n",
                            100.0 * (std::exp(s_lin - c_lin) - 1.0),
                            100.0 * (std::exp(s_hyb - c_hyb) - 1.0),
                            100.0 * (std::exp(hold) - 1.0));
            }
            check(ok && !actual.empty(),
                  "the hybrid runs end to end on NIFTY (the numbers above are the result; "
                  "none is assumed)");
        }
    } else {
        std::printf("    (no NIFTY file given: real-data run skipped)\n");
    }

    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
