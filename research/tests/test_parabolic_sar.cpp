// analytics/tests/test_parabolic_sar.cpp -- Wilder's Parabolic SAR, the
// Fauza & Noviaty (2022) table re-added, and PSAR / moving average / buy-and-
// hold on NIFTY with costs.
//
// Optional argument: a daily CSV (time,open,high,low,close,...), normally
// dataset/spot/nifty/1d/all.csv.

#include <research/models/parabolic_sar.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

struct Book { double log_ret = 0.0; int trades = 0, wins = 0; };

/// Long/flat on `want[i]` decided at bar i's close, held over bar i+1;
/// `cost` per side as a fraction.
Book run(const std::vector<double>& close, const std::vector<int>& want, double cost) {
    Book b;
    int pos = 0;
    double entry = 0.0;
    for (std::size_t i = 0; i + 1 < close.size(); ++i) {
        if (want[i] != pos) {
            b.log_ret += std::log1p(-cost);
            if (want[i] == 1) { entry = close[i]; }
            else { ++b.trades; b.wins += close[i] > entry; }
            pos = want[i];
        }
        if (pos == 1) { b.log_ret += std::log(close[i + 1] / close[i]); }
    }
    if (pos == 1) { ++b.trades; b.wins += close.back() > entry; }
    return b;
}

} // namespace

int main(int argc, char** argv) {
    using namespace altair;
    std::printf("Parabolic SAR\n");

    // ---- the rule ----------------------------------------------------------
    {
        // A steady rise, then a break.
        std::vector<double> hi, lo;
        for (int i = 0; i < 30; ++i) { hi.push_back(101.0 + i); lo.push_back(99.0 + i); }
        for (int i = 0; i < 5; ++i) { hi.push_back(120.0 - 3 * i); lo.push_back(116.0 - 3 * i); }
        const auto s = parabolic_sar(hi, lo);
        bool below = true, rising = true;
        for (std::size_t i = 2; i < 30; ++i) {
            below = below && s.sar[i] <= lo[i - 1] && s.sar[i] <= lo[i - 2];
            rising = rising && s.sar[i] >= s.sar[i - 1];
        }
        check(below && rising && s.trend[29] == 1,
              "in a rise the stop climbs under the last two lows and the trend stays long");
        // AF: 0.02 rising by 0.02 at every new high, capped at 0.2, so late
        // in the rise the stop closes 20% of the gap to the high each bar.
        const double ep = hi[28], gap = ep - s.sar[28];
        check(std::fabs((s.sar[29] - s.sar[28]) - 0.2 * gap) < 1e-9,
              "acceleration reaches the 0.20 cap");
        int flip = -1;
        for (std::size_t i = 30; i < hi.size(); ++i) if (s.trend[i] == -1) { flip = static_cast<int>(i); break; }
        check(flip >= 30 && s.sar[static_cast<std::size_t>(flip) + 1] == ep + 1.0,
              "a low through the stop reverses to short, and the next stop is the old extreme");
    }

    // ---- the paper's CNN table, re-added -----------------------------------
    {
        // Table 3's 42 trade percentages (row 26 is printed twice; counted once).
        const double pct[] = {0.78, -0.40, -1.65, 8.66, 2.42, 2.53, -2.03, 0.83, 1.22, -4.92,
                              1.31, -0.42, 2.16, -1.76, -0.45, -5.50, 10.58, 0.89, -2.64, -4.55,
                              0.54, 3.28, 2.08, -2.03, 2.02, 6.90, 1.43, -0.92, 0.47, 0.49,
                              -0.49, 2.87, 5.26, 0.00, -0.71, -0.36, 0.36, 1.09, 0.37, -1.46,
                              -0.75, -0.37};
        double sum = 0.0, comp = 1.0, net = 1.0;
        for (double p : pct) {
            sum += p;
            comp *= 1.0 + p / 100.0;
            net *= (1.0 + p / 100.0) * (1.0 - 0.0015) * (1.0 - 0.0025);
        }
        std::printf("    CNN table: sum %.2f %% (paper 27.14), compounded %.2f %%, "
                    "after 0.15 %% buy + 0.25 %% sell %.2f %%\n",
                    sum, 100.0 * (comp - 1.0), 100.0 * (net - 1.0));
        check(std::fabs(sum - 27.13) < 0.02,
              "the paper's 27.14 % is the plain sum of its 42 trade percentages");
        check(net - 1.0 < 0.5 * (comp - 1.0),
              "and 42 round trips at typical Indonesian costs remove more than half of it");
    }

    // ---- NIFTY -------------------------------------------------------------
    if (argc > 1) {
        std::ifstream in(argv[1]);
        std::vector<double> hi, lo, cl;
        std::vector<std::string> dates;
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            std::stringstream ss(line);
            std::string f[5];
            for (auto& v : f) std::getline(ss, v, ',');
            const std::string d = f[0].substr(0, 10);
            if (d < "2005-01-01" || f[4].empty()) continue;
            dates.push_back(d);
            hi.push_back(std::stod(f[2])); lo.push_back(std::stod(f[3])); cl.push_back(std::stod(f[4]));
        }
        if (cl.size() > 2000) {
            const auto s = parabolic_sar(hi, lo);
            std::vector<int> want_sar(cl.size()), want_ma(cl.size(), 0), hold(cl.size(), 1);
            for (std::size_t i = 0; i < cl.size(); ++i) want_sar[i] = s.trend[i] > 0 ? 1 : 0;
            double acc = 0.0;
            for (std::size_t i = 0; i < cl.size(); ++i) {
                acc += cl[i];
                if (i >= 20) acc -= cl[i - 20];
                if (i >= 19) want_ma[i] = cl[i] > acc / 20.0 ? 1 : 0;
            }
            const double cost = 0.0003;   // NIFTY futures, per side
            const Book bs = run(cl, want_sar, cost), bm = run(cl, want_ma, cost), bh = run(cl, hold, cost);
            const auto pct = [](const Book& b) { return 100.0 * (std::exp(b.log_ret) - 1.0); };
            std::printf("    NIFTY %s..%s, long/flat, 3 bp a side:\n", dates.front().c_str(), dates.back().c_str());
            std::printf("      PSAR      %+8.0f %%  %4d trades, %4.1f %% won\n", pct(bs), bs.trades,
                        100.0 * bs.wins / std::max(1, bs.trades));
            std::printf("      20-day MA %+8.0f %%  %4d trades, %4.1f %% won\n", pct(bm), bm.trades,
                        100.0 * bm.wins / std::max(1, bm.trades));
            std::printf("      buy-hold  %+8.0f %%\n", pct(bh));
            check(std::isfinite(bs.log_ret) && std::isfinite(bm.log_ret) && bs.trades > 50,
                  "PSAR and the moving average run on 20 years of NIFTY (the numbers above "
                  "are the result)");
        } else {
            std::printf("    (NIFTY file too short: skipped)\n");
        }
    } else {
        std::printf("    (no NIFTY file given: real-data run skipped)\n");
    }

    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
