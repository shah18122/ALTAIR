// models/meta_trader.hpp -- altair_trader's model: one stack over every base
// model's out-of-sample output, refit every day on what was known before it.
//
// THE INPUTS ARE MODEL OUTPUTS, NOTHING ELSE. Each base model of the forecast
// curriculum (models/curriculum.hpp) says, at a close, which way the next
// close goes and how sure it is. Its conviction, 2 * p_up - 1 (or +-1 when it
// gives a call without a probability), is one input; every model on every
// daily track (NIFTY, BANKNIFTY, NIFTY futures, INDIA VIX; with and without
// the VIX forecast as a feed) is a column. The target is the next session's
// return in basis points.
//
// THE MODEL is ridge regression with exponential forgetting (old days weigh
// less; regimes change), solved from sufficient statistics so a daily refit
// costs one Cholesky per penalty. Several penalties are kept side by side;
// each is scored PREQUENTIALLY -- on the day's call it made before the day's
// outcome was known -- and the call of the one with the least recent squared
// error is used. Nothing is chosen by looking at the day it trades.
//
// STEP BY STEP: predict(x) makes today's call from the weights fitted on every
// earlier outcome; learn(y) adds today's outcome once it is known (the next
// close), scores every penalty on the call it made, and refits. A call is
// "ready" only after `min_days` outcomes.
//
// What the caller does with the call -- trade when the expected move beats
// the round trip's expenses -- is in app/trader_main.cpp. No threshold is
// tuned there either: the expenses are the exchange's, the move is the model's.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace altair::meta {

struct MetaConfig {
    std::vector<double> lambdas{1.0, 10.0, 100.0, 1000.0};   ///< ridge penalties kept side by side
    double half_life_days = 250.0;   ///< a day this old weighs half as much as today
    std::size_t min_days = 250;      ///< no call before this many outcomes
    std::size_t score_days = 60;     ///< a penalty is chosen only after this many scored calls
};

/// Ridge regression from decayed sufficient statistics. Column 0 is the
/// intercept (penalised a thousand times less than the rest).
class DecayedRidge {
public:
    DecayedRidge(std::size_t p, double decay) : p_(p), decay_(decay), a_(p * p, 0.0), b_(p, 0.0) {}

    void add(const std::vector<double>& x, double y) {
        for (double& v : a_) v *= decay_;
        for (double& v : b_) v *= decay_;
        for (std::size_t i = 0; i < p_; ++i) {
            if (x[i] == 0.0) continue;
            double* row = &a_[i * p_];
            for (std::size_t j = 0; j < p_; ++j) row[j] += x[i] * x[j];
            b_[i] += x[i] * y;
        }
        weight_ = weight_ * decay_ + 1.0;
    }

    /// (A + lambda D) w = b by Cholesky; false when not positive definite.
    [[nodiscard]] bool solve(double lambda, std::vector<double>& w) const {
        std::vector<double> l(a_);
        for (std::size_t i = 0; i < p_; ++i) l[i * p_ + i] += i == 0 ? lambda * 1e-3 : lambda;
        for (std::size_t j = 0; j < p_; ++j) {
            double d = l[j * p_ + j];
            for (std::size_t k = 0; k < j; ++k) d -= l[j * p_ + k] * l[j * p_ + k];
            if (!(d > 0.0)) return false;
            d = std::sqrt(d);
            l[j * p_ + j] = d;
            for (std::size_t i = j + 1; i < p_; ++i) {
                double s = l[i * p_ + j];
                for (std::size_t k = 0; k < j; ++k) s -= l[i * p_ + k] * l[j * p_ + k];
                l[i * p_ + j] = s / d;
            }
        }
        w.assign(p_, 0.0);
        std::vector<double> z(p_);
        for (std::size_t i = 0; i < p_; ++i) {
            double s = b_[i];
            for (std::size_t k = 0; k < i; ++k) s -= l[i * p_ + k] * z[k];
            z[i] = s / l[i * p_ + i];
        }
        for (std::size_t i = p_; i-- > 0;) {
            double s = z[i];
            for (std::size_t k = i + 1; k < p_; ++k) s -= l[k * p_ + i] * w[k];
            w[i] = s / l[i * p_ + i];
        }
        return true;
    }
    [[nodiscard]] double effective_days() const noexcept { return weight_; }

private:
    std::size_t p_;
    double decay_;
    std::vector<double> a_, b_;
    double weight_ = 0.0;
};

/// One day's call.
struct MetaCall {
    bool ready = false;      ///< enough outcomes behind it
    double mu = 0.0;         ///< expected next-session return, bp
    double se = 0.0;         ///< its recent prequential error, bp (root mean square)
    double lambda = 0.0;     ///< the penalty whose call this is
    std::size_t days = 0;    ///< outcomes learned so far
};

class MetaModel {
public:
    /// `features`: inputs per day, without the intercept.
    MetaModel(std::size_t features, MetaConfig cfg = {})
        : cfg_(std::move(cfg)), p_(features + 1),
          decay_(std::pow(0.5, 1.0 / std::max(1.0, cfg_.half_life_days))),
          ridge_(p_, decay_),
          w_(cfg_.lambdas.size(), std::vector<double>(p_, 0.0)),
          fitted_(cfg_.lambdas.size(), false),
          pred_(cfg_.lambdas.size(), 0.0),
          sse_(cfg_.lambdas.size(), 0.0), sw_(cfg_.lambdas.size(), 0.0), scored_(cfg_.lambdas.size(), 0) {}

    /// Today's call from today's inputs, on weights fitted before today.
    [[nodiscard]] MetaCall predict(const std::vector<double>& x) {
        x_.assign(p_, 0.0);
        x_[0] = 1.0;
        for (std::size_t i = 0; i < x.size() && i + 1 < p_; ++i) x_[i + 1] = std::isfinite(x[i]) ? x[i] : 0.0;
        for (std::size_t k = 0; k < w_.size(); ++k) {
            double s = 0.0;
            for (std::size_t i = 0; i < p_; ++i) s += w_[k][i] * x_[i];
            pred_[k] = fitted_[k] ? s : 0.0;
        }
        pending_ = true;
        MetaCall c;
        c.days = days_;
        const std::size_t k = best();
        if (k >= w_.size()) return c;
        chosen_ = k;
        c.lambda = cfg_.lambdas[k];
        c.mu = pred_[k];
        c.se = sw_[k] > 0.0 ? std::sqrt(sse_[k] / sw_[k]) : 0.0;
        c.ready = days_ >= cfg_.min_days;
        return c;
    }

    /// The outcome of the day last predicted (bp): every penalty is scored on
    /// the call it made, then the row is learned and every penalty refitted.
    void learn(double y) {
        if (!pending_ || !std::isfinite(y)) { pending_ = false; return; }
        pending_ = false;
        for (std::size_t k = 0; k < w_.size(); ++k) {
            if (!fitted_[k]) continue;
            const double e = pred_[k] - y;
            sse_[k] = sse_[k] * decay_ + e * e;
            sw_[k] = sw_[k] * decay_ + 1.0;
            ++scored_[k];
        }
        ridge_.add(x_, y);
        ++days_;
        for (std::size_t k = 0; k < w_.size(); ++k) fitted_[k] = ridge_.solve(cfg_.lambdas[k], w_[k]) || fitted_[k];
    }

    /// The weights behind the last call (intercept first); empty before one.
    [[nodiscard]] std::vector<double> weights() const {
        return chosen_ < w_.size() ? w_[chosen_] : std::vector<double>{};
    }
    [[nodiscard]] std::size_t days() const noexcept { return days_; }

private:
    /// The penalty with the least recent prequential error among those scored
    /// long enough; before any is, the middle one (its calls are not ready).
    [[nodiscard]] std::size_t best() const {
        std::size_t k = w_.size();
        double e = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < w_.size(); ++i) {
            if (!fitted_[i] || scored_[i] < cfg_.score_days || !(sw_[i] > 0.0)) continue;
            const double m = sse_[i] / sw_[i];
            if (m < e) { e = m; k = i; }
        }
        if (k == w_.size() && !w_.empty() && fitted_[w_.size() / 2]) k = w_.size() / 2;
        return k;
    }

    MetaConfig cfg_;
    std::size_t p_;
    double decay_;
    DecayedRidge ridge_;
    std::vector<std::vector<double>> w_;
    std::vector<bool> fitted_;
    std::vector<double> pred_, sse_, sw_;
    std::vector<std::size_t> scored_;
    std::vector<double> x_;
    bool pending_ = false;
    std::size_t days_ = 0;
    std::size_t chosen_ = static_cast<std::size_t>(-1);
};

} // namespace altair::meta
