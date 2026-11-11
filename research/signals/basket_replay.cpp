// research/signals/basket_replay.cpp -- run the live basket-alpha code (live/basket_alpha.hpp) over
// Drive tick prints, print by print, on days the fit never saw, and score it.
//
//   g++ -std=c++23 -O2 -I. -Icore research/signals/basket_replay.cpp -o basket_replay
//   python3 research/signals/replay_events.py ... | ./basket_replay config.csv [cost_bp]
//
// Input on stdin, in time order: "D,YYYYMMDD" starts a day; "sec,j,price,qty" is a print
// (sec = seconds since midnight; j = -1 the future, else the stock's index in the config).
// Every 5 s from 09:20 to 15:15 (as the live model): both forecasts, scored against the future's
// last price 1 s later and h s after that. Trades exactly as the live rule: one lot when
// |f60| > cost + 1 bp, entered at the first future print after the signal second, out 60 s later.
// Also prints what a looser threshold would have done (to show where the edge stops paying).
#include <live/basket_alpha.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace altair::live;

namespace {
struct Row { long long day, t; double f10, f60; };
struct Corr {
    double n = 0, sp = 0, sy = 0, spp = 0, syy = 0, spy = 0, se = 0, sy2 = 0;
    void add(double p, double y) { n += 1; sp += p; sy += y; spp += p * p; syy += y * y; spy += p * y; se += (y - p) * (y - p); sy2 += y * y; }
    [[nodiscard]] double ic() const { const double c = spy / n - sp / n * sy / n; return c / std::sqrt((spp / n - sp * sp / n / n) * (syy / n - sy * sy / n / n)); }
    [[nodiscard]] double r2() const { return 1.0 - se / sy2; }
};
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: basket_replay CONFIG.csv [cost_bp] < events\n"); return 2; }
    const auto p = read_basket_alpha(argv[1]);
    if (!p.ok) { std::fprintf(stderr, "config: %s\n", p.why.c_str()); return 2; }
    const double cost_bp = argc > 2 ? std::atof(argv[2]) : 5.6;
    std::vector<double> betas;
    for (const auto& b : p.betas) betas.push_back(b.second);

    // One day at a time: keep the future's per-second last price to score afterwards.
    std::map<long long, Corr> ic10_day, ic60_day;
    Corr all10, all60;
    const std::vector<double> thresholds{0.0, 1.0, 2.0, 3.0, cost_bp + 1.0};
    std::vector<double> tr_n(thresholds.size()), tr_g(thresholds.size());
    long long day = 0;
    BasketAlphaState st(betas.size(), betas, p.ew_half_s);
    std::vector<double> fpx(86400, std::nan(""));
    std::vector<Row> rows;
    long long last_sample = -1;

    auto finish_day = [&]() {
        if (day == 0) return;
        for (long long s = 1; s < 86400; ++s) if (!std::isfinite(fpx[s]) && std::isfinite(fpx[s - 1])) fpx[s] = fpx[s - 1];
        Corr& c10 = ic10_day[day];
        Corr& c60 = ic60_day[day];
        for (const auto& r : rows) {
            const double a = fpx[r.t + 1];
            if (!std::isfinite(a)) continue;
            const double y10 = std::log(fpx[r.t + 11] / a) * 1e4, y60 = std::log(fpx[r.t + 61] / a) * 1e4;
            if (std::isfinite(y10)) { c10.add(r.f10, y10); all10.add(r.f10, y10); }
            if (std::isfinite(y60)) { c60.add(r.f60, y60); all60.add(r.f60, y60); }
            // trade rule: enter at the price 1 s later, out 60 s after that; one position at a time
        }
        for (std::size_t k = 0; k < thresholds.size(); ++k) {
            long long busy_until = -1;
            for (const auto& r : rows) {
                if (r.t <= busy_until || !(std::fabs(r.f60) > thresholds[k])) continue;
                const double a = fpx[r.t + 1], b = fpx[r.t + 61];
                if (!std::isfinite(a) || !std::isfinite(b)) continue;
                tr_n[k] += 1;
                tr_g[k] += (r.f60 > 0 ? 1.0 : -1.0) * std::log(b / a) * 1e4;
                busy_until = r.t + 61;
            }
        }
        rows.clear();
        std::fill(fpx.begin(), fpx.end(), std::nan(""));
    };

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (line[0] == 'D') {
            finish_day();
            day = std::atoll(line.c_str() + 2);
            st = BasketAlphaState(betas.size(), betas, p.ew_half_s);
            last_sample = -1;
            continue;
        }
        long long sec = 0; int j = 0; double px = 0, q = 0;
        if (std::sscanf(line.c_str(), "%lld,%d,%lf,%lf", &sec, &j, &px, &q) != 4) continue;
        if (j < 0) { st.on_future(sec, px, q); if (sec >= 0 && sec < 86400) fpx[sec] = px; }
        else st.on_stock(sec, static_cast<std::size_t>(j), px);
        const long long t = st.last_closed_sec();
        if (t < 0 || t == last_sample || t % 5 != 0) continue;
        last_sample = t;
        if (t < 9 * 3600 + 20 * 60 || t >= 15 * 3600 + 15 * 60) continue;
        std::array<double, kBasketFeatures> x{};
        if (!st.features(x)) continue;
        rows.push_back({day, t, basket_forecast(p, 0, x), basket_forecast(p, 1, x)});
    }
    finish_day();

    int pos10 = 0, pos60 = 0;
    for (const auto& [d, c] : ic10_day) pos10 += c.n > 50 && c.ic() > 0;
    for (const auto& [d, c] : ic60_day) pos60 += c.n > 50 && c.ic() > 0;
    std::printf("days %zu\n", ic10_day.size());
    std::printf("10 s: OOS R2 %+.3f%%, IC %+.4f, IC>0 on %d of %zu days, n %.0f\n", all10.r2() * 100, all10.ic(), pos10, ic10_day.size(), all10.n);
    std::printf("60 s: OOS R2 %+.3f%%, IC %+.4f, IC>0 on %d of %zu days, n %.0f\n", all60.r2() * 100, all60.ic(), pos60, ic60_day.size(), all60.n);
    std::printf("trades (60 s hold, cost %.1f bp round trip):\n", cost_bp);
    for (std::size_t k = 0; k < thresholds.size(); ++k) {
        const double g = tr_n[k] > 0 ? tr_g[k] / tr_n[k] : std::nan("");
        std::printf("  |f60| > %4.1f bp: %6.0f trades, gross %+6.2f bp, net %+6.2f bp a trade%s\n", thresholds[k], tr_n[k], g, g - cost_bp,
                    k + 1 == thresholds.size() ? "   <- the live rule" : "");
    }
    return 0;
}
