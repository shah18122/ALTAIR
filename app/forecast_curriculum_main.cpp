// app/forecast_curriculum_main.cpp -- altair_forecast_curriculum.
//
// Walks every forecaster in the Model Atlas that can make a next-bar call
// through the doubling curriculum (models/curriculum.hpp) on the dataset:
// learn 3 days, forecast the next 3, keep the record, refit on 6, forecast 6,
// ... to the end of the data. Two questions per track:
//   direction  which way will the next close go? (models/curriculum.hpp)
//   range      will it land inside a band that is right 80 % of the time,
//              and how narrow can that band be? (models/band_curriculum.hpp)
// Tracks: NIFTY, BANKNIFTY and INDIA VIX at 1m, 5m, 15m, 60m and 1d, NIFTY
// futures daily, and the daily/hourly index tracks again with the VIX
// model's forecast as an input.
//
// Offline and read-only: it reads dataset/ and writes
//   <out>/forecast_curriculum.xlsx   Summary, Bands, Frontier, Atlas coverage,
//                                    Data, Method, one sheet per track
//   <out>/forecast_curriculum.txt    the summary as text
//   <out>/forecast_log/<track>.csv   every forecast (daily and hourly tracks)

#include <app/forecast_tracks.hpp>
#include <app/xlsx_writer.hpp>
#include <models/band_curriculum.hpp>
#include <models/curriculum.hpp>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace ft = altair::forecast_tracks;
namespace da = altair::data_audit;
using altair::CurriculumRun;
using altair::CurriculumSummary;
using altair::CurriculumTrack;
using altair::xlsx::XlsxCell;
using altair::xlsx::XlsxSheet;

/// Accuracy when a model calls only its most confident share of rows.
struct FrontierRow {
    std::size_t model = 0;
    std::size_t scored = 0;
    std::vector<double> acc;          ///< at kFrontierLevels
    std::vector<std::size_t> n;       ///< rows in each level
    double best_level = 0.0;          ///< largest share with accuracy >= 80 % on >= 30 rows; 0 if none
    double best_acc = 0.0, best_low = 0.0, best_up_rate = 0.0;
    std::size_t best_n = 0;
};

inline constexpr double kFrontierLevels[] = {0.001, 0.005, 0.01, 0.02, 0.05, 0.10, 0.20, 0.50, 1.0};

struct TrackResult {
    CurriculumTrack track;
    ft::TrackInfo info;
    CurriculumRun run;
    altair::BandRun bands;
    bool ok = false, bands_ok = false;
    std::string error;
    double seconds = 0.0;
    std::vector<CurriculumSummary> summary;   ///< [model]
    std::vector<altair::BandTally> band_summary;
    std::vector<FrontierRow> frontier;
    bool log = false;                         ///< daily and hourly: small enough for a CSV
};

/// `v` with `decimals` places; `sign` adds a + to positive numbers.
std::string fixed(double v, int decimals, bool sign = false) {
    char buf[64];
    std::snprintf(buf, sizeof buf, sign ? "%+.*f" : "%.*f", decimals, v);
    return buf;
}

std::string pad(std::string s, std::size_t width) {
    if (s.size() < width) { s.append(width - s.size(), ' '); }
    return s;
}

std::string slug(const std::string& name) {
    std::string s;
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!s.empty() && s.back() != '_') {
            s += '_';
        }
    }
    return s;
}

std::string day_text(std::int64_t t) { return da::format_audit_time(t, true); }

/// One plain sentence on what a model's record means.
std::string verdict(const CurriculumSummary& s, std::size_t tests) {
    if (s.all.scored() < 100) { return "too few forecasts to judge"; }
    if (s.p_adjusted < 0.05) {
        if (s.accuracy < 0.5) { return "reliably WRONG (significant) - worse than a coin"; }
        if (s.p_constant_adjusted < 0.05) { return "beats a coin AND the best constant call (significant)"; }
        return s.z_vs_constant > 2.0
                   ? "beats a coin (significant); its edge over the best constant call is not significant after "
                         + std::to_string(tests) + " tests"
                   : "beats a coin (significant) but not the best constant call";
    }
    if (s.p_vs_half < 0.05) {
        return "above a coin by chance-level evidence (not significant after " + std::to_string(tests) + " tests)";
    }
    return "indistinguishable from a coin";
}

double pct(double v) { return std::isfinite(v) ? 100.0 * v : v; }

XlsxSheet summary_sheet(const std::vector<TrackResult>& results, std::size_t tests) {
    XlsxSheet sh;
    sh.name = "Summary";
    sh.freeze_rows = 1;
    const char* head[] = {"Track", "Model", "Family", "Forecasts", "Abstained", "Coverage %", "Right", "Wrong", "No direction", "Flat",
                          "Accuracy %", "95% low %", "95% high %", "z vs coin", "p vs coin",
                          "p (Bonferroni)", "Up-rate on same bars %", "vs best constant call (pts)", "p vs constant (Bonferroni)",
                          "Brier", "RMSE bp", "Random walk RMSE bp", "Skill vs RW %", "Price verdict",
                          "Trades (clear cost)", "Trade hit %", "Net bp / trade", "Net t-stat", "Net bp total",
                          "Final-stage accuracy %", "Fit s", "Verdict"};
    std::vector<XlsxCell> h;
    for (const char* c : head) { h.push_back(XlsxCell::str(c, true)); }
    sh.rows.push_back(h);
    sh.widths = {24, 24, 11, 10, 10, 10, 9, 9, 10, 7, 10, 9, 9, 9, 9, 11, 12, 12, 12, 8, 9, 11, 10, 22, 11, 10, 10, 9, 11, 11, 8, 52};
    for (const auto& r : results) {
        if (!r.ok) {
            sh.rows.push_back({XlsxCell::str(r.track.name), XlsxCell::str("-"), XlsxCell::str("-"),
                               XlsxCell::str("not run: " + r.error)});
            continue;
        }
        std::vector<std::size_t> order(r.summary.size());
        for (std::size_t m = 0; m < order.size(); ++m) { order[m] = m; }
        std::stable_sort(order.begin(), order.end(), [&r](std::size_t a, std::size_t b) {
            const double x = r.summary[a].all.scored() > 0 ? r.summary[a].accuracy : -1.0;
            const double y = r.summary[b].all.scored() > 0 ? r.summary[b].accuracy : -1.0;
            return x > y;
        });
        const auto& last = r.run.stages.back();
        for (const std::size_t m : order) {
            const auto& s = r.summary[m];
            const auto fin = altair::curriculum_tally(r.track, r.run, m, last.test_begin, last.test_end);
            const bool scored = s.all.scored() > 0;
            const double nan = std::numeric_limits<double>::quiet_NaN();
            sh.rows.push_back({
                XlsxCell::str(r.track.name), XlsxCell::str(r.run.models[m]), XlsxCell::str(r.run.families[m]),
                XlsxCell::num(static_cast<double>(s.all.forecasts)), XlsxCell::num(static_cast<double>(s.all.abstained)),
                XlsxCell::num(s.all.forecasts + s.all.abstained > 0
                                  ? 100.0 * static_cast<double>(s.all.forecasts)
                                        / static_cast<double>(s.all.forecasts + s.all.abstained)
                                  : nan),
                XlsxCell::num(static_cast<double>(s.all.right)), XlsxCell::num(static_cast<double>(s.all.wrong)),
                XlsxCell::num(static_cast<double>(s.all.no_direction)), XlsxCell::num(static_cast<double>(s.all.flat)),
                XlsxCell::num(scored ? pct(s.accuracy) : nan, true),
                XlsxCell::num(scored ? pct(s.lo95) : nan), XlsxCell::num(scored ? pct(s.hi95) : nan),
                XlsxCell::num(scored ? s.z_vs_half : nan), XlsxCell::num(scored ? s.p_vs_half : nan),
                XlsxCell::num(scored ? s.p_adjusted : nan), XlsxCell::num(scored ? pct(s.up_rate) : nan),
                XlsxCell::num(scored ? 100.0 * (s.accuracy - s.best_constant) : nan),
                XlsxCell::num(scored ? s.p_constant_adjusted : nan),
                XlsxCell::num(s.all.brier()),
                XlsxCell::num(s.have_price ? s.price.rmse_model_bps : nan),
                XlsxCell::num(s.have_price ? s.price.rmse_naive_bps : nan),
                XlsxCell::num(s.have_price ? 100.0 * s.price.skill : nan),
                XlsxCell::str(s.have_price ? altair::verdict_text(s.verdict) : "-"),
                XlsxCell::num(r.track.tradable ? static_cast<double>(s.all.trades) : nan),
                XlsxCell::num(s.all.trades > 0 ? 100.0 * static_cast<double>(s.all.trade_wins)
                                                     / static_cast<double>(s.all.trades) : nan),
                XlsxCell::num(s.all.trades > 0 ? s.all.net_bp / static_cast<double>(s.all.trades) : nan),
                XlsxCell::num(s.all.net_t()),
                XlsxCell::num(s.all.trades > 0 ? s.all.net_bp : nan),
                XlsxCell::num(fin.scored() > 0 ? pct(fin.accuracy()) : nan),
                XlsxCell::num(std::round(r.run.fit_seconds[m] * 10.0) / 10.0),
                XlsxCell::str(verdict(s, tests)),
            });
        }
        sh.rows.push_back({});
    }
    return sh;
}

/// Rank a model's scored rows by confidence (|P(up) - 0.5|) and read the
/// accuracy of its top slices. The ranking uses the whole test period's
/// confidences -- no outcomes -- so the slice thresholds are not ones a trader
/// could have known in advance; the ex-ante versions are the Stack (confident
/// ...) and Consensus ensembles, which are scored like any model.
FrontierRow compute_frontier(const CurriculumTrack& t, const CurriculumRun& run, std::size_t m) {
    FrontierRow f;
    f.model = m;
    std::vector<std::pair<double, std::pair<bool, bool>>> scored;   // confidence, (right, went up)
    for (std::size_t i = run.first_row; i < t.rows(); ++i) {
        const altair::CurriculumCall c = run.calls[m][i - run.first_row];
        const double r = t.ret(i);
        if (!c.made || !std::isfinite(c.p_up) || r == 0.0) { continue; }
        const int out = r > 0.0 ? 1 : -1;
        scored.push_back({std::fabs(c.p_up - 0.5), {c.dir == out, out > 0}});
    }
    f.scored = scored.size();
    if (scored.empty()) { return f; }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::size_t> right(scored.size() + 1, 0), ups(scored.size() + 1, 0);
    for (std::size_t k = 0; k < scored.size(); ++k) {
        right[k + 1] = right[k] + (scored[k].second.first ? 1 : 0);
        ups[k + 1] = ups[k] + (scored[k].second.second ? 1 : 0);
    }
    const auto wilson_low = [](double p, double n) {
        const double z = 1.959963984540054;
        return (p + z * z / (2 * n) - z * std::sqrt(p * (1 - p) / n + z * z / (4 * n * n))) / (1 + z * z / n);
    };
    for (const double level : kFrontierLevels) {
        const std::size_t n = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(level * static_cast<double>(scored.size()))));
        f.n.push_back(n);
        const double acc = static_cast<double>(right[n]) / static_cast<double>(n);
        f.acc.push_back(acc);
        if (n >= 30 && acc >= 0.80 && level > f.best_level) {
            f.best_level = level;
            f.best_acc = acc;
            f.best_n = n;
            f.best_low = wilson_low(acc, static_cast<double>(n));
            f.best_up_rate = static_cast<double>(ups[n]) / static_cast<double>(n);
        }
    }
    return f;
}

XlsxSheet frontier_sheet(const std::vector<TrackResult>& results) {
    XlsxSheet sh;
    sh.name = "Frontier";
    sh.freeze_rows = 1;
    std::vector<XlsxCell> h{XlsxCell::str("Track", true), XlsxCell::str("Model", true), XlsxCell::str("Scored calls", true)};
    for (const double l : kFrontierLevels) {
        h.push_back(XlsxCell::str("Acc % top " + fixed(100.0 * l, l < 0.01 ? 1 : 0) + "%", true));
    }
    for (const char* c : {"Largest share at >= 80 % (n >= 30)", "Its accuracy %", "Its calls", "Wilson 95 % low %",
                          "Calls per trading day", "Up-rate on those bars %"}) {
        h.push_back(XlsxCell::str(c, true));
    }
    sh.rows.push_back(h);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (const auto& r : results) {
        if (!r.ok) { continue; }
        std::vector<const FrontierRow*> rows;
        for (const auto& f : r.frontier) { if (f.scored > 0) rows.push_back(&f); }
        std::stable_sort(rows.begin(), rows.end(), [](const FrontierRow* a, const FrontierRow* b) {
            return a->acc.size() > 3 && b->acc.size() > 3 && a->acc[3] > b->acc[3];   // by the top-2 % slice
        });
        for (const FrontierRow* f : rows) {
            std::vector<XlsxCell> row{XlsxCell::str(r.track.name), XlsxCell::str(r.run.models[f->model]),
                                      XlsxCell::num(static_cast<double>(f->scored))};
            for (std::size_t k = 0; k < f->acc.size(); ++k) { row.push_back(XlsxCell::num(pct(f->acc[k]))); }
            const bool any = f->best_level > 0.0;
            row.push_back(any ? XlsxCell::str(fixed(100.0 * f->best_level, f->best_level < 0.01 ? 1 : 0) + "%")
                              : XlsxCell::str("never"));
            row.push_back(XlsxCell::num(any ? pct(f->best_acc) : nan));
            row.push_back(XlsxCell::num(any ? static_cast<double>(f->best_n) : nan));
            row.push_back(XlsxCell::num(any ? pct(f->best_low) : nan));
            row.push_back(XlsxCell::num(any ? static_cast<double>(f->best_n) / static_cast<double>(r.track.days()) : nan));
            row.push_back(XlsxCell::num(any ? pct(f->best_up_rate) : nan));
            sh.rows.push_back(row);
        }
        sh.rows.push_back({});
    }
    sh.widths = {24, 26, 10, 11, 11, 10, 10, 10, 10, 10, 10, 11, 16, 11, 9, 12, 12, 12};
    return sh;
}

std::string band_verdict(const altair::BandTally& b, const altair::BandTally& base) {
    if (b.made < 100) { return "too few bands to judge"; }
    const double cov = b.coverage();
    const double se = std::sqrt(altair::kBandCoverage * (1.0 - altair::kBandCoverage) / static_cast<double>(b.made));
    const bool calibrated = std::fabs(cov - altair::kBandCoverage) < std::max(0.02, 3.0 * se);
    const double skill = 1.0 - b.mean_score_bp() / base.mean_score_bp();
    std::string out = calibrated ? "calibrated" : (cov < altair::kBandCoverage ? "UNDER-covers" : "over-covers");
    if (std::isfinite(skill)) {
        out += skill > 0.005 ? ", " + fixed(100.0 * skill, 1) + " % better score than the constant band"
             : skill < -0.005 ? ", " + fixed(-100.0 * skill, 1) + " % worse than the constant band"
                              : ", same as the constant band";
    }
    return out;
}

XlsxSheet bands_sheet(const std::vector<TrackResult>& results) {
    XlsxSheet sh;
    sh.name = "Bands";
    sh.freeze_rows = 1;
    const char* head[] = {"Track", "Model", "Family", "Bands", "Abstained", "Hit rate %", "Target %",
                          "Mean width bp", "Interval score bp", "Width vs constant %", "Score skill vs constant %",
                          "Hit rate, wide half %", "Hit rate, narrow half %", "Fit s", "Verdict"};
    std::vector<XlsxCell> h;
    for (const char* c : head) { h.push_back(XlsxCell::str(c, true)); }
    sh.rows.push_back(h);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (const auto& r : results) {
        if (!r.bands_ok || r.band_summary.empty()) { continue; }
        const auto& base = r.band_summary.front();   // Constant sigma (GBM)
        std::vector<std::size_t> order(r.band_summary.size());
        for (std::size_t m = 0; m < order.size(); ++m) { order[m] = m; }
        std::stable_sort(order.begin(), order.end(), [&r](std::size_t a, std::size_t b) {
            const double x = r.band_summary[a].made > 0 ? r.band_summary[a].mean_score_bp() : 1e300;
            const double y = r.band_summary[b].made > 0 ? r.band_summary[b].mean_score_bp() : 1e300;
            return x < y;
        });
        for (const std::size_t m : order) {
            const auto& b = r.band_summary[m];
            const std::size_t lo_made = b.made - b.hi_made, lo_hits = b.hits - b.hi_hits;
            sh.rows.push_back({
                XlsxCell::str(r.track.name), XlsxCell::str(r.bands.models[m]), XlsxCell::str(r.bands.families[m]),
                XlsxCell::num(static_cast<double>(b.made)), XlsxCell::num(static_cast<double>(b.abstained)),
                XlsxCell::num(pct(b.coverage()), true), XlsxCell::num(100.0 * altair::kBandCoverage),
                XlsxCell::num(b.mean_width_bp()), XlsxCell::num(b.mean_score_bp()),
                XlsxCell::num(b.made > 0 ? 100.0 * (b.mean_width_bp() / base.mean_width_bp() - 1.0) : nan),
                XlsxCell::num(b.made > 0 ? 100.0 * (1.0 - b.mean_score_bp() / base.mean_score_bp()) : nan),
                XlsxCell::num(b.hi_made > 0 ? 100.0 * static_cast<double>(b.hi_hits) / static_cast<double>(b.hi_made) : nan),
                XlsxCell::num(lo_made > 0 ? 100.0 * static_cast<double>(lo_hits) / static_cast<double>(lo_made) : nan),
                XlsxCell::num(std::round(r.bands.fit_seconds[m] * 10.0) / 10.0),
                XlsxCell::str(band_verdict(b, base)),
            });
        }
        sh.rows.push_back({});
    }
    sh.widths = {24, 26, 11, 10, 10, 10, 9, 12, 13, 12, 13, 12, 12, 8, 60};
    return sh;
}

/// Every Model Atlas row (desktop/atlas_data.hpp) and what this run did with it.
XlsxSheet atlas_sheet() {
    struct Row { const char* section; const char* name; const char* use; };
    static const Row rows[] = {
        {"1. Statistical alpha", "Mean reversion", "Mean reversion (last move); Mean reversion (z-score band)"},
        {"1. Statistical alpha", "Momentum / trend following", "Momentum (last move); Momentum (tuned lookback)"},
        {"1. Statistical alpha", "Cointegration", "Pairs (cointegration): NIFTY against BANKNIFTY and back"},
        {"1. Statistical alpha", "Pairs trading", "Pairs (cointegration)"},
        {"1. Statistical alpha", "Statistical arbitrage", "Pairs (cointegration) -- a two-instrument universe"},
        {"1. Statistical alpha", "Factor models (Fama-French, PCA)", "Autoencoder + logistic (PCA factors of the features); cross-sectional factors need a stock universe"},
        {"1. Statistical alpha", "Relative value", "not run: needs fundamentals, not in the dataset"},
        {"1. Statistical alpha", "Spread / calendar trading", "not run: needs two futures expiries; the dataset has the near month only"},
        {"1. Statistical alpha", "Regime detection", "Hurst regime switch; k-means regimes; Markov chain; Hidden Markov model"},
        {"2. Machine learning", "Linear / ridge regression", "Ridge regression"},
        {"2. Machine learning", "Gradient boosting (GBDT / LightGBM)", "Gradient boosting; Gradient boosting on |r| (bands)"},
        {"2. Machine learning", "Decision tree", "Decision tree"},
        {"2. Machine learning", "MLP / feedforward network", "Neural net (MLP)"},
        {"2. Machine learning", "LSTM / GRU", "LSTM; GRU"},
        {"2. Machine learning", "Transformer / attention", "Transformer"},
        {"2. Machine learning", "CNN", "CNN (random kernels)"},
        {"2. Machine learning", "Random forest", "Random forest; Random forest on |r| (bands)"},
        {"2. Machine learning", "Logistic regression", "Logistic regression; the Stack's meta-model"},
        {"2. Machine learning", "SVM / KNN / autoencoder", "SVM (RBF); k-nearest neighbours; Autoencoder + logistic"},
        {"3. Time series & volatility", "AR (autoregression)", "AR(2)"},
        {"3. Time series & volatility", "ARMA / ARIMA / SARIMA", "ARMA(1,1); Seasonal AR (SARIMA)"},
        {"3. Time series & volatility", "VAR (vector autoregression)", "VAR(1)"},
        {"3. Time series & volatility", "Kalman filter", "Kalman filter (drift)"},
        {"3. Time series & volatility", "Hidden Markov model", "Hidden Markov model"},
        {"3. Time series & volatility", "Markov regime chain", "Markov chain"},
        {"3. Time series & volatility", "Ornstein-Uhlenbeck", "Ornstein-Uhlenbeck"},
        {"3. Time series & volatility", "Brownian motion / GBM", "Always majority (the drift); Constant sigma (GBM) band"},
        {"3. Time series & volatility", "Historical volatility & EWMA", "Historical vol (20); EWMA (0.94) bands"},
        {"3. Time series & volatility", "GARCH / GJR-GARCH", "GARCH(1,1); GJR-GARCH bands"},
        {"3. Time series & volatility", "EGARCH", "EGARCH band"},
        {"3. Time series & volatility", "Heston stochastic volatility", "Heston variance drift band"},
        {"3. Time series & volatility", "Hurst exponent", "Hurst regime switch"},
        {"4. Microstructure & execution", "Order-book imbalance, microprice, VPIN, Kyle's lambda", "not run: need the order book and trade tape; the dataset is OHLC bars"},
        {"4. Microstructure & execution", "Slippage, TWAP/VWAP/POV, Almgren-Chriss, shortfall, queue, fill probability, Hawkes", "not forecasters: they execute a decided trade"},
        {"5. Risk", "VaR, CVaR, stress, covariance, drawdown, beta, factor risk", "not forecasters of the next bar; the band models are the forecasting half of VaR"},
        {"5. Risk", "Conformal risk control", "every band is conformally calibrated on its own past errors"},
        {"6. Options & derivatives", "Black-Scholes ... finite-difference PDE", "not run: they price options; no option chain in the dataset"},
        {"7. Portfolio construction", "Sizing, Kelly, vol targeting, risk parity, min variance, Black-Litterman, MVO", "not forecasters: they size and combine positions"},
        {"8. Alternative data", "News / NLP / sentiment; event models", "not run: no text or event data in the dataset"},
        {"9. Simulation", "Monte Carlo", "Constant sigma (GBM); Jump diffusion (Merton) bands"},
        {"9. Simulation", "Bootstrapping", "Bootstrap quantile band"},
        {"9. Simulation", "Jump diffusion", "Jump diffusion (Merton) band"},
        {"9. Simulation", "Walk-forward & purged CV", "the curriculum itself: doubling walk-forward, no look-ahead"},
        {"9. Simulation", "Agent-based simulation", "not a forecaster: a synthetic market"},
        {"10. Reinforcement learning", "Q-learning", "not run: its QLearner learns execution aggression, not direction"},
        {"10. Reinforcement learning", "DQN / PPO / actor-critic", "DQN (reinforcement)"},
        {"Cross-cutting", "Ensemble aggregator", "Vote, Champion, Hedge, Stack, Consensus, Vol ensemble, Best band so far"},
        {"Cross-cutting", "Forecast scorecard", "the price skill and verdict columns"},
        {"Cross-cutting", "Transaction cost calculator", "the cost hurdle (STT dated as in config/charges.toml)"},
        {"Cross-cutting", "Model scorecards & drift, feature registry, DCF", "not forecasters of the next bar"},
    };
    XlsxSheet sh;
    sh.name = "Atlas coverage";
    sh.freeze_rows = 1;
    sh.rows.push_back({XlsxCell::str("Atlas section", true), XlsxCell::str("Atlas row", true),
                       XlsxCell::str("In this run", true)});
    for (const Row& r : rows) {
        sh.rows.push_back({XlsxCell::str(r.section), XlsxCell::str(r.name), XlsxCell::str(r.use)});
    }
    sh.widths = {30, 60, 110};
    return sh;
}

XlsxSheet track_sheet(const TrackResult& r) {
    XlsxSheet sh;
    sh.name = r.track.name;
    const auto& run = r.run;
    const std::size_t models = run.models.size();
    const auto& t = r.track;
    sh.rows.push_back({XlsxCell::str(t.name + " - " + t.horizon + " direction, doubling curriculum", true)});
    sh.rows.push_back({XlsxCell::str(std::to_string(t.rows()) + " decisions over " + std::to_string(t.days())
                                     + " trading days, " + day_text(t.t.front()) + " to " + day_text(t.t.back())
                                     + ". Each stage learns every day before it and forecasts the block; "
                                       "the model is frozen inside a block and refitted after it.")});
    sh.rows.push_back({});
    const auto header = [&](const char* title) {
        std::vector<XlsxCell> h{XlsxCell::str(title, true), XlsxCell::str("Learned days", true),
                                XlsxCell::str("Forecast days", true), XlsxCell::str("Learned", true),
                                XlsxCell::str("Forecast", true)};
        for (const auto& m : run.models) { h.push_back(XlsxCell::str(m, true)); }
        sh.rows.push_back(h);
    };
    const auto stage_cells = [&](const altair::CurriculumStage& st) {
        return std::vector<XlsxCell>{
            XlsxCell::num(static_cast<double>(st.index)), XlsxCell::num(st.train_days), XlsxCell::num(st.test_days),
            XlsxCell::str(day_text(t.t.front()) + " to " + day_text(t.t[st.train_rows - 1])),
            XlsxCell::str(day_text(t.t[st.test_begin]) + " to " + day_text(t.t[st.test_end - 1]))};
    };
    const double nan = std::numeric_limits<double>::quiet_NaN();

    header("Accuracy % in the stage");
    for (const auto& st : run.stages) {
        auto row = stage_cells(st);
        for (std::size_t m = 0; m < models; ++m) {
            const auto k = altair::curriculum_tally(t, run, m, st.test_begin, st.test_end);
            row.push_back(XlsxCell::num(k.scored() > 0 ? pct(k.accuracy()) : nan));
        }
        sh.rows.push_back(row);
    }
    sh.rows.push_back({});
    header("Accuracy % so far");
    for (const auto& st : run.stages) {
        auto row = stage_cells(st);
        for (std::size_t m = 0; m < models; ++m) {
            const auto k = altair::curriculum_tally(t, run, m, run.first_row, st.test_end);
            row.push_back(XlsxCell::num(k.scored() > 0 ? pct(k.accuracy()) : nan));
        }
        sh.rows.push_back(row);
    }
    sh.rows.push_back({});
    header("Right / scored in the stage");
    for (const auto& st : run.stages) {
        auto row = stage_cells(st);
        for (std::size_t m = 0; m < models; ++m) {
            const auto k = altair::curriculum_tally(t, run, m, st.test_begin, st.test_end);
            row.push_back(XlsxCell::str(k.forecasts == 0 ? std::string{"abstained"}
                                                         : std::to_string(k.right) + " / " + std::to_string(k.scored())));
        }
        sh.rows.push_back(row);
    }
    sh.rows.push_back({});
    header("What the stage's fit chose");
    for (const auto& st : run.stages) {
        auto row = stage_cells(st);
        for (std::size_t m = 0; m < models; ++m) { row.push_back(XlsxCell::str(run.notes[m][st.index])); }
        sh.rows.push_back(row);
    }
    sh.rows.push_back({});
    sh.rows.push_back({XlsxCell::str("Track record going into each stage", true), XlsxCell::str("Champion", true),
                       XlsxCell::str("Hedge's top weights", true)});
    for (const auto& st : run.stages) {
        const int c = run.champion[st.index];
        const auto& w = run.hedge_weight[st.index];
        std::vector<std::size_t> idx(w.size());
        for (std::size_t m = 0; m < idx.size(); ++m) { idx[m] = m; }
        std::stable_sort(idx.begin(), idx.end(), [&w](std::size_t a, std::size_t b) { return w[a] > w[b]; });
        std::string top;
        for (std::size_t k = 0; k < idx.size() && k < 3 && w[idx[k]] > 0.0; ++k) {
            if (!top.empty()) { top += ", "; }
            top += run.models[idx[k]] + " " + fixed(100.0 * w[idx[k]], 0) + "%";
        }
        sh.rows.push_back({XlsxCell::str("stage " + std::to_string(st.index)),
                           XlsxCell::str(c >= 0 ? run.models[static_cast<std::size_t>(c)] : "none yet"),
                           XlsxCell::str(top.empty() ? "none yet" : top)});
    }
    if (r.bands_ok) {
        const auto& b = r.bands;
        const auto bheader = [&](const char* title) {
            std::vector<XlsxCell> hh{XlsxCell::str(title, true), XlsxCell::str("Learned days", true),
                                     XlsxCell::str("Forecast days", true), XlsxCell::str("Learned", true),
                                     XlsxCell::str("Forecast", true)};
            for (const auto& m : b.models) { hh.push_back(XlsxCell::str(m, true)); }
            sh.rows.push_back(hh);
        };
        sh.rows.push_back({});
        sh.rows.push_back({XlsxCell::str("Range: will the next close land inside the band? Target 80 %.", true)});
        bheader("Band hit rate % in the stage");
        for (const auto& st : b.stages) {
            auto row = stage_cells(st);
            for (std::size_t m = 0; m < b.models.size(); ++m) {
                const auto k = altair::band_tally(t, b, m, st.test_begin, st.test_end);
                row.push_back(XlsxCell::num(k.made > 0 ? pct(k.coverage()) : nan));
            }
            sh.rows.push_back(row);
        }
        sh.rows.push_back({});
        bheader("Mean band width bp in the stage");
        for (const auto& st : b.stages) {
            auto row = stage_cells(st);
            for (std::size_t m = 0; m < b.models.size(); ++m) {
                const auto k = altair::band_tally(t, b, m, st.test_begin, st.test_end);
                row.push_back(XlsxCell::num(k.made > 0 ? k.mean_width_bp() : nan));
            }
            sh.rows.push_back(row);
        }
        sh.rows.push_back({});
        bheader("Band calibration and fit");
        for (const auto& st : b.stages) {
            auto row = stage_cells(st);
            for (std::size_t m = 0; m < b.models.size(); ++m) { row.push_back(XlsxCell::str(b.notes[m][st.index])); }
            sh.rows.push_back(row);
        }
    }
    sh.widths = {26, 12, 13, 26, 26};
    for (std::size_t m = 0; m < models; ++m) { sh.widths.push_back(14); }
    return sh;
}

XlsxSheet data_sheet(const std::vector<TrackResult>& results) {
    XlsxSheet sh;
    sh.name = "Data";
    sh.freeze_rows = 1;
    const char* head[] = {"Track", "Source", "Files", "Rows read", "Parse errors", "Duplicates", "Conflicts (kept first)",
                          "Seconds floored", "Bars after cleaning", "Bad prices dropped", "OHLC repaired",
                          "Short sessions excluded", "Bars before INDIA VIX", "Rows without VIX",
                          "Rows without spot", "Rows without VIX forecast", "Roll outcomes excluded", "Expiries", "Basis on expiry bp",
                          "Basis jump next day bp", "Warm-up bars", "Decisions", "Trading days",
                          "First decision", "Last decision", "Features", "Cost"};
    std::vector<XlsxCell> h;
    for (const char* c : head) { h.push_back(XlsxCell::str(c, true)); }
    sh.rows.push_back(h);
    for (const auto& r : results) {
        const auto& i = r.info;
        const auto n = [](std::size_t v) { return XlsxCell::num(static_cast<double>(v)); };
        std::string features;
        for (const auto& f : r.track.feature_names) { features += (features.empty() ? "" : ", ") + f; }
        sh.rows.push_back({XlsxCell::str(r.track.name), XlsxCell::str(i.source), n(i.files), n(i.rows_read),
                           n(i.parse_errors), n(i.duplicates), n(i.conflicts), n(i.seconds_floored), n(i.bars),
                           n(i.bad_price), n(i.ohlc_repaired), n(i.short_days), n(i.before_vix), n(i.no_vix),
                           n(i.no_spot), n(i.no_vix_forecast), n(i.roll_excluded), n(i.expiries), XlsxCell::num(i.basis_on_expiry_bp),
                           XlsxCell::num(i.basis_jump_after_bp), n(i.warmup), n(i.rows),
                           XlsxCell::num(i.days), XlsxCell::str(i.first), XlsxCell::str(i.last),
                           XlsxCell::str(features), XlsxCell::str(i.cost_note)});
    }
    sh.widths = {24, 28, 7, 10, 8, 10, 10, 9, 10, 9, 9, 10, 10, 9, 9, 10, 10, 8, 9, 9, 9, 10, 9, 26, 26, 90, 40};
    return sh;
}

XlsxSheet method_sheet(const std::vector<std::string>& lines) {
    XlsxSheet sh;
    sh.name = "Method";
    for (std::size_t k = 0; k < lines.size(); ++k) { sh.rows.push_back({XlsxCell::str(lines[k], k == 0)}); }
    sh.widths = {160};
    return sh;
}

void write_log(const fs::path& dir, const TrackResult& r) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / (slug(r.track.name) + ".csv");
    std::ofstream out(file.string() + ".tmp", std::ios::binary | std::ios::trunc);
    out << "time,stage,learned_days,model,last_price,next_price,forecast_price,p_up,call,moved,result,net_bp\n";
    const auto& t = r.track;
    const auto& run = r.run;
    char buf[320];
    for (std::size_t i = run.first_row; i < t.rows(); ++i) {
        const std::size_t st = run.stage_of[i - run.first_row];
        const int moved = t.ret(i) > 0.0 ? 1 : (t.ret(i) < 0.0 ? -1 : 0);
        const std::string when = da::format_audit_time(t.t[i], false);
        for (std::size_t m = 0; m < run.models.size(); ++m) {
            const altair::CurriculumCall c = run.calls[m][i - run.first_row];
            if (!c.made) { continue; }   // abstentions are in the workbook, stage by stage
            const char* result = moved == 0 ? "FLAT" : (c.dir == moved ? "RIGHT" : "WRONG");
            const double net = altair::curriculum_trade_bp(t, c, i);
            // Quoted: "ARMA(1,1)" carries a comma.
            std::snprintf(buf, sizeof buf, "%s,%zu,%d,\"%s\",%.2f,%.2f,%.2f,", when.c_str(), st,
                          run.stages[st].train_days, run.models[m].c_str(), t.anchor[i], t.actual[i],
                          t.anchor[i] * std::exp(c.mu));
            out << buf;
            if (std::isfinite(c.p_up)) { out << fixed(c.p_up, 4); }
            out << ',' << (c.dir > 0 ? "UP" : (c.dir < 0 ? "DOWN" : "NONE")) << ','
                << (moved > 0 ? "UP" : (moved < 0 ? "DOWN" : "FLAT")) << ',' << result << ',';
            if (std::isfinite(net)) { out << fixed(net, 2); }
            out << '\n';
        }
    }
    out.close();
    if (!out) {
        std::printf("  could not write %s\n", file.string().c_str());
        fs::remove(file.string() + ".tmp", ec);
        return;
    }
    fs::rename(file.string() + ".tmp", file, ec);
}

void usage(const char* exe) {
    std::printf(
        "  Train every Model Atlas forecaster on a doubling curriculum, for direction and for\n"
        "  80 %% range bands, on every timeframe, and keep the record.\n\n"
        "    %s [--dataset DIR] [--out DIR] [--first-days N] [--cap-days N] [--jobs N]\n"
        "       [--other-cost-bp X] [--only TEXT] [--no-bands] [--no-log]\n\n"
        "    --dataset DIR      default dataset\n"
        "    --out DIR          default data/verified\n"
        "    --first-days N     days learned before the first forecast (default 3)\n"
        "    --cap-days N       stop doubling once a block exceeds N days; 0 = pure doubling (default)\n"
        "    --jobs N           tracks run in parallel (default: the machine's cores)\n"
        "    --other-cost-bp X  round-trip charges besides STT, incl. one tick of slippage (default 1.3)\n"
        "    --only TEXT        run only tracks whose name contains TEXT (e.g. daily, 5m, NIFTY)\n"
        "    --no-bands         skip the range-band curriculum\n"
        "    --no-log           skip the per-forecast CSV logs (written for daily and hourly tracks)\n", exe);
}

bool parse_int(std::string_view v, long& out) {
    const auto r = std::from_chars(v.data(), v.data() + v.size(), out);
    return r.ec == std::errc{} && r.ptr == v.data() + v.size();
}

/// One track to build: where its bars live and how to read them.
struct TrackSpec {
    std::string name, instrument;
    std::string dir;          ///< under the dataset, e.g. "spot/nifty"
    int tf = 0;               ///< minutes; da::kDailyTf for daily
    bool vix_itself = false, futures = false;
    std::string pair_dir, pair_name;
};

std::string tf_dir(int tf) {
    return tf == da::kDailyTf ? "1d" : std::to_string(tf) + "m";
}

std::string tf_label(int tf) {
    return tf == da::kDailyTf ? "daily" : (tf == 60 ? "hourly" : std::to_string(tf) + "m");
}

} // namespace

int main(int argc, char** argv) {
    fs::path root = "dataset";
    fs::path out = "data/verified";
    long first_days = 3, cap_days = 0, jobs = static_cast<long>(std::max(1u, std::thread::hardware_concurrency()));
    double other_cost = 1.3;
    std::string only;
    bool log = true, bands = true;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        const bool has = i + 1 < argc;
        if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        if (a == "--dataset" && has) { root = argv[++i]; continue; }
        if (a == "--out" && has) { out = argv[++i]; continue; }
        if (a == "--only" && has) { only = argv[++i]; continue; }
        if (a == "--no-log") { log = false; continue; }
        if (a == "--no-bands") { bands = false; continue; }
        if (a == "--first-days" && has) {
            if (!parse_int(argv[++i], first_days) || first_days < 1 || first_days > 1000) { usage(argv[0]); return 2; }
            continue;
        }
        if (a == "--cap-days" && has) {
            if (!parse_int(argv[++i], cap_days) || cap_days < 0 || cap_days > 100000) { usage(argv[0]); return 2; }
            continue;
        }
        if (a == "--jobs" && has) {
            if (!parse_int(argv[++i], jobs) || jobs < 1 || jobs > 64) { usage(argv[0]); return 2; }
            continue;
        }
        if (a == "--other-cost-bp" && has) {
            const std::string v{argv[++i]};
            char* end = nullptr;
            other_cost = std::strtod(v.c_str(), &end);
            if (end == v.c_str() || *end != '\0' || !(other_cost >= 0.0) || other_cost > 1000.0) { usage(argv[0]); return 2; }
            continue;
        }
        usage(argv[0]);
        return 2;
    }

    const auto t_start = std::chrono::steady_clock::now();
    std::printf("Forecast curriculum over %s (%ld parallel jobs)\n", root.string().c_str(), jobs);

    // The tracks: every instrument at every timeframe, 1m first so the longest
    // jobs start first.
    std::vector<TrackSpec> specs;
    for (const int tf : {1, 5, 15, 60, da::kDailyTf}) {
        specs.push_back({"NIFTY " + tf_label(tf), "NIFTY", "spot/nifty", tf, false, false, "spot/banknifty", "BANKNIFTY"});
        specs.push_back({"BANKNIFTY " + tf_label(tf), "BANKNIFTY", "spot/banknifty", tf, false, false, "spot/nifty", "NIFTY"});
        specs.push_back({"INDIA VIX " + tf_label(tf), "INDIA VIX", "spot/indiavix", tf, true, false, "", ""});
    }
    specs.push_back({"NIFTY FUT daily", "NIFTY FUT", "fut/nifty", da::kDailyTf, false, true, "", ""});
    const auto wanted = [&only](const std::string& name) { return only.empty() || name.find(only) != std::string::npos; };
    std::erase_if(specs, [&](const TrackSpec& t) { return !wanted(t.name); });
    const bool want_fc = wanted("NIFTY daily + VIX fc") || wanted("BANKNIFTY daily + VIX fc")
                      || wanted("NIFTY FUT daily + VIX fc") || wanted("NIFTY hourly + VIX fc")
                      || wanted("BANKNIFTY hourly + VIX fc");
    if (want_fc && std::none_of(specs.begin(), specs.end(), [](const TrackSpec& t) { return t.name == "INDIA VIX daily"; })) {
        specs.push_back({"INDIA VIX daily", "INDIA VIX", "spot/indiavix", da::kDailyTf, true, false, "", ""});
    }

    // Every series the tracks need, loaded and cleaned once, before any thread starts.
    struct Series { std::vector<da::AuditBar> bars; ft::TrackInfo info; };
    std::map<std::string, Series> series;
    const auto need = [&](const std::string& dir, int tf) {
        const std::string key = dir + "/" + tf_dir(tf);
        if (series.contains(key)) { return; }
        Series sr;
        sr.bars = ft::load_bars(root / key, tf, sr.info);
        ft::clean_bars(sr.bars, sr.info);
        series.emplace(key, std::move(sr));
    };
    for (const auto& t : specs) {
        need(t.dir, t.tf);
        if (!t.vix_itself) { need("spot/indiavix", t.tf); }
        if (t.futures) { need("spot/nifty", t.tf); }
        if (!t.pair_dir.empty()) { need(t.pair_dir, t.tf); }
    }
    if (want_fc) {
        for (const int tf : {60, da::kDailyTf}) { need("spot/nifty", tf); need("spot/banknifty", tf); need("spot/indiavix", tf); }
        need("fut/nifty", da::kDailyTf);
    }
    for (const auto& [key, sr] : series) {
        if (sr.bars.empty()) { std::printf("  no data under %s\n", (root / key).string().c_str()); return 1; }
    }
    const auto bars = [&](const std::string& dir, int tf) -> const std::vector<da::AuditBar>* {
        const auto it = series.find(dir + "/" + tf_dir(tf));
        return it == series.end() ? nullptr : &it->second.bars;
    };
    const std::string cost_text = "2 bp STT to 2026-03-31, 5 bp from 2026-04-01, + " + fixed(other_cost, 1)
                                  + " bp charges and one tick";

    altair::CurriculumOptions opt;
    opt.first_days = static_cast<std::int32_t>(first_days);
    opt.step_cap_days = static_cast<std::int32_t>(cap_days);
    std::mutex print_mu;

    // Build a track from its spec (and, for the "+ VIX fc" tracks, the forecasts).
    const auto build = [&](const TrackSpec& t, const ft::VixForecast* fc, TrackResult& r) {
        r.info = series.at(t.dir + "/" + tf_dir(t.tf)).info;
        r.info.cost_note = t.vix_itself ? "not tradable" : cost_text;
        const auto* own = bars(t.dir, t.tf);
        const auto* vix = t.vix_itself ? nullptr : bars("spot/indiavix", t.tf);
        const auto* pair = t.pair_dir.empty() ? nullptr : bars(t.pair_dir, t.tf);
        if (t.tf == da::kDailyTf) {
            r.track = ft::build_daily({t.name, t.instrument, own, vix, t.futures ? bars("spot/nifty", t.tf) : nullptr,
                                       !t.vix_itself, other_cost, fc, pair, t.pair_name}, r.info);
        } else if (t.tf == 60) {
            r.track = ft::build_hourly({t.name, t.instrument, own, vix, !t.vix_itself, other_cost, fc, pair, t.pair_name}, r.info);
        } else {
            r.track = ft::build_intraday({t.name, t.instrument, t.tf, own, vix, !t.vix_itself, other_cost, pair, t.pair_name}, r.info);
        }
        r.log = log && t.tf >= 60;
    };
    const auto run_one = [&](TrackResult& r) {
        const auto t0 = std::chrono::steady_clock::now();
        auto models = altair::curriculum_default_models();
        auto run = altair::curriculum_run(r.track, models, opt);
        if (!run) {
            r.error = altair::curriculum_error_text(run.error());
            const std::lock_guard<std::mutex> lock(print_mu);
            std::printf("  %-26s not run: %s\n", r.track.name.c_str(), r.error.c_str());
            return;
        }
        r.run = std::move(*run);
        r.ok = true;
        for (std::size_t m = 0; m < r.run.models.size(); ++m) { r.frontier.push_back(compute_frontier(r.track, r.run, m)); }
        if (bands) {
            auto bm = altair::band_default_models();
            auto br = altair::band_run(r.track, bm, opt);
            if (br) {
                r.bands = std::move(*br);
                r.bands_ok = true;
                for (std::size_t m = 0; m < r.bands.models.size(); ++m) {
                    r.band_summary.push_back(altair::band_tally(r.track, r.bands, m, r.bands.first_row, r.track.rows()));
                }
            }
        }
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const std::lock_guard<std::mutex> lock(print_mu);
        std::printf("  %-26s %8zu decisions, %5d days, %2zu stages, %zu + %zu look-ahead refusals, %.0f s\n",
                    r.track.name.c_str(), r.track.rows(), r.track.days(), r.run.stages.size(), r.run.lookahead_refusals,
                    r.bands_ok ? r.bands.lookahead_refusals : 0, r.seconds);
        std::fflush(stdout);
    };
    const auto run_all = [&](std::vector<TrackResult>& rs, std::size_t from, const std::vector<std::function<void(TrackResult&)>>& makers) {
        std::atomic<std::size_t> next{0};
        const auto worker = [&]() {
            for (std::size_t k = next++; k < makers.size(); k = next++) {
                makers[k](rs[from + k]);
                run_one(rs[from + k]);
            }
        };
        std::vector<std::thread> pool;
        const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(jobs), makers.size());
        for (std::size_t k = 0; k < n; ++k) { pool.emplace_back(worker); }
        for (auto& th : pool) { th.join(); }
    };

    std::vector<TrackResult> results(specs.size());
    {
        std::vector<std::function<void(TrackResult&)>> makers;
        for (const auto& t : specs) { makers.push_back([&, t](TrackResult& r) { build(t, nullptr, r); }); }
        run_all(results, 0, makers);
    }

    // The VIX model's out-of-sample forecasts -- Hedge's, which picks models by
    // their record in finished stages, not by hindsight -- as an input to the
    // index models. Run beside the originals so the two can be compared.
    ft::VixForecast vix_fc;
    for (const auto& r : results) {
        if (!r.ok || r.track.name != "INDIA VIX daily") { continue; }
        const auto h = std::find(r.run.models.begin(), r.run.models.end(), std::string{"Hedge"});
        const auto m = static_cast<std::size_t>(h - r.run.models.begin());
        for (std::size_t i = r.run.first_row; m < r.run.models.size() && i < r.track.rows(); ++i) {
            const altair::CurriculumCall c = r.run.calls[m][i - r.run.first_row];
            if (c.made && std::isfinite(c.p_up)) { vix_fc[da::audit_day(r.track.t[i])] = c.p_up; }
        }
    }
    if (want_fc && !vix_fc.empty()) {
        std::vector<TrackSpec> fc_specs;
        for (const auto& t : std::vector<TrackSpec>{
                 {"NIFTY daily + VIX fc", "NIFTY", "spot/nifty", da::kDailyTf, false, false, "spot/banknifty", "BANKNIFTY"},
                 {"BANKNIFTY daily + VIX fc", "BANKNIFTY", "spot/banknifty", da::kDailyTf, false, false, "spot/nifty", "NIFTY"},
                 {"NIFTY FUT daily + VIX fc", "NIFTY FUT", "fut/nifty", da::kDailyTf, false, true, "", ""},
                 {"NIFTY hourly + VIX fc", "NIFTY", "spot/nifty", 60, false, false, "spot/banknifty", "BANKNIFTY"},
                 {"BANKNIFTY hourly + VIX fc", "BANKNIFTY", "spot/banknifty", 60, false, false, "spot/nifty", "NIFTY"}}) {
            if (wanted(t.name)) { fc_specs.push_back(t); }
        }
        const std::size_t from = results.size();
        results.resize(from + fc_specs.size());
        std::vector<std::function<void(TrackResult&)>> makers;
        for (const auto& t : fc_specs) { makers.push_back([&, t](TrackResult& r) { build(t, &vix_fc, r); }); }
        run_all(results, from, makers);
    } else if (want_fc) {
        std::printf("  (no INDIA VIX daily forecasts: the '+ VIX fc' tracks were not built)\n");
    }
    // A track added only to feed the "+ VIX fc" tracks is not reported.
    if (!wanted("INDIA VIX daily")) {
        std::erase_if(results, [](const TrackResult& r) { return r.track.name == "INDIA VIX daily"; });
    }

    std::size_t tests = 0;
    for (const auto& r : results) { tests += r.ok ? r.run.models.size() : 0; }
    for (auto& r : results) {
        if (!r.ok) { continue; }
        for (std::size_t m = 0; m < r.run.models.size(); ++m) {
            r.summary.push_back(altair::curriculum_summary(r.track, r.run, m, tests));
        }
    }

    std::ostringstream text;
    text << "Forecast curriculum: learn 3 days, forecast, keep score, refit on 6, 12, 24 ... days.\n"
         << "Accuracy = right / (right + wrong); unchanged bars and abstentions are not scored.\n"
         << "Significance is Bonferroni-corrected over " << tests << " model-track tests.\n";
    for (const auto& r : results) {
        if (!r.ok) { continue; }
        text << "\n" << r.track.name << " (" << r.track.rows() << " decisions, " << r.run.stages.size()
             << " stages, " << r.track.days() << " days)\n";
        std::vector<std::size_t> order(r.summary.size());
        for (std::size_t m = 0; m < order.size(); ++m) { order[m] = m; }
        std::stable_sort(order.begin(), order.end(), [&r](std::size_t a, std::size_t b) {
            const double x = r.summary[a].all.scored() > 0 ? r.summary[a].accuracy : -1.0;
            const double y = r.summary[b].all.scored() > 0 ? r.summary[b].accuracy : -1.0;
            return x > y;
        });
        for (const std::size_t m : order) {
            const auto& s = r.summary[m];
            if (s.all.scored() == 0) {
                text << "  " << pad(r.run.models[m], 30) << "  never forecast\n";
                continue;
            }
            text << "  " << pad(r.run.models[m], 30) << pad(fixed(pct(s.accuracy), 2) + "%", 7)
                 << "  [" << fixed(pct(s.lo95), 1) << ", " << fixed(pct(s.hi95), 1) << "]"
                 << "  n=" << s.all.scored() << "  up-rate " << fixed(pct(s.up_rate), 1) + "%";
            if (s.have_price) { text << "  skill vs RW " << fixed(100.0 * s.price.skill, 2, true) + "%"; }
            if (s.all.trades > 0) {
                text << "  net " << fixed(s.all.net_bp / static_cast<double>(s.all.trades), 2, true) << " bp/trade (t "
                     << fixed(s.all.net_t(), 1, true) << ")";
            }
            text << "  -- " << verdict(s, tests) << "\n";
        }
        // Where, if anywhere, direction reaches 80 %.
        const FrontierRow* best = nullptr;
        for (const auto& f : r.frontier) {
            if (f.best_level > 0.0 && (best == nullptr || f.best_level > best->best_level)) { best = &f; }
        }
        if (best != nullptr) {
            text << "  >= 80 % direction: " << r.run.models[best->model] << ", its most confident "
                 << fixed(100.0 * best->best_level, best->best_level < 0.01 ? 1 : 0) << "% of calls: "
                 << fixed(pct(best->best_acc), 1) << "% on " << best->best_n << " calls (Wilson low "
                 << fixed(pct(best->best_low), 1) << "%, up-rate on those bars " << fixed(pct(best->best_up_rate), 1)
                 << "%)\n";
        } else {
            text << "  >= 80 % direction: no model reaches it on any slice of 30+ calls\n";
        }
        if (r.bands_ok && !r.band_summary.empty()) {
            std::size_t bestb = 0;
            for (std::size_t m = 1; m < r.band_summary.size(); ++m) {
                if (r.band_summary[m].made > 100
                    && r.band_summary[m].mean_score_bp() < r.band_summary[bestb].mean_score_bp()) { bestb = m; }
            }
            const auto& bb = r.band_summary[bestb];
            text << "  80 % band: best " << r.bands.models[bestb] << ", hit rate " << fixed(pct(bb.coverage()), 1)
                 << "% with " << fixed(bb.mean_width_bp(), 1) << " bp width -- " << band_verdict(bb, r.band_summary.front())
                 << "\n";
        }
    }

    const std::vector<std::string> method = {
        "How the curriculum works",
        "Every model walks the same doubling schedule over trading days: learn days [0, 3) and forecast days [3, 6); "
        "then learn [0, 6) and forecast [6, 12); then 12, 24, 48 ... until the data ends. The last block is what is left.",
        "Inside a block the model is frozen: it never sees the outcomes it is forecasting. After the block it is refitted "
        "from scratch on everything seen so far. That refit is the 'update'.",
        "DIRECTION. A call is RIGHT when the next close moved the called way, WRONG otherwise (a call with no direction is "
        "WRONG and also counted under 'No direction'; an exact 50/50 is broken by the expected return). An unchanged close "
        "is neither. A model that cannot fit yet ABSTAINS; abstentions are listed per stage, not scored.",
        "RANGE. A band model forecasts the scale of the next move; the band is k times it either side of the last price, "
        "with k the 80th percentile of the model's own past out-of-sample |move| / scale (in-sample until 50 exist). "
        "Every band aims at 80 %; they compete on the interval (Winkler) score -- width plus 5x any miss -- against the "
        "constant-sigma band. A wide band hits often and scores badly, so hit rate alone proves nothing.",
        "FRONTIER. For each probabilistic model, its scored calls ranked by confidence |P(up) - 0.5|; the accuracy of the "
        "top 0.1 %, 0.5 %, 1 % ... 100 %; and the largest slice still >= 80 % on 30+ calls. The ranking uses the whole "
        "test period's confidences (no outcomes), so a trader would not have known the threshold in advance -- the "
        "ex-ante versions are the Stack (confident ...) and Consensus ensembles, scored like any model on the Summary sheet.",
        "Timeframes: daily (decide at 15:30, outcome next close), hourly (decide at the close of each of a full day's first "
        "six bars), and 15m, 5m and 1m (decide at the close of every bar of a full 09:15-15:30 session but the last). "
        "Nothing is forecast across the night. Features at every timeframe include INDIA VIX at the same moment; "
        "NIFTY and BANKNIFTY carry each other's price for the pairs model.",
        "Features are standardised on the training window only (models/dataset.hpp Scaler). Classical models tune one "
        "hyper-parameter on the last quarter of their training window. Ensembles, all from finished stages only: Vote, "
        "Champion, Hedge, Stack (a logistic regression on every model's past out-of-sample calls), Stack (confident "
        "third / 10% / 2%), Consensus 75% / 90%. A filtered ensemble abstains otherwise: read its accuracy with its Coverage.",
        "Baselines: Coin flip (seeded), Always majority (the training up-rate), Momentum and Mean reversion (repeat or reverse "
        "the last move). The best constant call is max(up-rate, 1 - up-rate) on the same bars.",
        "Significance: z and two-sided p against 50 %, and one-sided p against the best constant call, both "
        "Bonferroni-corrected over every model on every track. Skill vs RW: 1 - RMSE(model price) / RMSE(last price).",
        "Trades: a call is acted on only when its expected move exceeds the round-trip cost of that day (futures STT 2 bp, "
        "5 bp from 2026-04-01, plus other charges and one tick). Net bp = direction x move - cost. VIX is not tradable.",
        "Data rules (see the Data sheet): seconds floored, repeated stamps keep the first bar, impossible OHLC widened and "
        "counted, intraday only on full sessions, NIFTY futures daily only with roll-crossing outcomes excluded, rows "
        "without a same-time INDIA VIX bar (or pair price) dropped, history before INDIA VIX starts (2015) not used.",
        "Visible truncations: the transformer trains by finite differences with 400 SGD steps a stage; the SVM learns from "
        "its window's latest 1,500 rows, kNN from 20,000, the HMM's Baum-Welch from 50,000, the DQN's replay from 4,096; "
        "GARCH/GJR/EGARCH fit on the latest 100,000 returns and filter over all of them. Each says so in its stage notes.",
    };

    std::error_code ec;
    fs::create_directories(out, ec);
    std::vector<XlsxSheet> sheets{summary_sheet(results, tests), bands_sheet(results), frontier_sheet(results),
                                  atlas_sheet(), data_sheet(results), method_sheet(method)};
    for (const auto& r : results) {
        if (r.ok) { sheets.push_back(track_sheet(r)); }
    }
    const auto book = altair::xlsx::build_workbook(sheets);
    if (!book) {
        std::printf("  could not build the workbook\n");
        return 1;
    }
    {
        std::ofstream f(out / "forecast_curriculum.xlsx", std::ios::binary | std::ios::trunc);
        f.write(book->data(), static_cast<std::streamsize>(book->size()));
        if (!f) { std::printf("  could not write %s\n", (out / "forecast_curriculum.xlsx").string().c_str()); return 1; }
    }
    {
        std::ofstream f(out / "forecast_curriculum.txt", std::ios::binary | std::ios::trunc);
        f << text.str();
    }
    for (const auto& r : results) {
        if (r.ok && r.log) { write_log(out / "forecast_log", r); }
    }
    std::size_t refusals = 0;
    for (const auto& r : results) { refusals += (r.ok ? r.run.lookahead_refusals : 0) + (r.bands_ok ? r.bands.lookahead_refusals : 0); }
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    std::printf("%s\n  wrote %s (%.0f s)\n", text.str().c_str(), (out / "forecast_curriculum.xlsx").string().c_str(), total);
    if (log) { std::printf("  wrote per-forecast logs (daily and hourly tracks) under %s\n", (out / "forecast_log").string().c_str()); }
    bool all_ok = !results.empty();
    for (const auto& r : results) { all_ok = all_ok && r.ok; }
    return all_ok && refusals == 0 ? 0 : 1;
}
