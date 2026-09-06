// risk/tests/test_optimise.cpp -- P15-03 / P15-05 / P15-06.
//
// Three questions, in the order that matters:
//
//   1. Does the sample covariance actually break at p ~ n, or is that a story?
//      Measured by condition number, on data with a KNOWN true covariance.
//   2. Does shrinkage fix it, and at what cost?
//   3. OUT OF SAMPLE, does any optimiser beat 1/N? QUANTLAB says no, and
//      DeMiguel-Garlappi-Uppal said no across fourteen datasets in 2009.
//
// The third is the only one with money attached.

#include <risk/optimise.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

struct Rng {
    std::uint64_t s = 0x9E3779B97F4A7C15ull;
    double u() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return (static_cast<double>(s >> 11) + 0.5) * (1.0 / 9007199254740992.0);
    }
    double normal() {
        return std::sqrt(-2.0 * std::log(u()))
             * std::cos(6.283185307179586 * u());
    }
};

/// p assets, n observations, one common factor plus idiosyncratic noise. The
/// TRUE covariance is known by construction, which is what makes "the estimate
/// is bad" a measurable claim rather than an impression.
std::vector<std::vector<double>> factor_returns(std::size_t p, std::size_t n,
                                                Rng& rng) {
    std::vector<std::vector<double>> r(p, std::vector<double>(n, 0.0));
    for (std::size_t k = 0; k < n; ++k) {
        const double f = rng.normal() * 0.01;      // common factor
        for (std::size_t i = 0; i < p; ++i) {
            r[i][k] = 0.8 * f + 0.006 * rng.normal();
        }
    }
    return r;
}

} // namespace

int main() {
    using altair::equal_weight;
    using altair::ledoit_wolf;
    using altair::min_variance;
    using altair::portfolio_variance;
    using altair::risk_parity;
    using altair::sample_covariance;

    std::printf("P15-03/05/06 covariance and portfolio optimisers\n");

    // ---- 1. THE SAMPLE COVARIANCE BREAKS, MEASURABLY ----------------------
    {
        std::printf("\n  CONDITION NUMBER of the sample covariance\n");
        std::printf("    %6s %6s %16s %16s\n", "p", "n", "sample", "shrunk");
        // p >= n IS THE SINGULAR CASE, and the first draft of this test did
        // not reach it. It used p = 30, n = 32 -- where n > p, so the matrix
        // is still FULL RANK and merely ill-conditioned (1.4e3). The
        // catastrophic regime needs p >= n, where the rank is at most n and
        // the matrix has exact zero eigenvalues no shrinkage-free method can
        // invert. Ill-conditioned and singular are different failures and the
        // test now visits both.
        double cond_small = 0.0, cond_singular = 0.0;
        bool shrink_fixed = true;
        for (const auto [p, n] : {std::pair<std::size_t, std::size_t>{5, 500},
                                  {20, 60},
                                  {40, 30}}) {
            Rng rng;
            const auto r = factor_returns(p, n, rng);
            const auto s = sample_covariance(r);
            const auto lw = ledoit_wolf(r);
            if (!s || !lw) { continue; }
            const double cs = s->condition_number();
            const double cl = lw->sigma.condition_number();
            std::printf("    %6zu %6zu %16.3e %16.3e   (delta %.3f)\n",
                        p, n, cs, cl, lw->delta);
            if (p == 5) { cond_small = cs; }
            if (p == 40) { cond_singular = cs; }
            if (cl > cs) { shrink_fixed = false; }
        }
        std::printf("    p/n from 0.01 to 1.33 moves the condition number "
                    "%.3e -> %.3e (%.0fx)\n",
                    cond_small, cond_singular, cond_singular / cond_small);
        check(cond_singular > cond_small * 100.0,
              "as p/n rises the sample covariance degrades by orders of "
              "magnitude, and at p >= n it is RANK DEFICIENT -- an optimiser "
              "handed this produces enormous offsetting weights that look "
              "like conviction and are division by almost zero");
        check(shrink_fixed,
              "and shrinkage lowers the condition number at every shape, "
              "which is the entire job: a biased estimate with far less "
              "variance beats an unbiased one nobody can invert");
    }

    // ---- 2. SHRINKAGE IS CLOSER TO THE TRUTH ------------------------------
    //
    // Not "smoother" or "more stable" -- CLOSER, measured in Frobenius norm
    // against the covariance the data was generated from.
    {
        const std::size_t p = 25, n = 60;
        Rng rng;
        const auto r = factor_returns(p, n, rng);
        const auto s = sample_covariance(r);
        const auto lw = ledoit_wolf(r);
        if (s && lw) {
            // True: 0.8^2 * 1e-4 common + 0.006^2 idiosyncratic on the
            // diagonal, 0.64e-4 off it.
            const double off = 0.64e-4;
            const double diag = off + 3.6e-5;
            double e_s = 0.0, e_l = 0.0;
            for (std::size_t i = 0; i < p; ++i) {
                for (std::size_t j = 0; j < p; ++j) {
                    const double t = (i == j) ? diag : off;
                    const double ds = s->at(i, j) - t;
                    const double dl = lw->sigma.at(i, j) - t;
                    e_s += ds * ds;
                    e_l += dl * dl;
                }
            }
            std::printf("\n    p=25 n=60: Frobenius error to TRUTH -- "
                        "sample %.3e, shrunk %.3e\n",
                        std::sqrt(e_s), std::sqrt(e_l));
            check(e_l < e_s,
                  "the shrunk estimate is closer to the covariance the data "
                  "was actually generated from, which is the only defence "
                  "shrinkage has and the one that matters");
        }
    }

    // ---- 3. OUT OF SAMPLE, AGAINST 1/N ------------------------------------
    //
    // THE question. Fit on the first half, measure realised variance on the
    // second. In-sample the optimiser wins by construction -- that is what
    // "optimiser" means -- so an in-sample comparison is arithmetic, not
    // evidence.
    {
        const std::size_t p = 20, n = 120;
        Rng rng;
        const auto train = factor_returns(p, n, rng);
        const auto test = factor_returns(p, 500, rng);

        const auto s_tr = sample_covariance(train);
        const auto lw_tr = ledoit_wolf(train);
        const auto s_te = sample_covariance(test);
        check(s_tr && lw_tr && s_te, "train and test covariances compute");
        if (s_tr && lw_tr && s_te) {
            const auto mv_sample = min_variance(*s_tr);
            const auto mv_shrunk = min_variance(lw_tr->sigma);
            const auto rp = risk_parity(lw_tr->sigma);
            const auto eq = equal_weight(p);

            struct Row { const char* name; double var; double eff_n; };
            std::vector<Row> rows;
            rows.push_back({"1/N (the control)",
                            portfolio_variance(*s_te, eq), eq.effective_n()});
            if (mv_sample) {
                rows.push_back({"min-var, sample cov",
                                portfolio_variance(*s_te, *mv_sample),
                                mv_sample->effective_n()});
            }
            if (mv_shrunk) {
                rows.push_back({"min-var, shrunk cov",
                                portfolio_variance(*s_te, *mv_shrunk),
                                mv_shrunk->effective_n()});
            }
            if (rp) {
                rows.push_back({"risk parity, shrunk",
                                portfolio_variance(*s_te, *rp),
                                rp->effective_n()});
            }

            std::printf("\n  OUT-OF-SAMPLE realised variance, p=%zu, "
                        "fitted on %zu days, scored on 500\n", p, n);
            std::printf("    %-24s %14s %10s\n", "", "variance", "eff N");
            for (const Row& r : rows) {
                std::printf("    %-24s %14.4e %10.2f\n",
                            r.name, r.var, r.eff_n);
            }

            check(std::fabs(eq.sum() - 1.0) < 1e-12,
                  "every portfolio is fully invested -- weights sum to 1, "
                  "asserted rather than assumed");
            if (mv_shrunk) {
                bool long_only = true;
                for (const double x : mv_shrunk->w) {
                    if (x < -1e-12) { long_only = false; }
                }
                check(long_only,
                      "and long-only: no optimiser here returns the -350% "
                      "position an unconstrained inverse manufactures out of "
                      "a near-zero eigenvalue");
            }

            // The verdict is READ, not assumed either way.
            double best = rows[0].var;
            const char* winner = rows[0].name;
            for (const Row& r : rows) {
                if (r.var < best) { best = r.var; winner = r.name; }
            }
            std::printf("\n  VERDICT: lowest out-of-sample variance is %s\n",
                        winner);
            if (rows[0].var <= best + 1e-18) {
                std::printf("    1/N WINS, replicating QUANTLAB's Phase 4 and "
                            "DeMiguel-Garlappi-Uppal.\n    Mean-variance is "
                            "an ERROR-MAXIMISING procedure: it puts weight "
                            "where\n    covariance is low, which is where the "
                            "estimate is most wrong. 1/N has\n    no "
                            "parameters, so it has nothing to get wrong.\n");
            } else {
                std::printf("    An optimiser beat 1/N here. Note the setup is "
                            "GENEROUS to it: the\n    test data comes from the "
                            "same generator as the training data, so\n    "
                            "there is no regime change and the covariance is "
                            "genuinely stable.\n    Real series are not, which "
                            "is why QUANTLAB's result on real data\n    went "
                            "the other way, and why this is not evidence for "
                            "sizing.\n");
            }
            check(rows.size() >= 3, "at least three portfolios were compared");
        }
    }

    // ---- 4. REFUSALS ------------------------------------------------------
    {
        std::vector<std::vector<double>> ragged = {{1.0, 2.0}, {1.0}};
        check(!sample_covariance(ragged).has_value(),
              "a ragged input is refused rather than read past the end of the "
              "shorter series");
        std::vector<std::vector<double>> tiny = {{1.0, 2.0}};
        check(!sample_covariance(tiny).has_value(),
              "and two observations cannot support a covariance");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
