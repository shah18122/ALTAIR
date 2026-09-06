// models/tests/test_neural_5m.cpp -- P8-16.
//
// TRAIN THE NEURAL TIER ON THE REAL 5-MINUTE SERIES, AND SEE WHETHER
// NONLINEAR STRUCTURE IS THERE.
//
// P8-15 measured a linear AR(3) at every horizon and found it worse than a
// constant. It also said why that was not yet an answer about NEURAL models:
// with 2,682 hourly bars a hidden-32 LSTM has 4,608 parameters and nonlinear
// structure is UNIDENTIFIABLE -- a different and more permanent problem than
// being wrong.
//
// P2-12e removed that objection. There are 211,492 five-minute returns over
// 11.6 years, so the question is now askable and this asks it.
//
// WHAT IS AND IS NOT BEING TRAINED, STATED BEFORE THE NUMBERS.
//
// `models/recurrent.hpp` is honest about this in its own header and it must
// not be blurred here: the recurrent WEIGHTS are fixed at their seeded
// initialisation and only the readout is solved, by ridge, in closed form.
// That is echo-state / reservoir computing. It is a real method, it trains end
// to end, and it is weaker than backpropagation through time. `models/mlp.hpp`
// is the same shape -- a fixed random hidden layer with a ridge-solved output.
//
// So this is NOT "an LSTM trained by gradient descent". It is a nonlinear
// random-feature model and a reservoir, both fitted exactly, and if either
// finds structure the AR missed then nonlinear structure exists and a
// gradient-trained network is worth the LibTorch dependency. If neither does,
// that is weak evidence rather than proof -- and saying which is the point.
//
// THE NORMALISATION IS FITTED ON THE TRAINING BLOCK ONLY.
//
// P8-02's scaler is correct and the trap is where it is FITTED. A mean and a
// standard deviation taken over the whole series and then "walked forward" is
// look-ahead of exactly the kind P8-13 measured in quantile boundaries: every
// training row is scaled by a constant that knows the test block. Each fold
// here computes its own from its own training rows.
//
// AND mlp.hpp's CROSS-SECTIONAL WARNING DOES NOT APPLY HERE.
//
// That header says a time-series z-score is the wrong tool -- for a
// CROSS-SECTIONAL model, ranking instruments against each other at one
// instant. This is a single instrument through time, so there is no
// cross-section to rank and the time-series scaler is the right one. Reading
// the warning as universal would be as wrong as ignoring it.

#include <models/dataset.hpp>
#include <models/attention.hpp>
#include <models/mlp.hpp>
#include <models/recurrent.hpp>
#include <backtest/validation.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

constexpr std::size_t kLags = 8;    // features: eight lagged 5-minute returns
constexpr std::size_t kHidden = 32;
constexpr std::size_t kSeq = 8;

struct Series {
    std::vector<double> close;
    std::vector<std::int64_t> session;
};

Series load_dir(const std::string& dir) {
    Series s;
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".csv") { files.push_back(e.path().string()); }
    }
    std::sort(files.begin(), files.end());
    for (const std::string& fn : files) {
        std::ifstream in(fn);
        if (!in) { continue; }
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            std::istringstream ss(line);
            std::string cell;
            int col = 0;
            std::string ts;
            double close = 0.0;
            while (std::getline(ss, cell, ',')) {
                if (col == 0) { ts = cell; }
                if (col == 4 && !cell.empty()) { close = std::atof(cell.c_str()); }
                ++col;
            }
            if (close > 0.0 && ts.size() >= 10) {
                s.close.push_back(close);
                s.session.push_back(std::atoll(ts.substr(0, 4).c_str()) * 10000
                                    + std::atoll(ts.substr(5, 2).c_str()) * 100
                                    + std::atoll(ts.substr(8, 2).c_str()));
            }
        }
    }
    return s;
}

/// Every `step`-th bar from each session's start.
Series downsample(const Series& b, std::size_t step) {
    Series d;
    std::size_t within = 0;
    for (std::size_t i = 0; i < b.close.size(); ++i) {
        if (i == 0 || b.session[i] != b.session[i - 1]) { within = 0; }
        if (within % step == step - 1) {
            d.close.push_back(b.close[i]);
            d.session.push_back(b.session[i]);
        }
        ++within;
    }
    return d;
}

struct Score {
    double rmse = 0.0;
    double dir = 0.0;
    std::size_t n = 0;
};

} // namespace

int main() {
    std::printf("P8-16 neural tier on real 5-minute NIFTY\n");

    const std::string root = ALTAIR_DATASET_DIR;
    const Series m1 = load_dir(root + "/spot/nifty/1m");
    std::printf("  1m: %zu bars\n", m1.close.size());
    if (m1.close.size() < 100000) {
        std::printf("  SKIP: need the fetched 1-minute series\n");
        return 0;
    }
    const Series m5 = downsample(m1, 5);

    // INTRADAY returns only. An overnight step is a 17.75-hour move carrying
    // every event between sessions; P8-15 excluded 448 of them for the same
    // reason and leaving them in inflates everything downstream.
    std::vector<double> r;
    std::size_t overnight = 0;
    for (std::size_t i = 0; i + 1 < m5.close.size(); ++i) {
        if (m5.session[i] != m5.session[i + 1]) { ++overnight; continue; }
        r.push_back(std::log(m5.close[i + 1] / m5.close[i]) * 10'000.0);  // bps
    }
    std::printf("  5m: %zu returns (%zu overnight steps excluded)\n",
                r.size(), overnight);
    check(r.size() > 150000, "enough 5-minute returns to identify a network");

    // ---- rows: eight lagged returns -> the next one ----------------------
    const std::size_t rows = r.size() - kLags;
    std::vector<double> xs(rows * kLags), ys(rows), ws(rows, 1.0);
    std::vector<std::size_t> bars(rows);
    std::vector<altair::LabelWindow> wins(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        for (std::size_t j = 0; j < kLags; ++j) {
            xs[i * kLags + j] = r[i + j];
        }
        ys[i] = r[i + kLags];
        bars[i] = i;
        wins[i] = altair::LabelWindow{i, i + 1};
    }
    std::printf("  %zu rows, %zu lagged features each\n", rows, kLags);

    altair::WalkForwardSpec wf{};
    wf.initial_train = 40000;
    wf.test_len = 10000;
    wf.step = 10000;
    wf.gap = 1;
    wf.expanding = true;
    const auto folds = altair::walk_forward_count(rows, wf);
    check(folds.has_value() && *folds >= 5, "enough walk-forward folds");
    if (!folds) { return 1; }
    std::printf("  %zu folds, %zu train / %zu test\n", *folds,
                wf.initial_train, wf.test_len);

    Score s_const{}, s_mlp{}, s_gru{};
    double sum_const = 0.0, sum_mlp = 0.0, sum_gru = 0.0;
    std::size_t up_const = 0, up_mlp = 0, up_gru = 0, scored = 0;

    for (std::size_t fi = 0; fi < *folds; ++fi) {
        const auto fold = altair::walk_forward_fold(rows, wf, fi);
        if (!fold) { break; }
        const altair::Block tr = fold->train;
        const altair::Block te = fold->test;

        // ---- scaler fitted on the TRAINING BLOCK ONLY --------------------
        double mu = 0.0, sd = 0.0;
        std::size_t nsc = 0;
        for (std::size_t i = tr.start; i < tr.end; ++i) {
            for (std::size_t j = 0; j < kLags; ++j) { mu += xs[i * kLags + j]; ++nsc; }
        }
        mu /= static_cast<double>(nsc);
        for (std::size_t i = tr.start; i < tr.end; ++i) {
            for (std::size_t j = 0; j < kLags; ++j) {
                const double dv = xs[i * kLags + j] - mu;
                sd += dv * dv;
            }
        }
        sd = std::sqrt(sd / static_cast<double>(nsc));
        if (!(sd > 0.0)) { sd = 1.0; }

        std::vector<double> zs(rows * kLags);
        for (std::size_t i = 0; i < rows; ++i) {
            for (std::size_t j = 0; j < kLags; ++j) {
                zs[i * kLags + j] = (xs[i * kLags + j] - mu) / sd;
            }
        }
        // The label keeps its units (bps): scaling it would make the RMSE
        // incomparable to P8-15's, and comparing to that is the whole point.
        altair::Dataset d{};
        d.x = altair::Matrix{zs.data(), rows, kLags};
        d.y = ys.data();
        d.weight = ws.data();
        d.bar = bars.data();
        d.window = wins.data();
        d.rows = rows;

        // The training mean is the constant baseline. Fitted on train, like
        // everything else.
        double train_mean = 0.0;
        for (std::size_t i = tr.start; i < tr.end; ++i) { train_mean += ys[i]; }
        train_mean /= static_cast<double>(tr.size());
        std::size_t tr_up = 0;
        for (std::size_t i = tr.start; i < tr.end; ++i) {
            if (ys[i] > 0.0) { ++tr_up; }
        }
        const bool const_up = tr_up * 2 > tr.size();

        altair::Mlp<kHidden, kLags> mlp(1.0);
        mlp.reset(0xA17A1Bu + fi);
        (void)mlp.train_epoch(d, tr, 0.0);

        altair::RecurrentReadout<altair::GruCell<16, kLags>, 16, kLags, kSeq>
            gru(1.0, 1.0);
        gru.reset(0x6C0FFEEu + fi);
        (void)gru.train_epoch(d, tr, 0.0);

        std::vector<double> p_mlp(rows, 0.0), p_gru(rows, 0.0);
        mlp.predict(d, te, p_mlp.data());
        gru.predict(d, te, p_gru.data());

        for (std::size_t i = te.start; i < te.end; ++i) {
            const double y = ys[i];
            sum_const += (train_mean - y) * (train_mean - y);
            sum_mlp += (p_mlp[i] - y) * (p_mlp[i] - y);
            sum_gru += (p_gru[i] - y) * (p_gru[i] - y);
            if (const_up == (y > 0.0)) { ++up_const; }
            if ((p_mlp[i] > 0.0) == (y > 0.0)) { ++up_mlp; }
            if ((p_gru[i] > 0.0) == (y > 0.0)) { ++up_gru; }
            ++scored;
        }
    }

    const double n = static_cast<double>(scored);
    s_const.rmse = std::sqrt(sum_const / n);
    s_mlp.rmse = std::sqrt(sum_mlp / n);
    s_gru.rmse = std::sqrt(sum_gru / n);
    s_const.dir = static_cast<double>(up_const) / n;
    s_mlp.dir = static_cast<double>(up_mlp) / n;
    s_gru.dir = static_cast<double>(up_gru) / n;
    const double se = std::sqrt(0.25 / n);

    std::printf("\n  %zu scored predictions\n\n", scored);
    std::printf("  %-22s %10s %10s %10s\n", "", "RMSE(bps)", "vs const",
                "direction");
    std::printf("  %-22s %10.4f %10s %10.4f\n", "constant (train mean)",
                s_const.rmse, "1.0000", s_const.dir);
    std::printf("  %-22s %10.4f %10.4f %10.4f\n", "MLP (random features)",
                s_mlp.rmse, s_mlp.rmse / s_const.rmse, s_mlp.dir);
    std::printf("  %-22s %10.4f %10.4f %10.4f\n", "GRU readout (reservoir)",
                s_gru.rmse, s_gru.rmse / s_const.rmse, s_gru.dir);
    std::printf("\n  directional edge over the constant, in standard errors:\n");
    std::printf("    MLP  %+.4f = %+.2f sigma\n", s_mlp.dir - s_const.dir,
                (s_mlp.dir - s_const.dir) / se);
    std::printf("    GRU  %+.4f = %+.2f sigma\n", s_gru.dir - s_const.dir,
                (s_gru.dir - s_const.dir) / se);

    check(scored > 50000, "a meaningful number of scored predictions");
    check(s_const.rmse > 0.0, "the constant baseline was computed");

    // ---- the verdict ------------------------------------------------------
    const double best = std::min(s_mlp.rmse, s_gru.rmse) / s_const.rmse;
    const double best_sig =
        std::max(s_mlp.dir - s_const.dir, s_gru.dir - s_const.dir) / se;
    // ---- ATTENTION, THE THIRD ARCHITECTURE ------------------------------
    //
    // P8-07 was the last of the neural tier still marked "synthetic only".
    // `variable_selection` is not a predictor and cannot be dropped into the
    // walk-forward above -- it is a softmax over features plus the magnitude
    // of the gated output -- so it is asked the question it can actually
    // answer, on the same 170,000 real rows: DOES IT SELECT ANYTHING?
    //
    // AND THE CONTROL IS THE POINT.
    //
    // A softmax always produces a winner. `concentration()` near 1.0 means an
    // even split and above 1.0 means one lag dominates, but on eight noisy
    // features some lag wins every single row by chance, so a raw
    // concentration number is uninterpretable on its own. So the identical
    // procedure runs on SHUFFLED features -- same values, same logits, order
    // destroyed -- and the two are compared. attention.hpp says the same thing
    // in its own words: the weights tell you which feature won a contest, not
    // whether the contest was about anything.
    {
        std::printf("\n  ATTENTION (P8-07) on the same %zu rows\n", rows);

        // Fixed random logits, in the same spirit as the reservoir above: no
        // training, so nothing here can overfit its way to a result.
        std::uint64_t st = 0xA77E27101u;
        auto rnd = [&st]() noexcept {
            st ^= st << 13; st ^= st >> 7; st ^= st << 17;
            return static_cast<double>(st >> 11) * (1.0 / 9007199254740992.0);
        };
        double logits[kLags];
        for (std::size_t j = 0; j < kLags; ++j) { logits[j] = rnd() * 2.0 - 1.0; }

        double conc_real = 0.0, mag_real = 0.0;
        double conc_shuf = 0.0, mag_shuf = 0.0;
        std::size_t n_sel = 0;
        double feats[kLags], shuf[kLags];

        for (std::size_t i = 0; i < rows; ++i) {
            for (std::size_t j = 0; j < kLags; ++j) {
                feats[j] = xs[i * kLags + j];
                shuf[j] = feats[j];
            }
            // Fisher-Yates on the row itself: the SAME eight numbers, so any
            // difference between the two lines below is about ORDER -- which
            // is the only thing a lag structure could be.
            for (std::size_t j = kLags; j > 1; --j) {
                const auto k = static_cast<std::size_t>(rnd()
                                   * static_cast<double>(j));
                const double t = shuf[j - 1];
                shuf[j - 1] = shuf[k < j ? k : j - 1];
                shuf[k < j ? k : j - 1] = t;
            }
            const auto a = altair::variable_selection(feats, logits, kLags);
            const auto b = altair::variable_selection(shuf, logits, kLags);
            if (!a || !b) { continue; }
            conc_real += a->concentration();
            mag_real += a->output_magnitude;
            conc_shuf += b->concentration();
            mag_shuf += b->output_magnitude;
            ++n_sel;
        }

        if (n_sel > 0) {
            const double nn = static_cast<double>(n_sel);
            conc_real /= nn; mag_real /= nn;
            conc_shuf /= nn; mag_shuf /= nn;
            std::printf("    %-22s %14s %16s\n", "", "concentration",
                        "output |magnitude|");
            std::printf("    %-22s %14.4f %16.4f\n", "real lag order",
                        conc_real, mag_real);
            std::printf("    %-22s %14.4f %16.4f\n", "shuffled lag order",
                        conc_shuf, mag_shuf);
            std::printf("    (concentration 1.0 = an even split over %zu"
                        " features)\n", kLags);

            check(n_sel > 100000,
                  "attention ran on a real number of rows, not a sample");
            // The softmax is a function of the LOGITS, which are fixed, so the
            // weights cannot depend on the data at all -- and the measured
            // equality is the demonstration of that, not an accident.
            check(std::fabs(conc_real - conc_shuf) < 1e-9,
                  "concentration is IDENTICAL on real and shuffled features, "
                  "because these weights are a function of the fixed logits "
                  "alone -- a softmax over learned-nothing logits selects "
                  "nothing, whatever the data says");
            std::printf("\n    So the weights are not evidence, and reading"
                        " them as feature importance\n    would be reading a"
                        " constant. What DOES move with the data is the\n"
                        "    output magnitude: %.4f real against %.4f"
                        " shuffled, a %.1f%% difference.\n",
                        mag_real, mag_shuf,
                        100.0 * (mag_real - mag_shuf)
                            / (mag_shuf > 0.0 ? mag_shuf : 1.0));
            std::printf("    Attention here needs TRAINED logits to say"
                        " anything, and training them\n    needs the"
                        " gradient tier that is not built. Marked accordingly"
                        " rather\n    than reported as a result.\n");
        }
    }

    std::printf("\n  VERDICT\n");
    if (best < 1.0 && best_sig > 2.0) {
        std::printf("    BOTH tests pass: a nonlinear model beats a constant "
                    "on RMSE (%.4f)\n    and on direction (%.2f sigma). "
                    "Nonlinear structure is present, and a\n    "
                    "gradient-trained network is now worth the LibTorch "
                    "dependency.\n", best, best_sig);
    } else {
        std::printf("    NO NONLINEAR EDGE EITHER. Best RMSE ratio %.4f "
                    "(1.0 is the constant),\n    best directional edge %.2f "
                    "sigma. The AR found nothing at this horizon\n    and "
                    "neither does a nonlinear random-feature model nor a "
                    "reservoir.\n", best, best_sig);
        std::printf("\n    THIS IS WEAK EVIDENCE, NOT PROOF, AND THE "
                    "DIFFERENCE MATTERS.\n    Both models fix their hidden "
                    "layers and solve only the readout, so\n    neither has "
                    "searched the space a backpropagated network would. What "
                    "it\n    does establish: the easy nonlinear structure -- "
                    "the kind a random\n    projection exposes -- is not "
                    "there. On %zu observations that is no\n    longer a "
                    "sample-size excuse.\n", scored);
    }
    std::printf("\n    Nothing here is net of cost, and at 5 minutes P8-15 "
                "measured that a\n    PERFECT oracle nets -0.07 bps/bar "
                "against a 5.5 bps round trip. Even a\n    real edge at this "
                "horizon would have to clear that first.\n");

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
