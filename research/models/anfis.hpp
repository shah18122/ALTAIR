// research/models/anfis.hpp -- a first-order Sugeno neuro-fuzzy model (ANFIS).
//
// Jang, "ANFIS: adaptive-network-based fuzzy inference system", IEEE SMC
// 23(3), 1993, as used by Atsalakis & Valavanis, "Forecasting stock market
// short-term trends using a neuro-fuzzy based methodology", Expert Systems
// with Applications 36(7), 2009, 10696-10707, doi 10.1016/j.eswa.2009.02.043.
//
// THE MODEL. Each input gets `mfs` Gaussian membership functions; every
// combination of one function per input is a rule, so there are mfs^inputs
// rules. A rule's firing strength is the product of its memberships, and the
// output is the strength-weighted average of the rules' linear functions of
// the inputs. Training is Jang's hybrid rule, once per epoch:
//   * with the memberships fixed, the linear consequents are the least-squares
//     solution (a small ridge keeps it defined where a rule never fires);
//   * with the consequents fixed, the centres and widths take one gradient
//     step on the squared error, normalised to length `step` (as MATLAB's
//     anfis does).
//
// THE PAPER'S SET-UP (its PR-ANFIS "plant"): three inputs, three membership
// functions each, 27 rules, 100 epochs, step 0.1, next-day direction of the
// close. Its third input u(k) comes from a second ANFIS (the "controller")
// driven by the rate of change of a 3-day moving average. Here that input is
// the causal rate of change of the 3-day average itself (built by the
// caller), which is what the controller is trained to reproduce.
//
// TWO CHOICES THE PAPER DOES NOT PIN DOWN.
//   * Initial centres sit at evenly spaced quantiles of each input, not
//     evenly over its range: one -13 % day (March 2020) would otherwise leave
//     two of three functions with almost no data.
//   * Gaussian, not the paper's two-sided "Gaussian-2", functions (which it
//     found gave a lower RMSE than bell and triangular ones). With its two
//     centres equal a Gaussian-2 is a Gaussian; the flat top is left out.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace altair {

struct AnfisConfig {
    std::size_t mfs = 3;        ///< membership functions per input
    std::size_t epochs = 100;
    double step = 0.1;          ///< length of each premise gradient step
    double ridge = 1e-6;        ///< on the consequent least squares
};

class Anfis {
public:
    explicit Anfis(AnfisConfig cfg = {}) : cfg_(cfg) {}

    /// Fit on rows `x` (all the same width) and targets `y`. Returns the
    /// training RMSE after the last epoch, or a negative number when the data
    /// cannot be fitted (empty, ragged, or more parameters than rows).
    double fit(const std::vector<std::vector<double>>& x, const std::vector<double>& y) {
        const std::size_t n = std::min(x.size(), y.size());
        if (n == 0 || cfg_.mfs < 1) { return -1.0; }
        d_ = x[0].size();
        for (std::size_t i = 0; i < n; ++i) { if (x[i].size() != d_) { return -1.0; } }
        rules_ = 1;
        for (std::size_t j = 0; j < d_; ++j) { rules_ *= cfg_.mfs; }
        const std::size_t p = rules_ * (d_ + 1);
        if (d_ == 0 || n <= p) { return -1.0; }

        // Inputs are standardised on the training rows, so that `step` is in
        // standard deviations whatever the input's units (a 0.1 step on raw
        // daily returns would throw every centre ten sigmas away).
        mean_.assign(d_, 0.0);
        inv_sd_.assign(d_, 1.0);
        for (std::size_t j = 0; j < d_; ++j) {
            double s1 = 0.0, s2 = 0.0;
            for (std::size_t i = 0; i < n; ++i) { s1 += x[i][j]; s2 += x[i][j] * x[i][j]; }
            const double m = s1 / static_cast<double>(n);
            const double v = s2 / static_cast<double>(n) - m * m;
            mean_[j] = m;
            inv_sd_[j] = v > 0.0 ? 1.0 / std::sqrt(v) : 1.0;
        }
        std::vector<std::vector<double>> z(n, std::vector<double>(d_));
        for (std::size_t i = 0; i < n; ++i) { z[i] = scaled(x[i]); }
        return fit_scaled(z, y, n, p);
    }

    [[nodiscard]] double predict(const std::vector<double>& x) const {
        if (!fitted_ || x.size() != d_) { return 0.0; }
        const auto z = scaled(x);
        std::vector<double> mu(d_ * cfg_.mfs), wbar(rules_);
        strengths(z, mu, wbar);
        double f = 0.0;
        for (std::size_t r = 0; r < rules_; ++r) { f += wbar[r] * rule_output(r, z); }
        return f;
    }

    [[nodiscard]] bool fitted() const noexcept { return fitted_; }
    [[nodiscard]] std::size_t rules() const noexcept { return rules_; }

private:
    AnfisConfig cfg_;
    std::size_t d_ = 0, rules_ = 0;
    std::vector<double> mean_, inv_sd_, c_, s_, theta_;
    bool fitted_ = false;

    [[nodiscard]] std::vector<double> scaled(const std::vector<double>& x) const {
        std::vector<double> z(d_);
        for (std::size_t j = 0; j < d_; ++j) { z[j] = (x[j] - mean_[j]) * inv_sd_[j]; }
        return z;
    }

    double fit_scaled(const std::vector<std::vector<double>>& x, const std::vector<double>& y,
                      std::size_t n, std::size_t p) {

        // Centres at quantiles, widths from their spacing.
        c_.assign(d_ * cfg_.mfs, 0.0);
        s_.assign(d_ * cfg_.mfs, 1.0);
        std::vector<double> col(n);
        for (std::size_t j = 0; j < d_; ++j) {
            for (std::size_t i = 0; i < n; ++i) { col[i] = x[i][j]; }
            std::sort(col.begin(), col.end());
            const double lo = col[n / 20], hi = col[n - 1 - n / 20];
            const double width = cfg_.mfs > 1 ? (hi - lo) / static_cast<double>(cfg_.mfs - 1) : (hi - lo);
            for (std::size_t m = 0; m < cfg_.mfs; ++m) {
                const double q = cfg_.mfs > 1 ? static_cast<double>(m) / static_cast<double>(cfg_.mfs - 1) : 0.5;
                c_[j * cfg_.mfs + m] = col[static_cast<std::size_t>(q * static_cast<double>(n - 1))];
                s_[j * cfg_.mfs + m] = std::max(width / 2.0, 1e-12);
            }
        }

        theta_.assign(p, 0.0);
        std::vector<double> wbar(rules_), mu(d_ * cfg_.mfs);
        double rmse = 0.0;
        for (std::size_t epoch = 0; epoch <= cfg_.epochs; ++epoch) {
            // Consequents by least squares: rows [wbar_r * x, wbar_r].
            std::vector<double> ata(p * p, 0.0), aty(p, 0.0), row(p);
            for (std::size_t i = 0; i < n; ++i) {
                strengths(x[i], mu, wbar);
                for (std::size_t r = 0; r < rules_; ++r) {
                    for (std::size_t j = 0; j < d_; ++j) { row[r * (d_ + 1) + j] = wbar[r] * x[i][j]; }
                    row[r * (d_ + 1) + d_] = wbar[r];
                }
                for (std::size_t a = 0; a < p; ++a) {
                    if (row[a] == 0.0) { continue; }
                    aty[a] += row[a] * y[i];
                    for (std::size_t b = a; b < p; ++b) { ata[a * p + b] += row[a] * row[b]; }
                }
            }
            for (std::size_t a = 0; a < p; ++a) {
                for (std::size_t b = 0; b < a; ++b) { ata[a * p + b] = ata[b * p + a]; }
                ata[a * p + a] += cfg_.ridge * static_cast<double>(n);
            }
            if (!solve(ata, aty, p)) { return -1.0; }
            theta_ = aty;

            // Error, and the gradient of the summed squared error w.r.t. the
            // centres and widths.
            std::vector<double> gc(c_.size(), 0.0), gs(s_.size(), 0.0);
            double sse = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                strengths(x[i], mu, wbar);
                double f = 0.0;
                std::vector<double> yr(rules_);
                for (std::size_t r = 0; r < rules_; ++r) {
                    yr[r] = rule_output(r, x[i]);
                    f += wbar[r] * yr[r];
                }
                const double e = f - y[i];
                sse += e * e;
                if (epoch == cfg_.epochs) { continue; }
                // df/dmu_{j,m} * mu_{j,m} = sum over rules using (j, m) of (y_r - f) wbar_r.
                for (std::size_t r = 0; r < rules_; ++r) {
                    const double g = 2.0 * e * (yr[r] - f) * wbar[r];
                    if (g == 0.0) { continue; }
                    std::size_t code = r;
                    for (std::size_t j = 0; j < d_; ++j) {
                        const std::size_t k = j * cfg_.mfs + code % cfg_.mfs;
                        code /= cfg_.mfs;
                        const double z = x[i][j] - c_[k], s = s_[k];
                        gc[k] += g * z / (s * s);
                        gs[k] += g * z * z / (s * s * s);
                    }
                }
            }
            rmse = std::sqrt(sse / static_cast<double>(n));
            if (epoch == cfg_.epochs) { break; }
            double norm = 0.0;
            for (double v : gc) { norm += v * v; }
            for (double v : gs) { norm += v * v; }
            norm = std::sqrt(norm);
            if (!(norm > 0.0)) { break; }
            for (std::size_t k = 0; k < c_.size(); ++k) {
                c_[k] -= cfg_.step * gc[k] / norm;
                s_[k] = std::max(1e-12, s_[k] - cfg_.step * gs[k] / norm);
            }
        }
        fitted_ = true;
        return rmse;
    }

    void strengths(const std::vector<double>& x, std::vector<double>& mu,
                   std::vector<double>& wbar) const {
        for (std::size_t j = 0; j < d_; ++j) {
            for (std::size_t m = 0; m < cfg_.mfs; ++m) {
                const std::size_t k = j * cfg_.mfs + m;
                const double z = (x[j] - c_[k]) / s_[k];
                mu[k] = std::exp(-0.5 * z * z);
            }
        }
        double total = 0.0;
        for (std::size_t r = 0; r < rules_; ++r) {
            double w = 1.0;
            std::size_t code = r;
            for (std::size_t j = 0; j < d_; ++j) {
                w *= mu[j * cfg_.mfs + code % cfg_.mfs];
                code /= cfg_.mfs;
            }
            wbar[r] = w;
            total += w;
        }
        // Far outside every function: fall back to equal weights rather than 0/0.
        for (double& w : wbar) { w = total > 1e-300 ? w / total : 1.0 / static_cast<double>(rules_); }
    }

    [[nodiscard]] double rule_output(std::size_t r, const std::vector<double>& x) const {
        const double* t = &theta_[r * (d_ + 1)];
        double v = t[d_];
        for (std::size_t j = 0; j < d_; ++j) { v += t[j] * x[j]; }
        return v;
    }

    /// Gaussian elimination with partial pivoting; the answer replaces b.
    static bool solve(std::vector<double>& a, std::vector<double>& b, std::size_t p) {
        for (std::size_t col = 0; col < p; ++col) {
            std::size_t piv = col;
            for (std::size_t r = col + 1; r < p; ++r) {
                if (std::fabs(a[r * p + col]) > std::fabs(a[piv * p + col])) { piv = r; }
            }
            if (!(std::fabs(a[piv * p + col]) > 1e-300)) { return false; }
            if (piv != col) {
                for (std::size_t k = 0; k < p; ++k) { std::swap(a[col * p + k], a[piv * p + k]); }
                std::swap(b[col], b[piv]);
            }
            for (std::size_t r = col + 1; r < p; ++r) {
                const double f = a[r * p + col] / a[col * p + col];
                if (f == 0.0) { continue; }
                for (std::size_t k = col; k < p; ++k) { a[r * p + k] -= f * a[col * p + k]; }
                b[r] -= f * b[col];
            }
        }
        for (std::size_t col = p; col-- > 0;) {
            double v = b[col];
            for (std::size_t k = col + 1; k < p; ++k) { v -= a[col * p + k] * b[k]; }
            b[col] = v / a[col * p + col];
        }
        return true;
    }
};

} // namespace altair
