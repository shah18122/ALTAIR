// models/arima_hybrid.hpp -- Zhang's ARIMA + neural-network hybrid.
//
// Zhang, "Time series forecasting using a hybrid ARIMA and neural network
// model", Neurocomputing 50, 2003, 159-175, doi 10.1016/S0925-2312(01)00702-0.
// Reviewed by Vishwas, Usha & Sangeetha, "Artificial intelligence-based stock
// market price prediction: a review", Mathematical Models in Engineering,
// 2025, doi 10.21595/mme.2025.24861 (via Alshawarbeh et al., its ref. [17]),
// and listed by Charithra et al. (forecast_metrics.hpp) among its references.
//
// THE MODEL. A series is a linear part plus a nonlinear part, y_t = L_t + N_t.
//   1. Fit an ARIMA; its one-step forecast is L^_t and its residual
//      e_t = y_t - L^_t is what the linear model could not explain.
//   2. Fit a feed-forward network to e_t from its own lags,
//      N^_t = f(e_{t-1}, ..., e_{t-n}).
//   3. The forecast is L^_t + N^_t.
// Here the series is the daily log return, so ARIMA(p, 1, q) on the log price
// is ARMA(p, q) on the return (models/time_series.hpp fits it). The network
// has one tanh hidden layer and a linear output, as Zhang's, trained by full-
// batch Adam on residuals standardised on the training rows, from a fixed
// seed so the result is repeatable.
//
// WHAT TO EXPECT. Stage 2 can only help when the residuals depend on their
// own past in a way a linear model misses. The test builds such a series and
// the hybrid wins there. On NIFTY daily returns it does not: neither stage
// beats "no change" out of sample (see test_arima_hybrid.cpp and ops/research-
// ideas.md 2j). The review quotes no number for this hybrid on Indian data.

#pragma once

#include <models/time_series.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

namespace altair {

struct ArimaHybridConfig {
    std::size_t p = 1, q = 0;      ///< ARMA orders on the return
    std::size_t lags = 4;          ///< network inputs: e_{t-1} .. e_{t-lags}
    std::size_t hidden = 6;
    std::size_t epochs = 400;
    double rate = 0.01;            ///< Adam step
    double decay = 1e-4;           ///< L2 on the network weights
    std::uint64_t seed = 2003;
};

struct ArimaHybridForecast {
    double linear = 0.0;   ///< ARMA alone
    double hybrid = 0.0;   ///< ARMA plus the network's residual forecast
};

class ArimaAnnHybrid {
public:
    explicit ArimaAnnHybrid(ArimaHybridConfig cfg = {}) : cfg_(cfg) {}

    /// Fit both stages on a return series. False when the ARMA cannot be
    /// fitted (too short, non-stationary) or too few residuals remain.
    bool fit(std::span<const double> y) {
        fitted_ = false;
        auto arma = fit_arma(y, cfg_.p, cfg_.q);
        if (!arma || cfg_.lags == 0 || cfg_.hidden == 0) { return false; }
        arma_ = *arma;
        std::size_t start = 0;
        for (auto l : arma_.ar_lags) { start = std::max(start, l); }
        for (auto l : arma_.ma_lags) { start = std::max(start, l); }
        const auto& e = arma_.residuals;
        if (e.size() < start + cfg_.lags + 10 * cfg_.hidden) { return false; }

        double s1 = 0.0, s2 = 0.0;
        for (std::size_t t = start; t < e.size(); ++t) { s1 += e[t]; s2 += e[t] * e[t]; }
        const double m = static_cast<double>(e.size() - start);
        mean_ = s1 / m;
        sd_ = std::sqrt(std::max(s2 / m - mean_ * mean_, 1e-300));

        std::vector<std::vector<double>> x;
        std::vector<double> target;
        for (std::size_t t = start + cfg_.lags; t < e.size(); ++t) {
            std::vector<double> row(cfg_.lags);
            for (std::size_t k = 0; k < cfg_.lags; ++k) { row[k] = (e[t - 1 - k] - mean_) / sd_; }
            x.push_back(std::move(row));
            target.push_back((e[t] - mean_) / sd_);
        }
        train(x, target);
        history_.assign(y.begin(), y.end());
        resid_ = e;
        fitted_ = true;
        return true;
    }

    /// One-step forecast of the next value, from everything seen so far.
    [[nodiscard]] ArimaHybridForecast forecast() const {
        if (!fitted_) { return {}; }
        ArimaHybridForecast f;
        f.linear = linear_next();
        std::vector<double> in(cfg_.lags);
        for (std::size_t k = 0; k < cfg_.lags; ++k) {
            in[k] = (resid_[resid_.size() - 1 - k] - mean_) / sd_;
        }
        f.hybrid = f.linear + mean_ + sd_ * net(in);
        return f;
    }

    /// Append the realised value; parameters stay fixed (walk-forward).
    void update(double y) {
        if (!fitted_) { return; }
        const double l = linear_next();
        history_.push_back(y);
        resid_.push_back(y - l);
    }

    [[nodiscard]] bool fitted() const noexcept { return fitted_; }
    /// Standard deviation of the ARMA's training residuals.
    [[nodiscard]] double residual_sd() const noexcept {
        return std::sqrt(std::max(0.0, arma_.residual_variance));
    }
    [[nodiscard]] std::size_t observed() const noexcept { return history_.size(); }

private:
    ArimaHybridConfig cfg_;
    LagModel arma_;
    std::vector<double> history_, resid_;
    double mean_ = 0.0, sd_ = 1.0;
    // Network: w1 [hidden x lags], b1 [hidden], w2 [hidden], b2.
    std::vector<double> w1_, b1_, w2_;
    double b2_ = 0.0;
    bool fitted_ = false;

    [[nodiscard]] double linear_next() const {
        double v = arma_.intercept;
        for (std::size_t i = 0; i < arma_.ar.size(); ++i) {
            v += arma_.ar[i] * history_[history_.size() - arma_.ar_lags[i]];
        }
        for (std::size_t i = 0; i < arma_.ma.size(); ++i) {
            v += arma_.ma[i] * resid_[resid_.size() - arma_.ma_lags[i]];
        }
        return v;
    }

    [[nodiscard]] double net(const std::vector<double>& in) const {
        double out = b2_;
        for (std::size_t h = 0; h < cfg_.hidden; ++h) {
            double a = b1_[h];
            for (std::size_t k = 0; k < cfg_.lags; ++k) { a += w1_[h * cfg_.lags + k] * in[k]; }
            out += w2_[h] * std::tanh(a);
        }
        return out;
    }

    void train(const std::vector<std::vector<double>>& x, const std::vector<double>& y) {
        const std::size_t H = cfg_.hidden, L = cfg_.lags, n = x.size();
        std::mt19937_64 rng(cfg_.seed);
        std::normal_distribution<double> N(0.0, 1.0);
        w1_.resize(H * L); b1_.assign(H, 0.0); w2_.resize(H); b2_ = 0.0;
        for (auto& w : w1_) { w = N(rng) / std::sqrt(static_cast<double>(L)); }
        for (auto& w : w2_) { w = N(rng) / std::sqrt(static_cast<double>(H)); }

        // Adam over the flattened parameters [w1, b1, w2, b2].
        const std::size_t P = H * L + H + H + 1;
        std::vector<double> m(P, 0.0), v(P, 0.0), g(P), hid(H);
        const double b1 = 0.9, b2 = 0.999, eps = 1e-8;
        for (std::size_t epoch = 1; epoch <= cfg_.epochs; ++epoch) {
            std::fill(g.begin(), g.end(), 0.0);
            for (std::size_t i = 0; i < n; ++i) {
                double out = b2_;
                for (std::size_t h = 0; h < H; ++h) {
                    double a = b1_[h];
                    for (std::size_t k = 0; k < L; ++k) { a += w1_[h * L + k] * x[i][k]; }
                    hid[h] = std::tanh(a);
                    out += w2_[h] * hid[h];
                }
                const double d = 2.0 * (out - y[i]) / static_cast<double>(n);
                for (std::size_t h = 0; h < H; ++h) {
                    g[H * L + H + h] += d * hid[h];
                    const double dh = d * w2_[h] * (1.0 - hid[h] * hid[h]);
                    g[H * L + h] += dh;
                    for (std::size_t k = 0; k < L; ++k) { g[h * L + k] += dh * x[i][k]; }
                }
                g[P - 1] += d;
            }
            for (std::size_t j = 0; j < H * L; ++j) { g[j] += 2.0 * cfg_.decay * w1_[j]; }
            for (std::size_t h = 0; h < H; ++h) { g[H * L + H + h] += 2.0 * cfg_.decay * w2_[h]; }
            const double c1 = 1.0 - std::pow(b1, static_cast<double>(epoch));
            const double c2 = 1.0 - std::pow(b2, static_cast<double>(epoch));
            for (std::size_t j = 0; j < P; ++j) {
                m[j] = b1 * m[j] + (1.0 - b1) * g[j];
                v[j] = b2 * v[j] + (1.0 - b2) * g[j] * g[j];
                const double step = cfg_.rate * (m[j] / c1) / (std::sqrt(v[j] / c2) + eps);
                if (j < H * L) { w1_[j] -= step; }
                else if (j < H * L + H) { b1_[j - H * L] -= step; }
                else if (j < P - 1) { w2_[j - H * L - H] -= step; }
                else { b2_ -= step; }
            }
        }
    }
};

} // namespace altair
