// analytics/hmm.hpp -- a hidden Markov model, and why it is not the chain
// already in models/markov.hpp.
//
// P14-06.
//
// OBSERVABLE VERSUS HIDDEN, WHICH IS THE ENTIRE DIFFERENCE.
//
// `models/markov.hpp` fits a chain whose states ARE the observations: bucket
// today's return, count transitions, done. It is exact, it needs no
// optimisation, and it can only ever say something about the buckets you chose.
//
// An HMM says the states are LATENT. What you see is a draw from a
// distribution whose parameters depend on a state you never observe, and both
// the states and the transitions have to be inferred together. That buys the
// thing the observable chain structurally cannot have: a "calm" state and a
// "stressed" state can both produce a return of -0.3%, and the model can still
// tell them apart using what came before and after.
//
// It costs three things, and each has bitten someone:
//
//   1. BAUM-WELCH FINDS A LOCAL OPTIMUM. Different seeds give different
//      models. `fit_hmm` therefore takes a seed and runs several restarts,
//      keeping the best likelihood -- and reports how many distinct optima it
//      found, because a run where every restart landed somewhere different is
//      a run whose answer is an accident.
//   2. LABEL SWITCHING. State 0 and state 1 are exchangeable; nothing in the
//      likelihood distinguishes them. So the fitted states are SORTED by
//      variance before being returned, making "state 0 is the calm one" true
//      by construction rather than by luck. Without that, a regime label is
//      not comparable across two runs, let alone across two days.
//   3. IT ALWAYS FITS. A two-state HMM on pure white noise returns two states
//      and a transition matrix, and they look plausible. The test therefore
//      runs it on noise and reads the separation, rather than admiring the
//      output on data that has regimes in it.
//
// SCALED FORWARD-BACKWARD, NOT LOG-SUM-EXP.
//
// The forward variables underflow to zero within a few hundred observations at
// double precision -- they are products of probabilities. Scaling each column
// to sum to one and accumulating the log of the scale factors is the standard
// fix, it is cheaper than log-sum-exp, and the accumulated log-scale IS the
// log-likelihood, which comes out for free.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <vector>

namespace altair {

enum class HmmError : std::uint8_t {
    TooFewSamples,
    BadParameter,
    /// The series has no variation, so no state can be distinguished.
    Degenerate,
    /// Every restart failed to produce a finite likelihood.
    DidNotConverge
};

/// A Gaussian-emission HMM with `k` states, fitted on a scalar series.
struct Hmm {
    std::size_t k = 0;
    std::vector<double> pi;        ///< k, initial state distribution
    std::vector<double> a;         ///< k*k transition, row-major
    std::vector<double> mu;        ///< k emission means
    std::vector<double> sigma;     ///< k emission standard deviations
    double log_likelihood = 0.0;
    /// How many of the restarts converged to (essentially) this likelihood.
    /// A 1 here means the answer is one local optimum out of many.
    std::size_t agreeing_restarts = 0;
    std::size_t restarts = 0;

    [[nodiscard]] double trans(std::size_t i, std::size_t j) const noexcept {
        return a[i * k + j];
    }
    /// Expected dwell in state i, 1 / (1 - a_ii). THE number that says whether
    /// a fitted regime is tradeable: a state you leave every 1.4 bars is a
    /// relabelling of noise whatever its likelihood.
    [[nodiscard]] double expected_dwell(std::size_t i) const noexcept {
        const double s = trans(i, i);
        return s < 1.0 ? 1.0 / (1.0 - s) : 1e18;
    }
};

namespace detail {

[[nodiscard]] inline double gauss(double x, double m, double s) noexcept {
    if (!(s > 1e-300)) { return 0.0; }
    const double z = (x - m) / s;
    return std::exp(-0.5 * z * z) / (s * 2.5066282746310002);
}

/// One Baum-Welch run from a given start. Returns the log-likelihood.
[[nodiscard]] inline double baum_welch(const std::vector<double>& x, Hmm& h,
                                       std::size_t iters) {
    const std::size_t n = x.size(), k = h.k;
    std::vector<double> alpha(n * k), beta(n * k), scale(n), gam(n * k),
        xi(k * k);
    double ll = 0.0;

    for (std::size_t it = 0; it < iters; ++it) {
        // ---- forward, SCALED --------------------------------------------
        ll = 0.0;
        for (std::size_t j = 0; j < k; ++j) {
            alpha[j] = h.pi[j] * gauss(x[0], h.mu[j], h.sigma[j]);
        }
        double s = 0.0;
        for (std::size_t j = 0; j < k; ++j) { s += alpha[j]; }
        if (!(s > 0.0)) { return -1e300; }
        scale[0] = s;
        for (std::size_t j = 0; j < k; ++j) { alpha[j] /= s; }
        ll += std::log(s);

        for (std::size_t t = 1; t < n; ++t) {
            s = 0.0;
            for (std::size_t j = 0; j < k; ++j) {
                double acc = 0.0;
                for (std::size_t i = 0; i < k; ++i) {
                    acc += alpha[(t - 1) * k + i] * h.trans(i, j);
                }
                alpha[t * k + j] = acc * gauss(x[t], h.mu[j], h.sigma[j]);
                s += alpha[t * k + j];
            }
            if (!(s > 0.0)) { return -1e300; }
            scale[t] = s;
            for (std::size_t j = 0; j < k; ++j) { alpha[t * k + j] /= s; }
            ll += std::log(s);
        }

        // ---- backward, using the SAME scale factors ----------------------
        for (std::size_t j = 0; j < k; ++j) { beta[(n - 1) * k + j] = 1.0; }
        for (std::size_t t = n - 1; t-- > 0;) {
            for (std::size_t i = 0; i < k; ++i) {
                double acc = 0.0;
                for (std::size_t j = 0; j < k; ++j) {
                    acc += h.trans(i, j)
                         * gauss(x[t + 1], h.mu[j], h.sigma[j])
                         * beta[(t + 1) * k + j];
                }
                beta[t * k + i] = acc / scale[t + 1];
            }
        }

        // ---- E step ------------------------------------------------------
        for (std::size_t t = 0; t < n; ++t) {
            double sum = 0.0;
            for (std::size_t j = 0; j < k; ++j) {
                gam[t * k + j] = alpha[t * k + j] * beta[t * k + j];
                sum += gam[t * k + j];
            }
            if (sum > 0.0) {
                for (std::size_t j = 0; j < k; ++j) { gam[t * k + j] /= sum; }
            }
        }
        std::fill(xi.begin(), xi.end(), 0.0);
        for (std::size_t t = 0; t + 1 < n; ++t) {
            double sum = 0.0;
            std::vector<double> tmp(k * k, 0.0);
            for (std::size_t i = 0; i < k; ++i) {
                for (std::size_t j = 0; j < k; ++j) {
                    tmp[i * k + j] = alpha[t * k + i] * h.trans(i, j)
                        * gauss(x[t + 1], h.mu[j], h.sigma[j])
                        * beta[(t + 1) * k + j];
                    sum += tmp[i * k + j];
                }
            }
            if (sum > 0.0) {
                for (std::size_t m = 0; m < k * k; ++m) {
                    xi[m] += tmp[m] / sum;
                }
            }
        }

        // ---- M step ------------------------------------------------------
        for (std::size_t j = 0; j < k; ++j) { h.pi[j] = gam[j]; }
        for (std::size_t i = 0; i < k; ++i) {
            double denom = 0.0;
            for (std::size_t j = 0; j < k; ++j) { denom += xi[i * k + j]; }
            for (std::size_t j = 0; j < k; ++j) {
                h.a[i * k + j] = denom > 0.0 ? xi[i * k + j] / denom
                                             : 1.0 / static_cast<double>(k);
            }
        }
        for (std::size_t j = 0; j < k; ++j) {
            double wsum = 0.0, msum = 0.0;
            for (std::size_t t = 0; t < n; ++t) {
                wsum += gam[t * k + j];
                msum += gam[t * k + j] * x[t];
            }
            if (!(wsum > 0.0)) { continue; }
            h.mu[j] = msum / wsum;
            double vsum = 0.0;
            for (std::size_t t = 0; t < n; ++t) {
                const double d = x[t] - h.mu[j];
                vsum += gam[t * k + j] * d * d;
            }
            // FLOORED, not left free. An unfloored variance collapses onto a
            // single observation, the likelihood goes to infinity, and the
            // model reports a spectacular fit to one data point.
            h.sigma[j] = std::max(std::sqrt(vsum / wsum), 1e-9);
        }
    }
    return ll;
}

} // namespace detail

/// Fit by Baum-Welch with multiple restarts.
[[nodiscard]] inline std::expected<Hmm, HmmError>
fit_hmm(const std::vector<double>& x, std::size_t k, std::uint64_t seed,
        std::size_t restarts = 8, std::size_t iters = 120) {
    if (x.size() < 100) { return std::unexpected(HmmError::TooFewSamples); }
    if (k < 2 || k > 6) { return std::unexpected(HmmError::BadParameter); }

    double mu = 0.0;
    for (const double v : x) { mu += v; }
    mu /= static_cast<double>(x.size());
    double sd = 0.0;
    for (const double v : x) { sd += (v - mu) * (v - mu); }
    sd = std::sqrt(sd / static_cast<double>(x.size()));
    // RELATIVE, not `sd > 0`. A vector of 500 identical 0.01s does not have
    // zero sample standard deviation in floating point: accumulating the mean
    // leaves a rounding residue, every deviation comes out around 9e-18, and
    // `sd > 0.0` passes. The model then fits two states to a constant. The
    // scale has to be the data's own, because 9e-18 is enormous for a series
    // of nanometres and nothing for a series of paise.
    const double scale = std::fabs(mu) > 0.0 ? std::fabs(mu) : 1.0;
    if (!(sd > 1e-12 * scale)) {
        return std::unexpected(HmmError::Degenerate);
    }

    std::uint64_t s = seed != 0 ? seed : 0x9E3779B9ull;
    auto rnd = [&s]() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0);
    };

    Hmm best;
    double best_ll = -1e300;
    std::vector<double> lls;
    for (std::size_t r = 0; r < restarts; ++r) {
        Hmm h;
        h.k = k;
        h.pi.assign(k, 1.0 / static_cast<double>(k));
        h.a.assign(k * k, 0.0);
        for (std::size_t i = 0; i < k; ++i) {
            for (std::size_t j = 0; j < k; ++j) {
                h.a[i * k + j] = (i == j ? 0.85 : 0.15
                    / static_cast<double>(k - 1));
            }
        }
        h.mu.assign(k, 0.0);
        h.sigma.assign(k, 0.0);
        for (std::size_t j = 0; j < k; ++j) {
            h.mu[j] = mu + sd * (rnd() * 2.0 - 1.0);
            // Spread the initial variances: identical starts make Baum-Welch
            // converge to k copies of one state.
            h.sigma[j] = sd * (0.5 + 1.5 * rnd());
        }
        const double ll = detail::baum_welch(x, h, iters);
        if (!std::isfinite(ll)) { continue; }
        lls.push_back(ll);
        if (ll > best_ll) { best_ll = ll; best = h; }
    }
    if (lls.empty()) { return std::unexpected(HmmError::DidNotConverge); }

    best.log_likelihood = best_ll;
    best.restarts = restarts;
    best.agreeing_restarts = 0;
    for (const double v : lls) {
        if (std::fabs(v - best_ll) < 1e-4 * std::fabs(best_ll)) {
            ++best.agreeing_restarts;
        }
    }

    // ---- LABEL SWITCHING: sort states by variance ------------------------
    //
    // Nothing in the likelihood distinguishes state 0 from state 1, so two
    // runs can produce the same model with the labels swapped. Sorting makes
    // "state 0 is the calm one" true by construction, which is what lets a
    // regime label be compared across runs at all.
    std::vector<std::size_t> ord(k);
    for (std::size_t i = 0; i < k; ++i) { ord[i] = i; }
    std::sort(ord.begin(), ord.end(),
              [&](std::size_t p, std::size_t q) {
                  return best.sigma[p] < best.sigma[q];
              });
    Hmm out = best;
    for (std::size_t i = 0; i < k; ++i) {
        out.mu[i] = best.mu[ord[i]];
        out.sigma[i] = best.sigma[ord[i]];
        out.pi[i] = best.pi[ord[i]];
        for (std::size_t j = 0; j < k; ++j) {
            out.a[i * k + j] = best.a[ord[i] * k + ord[j]];
        }
    }
    return out;
}

} // namespace altair
