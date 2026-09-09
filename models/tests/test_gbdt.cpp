// models/tests/test_gbdt.cpp -- P16-01..05.
//
// Four things, and the third is the one worth the card:
//
//   1. It learns a nonlinear function a linear model cannot.
//   2. GAIN IMPORTANCE LIES on correlated features, demonstrated on a
//      deliberately duplicated column -- and permutation importance does not.
//   3. META-LABELING on real NIFTY. QUANTLAB's Q6: the highest-value use of ML
//      here is FILTERING an existing signal, not predicting direction. P8-15
//      and P8-16 already found no direction to predict at 5 and 60 minutes.
//   4. The refusals.

#include <models/gbdt.hpp>

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

struct Rng {
    std::uint64_t s = 0x243F6A8885A308D3ull;
    double u() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return (static_cast<double>(s >> 11) + 0.5) * (1.0 / 9007199254740992.0);
    }
    double normal() {
        return std::sqrt(-2.0 * std::log(u()))
             * std::cos(6.283185307179586 * u());
    }
};

std::vector<double> load_closes(const std::string& path) {
    std::vector<double> out;
    std::ifstream f(path);
    if (!f) { return out; }
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string cell;
        int col = 0;
        double c = 0.0;
        while (std::getline(ss, cell, ',')) {
            if (col == 4 && !cell.empty()) { c = std::atof(cell.c_str()); }
            ++col;
        }
        if (c > 0.0) { out.push_back(c); }
    }
    return out;
}

} // namespace


// P33-04. LEAF-WISE GROWTH HAD NO TEST.
//
// grow_leafwise is the trickiest sixty lines in gbdt.hpp -- an explicit
// frontier, a best-anywhere search, and a leaf budget -- and nothing set
// `leaf_wise = true`. It was exercised only indirectly through the spot
// forecast, where nothing asserted it behaved as claimed.
//
// The header makes a specific, falsifiable claim: "at equal leaf count
// leaf-wise reaches lower training loss, because every split it made was the
// best one available." That is what is checked, along with the budget it is
// supposed to respect and the determinism rule 10 requires.
void leaf_wise_growth_does_what_the_header_claims()
{
    using altair::Frame;
    using altair::Gbdt;
    using altair::GbdtParams;
    using altair::Node;
    using altair::Tree;
    using altair::fit_gbdt;

    std::printf("\nN leaf_wise_growth_does_what_the_header_claims\n");

    constexpr std::size_t kN = 4000, kP = 4;
    Frame f;
    f.p = kP;
    f.rows = kN;
    f.x.resize(kN * kP);
    std::vector<double> y(kN);
    std::uint64_t s = 4242u;
    auto u = [&s]() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return static_cast<double>(s >> 11) / 9007199254740992.0;
    };
    for (std::size_t i = 0; i < kN; ++i) {
        double a = 0.0;
        for (std::size_t c = 0; c < kP; ++c) {
            const double v = u() * 2.0 - 1.0;
            f.x[i * kP + c] = v;
            a += (c == 0 ? 3.0 : (c == 1 ? -2.0 : 0.0)) * v;
        }
        // A kink, so trees have something a linear model would miss.
        y[i] = a + (f.x[i * kP] > 0.4 ? 5.0 : 0.0) + 0.10 * (u() - 0.5);
    }

    auto train_mse = [&](const Gbdt& m) {
        double e = 0.0;
        for (std::size_t i = 0; i < kN; ++i) {
            const double d = m.predict_row(&f.x[i * kP]) - y[i];
            e += d * d;
        }
        return e / static_cast<double>(kN);
    };

    // EQUAL LEAF BUDGET. Level-wise at depth 4 has at most 16 leaves, so
    // leaf-wise gets 16. Subsampling off, so the two see identical rows and
    // the comparison is between the growth policies and nothing else.
    GbdtParams lvl;
    lvl.trees = 60;
    lvl.max_depth = 4;
    lvl.learning_rate = 0.08;
    lvl.subsample = 1.0;
    lvl.min_leaf = 20;
    lvl.leaf_wise = false;

    GbdtParams leaf = lvl;
    leaf.leaf_wise = true;
    leaf.max_leaves = 16;
    leaf.max_depth = 12;      // leaf-wise needs room to be deep

    const auto ml = fit_gbdt(f, y, lvl);
    const auto mf = fit_gbdt(f, y, leaf);
    check(ml.has_value() && mf.has_value(), "both growth policies fit");
    if (!ml || !mf) { return; }

    const double e_lvl = train_mse(*ml), e_leaf = train_mse(*mf);
    std::printf("    training MSE: level-wise %.5f   leaf-wise %.5f\n",
                e_lvl, e_leaf);
    check(e_leaf < e_lvl,
          "at an equal leaf budget leaf-wise reaches LOWER training loss, "
          "which is the header's claim and the entire reason the flag "
          "exists");

    // THE BUDGET IS A BUDGET. A frontier loop that miscounts leaves grows
    // without bound, and nothing downstream would notice except the fit
    // getting mysteriously better.
    std::size_t worst = 0;
    for (const Tree& t : mf->tree) {
        std::size_t leaves = 0;
        for (const Node& nd : t.node) {
            if (nd.feature == Node::kLeaf) { ++leaves; }
        }
        if (leaves > worst) { worst = leaves; }
    }
    std::printf("    most leaves in any leaf-wise tree: %zu (budget %zu)\n",
                worst, leaf.max_leaves);
    check(worst <= leaf.max_leaves,
          "and no tree exceeds max_leaves -- a frontier that miscounts grows "
          "without bound and only looks like a better fit");

    // RULE 10: same seed, same model.
    const auto again = fit_gbdt(f, y, leaf);
    check(again.has_value(), "it fits again");
    if (again) {
        double diff = 0.0;
        for (std::size_t i = 0; i < kN; ++i) {
            diff += std::fabs(again->predict_row(&f.x[i * kP])
                              - mf->predict_row(&f.x[i * kP]));
        }
        check(diff == 0.0,
              "and two fits from one seed are bit-identical, which rule 10 "
              "needs and a frontier ordered by an unstable comparison would "
              "not give");
    }

    // A leaf budget below two is refused rather than producing a stump that
    // looks like a fitted model.
    GbdtParams bad = leaf;
    bad.max_leaves = 1;
    check(!fit_gbdt(f, y, bad).has_value(),
          "max_leaves = 1 is REFUSED");
}

int main() {
    using altair::Frame;
    using altair::Gbdt;
    using altair::GbdtParams;
    using altair::fit_gbdt;
    using altair::permutation_importance;

    std::printf("P16 gradient-boosted trees\n");
    leaf_wise_growth_does_what_the_header_claims();

    // ---- 1. IT LEARNS SOMETHING LINEAR REGRESSION CANNOT ------------------
    //
    // y = x0 * x1 -- a pure interaction with zero linear signal in either
    // feature alone. A linear model gets nothing; a tree finds it by splitting.
    {
        Rng rng;
        const std::size_t n = 4000, p = 4;
        Frame f;
        f.rows = n; f.p = p; f.x.assign(n * p, 0.0);
        std::vector<double> y(n);
        for (std::size_t r = 0; r < n; ++r) {
            for (std::size_t c = 0; c < p; ++c) {
                f.x[r * p + c] = rng.normal();
            }
            y[r] = f.at(r, 0) * f.at(r, 1) + 0.2 * rng.normal();
        }
        GbdtParams prm;
        prm.trees = 200;
        prm.max_depth = 4;
        const auto m = fit_gbdt(f, y, prm);
        check(m.has_value(), "the model fits");
        if (m) {
            double sse = 0.0, sst = 0.0, mu = 0.0;
            for (const double v : y) { mu += v; }
            mu /= static_cast<double>(n);
            for (std::size_t r = 0; r < n; ++r) {
                const double e = m->predict_row(&f.x[r * p]) - y[r];
                sse += e * e;
                sst += (y[r] - mu) * (y[r] - mu);
            }
            const double r2 = 1.0 - sse / sst;
            std::printf("    y = x0*x1 + noise, in-sample R2 = %.3f\n", r2);
            check(r2 > 0.6,
                  "it recovers a pure INTERACTION -- neither feature has any "
                  "linear signal on its own, so a linear model scores zero "
                  "here by construction");

            std::printf("    gain importance:");
            for (std::size_t c = 0; c < p; ++c) {
                std::printf(" x%zu=%.3f", c, m->gain[c]);
            }
            std::printf("\n");
            check(m->gain[0] + m->gain[1] > 3.0 * (m->gain[2] + m->gain[3]),
                  "and the importance puts the weight on the two features "
                  "that carry the signal, not the two that are noise");
        }
    }

    // ---- 2. GAIN IMPORTANCE LIES ON CORRELATED FEATURES -------------------
    //
    // x1 is an exact copy of x0. Between them they carry all the signal, so an
    // honest attribution splits the credit. Gain does not: whichever column
    // the tree reaches first absorbs it.
    {
        Rng rng;
        const std::size_t n = 3000, p = 3;
        Frame f;
        f.rows = n; f.p = p; f.x.assign(n * p, 0.0);
        std::vector<double> y(n);
        for (std::size_t r = 0; r < n; ++r) {
            const double a = rng.normal();
            f.x[r * p + 0] = a;
            f.x[r * p + 1] = a;                       // an EXACT duplicate
            f.x[r * p + 2] = rng.normal();            // noise
            y[r] = 2.0 * a + 0.1 * rng.normal();
        }
        GbdtParams prm;
        prm.trees = 150;
        const auto m = fit_gbdt(f, y, prm);
        if (m) {
            const auto perm = permutation_importance(*m, f, y, 0xFEED);
            std::printf("\n    x1 is an EXACT COPY of x0; x2 is noise\n");
            std::printf("    %-14s x0 %8.4f   x1 %8.4f   x2 %8.4f\n",
                        "gain:", m->gain[0], m->gain[1], m->gain[2]);
            std::printf("    %-14s x0 %8.4f   x1 %8.4f   x2 %8.4f\n",
                        "permutation:", perm[0], perm[1], perm[2]);

            const double gain_ratio =
                m->gain[1] > 1e-12 ? m->gain[0] / m->gain[1] : 1e9;
            std::printf("    gain ratio x0/x1 = %.1f (an honest split would "
                        "be near 1.0)\n", gain_ratio);
            check(gain_ratio > 3.0 || gain_ratio < 0.33,
                  "GAIN IMPORTANCE IS LOPSIDED between two IDENTICAL "
                  "features -- whichever the tree reaches first absorbs the "
                  "credit, and swapping the column order reverses the story");

            // THE LIE, MADE RIGOROUS.
            //
            // Gain says x1 is worth EXACTLY ZERO. If that meant what a reader
            // takes it to mean -- "x1 carries no information about y" -- then
            // a model trained on x1 ALONE would be useless. It is not: x1 is
            // an exact copy of x0, so it predicts y perfectly. Fitting that
            // model is the proof, and it costs one more fit.
            //
            // The first version of this check compared permutation importance
            // against gain numerically. They are in DIFFERENT UNITS -- MSE
            // increase versus summed variance reduction -- so the comparison
            // passed for arithmetic reasons rather than because anything had
            // been demonstrated.
            Frame solo;
            solo.rows = n; solo.p = 1; solo.x.assign(n, 0.0);
            for (std::size_t r = 0; r < n; ++r) { solo.x[r] = f.at(r, 1); }
            const auto m1 = fit_gbdt(solo, y, prm);
            if (m1) {
                double sse = 0.0, sst = 0.0, mu = 0.0;
                for (const double v : y) { mu += v; }
                mu /= static_cast<double>(n);
                for (std::size_t r = 0; r < n; ++r) {
                    const double e = m1->predict_row(&solo.x[r]) - y[r];
                    sse += e * e;
                    sst += (y[r] - mu) * (y[r] - mu);
                }
                const double r2_solo = 1.0 - sse / sst;
                std::printf("    a model on x1 ALONE scores R2 = %.3f\n",
                            r2_solo);
                check(r2_solo > 0.9,
                      "x1 alone predicts y almost perfectly, while gain "
                      "importance scored it EXACTLY ZERO -- so a zero there "
                      "means 'this model did not use it', never 'this feature "
                      "carries no information', and the two get confused every "
                      "time a feature-selection step reads that column");
            }

            // Permutation is honest about the model, which is a different and
            // also useful question: it measures what THIS model would lose,
            // and x1 is genuinely free to shuffle because the model never
            // looks at it.
            check(perm[0] > perm[2] * 10.0,
                  "permutation importance separates the feature the model "
                  "actually uses from the noise column by an order of "
                  "magnitude -- it answers 'what would this model lose', "
                  "which is the question worth asking before deployment");
        }
    }

    // ---- 3. META-LABELING ON REAL NIFTY -----------------------------------
    //
    // Q6. The model does NOT predict direction -- P8-15 and P8-16 established
    // there is nothing to predict at these horizons. It predicts whether a
    // SIMPLE EXISTING SIGNAL is about to be right, which is a different and
    // much easier question: the primary model decides the side, the secondary
    // decides the size.
    {
        const std::string path =
            std::string(ALTAIR_DATASET_DIR) + "/spot/nifty/1d/all.csv";
        const auto c = load_closes(path);
        if (c.size() < 3000) {
            std::printf("\n  SKIP: %s has %zu closes\n", path.c_str(), c.size());
        } else {
            std::vector<double> r;
            for (std::size_t i = 1; i < c.size(); ++i) {
                r.push_back(std::log(c[i] / c[i - 1]));
            }
            // PRIMARY SIGNAL: a plain 20-day momentum rule. Deliberately
            // mediocre -- the card is whether a filter improves it, and a
            // strong primary would leave nothing to improve.
            const std::size_t lag = 20;
            Frame f;
            f.p = 5;
            std::vector<double> label;
            std::vector<int> side;
            for (std::size_t i = lag; i + 1 < r.size(); ++i) {
                double mom = 0.0, vol = 0.0;
                for (std::size_t k = i - lag; k < i; ++k) {
                    mom += r[k];
                    vol += r[k] * r[k];
                }
                vol = std::sqrt(vol / static_cast<double>(lag));
                const int s = mom > 0.0 ? 1 : -1;
                // Features known AT i, label from i+1 -- strictly forward,
                // rule 7.
                f.x.push_back(mom);
                f.x.push_back(vol);
                f.x.push_back(r[i - 1]);
                f.x.push_back(r[i - 2]);
                f.x.push_back(std::fabs(r[i - 1]) / (vol + 1e-12));
                side.push_back(s);
                // META LABEL: did the primary signal make money next day?
                label.push_back(s * r[i] > 0.0 ? 1.0 : 0.0);
                ++f.rows;
            }
            const std::size_t split = f.rows / 2;
            Frame tr;
            tr.p = f.p; tr.rows = split;
            tr.x.assign(f.x.begin(), f.x.begin() + static_cast<long>(split * f.p));
            std::vector<double> ytr(label.begin(),
                                    label.begin() + static_cast<long>(split));

            GbdtParams prm;
            prm.trees = 120;
            prm.max_depth = 3;
            prm.learning_rate = 0.05;
            const auto m = fit_gbdt(tr, ytr, prm);
            check(m.has_value(), "the meta-model fits on the first half");
            if (m) {
                // Out of sample: keep only trades the filter likes.
                std::size_t n_all = 0, hit_all = 0, n_kept = 0, hit_kept = 0;
                for (std::size_t i = split; i < f.rows; ++i) {
                    const double p_hat = m->predict_row(&f.x[i * f.p]);
                    ++n_all;
                    if (label[i] > 0.5) { ++hit_all; }
                    if (p_hat > 0.5) {
                        ++n_kept;
                        if (label[i] > 0.5) { ++hit_kept; }
                    }
                }
                const double base = static_cast<double>(hit_all)
                                  / static_cast<double>(n_all);
                const double filt = n_kept > 0
                    ? static_cast<double>(hit_kept)
                          / static_cast<double>(n_kept) : 0.0;
                std::printf("\n  META-LABELING, out of sample on %zu days\n",
                            n_all);
                std::printf("    primary alone : %zu trades, hit rate %.4f\n",
                            n_all, base);
                std::printf("    filtered      : %zu trades, hit rate %.4f\n",
                            n_kept, filt);
                std::printf("    the filter declined %.0f%% of them\n",
                            100.0 * (1.0 - static_cast<double>(n_kept)
                                               / static_cast<double>(n_all)));

                check(n_kept > 50,
                      "the filter keeps enough trades for the comparison to "
                      "mean anything -- one that declines 99% has not "
                      "improved a strategy, it has stopped being one");

                // The lift is READ, not assumed. QUANTLAB measured 0.41 -> 0.56
                // on ITS primary signal; nothing says a 20-day momentum rule on
                // NIFTY behaves the same way.
                const double se = std::sqrt(base * (1.0 - base)
                                            / static_cast<double>(n_kept));
                const double z = se > 0.0 ? (filt - base) / se : 0.0;
                std::printf("    lift %+.4f = %+.2f sigma\n", filt - base, z);
                if (z > 2.0) {
                    std::printf("      -> a real filter. QUANTLAB's Q6 "
                                "replicates: meta-labeling is the\n         "
                                "highest-value ML use here, not direction "
                                "prediction.\n");
                } else {
                    std::printf("      -> NOT distinguishable from chance on "
                                "this primary signal.\n         QUANTLAB "
                                "measured 0.41 -> 0.56 on its own; that does "
                                "not\n         transfer, and nothing here is "
                                "sized on a %+.2f sigma lift.\n", z);
                }
                check(std::isfinite(z), "and the lift is reported with its "
                                        "error rather than as a bare number");
            }
        }
    }

    // ---- 4. REFUSALS ------------------------------------------------------
    {
        Frame tiny;
        tiny.rows = 10; tiny.p = 2; tiny.x.assign(20, 0.0);
        std::vector<double> y(10, 1.0);
        check(!fit_gbdt(tiny, y, GbdtParams{}).has_value(),
              "ten rows cannot support a boosted ensemble and are refused");
        GbdtParams bad;
        bad.learning_rate = 0.0;
        Frame f;
        f.rows = 100; f.p = 2; f.x.assign(200, 0.5);
        std::vector<double> yy(100, 1.0);
        check(!fit_gbdt(f, yy, bad).has_value(),
              "and a zero learning rate is refused -- it fits a hundred trees "
              "that change nothing and reports success");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
