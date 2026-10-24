// models/aro.hpp -- artificial rabbits optimisation (ARO), a derivative-free
// minimiser for hyperparameter search.
//
// Wang, Cao, Zhang, Mirjalili & Zhao, "Artificial rabbits optimization: a new bio-
// inspired meta-heuristic algorithm for solving engineering optimization
// problems", Engineering Applications of Artificial Intelligence 114, 2022,
// 105082, doi 10.1016/j.engappai.2022.105082, as written out in Gülmez,
// "Stock price prediction with optimized deep LSTM network with artificial
// rabbits optimization algorithm", Expert
// Systems with Applications 227, 2023, 120346, doi 10.1016/j.eswa.2023.120346
// (eqs. 6-20), which uses it to pick an LSTM's layers, units, dropout,
// optimiser and learning rate (50 rabbits x 50 iterations, 8.7 days of
// compute for the 30 DJIA stocks).
//
// THE ALGORITHM. n rabbits in d dimensions, T iterations. Each iteration, per
// rabbit i, energy A = 4 (1 - t/T) ln(1/r)  (eq. 20) chooses the move:
//   * A > 1, detour foraging (eqs. 6-11): pick another rabbit j,
//       v = x_j + R (x_i - x_j) + round(0.5 (0.05 + r1)) n1,
//       R = L c,  L = (e - e^((t-1)/T)^2) sin(2 pi r2),
//     c a 0/1 mask over ceil(r3 d) randomly chosen coordinates, n1 ~ N(0,1).
//   * A <= 1, random hiding (eqs. 12-18): a burrow on one random axis k,
//       b = x_i + H g_k x_i,  H = (T - t + 1)/T * n2,
//       v = x_i + R (r4 b - x_i).
//   * Greedy: keep v only if it is better (eq. 19). v is clipped to bounds.
//
// ONE READING AND ONE BIAS. Gülmez's eq. 13 prints H with r4 but then defines
// n2 ~ N(0,1) (eq. 14) and never uses it; Wang et al.'s reference code draws
// H from the normal, which is what is done here.
// The bias: in random hiding every masked coordinate becomes
// x (1 + L (r4 (1 + H g) - 1)), a random rescaling of x itself, so the move
// pulls toward (and is only precise near) the ORIGIN. Measured in the test,
// d = 10, 3,030 evaluations: sphere and Rastrigin centred at 0 are solved to
// 1e-14 or better; the same functions with the optimum moved to 2.5 reach
// only 0.0066 and 4.98 (random search 1.21 and 31.8). Benchmarks with the
// optimum at 0 flatter this algorithm. A hyperparameter search should centre
// its box on the expected answer, or treat ARO as a good random search.
//
// WHAT IT DID ON NIFTY. Re-running Gülmez's set-up (20 past closes -> next
// close, 2018-2023, 80/20) with ARO tuning a ridge model's window and
// penalty: test MAPE 0.832 % against "no change" 0.829 % (DM p 0.54). Tuning
// does not make a forecast where there is none. The paper reports no random-
// walk baseline for its DJIA stocks.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numbers>
#include <random>
#include <vector>

namespace altair {

struct AroConfig {
    std::size_t rabbits = 50;      ///< Gülmez: 50
    std::size_t iterations = 50;   ///< Gülmez: 50
    std::uint64_t seed = 2022;
};

struct AroResult {
    std::vector<double> best;
    double value = 0.0;
    std::size_t evaluations = 0;
};

/// Minimise f over the box [lo, hi]. lo and hi must have the same size.
[[nodiscard]] inline AroResult
aro_minimise(const std::function<double(const std::vector<double>&)>& f,
             const std::vector<double>& lo, const std::vector<double>& hi,
             AroConfig cfg = {}) {
    AroResult out;
    const std::size_t d = std::min(lo.size(), hi.size()), n = cfg.rabbits;
    if (d == 0 || n < 2 || cfg.iterations == 0) { return out; }
    std::mt19937_64 rng(cfg.seed);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    std::normal_distribution<double> N(0.0, 1.0);
    const auto clip = [&](std::vector<double>& v) {
        for (std::size_t k = 0; k < d; ++k) { v[k] = std::clamp(v[k], lo[k], hi[k]); }
    };

    std::vector<std::vector<double>> x(n, std::vector<double>(d));
    std::vector<double> fx(n);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t k = 0; k < d; ++k) { x[i][k] = lo[k] + U(rng) * (hi[k] - lo[k]); }
        fx[i] = f(x[i]);
        ++out.evaluations;
    }

    const double T = static_cast<double>(cfg.iterations);
    std::vector<std::size_t> perm(d);
    std::vector<double> v(d), c(d);
    for (std::size_t it = 1; it <= cfg.iterations; ++it) {
        const double t = static_cast<double>(it);
        const double e1 = (t - 1.0) / T;
        for (std::size_t i = 0; i < n; ++i) {
            // Running operator R = L c (eqs. 7-10).
            const double L = (std::numbers::e - std::exp(e1 * e1)) * std::sin(2.0 * std::numbers::pi * U(rng));
            for (std::size_t k = 0; k < d; ++k) { perm[k] = k; }
            std::shuffle(perm.begin(), perm.end(), rng);
            const auto m = static_cast<std::size_t>(std::ceil(U(rng) * static_cast<double>(d)));
            std::fill(c.begin(), c.end(), 0.0);
            for (std::size_t l = 0; l < std::max<std::size_t>(m, 1); ++l) { c[perm[l]] = 1.0; }

            double r = U(rng);
            if (r <= 0.0) { r = 1e-300; }
            const double A = 4.0 * (1.0 - t / T) * std::log(1.0 / r);
            if (A > 1.0) {
                std::size_t j = static_cast<std::size_t>(U(rng) * static_cast<double>(n - 1));
                if (j >= i) { ++j; }
                j = std::min(j, n - 1);
                const double jump = std::round(0.5 * (0.05 + U(rng))) * N(rng);
                for (std::size_t k = 0; k < d; ++k) {
                    v[k] = x[j][k] + L * c[k] * (x[i][k] - x[j][k]) + jump;
                }
            } else {
                const double H = (T - t + 1.0) / T * N(rng);
                const std::size_t kb = std::min(d - 1, static_cast<std::size_t>(std::ceil(U(rng) * static_cast<double>(d))) - 1);
                const double r4 = U(rng);
                for (std::size_t k = 0; k < d; ++k) {
                    const double b = x[i][k] + (k == kb ? H * x[i][k] : 0.0);
                    v[k] = x[i][k] + L * c[k] * (r4 * b - x[i][k]);
                }
            }
            clip(v);
            const double fv = f(v);
            ++out.evaluations;
            if (fv < fx[i]) { x[i] = v; fx[i] = fv; }
        }
    }
    const auto best = static_cast<std::size_t>(std::min_element(fx.begin(), fx.end()) - fx.begin());
    out.best = x[best];
    out.value = fx[best];
    return out;
}

} // namespace altair
