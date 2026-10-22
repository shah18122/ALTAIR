// analytics/tests/test_forecast_metrics.cpp -- forecast scores and the tests
// behind them, then the Charithra et al. (2026) NIFTY table re-checked.
//
// Optional argument: a daily CSV (time,open,high,low,close,...), normally
// dataset/spot/nifty/1d/all.csv. Without it the real-data section is skipped.

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

/// One-step random-walk forecasts: tomorrow = today. Row 0 has none, so the
/// scored series start at row 1.
struct Scored { std::vector<double> actual, predicted; };
Scored random_walk(const std::vector<double>& closes) {
    Scored s;
    for (std::size_t i = 1; i < closes.size(); ++i) {
        s.actual.push_back(closes[i]);
        s.predicted.push_back(closes[i - 1]);
    }
    return s;
}

} // namespace

int main(int argc, char** argv) {
    using namespace altair;
    std::printf("Forecast metrics\n");

    // ---- formulas ----------------------------------------------------------
    {
        const std::vector<double> a{100, 110, 99}, p{101, 108, 99};
        check(std::fabs(forecast_mape(a, p) - 100.0 * (0.01 + 2.0 / 110.0) / 3.0) < 1e-12,
              "MAPE is the mean of |error| / actual, in percent");
        check(std::fabs(forecast_rmse(a, p) - std::sqrt(5.0 / 3.0)) < 1e-12, "RMSE");
        const std::vector<double> c{100, 102, 101, 103}, f{0, 101, 103, 104};
        const auto t = forecast_direction(c, f);
        check(t.calls == 3 && t.hits == 2,
              "direction is scored against the last known close, not the forecast's "
              "own previous value (up/hit, up/miss, up/hit)");
    }

    // ---- the tests behave --------------------------------------------------
    {
        std::mt19937_64 rng(518);
        std::normal_distribution<double> N;
        std::vector<double> ea(2000), eb(2000), ec(2000);
        for (std::size_t i = 0; i < ea.size(); ++i) {
            ea[i] = N(rng); eb[i] = N(rng); ec[i] = 0.8 * N(rng);
        }
        const auto same = diebold_mariano(ea, eb);
        const auto better = diebold_mariano(ea, ec);
        std::printf("    DM equal %.2f (p %.2f), 20%% smaller errors %.2f (p %.1e)\n",
                    same.stat, same.p_value, better.stat, better.p_value);
        check(same.p_value > 0.05, "DM does not reject for two equally good forecasts");
        check(better.stat > 0.0 && better.p_value < 1e-6,
              "and finds the better one, sign positive for the second forecast");

        // Random calls against random outcomes: about half right, and PT
        // knows that 55 % is not luck only when there are enough calls.
        std::bernoulli_distribution coin(0.5), skill(0.55);
        DirectionTally luck, edge;
        for (int i = 0; i < 2000; ++i) {
            const bool up = coin(rng);
            const bool call_luck = coin(rng), call_edge = skill(rng) ? up : !up;
            ++luck.calls; ++edge.calls;
            luck.hits += call_luck == up; edge.hits += call_edge == up;
            luck.actual_up += up; edge.actual_up += up;
            luck.predicted_up += call_luck; edge.predicted_up += call_edge;
        }
        std::printf("    PT luck %.2f (p %.2f), 55%% skill %.2f (p %.1e)\n",
                    pesaran_timmermann(luck).stat, pesaran_timmermann(luck).p_value,
                    pesaran_timmermann(edge).stat, pesaran_timmermann(edge).p_value);
        check(pesaran_timmermann(luck).p_value > 0.01 && pesaran_timmermann(edge).p_value < 0.001,
              "Pesaran-Timmermann: coin flips are not skill, a 55% edge on 2,000 calls is");
    }

    // ---- the paper's own Table 4 -------------------------------------------
    //
    // The ten rows it prints (two runs of five days); the first row of each
    // run has no previous close, so eight forecasts are scored.
    {
        const std::vector<std::vector<double>> actual{
            {21741.9, 21665.8, 21517.35, 21658.6, 21710.8},
            {25044.35, 25244.75, 25549, 25637.8, 25517.05}};
        const std::vector<std::vector<double>> cnn{
            {21756.73242, 21773.54688, 21683.93945, 21528.99805, 21692.4043},
            {24971.25, 25088.84375, 25281.86523, 25602.07031, 25669.25586}};
        const std::vector<std::vector<double>> lstm{
            {21810.11133, 21824.08203, 21769.85938, 21646.10742, 21623.79297},
            {24955.95703, 24994.12109, 25103.87305, 25328.12891, 25510.0625}};
        std::vector<double> a, c, l, rw;
        for (std::size_t r = 0; r < 2; ++r) {
            for (std::size_t i = 1; i < 5; ++i) {
                a.push_back(actual[r][i]); c.push_back(cnn[r][i]);
                l.push_back(lstm[r][i]); rw.push_back(actual[r][i - 1]);
            }
        }
        const double m_cnn = forecast_mape(a, c), m_lstm = forecast_mape(a, l),
                     m_rw = forecast_mape(a, rw);
        std::printf("    Table 4, 8 rows: CNN %.3f %%  LSTM %.3f %%  random walk %.3f %%\n",
                    m_cnn, m_lstm, m_rw);
        check(std::fabs(m_cnn - 0.544) < 0.001 && std::fabs(m_rw - 0.592) < 0.001,
              "on the rows the paper prints, CNN is close to a tie with tomorrow = today");
        check(m_lstm > m_rw, "and the LSTM is worse than tomorrow = today");
        // Direction within each run only (the gap between runs is not a day).
        DirectionTally dc, dl;
        for (std::size_t r = 0; r < 2; ++r) {
            const auto tc = forecast_direction(actual[r], cnn[r]);
            const auto tl = forecast_direction(actual[r], lstm[r]);
            dc.hits += tc.hits; dc.calls += tc.calls;
            dl.hits += tl.hits; dl.calls += tl.calls;
        }
        std::printf("    direction: CNN %zu/%zu  LSTM %zu/%zu (paper: 71.2 %% and 69.5 %%)\n",
                    dc.hits, dc.calls, dl.hits, dl.calls);
        check(dl.hits == 2 && dl.calls == 8, "LSTM calls direction right on 2 of these 8");
    }

    // ---- leakage looks like skill ------------------------------------------
    //
    // A "forecast" that is a centred 3-day average of the closes (what
    // whole-series smoothing feeds a model) scores far better than any
    // honest forecast can, on a pure random walk with nothing to predict.
    {
        std::mt19937_64 rng(2026);
        std::normal_distribution<double> N(0.0, 0.009);
        std::vector<double> px{10000.0};
        for (int i = 0; i < 2000; ++i) px.push_back(px.back() * std::exp(N(rng)));
        const auto honest = random_walk(px);
        std::vector<double> a, leak;
        for (std::size_t i = 1; i + 1 < px.size(); ++i) {
            a.push_back(px[i]);
            leak.push_back((px[i - 1] + px[i] + px[i + 1]) / 3.0);
        }
        const double m_h = forecast_mape(honest.actual, honest.predicted);
        const double m_l = forecast_mape(a, leak);
        std::printf("    random walk: honest MAPE %.3f %%, centred-smoother 'forecast' %.3f %%\n",
                    m_h, m_l);
        check(std::fabs(m_h - 0.9 * std::sqrt(2.0 / std::acos(-1.0))) < 0.05,
              "an honest one-day MAPE is about sigma x sqrt(2/pi), the mean |return|");
        check(m_l < 0.7 * m_h, "a forecast that peeks at tomorrow beats it by 30% or more");
    }

    // ---- real NIFTY closes -------------------------------------------------
    if (argc > 1) {
        std::ifstream in(argv[1]);
        std::vector<double> closes;
        std::vector<std::string> dates;
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            std::stringstream ss(line);
            std::string f[5];
            for (auto& x : f) std::getline(ss, x, ',');
            const std::string d = f[0].substr(0, 10);
            if (d < "2017-01-01" || d > "2025-06-30" || f[4].empty()) continue;
            dates.push_back(d);
            closes.push_back(std::stod(f[4]));
        }
        if (closes.size() < 1000) {
            std::printf("    %s: not enough 2017-25 rows, skipped\n", argv[1]);
        } else {
            const auto all = random_walk(closes);
            const std::size_t k = closes.size() * 4 / 5;
            const std::vector<double> tail(closes.begin() + static_cast<long>(k) - 1, closes.end());
            const auto test = random_walk(tail);
            const double m_all = forecast_mape(all.actual, all.predicted);
            const double m_test = forecast_mape(test.actual, test.predicted);
            std::printf("    NIFTY %s..%s, %zu closes: random walk MAPE %.3f %% "
                        "(last fifth from %s: %.3f %%; the paper says 1.47 %%)\n",
                        dates.front().c_str(), dates.back().c_str(), closes.size(),
                        m_all, dates[k].c_str(), m_test);
            check(m_all < 0.8 && m_test < 0.8,
                  "on real NIFTY, tomorrow = today scores under 0.8 %: better than the "
                  "paper's LSTM and ANN, about level with its CNN");
        }
    } else {
        std::printf("    (no NIFTY file given: real-data check skipped)\n");
    }

    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
