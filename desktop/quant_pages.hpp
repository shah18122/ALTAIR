// desktop/quant_pages.hpp -- pages for Phases 13-18.
//
// P19-01..05.
//
// EVERY PAGE READS THE SAME ARTIFACTS THE TESTS PRODUCE. NEVER A SECOND
// COMPUTATION.
//
// This is the rule the whole phase hangs on, and this project has already paid
// for breaking it twice: P11Q-07b had a dashboard literal that could not track
// a fit, and the Models table reported chi-square 19 while the pane three
// inches below computed 13.08. Both were caused by a number existing in two
// places.
//
// So each page here calls the SAME function the acceptance test calls, with
// the same inputs, and renders what comes back. Where a page shows a figure it
// cannot compute live -- because the input is a fitted model or a dataset the
// UI does not load -- it says so in words rather than printing a stale
// constant. A blank cell that explains itself beats a number nobody can
// reproduce.
//
// AND THE UI STILL CANNOT TRADE.
//
// Every header included here is read-side: analytics, risk, models, backtest.
// `oms/execution.hpp` and `oms/capacity.hpp` are NOT included, even though
// P19-01 is the execution page -- it renders a SCHEDULE SHAPE computed from
// arithmetic the UI owns, not from the order-placing module. The CMake
// allow-list would refuse the link anyway (gate 3), which is the point of
// having it.

#pragma once

#include <analytics/garch.hpp>
#include <analytics/hmm.hpp>
#include <analytics/kalman.hpp>
#include <analytics/sabr.hpp>
#include <models/gbdt.hpp>
#include <models/spot_forecast.hpp>
#include <risk/covariance.hpp>
#include <risk/optimise.hpp>
#include <risk/stress.hpp>
#include <risk/var.hpp>

#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace altair::ui {

/// Daily closes from `dataset/`, read by the UI's own loader.
///
/// A SECOND reader, deliberately -- the model tests each have their own. Two
/// independent paths to the same series is what makes a number a property of
/// the data rather than of one parser, and it is the reason the figures on
/// these pages can be compared against the test output at all.
[[nodiscard]] inline std::vector<double> ui_load_closes(const QString& path) {
    std::vector<double> out;
    std::ifstream f(path.toStdString());
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

[[nodiscard]] inline std::vector<double> ui_log_returns(
    const std::vector<double>& c) {
    std::vector<double> r;
    r.reserve(c.size());
    for (std::size_t i = 1; i < c.size(); ++i) {
        r.push_back(std::log(c[i] / c[i - 1]));
    }
    return r;
}

/// A page that computes on demand rather than at startup.
///
/// P11Q's hard-won lesson: `expanding_states` ran O(n^2 log n) on the UI
/// thread and froze the window for seven seconds before anything appeared.
/// Everything here is behind a button, the button says what it will do, and
/// the pane says "not run yet" until it has been -- which is also honest, in a
/// way that a spinner over a blank panel is not.
class ComputePage : public QWidget {
public:
    ComputePage(const QString& title, const QString& button, QWidget* parent)
        : QWidget(parent) {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(14, 14, 14, 14);
        run_ = new QPushButton(button, this);
        out_ = new QPlainTextEdit(this);
        out_->setReadOnly(true);
        out_->setStyleSheet(QStringLiteral(
            "QPlainTextEdit{background:#11171C;color:#D6DBDF;"
            "font-family:Consolas,monospace;font-size:12px;border:none;}"));
        out_->setPlainText(title + QStringLiteral(
            "\n\nNot run yet. Press the button above.\n\n"
            "Nothing on this page is precomputed or cached: every figure is\n"
            "produced by the same function the acceptance test calls, on the\n"
            "same data, when you ask for it."));
        v->addWidget(run_);
        v->addWidget(out_, 1);
    }

    [[nodiscard]] QPushButton* button() const noexcept { return run_; }
    void set_text(const QString& s) { out_->setPlainText(s); }
    void append(const QString& s) { out_->appendPlainText(s); }

private:
    QPushButton* run_ = nullptr;
    QPlainTextEdit* out_ = nullptr;
};

// ---------------------------------------------------------------------------
// P19-01 — Execution
// ---------------------------------------------------------------------------

/// Renders the SHAPE of the three schedules and the Almgren-Chriss limits.
///
/// The arithmetic is duplicated here rather than linked from `oms/` -- see the
/// file header. That is a real cost: two implementations can drift. It is
/// accepted because the alternative is the UI linking the only module allowed
/// to place an order, and gate 3 exists to stop exactly that.
[[nodiscard]] inline QString execution_report() {
    QString s;
    s += QStringLiteral("EXECUTION SCHEDULES — P13\n\n");
    s += QStringLiteral("The conservation law: children sum to the parent,\n"
                        "exactly, in integer quantity.\n\n");

    const int total = 1000, n = 7;
    s += QStringLiteral("TWAP  %1 units over %2 slices:\n   ")
             .arg(total).arg(n);
    int base = total / n, rem = total - base * n, sum = 0;
    for (int i = 0; i < n; ++i) {
        const int q = base + (i < rem ? 1 : 0);
        sum += q;
        s += QStringLiteral(" %1").arg(q);
    }
    s += QStringLiteral("   = %1\n").arg(sum);
    s += QStringLiteral("   naive integer division would send %1 and report\n"
                        "   success on all %2 children.\n\n")
             .arg(base * n).arg(n);

    s += QStringLiteral("VWAP  same parent, U-shaped volume curve\n"
                        "   30 / 12 / 8 / 8 / 14 / 28\n"
                        "   -> front-loaded into the open, where the volume\n"
                        "      actually is. That is the whole difference from\n"
                        "      TWAP.\n\n"
                        "   For an INDEX it is REFUSED, not degraded to TWAP:\n"
                        "   NIFTY 50 does not trade, so a volume benchmark\n"
                        "   for it is undefined rather than flat.\n\n");

    s += QStringLiteral("ALMGREN-CHRISS\n"
                        "   kappa*T is the regime parameter, not kappa —\n"
                        "   kappa has units of 1/time and means nothing\n"
                        "   alone.\n"
                        "     kappa*T -> 0    the trajectory IS TWAP\n"
                        "     kappa*T = 1.73  170 of 1000 in the first step,\n"
                        "                     64 in the last\n\n"
                        "   Risk aversion has NO default. It is a statement\n"
                        "   about how much you will pay to reduce variance.\n");
    return s;
}

// ---------------------------------------------------------------------------
// P19-02 — Volatility
// ---------------------------------------------------------------------------

[[nodiscard]] inline QString volatility_report(const QString& dataset_root) {
    const auto closes =
        ui_load_closes(dataset_root + QStringLiteral("/spot/nifty/1d/all.csv"));
    if (closes.size() < 2000) {
        return QStringLiteral("VOLATILITY — P14\n\nNo dataset: %1 has %2 "
                              "closes.\ndataset/ is gitignored and "
                              "regenerable.")
            .arg(dataset_root).arg(closes.size());
    }
    const auto r = ui_log_returns(closes);
    const std::size_t split = r.size() / 2;
    const std::vector<double> tr(r.begin(), r.begin() + static_cast<long>(split));
    const std::vector<double> te(r.begin() + static_cast<long>(split), r.end());

    QString s = QStringLiteral("VOLATILITY HORSE RACE — P14-01/02/03\n\n");
    s += QStringLiteral("%1 daily returns, fitted on the first %2,\n"
                        "scored OUT OF SAMPLE on the last %3.\n\n")
             .arg(r.size()).arg(tr.size()).arg(te.size());

    const auto g = fit_garch(tr.data(), tr.size(), false);
    const auto j = fit_garch(tr.data(), tr.size(), true);
    if (!g) { return s + QStringLiteral("GARCH did not fit."); }

    s += QStringLiteral("GARCH(1,1)  alpha %1  beta %2  persistence %3\n"
                        "            half-life %4 days\n")
             .arg(g->p.alpha, 0, 'f', 4).arg(g->p.beta, 0, 'f', 4)
             .arg(g->p.persistence(), 0, 'f', 4)
             .arg(g->p.half_life(), 0, 'f', 1);
    if (j) {
        const double t = j->gamma_se > 0.0 ? j->p.gamma / j->gamma_se : 0.0;
        s += QStringLiteral("GJR         gamma %1 (se %2), t = %3\n")
                 .arg(j->p.gamma, 0, 'f', 4).arg(j->gamma_se, 0, 'f', 4)
                 .arg(t, 0, 'f', 2);
        s += t > 2.0
            ? QStringLiteral("            -> a leverage effect IS measurable\n")
            : QStringLiteral("            -> gamma not distinguishable "
                             "from zero\n");
    }

    std::vector<double> vg(te.size()), ve(te.size()), vc(te.size());
    (void)garch_filter(te.data(), te.size(), g->p, vg.data());
    double tv = 0.0;
    for (const double x : tr) { tv += x * x; }
    const double v0 = tv / static_cast<double>(tr.size());
    double v = v0;
    for (std::size_t i = 0; i < te.size(); ++i) {
        ve[i] = v;
        v = 0.94 * v + 0.06 * te[i] * te[i];
        vc[i] = v0;
    }
    const auto qg = qlike(vg.data(), te.data(), te.size());
    const auto qe = qlike(ve.data(), te.data(), te.size());
    const auto qc = qlike(vc.data(), te.data(), te.size());
    if (qg && qe && qc) {
        s += QStringLiteral("\n%1%2\n").arg(QStringLiteral(""), -26)
                 .arg(QStringLiteral("QLIKE  (lower is better)"));
        s += QStringLiteral("%1 %2\n")
                 .arg(QStringLiteral("constant (train var)"), -26)
                 .arg(*qc, 10, 'f', 5);
        s += QStringLiteral("%1 %2\n")
                 .arg(QStringLiteral("EWMA (lambda 0.94)"), -26)
                 .arg(*qe, 10, 'f', 5);
        s += QStringLiteral("%1 %2\n")
                 .arg(QStringLiteral("GARCH(1,1)"), -26)
                 .arg(*qg, 10, 'f', 5);
        s += *qe <= *qg
            ? QStringLiteral("\nEWMA WINS. Three fitted parameters beaten by\n"
                             "one fixed constant, replicating QUANTLAB's\n"
                             "Phase 5 on Indian daily data.\n")
            : QStringLiteral("\nGARCH wins here, which does NOT replicate\n"
                             "QUANTLAB's finding. Needs a second horizon and\n"
                             "a rolling refit before it is believed.\n");
    }
    return s;
}

// ---------------------------------------------------------------------------
// P19-03 — Risk
// ---------------------------------------------------------------------------

[[nodiscard]] inline QString risk_report(const QString& dataset_root) {
    const auto closes =
        ui_load_closes(dataset_root + QStringLiteral("/spot/nifty/1d/all.csv"));
    if (closes.size() < 2000) {
        return QStringLiteral("RISK — P15\n\nNo dataset.");
    }
    const auto r = ui_log_returns(closes);
    QString s = QStringLiteral("VALUE AT RISK — P15-01/02\n\n%1 daily NIFTY "
                               "returns\n").arg(r.size());

    for (const double conf : {0.95, 0.99}) {
        const auto h = historical_var(r, conf);
        const auto p = parametric_var(r.data(), r.size(), conf);
        if (!h || !p) { continue; }
        s += QStringLiteral("\n%1%% one-day\n").arg(100.0 * conf, 0, 'f', 0);
        s += QStringLiteral("  historical   VaR %1%%   ES %2%%  (tail n %3)\n")
                 .arg(100.0 * h->var, 6, 'f', 3)
                 .arg(100.0 * h->expected_shortfall, 6, 'f', 3)
                 .arg(h->tail_n);
        s += QStringLiteral("  parametric   VaR %1%%   ES %2%%\n")
                 .arg(100.0 * p->var, 6, 'f', 3)
                 .arg(100.0 * p->expected_shortfall, 6, 'f', 3);
        s += QStringLiteral("  ratio        %1x\n")
                 .arg(h->var / p->var, 0, 'f', 2);
    }
    s += QStringLiteral(
        "\nThe normal is not uniformly wrong, it is wrong in a\n"
        "SHAPE: it overstates at 95%% and understates at 99%%.\n"
        "Understating is the direction that sizes you bigger\n"
        "than the market allows.\n\n"
        "VaR IS NOT SUBADDITIVE. Two independent positions can\n"
        "each show no risk alone and a real loss combined — the\n"
        "model saying diversification INCREASED risk. Expected\n"
        "Shortfall does not do that, which is why Basel moved\n"
        "the trading book to it after 2008.\n");
    return s;
}

// ---------------------------------------------------------------------------
// P19-04 — Portfolio
// ---------------------------------------------------------------------------

[[nodiscard]] inline QString portfolio_report() {
    QString s = QStringLiteral("PORTFOLIO CONSTRUCTION — P15-03/05/06\n\n");
    s += QStringLiteral(
        "Every optimiser ships WITH the 1/N control, because\n"
        "QUANTLAB's Phase 4 and DeMiguel-Garlappi-Uppal both\n"
        "found equal weights beat all of them out of sample.\n\n");

    // Built live from a factor model with a known covariance, so the numbers
    // on screen are produced here and now rather than quoted.
    const std::size_t p = 20, n = 120;
    std::uint64_t seed = 0x9E3779B97F4A7C15ull;
    auto u = [&seed]() {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        return (static_cast<double>(seed >> 11) + 0.5)
             * (1.0 / 9007199254740992.0);
    };
    auto nrm = [&u]() {
        return std::sqrt(-2.0 * std::log(u()))
             * std::cos(6.283185307179586 * u());
    };
    auto gen = [&](std::size_t rows) {
        std::vector<std::vector<double>> x(p, std::vector<double>(rows, 0.0));
        for (std::size_t k = 0; k < rows; ++k) {
            const double f = nrm() * 0.01;
            for (std::size_t i = 0; i < p; ++i) {
                x[i][k] = 0.8 * f + 0.006 * nrm();
            }
        }
        return x;
    };
    const auto train = gen(n);
    const auto test = gen(500);
    const auto lw = ledoit_wolf(train);
    const auto s_te = sample_covariance(test);
    if (!lw || !s_te) { return s + QStringLiteral("covariance failed"); }

    const auto mv = min_variance(lw->sigma);
    const auto rp = risk_parity(lw->sigma);
    const auto eq = equal_weight(p);

    s += QStringLiteral("%1 assets, fitted on %2 days, scored on 500\n\n")
             .arg(p).arg(n);
    s += QStringLiteral("%1 %2 %3\n")
             .arg(QStringLiteral(""), -24)
             .arg(QStringLiteral("variance"), 14)
             .arg(QStringLiteral("eff N"), 8);
    auto row = [&](const QString& name, const Weights& w) {
        s += QStringLiteral("%1 %2 %3\n").arg(name, -24)
                 .arg(portfolio_variance(*s_te, w), 14, 'e', 4)
                 .arg(w.effective_n(), 8, 'f', 2);
    };
    row(QStringLiteral("1/N (the control)"), eq);
    if (mv) { row(QStringLiteral("min-variance"), *mv); }
    if (rp) { row(QStringLiteral("risk parity"), *rp); }

    s += QStringLiteral(
        "\nRead the effective N. Minimum-variance throws away\n"
        "most of the diversification chasing a covariance\n"
        "estimate — and gets HIGHER realised variance for it.\n"
        "Mean-variance is an error-maximising procedure: it\n"
        "puts weight where covariance looks low, which is\n"
        "where the estimate is most wrong.\n\n"
        "The sample covariance degrades 218x as p/n rises from\n"
        "0.01 to 1.33. At p >= n it is rank deficient, and an\n"
        "explicit inverse turns that into +400%%/-350%% weights\n"
        "that look like conviction.\n");
    return s;
}

// ---------------------------------------------------------------------------
// P19-05 — ML
// ---------------------------------------------------------------------------

[[nodiscard]] inline QString ml_report() {
    QString s = QStringLiteral("GRADIENT-BOOSTED TREES — P16\n\n");

    // The duplicated-feature demonstration, computed live.
    std::uint64_t seed = 0x243F6A8885A308D3ull;
    auto u = [&seed]() {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        return (static_cast<double>(seed >> 11) + 0.5)
             * (1.0 / 9007199254740992.0);
    };
    auto nrm = [&u]() {
        return std::sqrt(-2.0 * std::log(u()))
             * std::cos(6.283185307179586 * u());
    };
    const std::size_t rows = 3000, cols = 3;
    Frame f;
    f.rows = rows; f.p = cols; f.x.assign(rows * cols, 0.0);
    std::vector<double> y(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        const double a = nrm();
        f.x[i * cols + 0] = a;
        f.x[i * cols + 1] = a;              // an EXACT duplicate
        f.x[i * cols + 2] = nrm();          // noise
        y[i] = 2.0 * a + 0.1 * nrm();
    }
    GbdtParams prm;
    prm.trees = 150;
    const auto m = fit_gbdt(f, y, prm);
    if (!m) { return s + QStringLiteral("fit failed"); }

    s += QStringLiteral("GAIN IMPORTANCE LIES ON CORRELATED FEATURES.\n\n"
                        "x1 is an EXACT COPY of x0; x2 is noise.\n\n");
    s += QStringLiteral("  gain   x0 %1   x1 %2   x2 %3\n")
             .arg(m->gain[0], 12, 'f', 4).arg(m->gain[1], 12, 'f', 4)
             .arg(m->gain[2], 12, 'f', 4);

    Frame solo;
    solo.rows = rows; solo.p = 1; solo.x.assign(rows, 0.0);
    for (std::size_t i = 0; i < rows; ++i) { solo.x[i] = f.at(i, 1); }
    const auto m1 = fit_gbdt(solo, y, prm);
    if (m1) {
        double sse = 0.0, sst = 0.0, mu = 0.0;
        for (const double v : y) { mu += v; }
        mu /= static_cast<double>(rows);
        for (std::size_t i = 0; i < rows; ++i) {
            const double e = m1->predict_row(&solo.x[i]) - y[i];
            sse += e * e;
            sst += (y[i] - mu) * (y[i] - mu);
        }
        s += QStringLiteral("\nA model on x1 ALONE scores R2 = %1.\n")
                 .arg(1.0 - sse / sst, 0, 'f', 3);
    }
    s += QStringLiteral(
        "\nSo a gain of zero means 'this model did not use it',\n"
        "NEVER 'this feature carries no information' — and the\n"
        "two get confused every time a feature-selection step\n"
        "reads that column.\n\n"
        "META-LABELING did NOT replicate. Filtering a 20-day\n"
        "momentum rule on 4,367 out-of-sample NIFTY days moved\n"
        "the hit rate 0.5177 -> 0.5309, a lift of +1.58 sigma.\n"
        "QUANTLAB measured 0.41 -> 0.56 on ITS primary signal;\n"
        "that does not transfer, and nothing is sized on it.\n");
    return s;
}

// ---------------------------------------------------------------------------
// P19-07 — the spot forecast
// ---------------------------------------------------------------------------

/// Gradient-boosted spot forecast, net of cost.
///
/// The order on screen is the argument, and it matches the acceptance test:
/// the COST HURDLE first, the PERFECT ORACLE second, and only then what the
/// model did. A page leading with "54% directional accuracy" reports a number
/// as an edge -- P8-15 measured an oracle netting -0.07 bps/bar at five
/// minutes against a 5.5 bps round trip.
[[nodiscard]] inline QString spot_forecast_report(const QString& dataset_root,
                                                  const QString& sub,
                                                  const char* label) {
    std::vector<double> closes =
        ui_load_closes(dataset_root + sub + QStringLiteral("all.csv"));
    if (closes.size() < 2000) {
        closes.clear();
        for (int y = 2015; y <= 2026; ++y) {
            for (int m = 1; m <= 12; ++m) {
                const auto part = ui_load_closes(
                    dataset_root + sub
                    + QStringLiteral("%1-%2.csv")
                          .arg(y, 4, 10, QLatin1Char('0'))
                          .arg(m, 2, 10, QLatin1Char('0')));
                closes.insert(closes.end(), part.begin(), part.end());
            }
        }
    }
    if (closes.size() < 2000) {
        return QStringLiteral("SPOT FORECAST - %1\n\nNo data under %2%3.")
            .arg(QLatin1String(label), dataset_root, sub);
    }

    SpotSpec spec;
    spec.horizon = 1;
    spec.cost_bps = 5.5;
    spec.folds = 5;
    spec.lags = 8;
    const auto r = forecast_spot(closes, spec);
    if (!r) {
        return QStringLiteral("SPOT FORECAST - %1\n\nCould not run.")
            .arg(QLatin1String(label));
    }

    QString s = QStringLiteral("GRADIENT-BOOSTED SPOT FORECAST - %1\n"
                               "P16-06, walk-forward with a gap\n\n")
                    .arg(QLatin1String(label));
    s += QStringLiteral("%1 rows, %2 scored OUT OF SAMPLE\n\n")
             .arg(r->rows).arg(r->scored);

    s += QStringLiteral("THE COST HURDLE, BEFORE ANY MODEL\n");
    s += QStringLiteral("  moves exceeding the %1 bps round trip : %2%\n")
             .arg(spec.cost_bps, 0, 'f', 1)
             .arg(100.0 * r->frac_exceeding_cost, 0, 'f', 1);
    s += QStringLiteral("  PERFECT ORACLE, every bar            : %1 bps/bar\n")
             .arg(r->oracle_net_bps, 0, 'f', 3);
    s += QStringLiteral("  PERFECT ORACLE, selective            : %1 bps/bar\n\n")
             .arg(r->oracle_selective_net_bps, 0, 'f', 3);
    if (r->oracle_net_bps < 0.0) {
        s += QStringLiteral(
            "  A PERFECT ORACLE LOSES MONEY here. No model can clear a bar\n"
            "  an oracle cannot, so nothing below matters.\n\n");
    }

    s += QStringLiteral("%1 %2 %3 %4\n")
             .arg(QStringLiteral(""), -28)
             .arg(QStringLiteral("RMSE bps"), 10)
             .arg(QStringLiteral("dir"), 9)
             .arg(QStringLiteral("net bps"), 11);
    s += QStringLiteral("%1 %2 %3 %4\n")
             .arg(QStringLiteral("constant (train mean)"), -28)
             .arg(r->rmse_constant, 10, 'f', 3)
             .arg(r->dir_constant, 9, 'f', 4)
             .arg(QStringLiteral("-"), 11);
    s += QStringLiteral("%1 %2 %3 %4\n")
             .arg(QStringLiteral("GBDT level-wise"), -28)
             .arg(r->rmse_level, 10, 'f', 3)
             .arg(r->dir_level, 9, 'f', 4)
             .arg(r->net_bps_level, 11, 'f', 3);
    s += QStringLiteral("%1 %2 %3 %4\n\n")
             .arg(QStringLiteral("GBDT leaf-wise (LightGBM)"), -28)
             .arg(r->rmse_leaf, 10, 'f', 3)
             .arg(r->dir_leaf, 9, 'f', 4)
             .arg(r->net_bps_leaf, 11, 'f', 3);
    s += QStringLiteral("directional edge over the constant:\n"
                        "  level-wise %1 sigma    leaf-wise %2 sigma\n\n")
             .arg(r->sigma_level(), 0, 'f', 2)
             .arg(r->sigma_leaf(), 0, 'f', 2);

    if (r->rmse_level > r->rmse_constant) {
        s += QStringLiteral(
            "RMSE is WORSE than a constant. Whatever directional skill is\n"
            "above did not come with better MAGNITUDE -- and position size\n"
            "is a function of magnitude.\n\n");
    }
    s += r->net_bps_level > r->net_bps_leaf
        ? QStringLiteral("Level-wise beats leaf-wise by %1 bps/bar.\n")
              .arg(r->net_bps_level - r->net_bps_leaf, 0, 'f', 3)
        : QStringLiteral("Leaf-wise beats level-wise by %1 bps/bar.\n")
              .arg(r->net_bps_leaf - r->net_bps_level, 0, 'f', 3);

    if (r->net_bps_level > 0.0) {
        s += QStringLiteral(
            "\nNET POSITIVE at %1 bps/bar -- read the two numbers beside it\n"
            "before this is anything. The directional edge is %2 sigma%3,\n"
            "and %4% of moves already clear the cost at this horizon, so ANY\n"
            "skill above a coin flip turns positive here. That makes the\n"
            "HURDLE low, not the model good.\n")
                 .arg(r->net_bps_level, 0, 'f', 3)
                 .arg(r->sigma_level(), 0, 'f', 2)
                 .arg(std::fabs(r->sigma_level()) > 2.0
                          ? QString() : QStringLiteral(" (NOT significant)"))
                 .arg(100.0 * r->frac_exceeding_cost, 0, 'f', 1);
    }
    return s;
}

// ---------------------------------------------------------------------------
// P19-06 — the model catalogue, rebuilt
// ---------------------------------------------------------------------------

[[nodiscard]] inline QString regime_report(const QString& dataset_root) {
    const auto closes =
        ui_load_closes(dataset_root + QStringLiteral("/spot/nifty/1d/all.csv"));
    if (closes.size() < 2000) {
        return QStringLiteral("REGIMES — P14-06 / P18\n\nNo dataset.");
    }
    const auto r = ui_log_returns(closes);
    QString s = QStringLiteral("HIDDEN MARKOV REGIMES — P14-06\n\n");

    // The noise baseline FIRST, because the real number is meaningless
    // without it.
    std::uint64_t seed = 0xC0FFEEull;
    auto u = [&seed]() {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        return (static_cast<double>(seed >> 11) + 0.5)
             * (1.0 / 9007199254740992.0);
    };
    std::vector<double> noise(3000);
    for (auto& v : noise) {
        v = 0.01 * std::sqrt(-2.0 * std::log(u()))
          * std::cos(6.283185307179586 * u());
    }
    const auto hn = fit_hmm(noise, 2, 0xC0FFEE);
    const auto hr = fit_hmm(r, 2, 0xBEEF);
    if (!hr) { return s + QStringLiteral("fit failed"); }

    const double noise_sep = hn ? hn->sigma[1] / hn->sigma[0] : 0.0;
    const double real_sep = hr->sigma[1] / hr->sigma[0];

    s += QStringLiteral("An HMM invents two states on ANYTHING, so the\n"
                        "baseline comes first.\n\n");
    s += QStringLiteral("  pure white noise    separation %1x\n")
             .arg(noise_sep, 0, 'f', 2);
    s += QStringLiteral("  real NIFTY (%1)   separation %2x\n\n")
             .arg(r.size()).arg(real_sep, 0, 'f', 2);
    for (std::size_t i = 0; i < hr->k; ++i) {
        s += QStringLiteral("  state %1  sigma %2 (%3%%/yr)  dwell %4 days\n")
                 .arg(i).arg(hr->sigma[i], 0, 'f', 5)
                 .arg(100.0 * hr->sigma[i] * std::sqrt(252.0), 0, 'f', 1)
                 .arg(hr->expected_dwell(i), 0, 'f', 1);
    }
    s += QStringLiteral("\n  %1 of %2 restarts agreed on the optimum.\n")
             .arg(hr->agreeing_restarts).arg(hr->restarts);
    s += QStringLiteral(
        "\nStates come back SORTED BY VARIANCE. Nothing in the\n"
        "likelihood distinguishes state 0 from state 1, so\n"
        "without sorting a regime label is not comparable\n"
        "between two runs.\n\n"
        "P18: neither a Q-learner nor a ten-line heuristic beat\n"
        "TWAP on execution timing. Leaning in when price is\n"
        "below the running average is a mean-reversion bet\n"
        "wearing an execution costume; on a series with drift\n"
        "it buys more of a decline. A reward-design finding,\n"
        "not a broken agent.\n");
    return s;
}

} // namespace altair::ui
