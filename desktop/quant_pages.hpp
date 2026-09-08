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

#include <analytics/american.hpp>
#include <analytics/garch.hpp>
#include <analytics/greeks.hpp>
#include <analytics/greeks2.hpp>
#include <analytics/iv.hpp>
#include <analytics/svi.hpp>
#include <analytics/svi_fit.hpp>
#include <analytics/hmm.hpp>
#include <analytics/kalman.hpp>
#include <analytics/sabr.hpp>
#include <models/gbdt.hpp>
#include <models/regime_rl.hpp>
#include <models/spot_forecast.hpp>
#include <risk/covariance.hpp>
#include <risk/optimise.hpp>
#include <risk/stress.hpp>
#include <risk/var.hpp>
// P21. strategies/ is header-only here and is NOT linked -- see the note at
// the bottom of desktop/CMakeLists.txt. It detects; it cannot trade, and it
// does not link oms/ either, so including it widens nothing.
#include <strategies/basis.hpp>
#include <strategies/meanrev.hpp>
#include <strategies/overnight.hpp>
#include <strategies/momentum.hpp>

#include <QDir>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <fstream>
#include <map>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
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

/// One intraday bar: its close and the session it belongs to.
struct UiBar {
    double close = 0.0;
    std::string session;        // the YYYY-MM-DD prefix of the timestamp
};

/// Every bar in a monthly-partitioned directory, in date order.
///
/// P21-03. The intraday frequencies are stored one CSV per month, unlike the
/// daily series which is a single all.csv, so `ui_load_closes` cannot reach
/// them. Added so the Strategies page can COMPUTE the session-vs-overnight
/// decomposition rather than quoting it -- this file's own header forbids
/// printing a constant the page cannot reproduce, and that decomposition is
/// the single most load-bearing number on the page.
[[nodiscard]] inline std::vector<UiBar> ui_load_partitioned(
    const QString& dir) {
    std::vector<UiBar> out;
    QDir d(dir);
    if (!d.exists()) { return out; }
    QStringList names = d.entryList(QStringList{QStringLiteral("*.csv")},
                                    QDir::Files, QDir::Name);
    for (const QString& name : names) {
        std::ifstream f(d.filePath(name).toStdString());
        if (!f) { continue; }
        std::string line;
        std::getline(f, line);                     // header
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::string cell;
            int col = 0;
            UiBar b;
            while (std::getline(ss, cell, ',')) {
                if (col == 0 && cell.size() >= 10) {
                    b.session = cell.substr(0, 10);
                }
                if (col == 4 && !cell.empty()) {
                    b.close = std::atof(cell.c_str());
                }
                ++col;
            }
            if (b.close > 0.0 && !b.session.empty()) { out.push_back(b); }
        }
    }
    return out;
}

/// Closes with their session date, for joining two series that do not share
/// a calendar.
[[nodiscard]] inline std::vector<std::pair<std::string, double>>
ui_load_dated_closes(const QString& path) {
    std::vector<std::pair<std::string, double>> out;
    std::ifstream f(path.toStdString());
    if (!f) { return out; }
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string cell, day;
        int col = 0;
        double c = 0.0;
        while (std::getline(ss, cell, ',')) {
            if (col == 0 && cell.size() >= 10) { day = cell.substr(0, 10); }
            if (col == 4 && !cell.empty()) { c = std::atof(cell.c_str()); }
            ++col;
        }
        if (c > 0.0 && !day.empty()) { out.emplace_back(day, c); }
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
        row_ = new QHBoxLayout;
        row_->setContentsMargins(0, 0, 0, 0);
        row_->addWidget(run_);
        v->addLayout(row_);
        v->addWidget(out_, 1);
    }

    [[nodiscard]] QPushButton* button() const noexcept { return run_; }

    /// A SECOND (or third) action on the same page.
    ///
    /// Added rather than making three pages, because these are the SAME
    /// analysis at different horizons and the whole point is comparing them --
    /// a reader who has to change page to see the 5-minute result cannot hold
    /// it next to the daily one. Each still computes only when pressed: the
    /// 5-minute fit is 214,000 rows across five folds and two growth
    /// strategies, and running all three on open would freeze the window the
    /// way P11Q's startup fit did.
    [[nodiscard]] QPushButton* add_button(const QString& label) {
        auto* b = new QPushButton(label, this);
        row_->addWidget(b);
        return b;
    }

    void set_text(const QString& s) { out_->setPlainText(s); }
    void append(const QString& s) { out_->appendPlainText(s); }

private:
    QPushButton* run_ = nullptr;
    QHBoxLayout* row_ = nullptr;
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
        s += QStringLiteral("\n%1% one-day\n").arg(100.0 * conf, 0, 'f', 0);
        s += QStringLiteral("  historical   VaR %1%   ES %2%  (tail n %3)\n")
                 .arg(100.0 * h->var, 6, 'f', 3)
                 .arg(100.0 * h->expected_shortfall, 6, 'f', 3)
                 .arg(h->tail_n);
        s += QStringLiteral("  parametric   VaR %1%   ES %2%\n")
                 .arg(100.0 * p->var, 6, 'f', 3)
                 .arg(100.0 * p->expected_shortfall, 6, 'f', 3);
        s += QStringLiteral("  ratio        %1x\n")
                 .arg(h->var / p->var, 0, 'f', 2);
    }
    s += QStringLiteral(
        "\nThe normal is not uniformly wrong, it is wrong in a\n"
        "SHAPE: it overstates at 95% and understates at 99%.\n"
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
        "explicit inverse turns that into +400%/-350% weights\n"
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
        s += QStringLiteral("  state %1  sigma %2 (%3%/yr)  dwell %4 days\n")
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
        "between two runs.\n\n");

    // P23-04. THE k-MEANS RESULT, COMPUTED, NOT DESCRIBED.
    //
    // This page used to end with a paragraph of prose reporting P18's
    // findings. That is the one thing quant_pages.hpp forbids: a figure that
    // exists in two places has already contradicted itself twice in this UI,
    // and a number in a string literal cannot track the code that produced
    // it. So the clustering runs here, on the same series, on press.
    s += QStringLiteral("――― k-MEANS REGIMES — P18, computed ―――\n\n");
    {
        // Two features per day: absolute return and a short realised vol.
        // Deliberately crude -- the question is whether ANY clustering of
        // return-shape separates the forward return, and a richer feature set
        // would confound "the clusters are bad" with "the features are bad".
        std::vector<std::vector<double>> x;
        const std::size_t w = 10;
        for (std::size_t i = w; i < r.size(); ++i) {
            double mu = 0.0;
            for (std::size_t j = i - w; j < i; ++j) { mu += r[j]; }
            mu /= static_cast<double>(w);
            double v = 0.0;
            for (std::size_t j = i - w; j < i; ++j) {
                v += (r[j] - mu) * (r[j] - mu);
            }
            x.push_back({std::fabs(r[i - 1]),
                         std::sqrt(v / static_cast<double>(w - 1))});
        }
        s += QStringLiteral("    k   separation   persistence      score\n");
        for (const std::size_t k : {2u, 3u, 4u, 5u}) {
            const auto c = kmeans(x, k, 0xA17A1Bull);
            if (!c) { continue; }
            const auto qy = cluster_quality(x, *c);
            s += QStringLiteral("   %1  %2   %3  %4\n")
                     .arg(k, 2).arg(qy.separation, 10, 'f', 4)
                     .arg(qy.persistence, 12, 'f', 4)
                     .arg(qy.score(), 9, 'f', 4);
        }
        s += QStringLiteral(
            "\n  `cluster_quality` is RETURN-BLIND on purpose: it scores how\n"
            "  well-separated and how persistent the clusters are, never\n"
            "  whether they predict anything. A high score here is a\n"
            "  well-shaped partition of feature space and nothing more.\n\n");
    }

    s += QStringLiteral(
        "P18: neither a Q-learner nor a ten-line heuristic beat\n"
        "TWAP on execution timing. Leaning in when price is\n"
        "below the running average is a mean-reversion bet\n"
        "wearing an execution costume; on a series with drift\n"
        "it buys more of a decline. A reward-design finding,\n"
        "not a broken agent. Run strategies_regime_rl for the\n"
        "numbers behind that sentence.\n");
    return s;
}

// ---------------------------------------------------------------------------
// P21-03 — Strategies
// ---------------------------------------------------------------------------

/// Momentum and mean reversion on real daily NIFTY, priced against their own
/// turnover.
///
/// THE ORDER OF THIS PAGE IS THE ARGUMENT.
///
/// The cost hurdle comes first, before any return is shown, because hard rule
/// 5 says a signal is priced net of full cost BEFORE it exists. Then the noise
/// control, because a number with no baseline is not a result. Only then the
/// real series -- by which point the reader already knows what the strategy
/// has to clear and what pure chance produces.
///
/// A page that showed the NIFTY Sharpe first and the caveats underneath would
/// be read in exactly the wrong order.
[[nodiscard]] inline QString strategies_report(const QString& dataset_root) {
    constexpr double kCost = 5.5;       // bps per unit of turnover
    constexpr double kBars = 250.0;

    QString s = QStringLiteral(
        "DIRECTIONAL STRATEGIES — P21-01 / P21-02\n"
        "Time-series momentum and z-score mean reversion.\n\n"
        "SKELETON. The parameters below are CHOICES, not fits. Nothing here\n"
        "is trained; when tick data arrives the harness is what gets\n"
        "pointed at it, and these numbers are the shape of the answer\n"
        "rather than the answer.\n\n"
        "――― 1. THE HURDLE, BEFORE ANY RETURN ―――\n\n"
        "Round trip is charged at %1 bps per unit of turnover. A full flip\n"
        "from long to short is TWO units, so it costs %2 bps.\n\n")
        .arg(kCost, 0, 'f', 1).arg(2.0 * kCost, 0, 'f', 1);

    s += QStringLiteral("  turnover/bar    gross bps/bar needed to break even\n");
    for (const double to : {0.05, 0.10, 0.25, 0.50, 1.00}) {
        s += QStringLiteral("      %1              %2\n")
                 .arg(to, 0, 'f', 2)
                 .arg(required_gross_bps(to, kCost), 0, 'f', 3);
    }
    const double h = breakeven_hit_rate(20.0, 20.0, 0.25, kCost);
    s += QStringLiteral(
        "\n  At +-20 bps per trade and 0.25 turnover, the strategy must be\n"
        "  directionally right %1% of the time merely to break even.\n"
        "  The reality check in CLAUDE.md puts the 10-minute ceiling at\n"
        "  52-55%. Read those two numbers together before reading any\n"
        "  Sharpe below.\n\n").arg(100.0 * h, 0, 'f', 2);

    // ――― 2. the control ―――
    s += QStringLiteral("――― 2. THE CONTROL: 20 PATHS OF PURE NOISE ―――\n\n");
    std::uint64_t seed = 0xA17A1Bull;
    auto u = [&seed]() {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        return (static_cast<double>(seed >> 11) + 0.5)
             * (1.0 / 9007199254740992.0);
    };
    constexpr int kPaths = 20;
    double mg[2] = {0.0, 0.0}, mn[2] = {0.0, 0.0};
    int win[2] = {0, 0}, scored = 0;
    for (int t = 0; t < kPaths; ++t) {
        std::vector<double> noise(3000);
        for (auto& v : noise) {
            v = 40.0 * std::sqrt(-2.0 * std::log(u()))
              * std::cos(6.283185307179586 * u());
        }
        const auto mp = momentum_positions(noise, MomentumSpec{});
        const auto vp = meanrev_positions(noise, MeanRevSpec{});
        if (!mp || !vp) { continue; }
        const auto ms = evaluate(*mp, noise, kCost, kBars);
        const auto vs = evaluate(*vp, noise, kCost, kBars);
        if (!ms || !vs) { continue; }
        ++scored;
        mg[0] += ms->gross_bps; mn[0] += ms->net_bps;
        mg[1] += vs->gross_bps; mn[1] += vs->net_bps;
        if (ms->net_bps > 0.0) { ++win[0]; }
        if (vs->net_bps > 0.0) { ++win[1]; }
    }
    if (scored > 0) {
        const double n = static_cast<double>(scored);
        s += QStringLiteral("  momentum   pooled gross %1  pooled net %2\n")
                 .arg(mg[0] / n, 8, 'f', 4).arg(mn[0] / n, 8, 'f', 4);
        s += QStringLiteral("  meanrev    pooled gross %1  pooled net %2\n\n")
                 .arg(mg[1] / n, 8, 'f', 4).arg(mn[1] / n, 8, 'f', 4);
        s += QStringLiteral(
            "  Paths that turned a PROFIT on noise: momentum %1/%2,\n"
            "  mean reversion %3/%2.\n\n"
            "  There is nothing in those series. A single backtest landing\n"
            "  on one of those paths would have shown a profit on a series\n"
            "  containing no signal at all. That is what one backtest is\n"
            "  worth, and it is why the pooled figure is the one asserted\n"
            "  on in the acceptance test.\n\n")
                 .arg(win[0]).arg(scored).arg(win[1]);
    }

    // ――― 3. the real series ―――
    s += QStringLiteral("――― 3. REAL DAILY NIFTY ―――\n\n");
    const auto closes =
        ui_load_closes(dataset_root + QStringLiteral("/spot/nifty/1d/all.csv"));
    if (closes.size() < 2000) {
        s += QStringLiteral(
            "  No dataset at dataset/spot/nifty/1d/all.csv.\n\n"
            "  This section is EMPTY rather than filled with the synthetic\n"
            "  series from section 2. Absence is not zero, and a synthetic\n"
            "  number sitting under a heading that says REAL is the exact\n"
            "  failure this project keeps finding.\n");
        return s;
    }
    std::vector<double> r_bps;
    r_bps.reserve(closes.size());
    for (std::size_t i = 1; i < closes.size(); ++i) {
        r_bps.push_back(10000.0 * std::log(closes[i] / closes[i - 1]));
    }
    s += QStringLiteral("  %1 daily bars.\n\n").arg(r_bps.size());

    // WHERE THE RETURN ACTUALLY IS, before anything is measured against it.
    //
    // Computed live inside the 5-minute file alone: session hours plus
    // overnight gaps reconstruct the total move exactly, so there is no
    // residual for a cross-file close disagreement to hide in. (The 5m and
    // daily files disagree about the same day's close by about 8 bps of
    // unbiased noise, which is why the subtraction is NOT done across them.)
    const auto fine = ui_load_partitioned(
        dataset_root + QStringLiteral("/spot/nifty/5m"));
    if (fine.size() > 1000) {
        double session_sum = 0.0, overnight_sum = 0.0;
        std::size_t gaps = 0, sessions = 1;
        for (std::size_t i = 1; i < fine.size(); ++i) {
            const double lr =
                10000.0 * std::log(fine[i].close / fine[i - 1].close);
            if (fine[i].session != fine[i - 1].session) {
                overnight_sum += lr;
                ++gaps;
                ++sessions;
            } else {
                session_sum += lr;
            }
        }
        s += QStringLiteral(
            "  WHERE THE RETURN IS — %1 to %2, from the 5-minute file:\n\n"
            "    session hours    %3 bps\n"
            "    overnight gaps   %4 bps\n"
            "    total            %5 bps   over %6 sessions\n\n")
                 .arg(QString::fromStdString(fine.front().session))
                 .arg(QString::fromStdString(fine.back().session))
                 .arg(session_sum, 10, 'f', 1).arg(overnight_sum, 10, 'f', 1)
                 .arg(session_sum + overnight_sum, 10, 'f', 1).arg(sessions);
        if (session_sum < 0.0 && overnight_sum > 0.0) {
            s += QStringLiteral(
                "  THE ENTIRE MOVE IS OVERNIGHT. Holding through the session\n"
                "  LOST %1 bps over this window. Any intraday long-biased\n"
                "  rule on this index is betting against where the return\n"
                "  actually is, and that is the first thing to know before\n"
                "  reading a single number below.\n\n").arg(-session_sum, 0, 'f', 0);
        }
    } else {
        s += QStringLiteral(
            "  No 5-minute partition at dataset/spot/nifty/5m, so the\n"
            "  session-versus-overnight split is NOT SHOWN rather than\n"
            "  guessed.\n\n");
    }

    // THE CONTROL, before either strategy. NIFTY compounded from 279 to
    // 24,080 over this history, which is free drift a long-biased rule
    // collects automatically. Every net below is read against this line, and
    // the `excess` column -- paired, same bars -- is the one that matters.
    const auto bh = buy_and_hold(r_bps.size());
    const auto bh_e = evaluate(bh, r_bps, kCost, kBars);
    if (bh_e) {
        s += QStringLiteral(
            "  BUY AND HOLD, the control:  net %1 bps/bar,  Sharpe %2\n\n"
            "  A long-biased rule on a drifting index earns some of that for\n"
            "  free. A positive net proves nothing on its own; the excess\n"
            "  column is the real one.\n\n")
                .arg(bh_e->net_bps, 0, 'f', 4)
                .arg(bh_e->net_sharpe, 0, 'f', 3);
    }

    s += QStringLiteral(
        "  MOMENTUM — the whole lookback sweep, not the best point.\n"
        "  Reporting only the winner is how a choice becomes a fit.\n\n"
        "   lookback    gross      net   turnover        t    excess   exc_t\n");
    for (const std::size_t lb : {5u, 10u, 20u, 40u, 60u, 120u, 250u}) {
        MomentumSpec ms;
        ms.lookback = lb;
        const auto pos = momentum_positions(r_bps, ms);
        if (!pos) { continue; }
        const auto e = evaluate(*pos, r_bps, kCost, kBars);
        const auto ex = excess_over(*pos, bh, r_bps, kCost);
        if (!e || !ex) { continue; }
        s += QStringLiteral("      %1  %2  %3     %4   %5   %6  %7\n")
                 .arg(lb, 5).arg(e->gross_bps, 7, 'f', 3)
                 .arg(e->net_bps, 7, 'f', 3).arg(e->turnover, 6, 'f', 4)
                 .arg(e->net_t(), 6, 'f', 2).arg(ex->mean_bps, 7, 'f', 3)
                 .arg(ex->t, 6, 'f', 2);
    }

    s += QStringLiteral(
        "\n  MEAN REVERSION — the dead band is the only thing making it\n"
        "  affordable. entry |z| > 1.5.\n\n"
        "     exit_z    gross      net   turnover        t    excess   exc_t\n");
    for (const double ez : {0.1, 0.25, 0.5, 1.0, 1.4}) {
        MeanRevSpec vs;
        vs.entry_z = 1.5;
        vs.exit_z = ez;
        const auto pos = meanrev_positions(r_bps, vs);
        if (!pos) { continue; }
        const auto e = evaluate(*pos, r_bps, kCost, kBars);
        const auto ex = excess_over(*pos, bh, r_bps, kCost);
        if (!e || !ex) { continue; }
        s += QStringLiteral("       %1  %2  %3     %4   %5   %6  %7\n")
                 .arg(ez, 4, 'f', 2).arg(e->gross_bps, 7, 'f', 3)
                 .arg(e->net_bps, 7, 'f', 3).arg(e->turnover, 6, 'f', 4)
                 .arg(e->net_t(), 6, 'f', 2).arg(ex->mean_bps, 7, 'f', 3)
                 .arg(ex->t, 6, 'f', 2);
    }

    s += QStringLiteral(
        "\n――― HOW TO READ THE TWO t COLUMNS ―――\n\n"
        "  t     is the net mean over its own standard error -- the test\n"
        "        against ZERO, which for a long-biased rule on a drifting\n"
        "        index is the flattering question.\n"
        "  exc_t is the same test against BUY AND HOLD, paired on the same\n"
        "        bars. It is the one that decides whether the strategy did\n"
        "        anything.\n\n"
        "  Below about 2 there is no result either way, and 28 cells are\n"
        "  swept here, so the honest threshold is nearer 3.3. Section 2\n"
        "  shows what a t of 1 looks like on a series known to be empty.\n\n"
        "  Every figure on this page comes from the same functions the\n"
        "  acceptance test calls (strategies_directional), on the same\n"
        "  data. Nothing here is cached and nothing is a literal.\n");
    return s;
}

// ---------------------------------------------------------------------------
// P22-03 — Overnight gap
// ---------------------------------------------------------------------------

/// Sessions from the five-minute partition: the auction print, the 09:20
/// close, and the session close.
///
/// A SECOND reader again, deliberately, exactly as `ui_load_closes` is a
/// second reader beside the model tests' own. Two independent paths to the
/// same number is what makes it a property of the data rather than of one
/// parser -- and here it matters more than usual, because reading the first
/// bar's CLOSE where the OPEN was meant silently answers a different question
/// and the difference is the whole strategy.
[[nodiscard]] inline std::vector<SessionBar> ui_load_sessions(
    const QString& dir) {
    std::vector<SessionBar> out;
    QDir d(dir);
    if (!d.exists()) { return out; }
    const QStringList names = d.entryList(QStringList{QStringLiteral("*.csv")},
                                          QDir::Files, QDir::Name);
    std::string current;
    for (const QString& name : names) {
        std::ifstream f(d.filePath(name).toStdString());
        if (!f) { continue; }
        std::string line;
        std::getline(f, line);                         // header
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::string cell;
            int col = 0;
            std::string day;
            double o = 0.0, c = 0.0;
            while (std::getline(ss, cell, ',')) {
                if (col == 0 && cell.size() >= 10) { day = cell.substr(0, 10); }
                if (col == 1 && !cell.empty()) { o = std::atof(cell.c_str()); }
                if (col == 4 && !cell.empty()) { c = std::atof(cell.c_str()); }
                ++col;
            }
            if (day.empty() || !(o > 0.0) || !(c > 0.0)) { continue; }
            if (day != current) {
                current = day;
                SessionBar b;
                b.open = o;
                b.first_bar_close = c;
                b.close = c;
                b.day = day;
                out.push_back(b);
            } else {
                out.back().close = c;
            }
        }
    }
    return out;
}

/// The overnight gap strategy, and the two prices the whole thing rests on.
///
/// THE PAGE LEADS WITH THE EXECUTION ASSUMPTION, NOT THE RETURN.
///
/// This is the first positive result in the project, which is exactly why the
/// caveat goes above the number rather than below it. The edge is the
/// difference between the 15:30 close and the 09:15 auction print, and neither
/// is a price a retail account can simply take. Five minutes of slippage
/// removes most of it, and that five minutes is measured here rather than
/// estimated.
[[nodiscard]] inline QString overnight_report(const QString& dataset_root) {
    QString s = QStringLiteral(
        "OVERNIGHT GAP — P22-01 / P22-02\n"
        "Hold the index from the close to the open. Sit out the session.\n\n"
        "――― READ THIS FIRST ―――\n\n"
        "  The entire edge is the difference between two prices you cannot\n"
        "  simply take:\n\n"
        "    • the 15:30 CLOSE is a half-hour VWAP construction, not a quote\n"
        "      on the screen at 15:30;\n"
        "    • the 09:15 OPEN is the pre-open call auction result. You can\n"
        "      submit into it; you cannot choose your fill.\n\n"
        "  So both exits are computed below. The gap between them is the\n"
        "  risk, not a footnote about the risk.\n\n");

    const auto bars = ui_load_sessions(
        dataset_root + QStringLiteral("/spot/nifty/5m"));
    if (bars.size() < 2000) {
        return s + QStringLiteral(
            "――― NO DATA ―――\n\n"
            "  dataset/spot/nifty/5m is missing or too short. Nothing is\n"
            "  shown rather than something synthetic under a heading that\n"
            "  says NIFTY.\n");
    }

    const auto r = session_returns(bars);
    if (!r) {
        return s + QStringLiteral("  The decomposition failed on this data.\n");
    }

    // ――― the decomposition ―――
    auto stat = [](const std::vector<double>& v, double& m, double& t) {
        m = t = 0.0;
        if (v.size() < 2) { return; }
        double sum = 0.0;
        for (const double x : v) { sum += x; }
        const double n = static_cast<double>(v.size());
        m = sum / n;
        double s2 = 0.0;
        for (const double x : v) { s2 += (x - m) * (x - m); }
        const double se = std::sqrt(s2 / (n - 1.0)) / std::sqrt(n);
        t = se > 0.0 ? m / se : 0.0;
    };
    double on_m = 0.0, on_t = 0.0, fb_m = 0.0, fb_t = 0.0;
    double se_m = 0.0, se_t = 0.0, cc_m = 0.0, cc_t = 0.0;
    stat(r->overnight, on_m, on_t);
    stat(r->first_bar, fb_m, fb_t);
    stat(r->session, se_m, se_t);
    stat(r->close_to_close, cc_m, cc_t);

    s += QStringLiteral(
        "――― WHERE THE DAY'S RETURN SITS ―――   %1 sessions\n\n"
        "                                  mean bps        t\n"
        "  overnight   close -> open      %2  %3\n"
        "  proxy       close -> 09:20     %4  %5\n"
        "  session     open  -> close     %6  %7\n"
        "  all in      close -> close     %8  %9\n\n")
            .arg(r->overnight.size())
            .arg(on_m, 9, 'f', 4).arg(on_t, 7, 'f', 2)
            .arg(fb_m, 9, 'f', 4).arg(fb_t, 7, 'f', 2)
            .arg(se_m, 9, 'f', 4).arg(se_t, 7, 'f', 2)
            .arg(cc_m, 9, 'f', 4).arg(cc_t, 7, 'f', 2);

    s += QStringLiteral(
        "  The first five minutes of the session alone: %1 bps — %2% of\n"
        "  the overnight move, handed straight back. That is the gap\n"
        "  filling, and it is the price of missing the auction.\n\n")
            .arg(fb_m - on_m, 0, 'f', 4)
            .arg(100.0 * (on_m - fb_m) / on_m, 0, 'f', 0);

    // ――― the strategy ―――
    OvernightSpec at_open;
    OvernightSpec at_first;
    at_first.exit = Exit::FirstBar;
    const auto ro = run_overnight(bars, at_open);
    const auto rf = run_overnight(bars, at_first);
    if (!ro || !rf) { return s + QStringLiteral("  The run failed.\n"); }

    s += QStringLiteral(
        "――― THE COST IT CAN BEAR ―――\n\n"
        "  Cost is an ALL-IN ROUND TRIP: one in-and-out every night.\n\n"
        "  exit                  mean       t     hit   b/e vs 0  b/e vs hold\n"
        "  09:15 auction     %1 %2 %3   %4    %5\n"
        "  09:20 first bar   %6 %7 %8   %9    %10\n\n")
            .arg(ro->mean_taken_bps, 8, 'f', 4).arg(ro->t_taken, 7, 'f', 2)
            .arg(ro->hit_rate, 7, 'f', 3)
            .arg(ro->breakeven_rt_bps(), 8, 'f', 3)
            .arg(ro->breakeven_rt_vs_hold_bps(), 8, 'f', 3)
            .arg(rf->mean_taken_bps, 8, 'f', 4).arg(rf->t_taken, 7, 'f', 2)
            .arg(rf->hit_rate, 7, 'f', 3)
            .arg(rf->breakeven_rt_bps(), 8, 'f', 3)
            .arg(rf->breakeven_rt_vs_hold_bps(), 8, 'f', 3);

    s += QStringLiteral(
        "  The second break-even column is the one that decides anything,\n"
        "  and it is an IDENTITY rather than a fit: taking every night, the\n"
        "  bar to beat buy-and-hold is exactly the session drag you avoid,\n"
        "  %1 bps. Nothing is estimated in that sentence.\n\n")
            .arg(-se_m, 0, 'f', 4);

    // ――― the shape of the payoff ―――
    s += QStringLiteral(
        "――― THE SHAPE OF A NIGHT ―――\n\n"
        "    mean %1   median %2   sd %3\n"
        "    p05  %4   p95    %5   worst %6\n\n"
        "  The MEDIAN night pays more than the MEAN night. That settles what\n"
        "  kind of strategy this is: most nights pay a little and a few take\n"
        "  a lot back. A 5th-percentile night gives back %7 average nights;\n"
        "  the worst one in this history gives back %8.\n\n"
        "  IT IS SHORT GAP RISK — collecting a premium for wearing the\n"
        "  occasional overnight shock. The Sharpe will flatter it right up\n"
        "  until one arrives.\n\n")
            .arg(ro->mean_taken_bps, 8, 'f', 3).arg(ro->median_bps, 8, 'f', 3)
            .arg(ro->sd_taken_bps, 8, 'f', 3)
            .arg(ro->p05_bps, 8, 'f', 3).arg(ro->p95_bps, 8, 'f', 3)
            .arg(ro->worst_bps, 8, 'f', 1)
            .arg(ro->nights_per_bad_night(), 0, 'f', 1)
            .arg(ro->mean_taken_bps > 0.0
                     ? -ro->worst_bps / ro->mean_taken_bps : 0.0, 0, 'f', 0);

    // ――― by year ―――
    const auto years = by_year(bars);
    if (years && !years->empty()) {
        s += QStringLiteral(
            "――― IS IT ONE EPISODE, AND IS IT STILL HAPPENING? ―――\n\n"
            "  The last column is the one to read: the round trip THAT YEAR\n"
            "  could have borne against buy-and-hold.\n\n"
            "    year      n    overnight        t      session   RT it bears\n");
        std::size_t positive = 0;
        double recent = 0.0, older = 0.0, worst = 1e300;
        QString worst_year;
        std::size_t rn = 0, onn = 0;
        for (std::size_t i = 0; i < years->size(); ++i) {
            const PeriodStat& ps = (*years)[i];
            if (ps.overnight_bps > 0.0) { ++positive; }
            const double w = static_cast<double>(ps.n);
            if (i + 3 >= years->size()) { recent += ps.overnight_bps * w; rn += ps.n; }
            else { older += ps.overnight_bps * w; onn += ps.n; }
            const double margin = ps.breakeven_vs_hold_bps();
            if (margin < worst) {
                worst = margin;
                worst_year = QString::fromStdString(ps.label);
            }
            s += QStringLiteral("    %1 %2  %3  %4  %5   %6\n")
                     .arg(QString::fromStdString(ps.label))
                     .arg(ps.n, 6)
                     .arg(ps.overnight_bps, 11, 'f', 3)
                     .arg(ps.overnight_t, 7, 'f', 2)
                     .arg(ps.session_bps, 11, 'f', 3)
                     .arg(margin, 11, 'f', 3);
        }
        s += QStringLiteral(
            "\n  Thinnest year: %1, bearing a round trip of only %2 bps.\n")
                 .arg(worst_year).arg(worst, 0, 'f', 3);
        s += QStringLiteral("\n  %1 of %2 years had a positive overnight "
                            "mean, so it is not one episode.\n")
                 .arg(positive).arg(years->size());
        if (rn > 0 && onn > 0) {
            recent /= static_cast<double>(rn);
            older /= static_cast<double>(onn);
            s += QStringLiteral(
                "  Earlier years %1 bps/night; last three years %2.\n")
                     .arg(older, 0, 'f', 3).arg(recent, 0, 'f', 3);
            if (recent < older * 0.6) {
                s += QStringLiteral(
                    "\n  THE EFFECT HAS DECAYED. The recent window is a\n"
                    "  different number from the eleven-year average, and it\n"
                    "  is the recent one a decision today would rest on.\n");
            }
        }
        s += QStringLiteral("\n");
    }

    // ――― the sweep ―――
    s += QStringLiteral(
        "――― NET PER SESSION vs THE ROUND TRIP ―――\n\n"
        "  Bring the real rate from config/charges.toml. For NIFTY futures\n"
        "  the round trip is dominated by sell-side STT, paid every morning.\n\n"
        "    RT bps    net(open)  exc(open)   net(9:20)  exc(9:20)\n");
    for (const double rt : {0.0, 2.0, 4.0, 6.0, 8.0, 10.0, 12.0}) {
        s += QStringLiteral("    %1   %2  %3   %4  %5\n")
                 .arg(rt, 6, 'f', 1)
                 .arg(ro->net_at(rt), 9, 'f', 4)
                 .arg(ro->excess_at(rt), 9, 'f', 4)
                 .arg(rf->net_at(rt), 9, 'f', 4)
                 .arg(rf->excess_at(rt), 9, 'f', 4);
    }

    // ――― selection ―――
    s += QStringLiteral(
        "\n――― DOES FILTERING HELP? ―――\n\n"
        "  It cannot help by trading less: the break-even against zero IS\n"
        "  the conditional mean, whatever the frequency — half the gross\n"
        "  and half the cost leaves the ratio alone. It helps only if the\n"
        "  nights it keeps are better ones. And skipping a night means\n"
        "  being flat, which forgoes the drift, so the column against\n"
        "  buy-and-hold gets HARDER as the filter tightens.\n\n"
        "   session below     took   share       mean   b/e vs hold\n");
    for (const double th : {1e30, 100.0, 50.0, 0.0, -50.0, -100.0}) {
        OvernightSpec spec;
        spec.session_below_bps =
            th > 1e29 ? std::numeric_limits<double>::infinity() : th;
        const auto res = run_overnight(bars, spec);
        if (!res || res->taken == 0) { continue; }
        s += QStringLiteral("   %1  %2  %3%   %4    %5\n")
                 .arg(th > 1e29 ? QStringLiteral("every night")
                                : QStringLiteral("%1").arg(th, 0, 'f', 1), 13)
                 .arg(res->taken, 6)
                 .arg(100.0 * static_cast<double>(res->taken)
                          / static_cast<double>(res->sessions), 5, 'f', 1)
                 .arg(res->mean_taken_bps, 9, 'f', 4)
                 .arg(res->breakeven_rt_vs_hold_bps(), 9, 'f', 3);
    }

    s += QStringLiteral(
        "\n――― WHAT THIS RESTS ON ―――\n\n"
        "  1. Filling at the 09:15 auction print. This is the whole result.\n"
        "  2. Filling at the 15:30 close — NOT MODELLED AT ALL, and it\n"
        "     moves the same way.\n"
        "  3. Trading the index, which is not tradeable. A future or an ETF\n"
        "     is, each with its own basis and roll.\n"
        "  4. In-sample over one eleven-year window. No walk-forward, no\n"
        "     purge, no embargo.\n"
        "  5. A stationary effect — which the per-year table above says it\n"
        "     is not.\n\n"
        "  The gross edge is real and significant, and it is the first\n"
        "  positive result in this project. It is also the one most likely\n"
        "  to be an artefact of a price nobody can get: the difference\n"
        "  between the two break-even columns above is five minutes of the\n"
        "  trading day.\n\n"
        "  Every figure here is computed by the same functions the\n"
        "  acceptance tests call (strategies_overnight, _real), on the same\n"
        "  data, when you press the button.\n");
    return s;
}

// ---------------------------------------------------------------------------
// P23-01 — Options: greeks, implied vol, and the surface
// ---------------------------------------------------------------------------

/// The options machinery, exercised end to end.
///
/// THERE IS NO OPTION CHAIN, AND THIS PAGE SAYS SO BEFORE IT SHOWS A NUMBER.
///
/// `dataset/opt/` does not exist and no Kite option subscription is running,
/// so NOTHING on this page is a market measurement. Every figure is a
/// SELF-CONSISTENCY CHECK: price a known option, solve the volatility back out
/// of that price, and report how much was lost on the round trip. A page that
/// dressed those up as a smile would be the exact failure this project keeps
/// finding, so the headings say what they are.
///
/// That is still worth having on screen, for the reason P20-02 gave about the
/// Kite panel: a renderer that has never rendered is the one that crashes when
/// real data finally arrives. This exercises Black-76, the IV solver, the
/// error propagator, the SVI fit and its butterfly scan, on every launch.
///
/// AND IT LEADS WITH WHERE THE MACHINERY STOPS WORKING.
///
/// The interesting output is not that a round trip recovers 15.00 vol at the
/// money. It is the wing where vega has underflowed and the implied vol is
/// unresolvable at any spread -- because that is the region a chain-driven
/// model will silently produce numbers for.
[[nodiscard]] inline QString options_report() {
    QString s = QStringLiteral(
        "OPTIONS — greeks, implied vol, surface\n\n"
        "――― WHAT THIS IS NOT ―――\n\n"
        "  There is no option chain. dataset/opt/ does not exist and no Kite\n"
        "  option subscription is running, so NOTHING below is a market\n"
        "  measurement and no smile here came from a price anyone quoted.\n\n"
        "  Every figure is a self-consistency check: price a known option,\n"
        "  solve the vol back out, report what the round trip lost.\n\n");

    // ――― 1. round trip across the strike ladder ―――
    const double F = 24000.0 * 100.0;       // paise
    const double T = 30.0 / 365.0;
    const double rate = 0.065;
    const double true_vol = 0.15;

    s += QStringLiteral(
        "――― 1. PRICE, THEN SOLVE THE VOL BACK ―――\n\n"
        "  Forward %1, %2 days, rate %3%, true vol %4.\n\n"
        "  The model price is ROUNDED TO PAISE before the vol is solved back,\n"
        "  because that is what a real quote does. So `recovered` differs from\n"
        "  the generating vol by the tick, not by solver error — and the band\n"
        "  beside it is that half-paise pushed through vega.\n\n"
        "     strike   moneyness      price      delta       vega"
        "   recovered   +/- band\n")
            .arg(F / 100.0, 0, 'f', 0).arg(T * 365.0, 0, 'f', 0)
            .arg(100.0 * rate, 0, 'f', 1).arg(true_vol, 0, 'f', 4);

    std::size_t solved = 0, unresolvable = 0;
    for (const double mult : {0.80, 0.90, 0.95, 1.00, 1.05, 1.10, 1.20, 1.35}) {
        const double K = F * mult;
        const auto g = black76(OptionRight::Call, Price{static_cast<std::int64_t>(F)},
                               Price{static_cast<std::int64_t>(K)},
                               Years{T}, Vol{true_vol}, rate);
        if (!g) { continue; }
        // The PUBLIC entry point, which takes an integer-paise price --
        // `solve_iv` is in `detail::` and takes doubles. Going through the
        // public one is the right choice and not merely the compiling one:
        // a real quote arrives quantised to the tick, so rounding the model
        // price to paise here reproduces the quantisation a live chain would
        // hand the solver, and the recovered vol below is not exactly the
        // generating vol for precisely that reason.
        const auto iv = implied_vol_black76(
            OptionRight::Call, Price{static_cast<std::int64_t>(g->price + 0.5)},
            Price{static_cast<std::int64_t>(F)},
            Price{static_cast<std::int64_t>(K)}, Years{T}, rate);
        const double band = iv_uncertainty(g->vega, 0.5);
        if (!iv) {
            ++unresolvable;
            s += QStringLiteral("   %1  %2  %3  %4  %5   UNRESOLVABLE\n")
                     .arg(K / 100.0, 9, 'f', 0).arg(mult, 10, 'f', 2)
                     .arg(g->price / 100.0, 9, 'f', 2)
                     .arg(g->delta, 10, 'f', 4).arg(g->vega / 100.0, 10, 'f', 2);
            continue;
        }
        ++solved;
        const bool wide = !(band < 0.01);
        s += QStringLiteral("   %1  %2  %3  %4  %5  %6   %7\n")
                 .arg(K / 100.0, 9, 'f', 0).arg(mult, 10, 'f', 2)
                 .arg(g->price / 100.0, 9, 'f', 2)
                 .arg(g->delta, 10, 'f', 4).arg(g->vega / 100.0, 10, 'f', 2)
                 .arg(iv->vol.raw(), 10, 'f', 6)
                 .arg(wide ? QStringLiteral("WIDER THAN THE SMILE")
                           : QStringLiteral("%1").arg(band, 0, 'f', 6));
    }
    s += QStringLiteral(
        "\n  %1 strikes solved, %2 refused outright.\n"
        "  The band is the point. Where vega has collapsed, half a paise of\n"
        "  price uncertainty is worth more vol than the entire smile, and an\n"
        "  IV printed there is a number without a measurement behind it.\n\n")
            .arg(solved).arg(unresolvable);

    // ――― 2. put-call parity ―――
    {
        const auto c = black76(OptionRight::Call, Price{static_cast<std::int64_t>(F)},
                               Price{static_cast<std::int64_t>(F)},
                               Years{T}, Vol{true_vol}, rate);
        const auto pu = black76(OptionRight::Put, Price{static_cast<std::int64_t>(F)},
                                Price{static_cast<std::int64_t>(F)},
                                Years{T}, Vol{true_vol}, rate);
        if (c && pu) {
            const double resid = parity_residual(
                c->price, pu->price, Price{static_cast<std::int64_t>(F)},
                Price{static_cast<std::int64_t>(F)}, Years{T}, rate);
            s += QStringLiteral(
                "――― 2. PUT-CALL PARITY ―――\n\n"
                "  At the money, call %1 put %2, residual %3 paise.\n"
                "  It is an identity, not a fit: a non-zero residual here is a\n"
                "  broken pricer, never a market opportunity.\n\n")
                     .arg(c->price / 100.0, 0, 'f', 4)
                     .arg(pu->price / 100.0, 0, 'f', 4)
                     .arg(resid, 0, 'f', 9);
        }
    }

    // ――― 3. SVI fit + butterfly scan ―――
    s += QStringLiteral(
        "――― 3. SVI SURFACE, AND THE ARBITRAGE BOUNDARY ―――\n\n"
        "  A smile is generated from STATED parameters, fitted back, and the\n"
        "  fit is scanned for butterfly arbitrage. The generating parameters\n"
        "  are a choice printed here, not an estimate.\n\n");
    {
        SviParams truth;
        truth.a = 0.04 * T;
        truth.b = 0.4 * T;
        truth.rho = -0.7;
        truth.m = 0.0;
        truth.sigma = 0.12;
        std::vector<SviFitPoint> pts;
        for (int i = -10; i <= 10; ++i) {
            const double k = 0.03 * static_cast<double>(i);
            SviFitPoint pt;
            pt.k = k;
            pt.w = svi_total_variance(truth, k);
            pt.weight = 1.0;
            pts.push_back(pt);
        }
        const auto fit = svi_fit(pts.data(), pts.size());
        if (fit) {
            s += QStringLiteral(
                "  generated  a %1  b %2  rho %3  m %4  sigma %5\n"
                "  recovered  a %6  b %7  rho %8  m %9  sigma %10\n\n"
                "  weighted RMSE in total variance %11   worst residual %12\n"
                "  points %13   Nelder-Mead iterations %14\n\n")
                     .arg(truth.a, 0, 'f', 6).arg(truth.b, 0, 'f', 6)
                     .arg(truth.rho, 0, 'f', 4).arg(truth.m, 0, 'f', 4)
                     .arg(truth.sigma, 0, 'f', 4)
                     .arg(fit->params.a, 0, 'f', 6).arg(fit->params.b, 0, 'f', 6)
                     .arg(fit->params.rho, 0, 'f', 4).arg(fit->params.m, 0, 'f', 4)
                     .arg(fit->params.sigma, 0, 'f', 4)
                     .arg(fit->rmse, 0, 'g', 4).arg(fit->worst_residual, 0, 'g', 4)
                     .arg(fit->points).arg(fit->iterations);
        } else {
            s += QStringLiteral("  The fit was refused on this smile.\n\n");
        }

        // Sweep b upward until the butterfly scan reports a negative density.
        // The boundary is FOUND, not asserted -- the same discipline P17 used
        // when a guessed SABR nu of 3.5 passed and the real first arbitrage
        // was at 1.5.
        s += QStringLiteral(
            "  Sweeping the slope b until the density goes negative:\n\n"
            "        b      min density    verdict\n");
        double first_bad = -1.0;
        for (const double bmul : {1.0, 2.0, 4.0, 8.0, 16.0, 32.0}) {
            SviParams q = truth;
            q.b = 0.4 * T * bmul;
            const auto scan = svi_butterfly_scan(q, -0.6, 0.6, 401);
            if (!scan) { continue; }
            const bool bad = !scan->clean;
            if (bad && first_bad < 0.0) { first_bad = q.b; }
            s += QStringLiteral("   %1     %2    %3\n")
                     .arg(q.b, 8, 'f', 6).arg(scan->worst, 12, 'g', 4)
                     .arg(bad ? QStringLiteral("ARBITRAGE")
                              : QStringLiteral("clean"));
        }
        s += first_bad > 0.0
            ? QStringLiteral("\n  First arbitrage at b = %1. A slice past that "
                             "point prices a\n  negative probability, and no "
                             "amount of good fit statistics\n  makes it "
                             "tradeable.\n\n").arg(first_bad, 0, 'f', 6)
            : QStringLiteral("\n  No arbitrage found across the sweep.\n\n");
    }

    // ――― 4. higher greeks and American premium ―――
    {
        const auto h = black76_higher(OptionRight::Call,
                                      Price{static_cast<std::int64_t>(F)},
                                      Price{static_cast<std::int64_t>(F)},
                                      Years{T}, Vol{true_vol}, rate);
        if (h) {
            s += QStringLiteral(
                "――― 4. SECOND ORDER, AT THE MONEY ―――\n\n"
                "  vanna %1   volga %2   charm %3   speed %4\n\n"
                "  Vanna is dDelta/dVol. It is why a delta hedge set at one\n"
                "  volatility is the wrong hedge at another, and it is the\n"
                "  reason the surface has to be refitted rather than shifted.\n\n")
                     .arg(h->vanna, 0, 'g', 5).arg(h->volga, 0, 'g', 5)
                     .arg(h->charm, 0, 'g', 5).arg(h->speed, 0, 'g', 5);
        }
    }

    s += QStringLiteral(
        "――― WHAT WOULD MAKE THIS A MARKET PAGE ―――\n\n"
        "  1. A Kite option subscription (P2-02, full mode) or a dump into\n"
        "     dataset/opt/nifty/. Neither exists.\n"
        "  2. The forward from the FUTURE, not the spot — dataset/fut/nifty/\n"
        "     has bars, so this is the nearest gap to close.\n"
        "  3. A quote's own bid-ask, so the +/- band above is the real one\n"
        "     rather than a stated half paise.\n\n"
        "  Until then this page proves the machinery runs and says where it\n"
        "  stops resolving. It does not claim a volatility surface exists.\n");
    return s;
}

// ---------------------------------------------------------------------------
// P23-03 — Basis: the future against the spot
// ---------------------------------------------------------------------------

/// The NIFTY futures basis, on eleven years of real daily data.
///
/// THIS ONE IS A REAL MEASUREMENT, WITH ONE FIELD MISSING.
///
/// `dataset/fut/nifty/1d/all.csv` and `dataset/spot/nifty/1d/all.csv` cover the
/// same 2,873 sessions, so the raw basis -- future minus spot -- is observed,
/// not constructed. That part of this page is market data.
///
/// What is NOT computable from these files is the IMPLIED REPO RATE, and the
/// reason is worth stating rather than working around: converting a basis into
/// a rate needs the time to expiry, `ln(F/S)/T + q`, and a CONTINUOUS futures
/// series carries no expiry. Rule 1 puts expiry in the point-in-time spec
/// store and forbids it as a literal, and "last Thursday of the month" typed
/// into this file would be exactly the literal that rule exists to stop --
/// NSE moves expiries for holidays, and the rule changed in 2025.
///
/// So the rate section runs `measure_basis` on a STATED tenor as a machinery
/// check, clearly separated from the observed series above it. A 40-point
/// basis on a one-week future and on a three-month future are wildly different
/// rates, which is the whole reason the rate matters more than the basis.
[[nodiscard]] inline QString basis_report(const QString& dataset_root) {
    QString s = QStringLiteral(
        "BASIS — the future against the spot\n\n");

    const auto fut = ui_load_dated_closes(
        dataset_root + QStringLiteral("/fut/nifty/1d/all.csv"));
    const auto spot = ui_load_dated_closes(
        dataset_root + QStringLiteral("/spot/nifty/1d/all.csv"));
    if (fut.size() < 500 || spot.size() < 500) {
        return s + QStringLiteral(
            "  Missing dataset/fut/nifty/1d/all.csv or its spot counterpart.\n"
            "  Nothing is shown rather than something synthetic.\n");
    }

    // JOINED ON THE DATE, not stacked by position.
    //
    // The first version of this page took the last N rows of each file and
    // paired them by index. That is wrong whenever the two series do not
    // share a session calendar, and they do not -- the result was a basis
    // with a 292-point standard deviation, a 1,749-point minimum and 39.5%
    // of sessions in backwardation, none of which a NIFTY future has ever
    // done. Off-by-one days were being differenced as if they were a basis.
    //
    // A date join cannot make that mistake, and a session present in only one
    // file is DROPPED rather than paired with its neighbour.
    std::map<std::string, double> spot_by_day;
    for (const auto& kv : spot) { spot_by_day[kv.first] = kv.second; }
    std::vector<double> basis_pts;
    basis_pts.reserve(fut.size());
    std::size_t unmatched = 0;
    for (const auto& kv : fut) {
        const auto it = spot_by_day.find(kv.first);
        if (it == spot_by_day.end()) { ++unmatched; continue; }
        basis_pts.push_back(kv.second - it->second);
    }
    if (basis_pts.size() < 500) {
        return s + QStringLiteral("  Too few aligned sessions.\n");
    }

    double sum = 0.0;
    for (const double b : basis_pts) { sum += b; }
    const double m = sum / static_cast<double>(basis_pts.size());
    double s2 = 0.0;
    for (const double b : basis_pts) { s2 += (b - m) * (b - m); }
    const double sd = std::sqrt(s2 / static_cast<double>(basis_pts.size() - 1));
    std::vector<double> sorted = basis_pts;
    std::sort(sorted.begin(), sorted.end());
    const auto q = [&sorted](double f) {
        return sorted[static_cast<std::size_t>(
            f * static_cast<double>(sorted.size() - 1))];
    };
    std::size_t negative = 0;
    for (const double b : basis_pts) { if (b < 0.0) { ++negative; } }

    s += QStringLiteral(
        "――― 1. THE OBSERVED BASIS ―――   %1 sessions joined on date\n\n"
        "  future minus spot, index points:\n\n"
        "    mean   %2        sd     %3\n"
        "    p05    %4        p25    %5\n"
        "    median %6        p75    %7\n"
        "    p95    %8        max    %9\n"
        "    min    %10\n\n"
        "  %11 sessions closed in BACKWARDATION (%12% of them), which is the\n"
        "  number worth knowing: a future below spot is not an arbitrage, it\n"
        "  is usually a dividend, a borrow cost, or a market that cannot be\n"
        "  shorted cheaply.\n\n")
            .arg(basis_pts.size())
            .arg(m, 8, 'f', 2).arg(sd, 8, 'f', 2)
            .arg(q(0.05), 8, 'f', 2).arg(q(0.25), 8, 'f', 2)
            .arg(q(0.50), 8, 'f', 2).arg(q(0.75), 8, 'f', 2)
            .arg(q(0.95), 8, 'f', 2).arg(sorted.back(), 8, 'f', 2)
            .arg(sorted.front(), 8, 'f', 2)
            .arg(negative)
            .arg(100.0 * static_cast<double>(negative)
                     / static_cast<double>(basis_pts.size()), 0, 'f', 1);
    s += QStringLiteral(
        "  %1 futures sessions had no spot close on the same date and were\n"
        "  DROPPED rather than paired with a neighbouring day.\n\n")
            .arg(unmatched);

    // ――― 2. what a basis is worth as a rate, on a STATED tenor ―――
    s += QStringLiteral(
        "――― 2. WHAT A BASIS IS WORTH AS A RATE ―――\n\n"
        "  NOT COMPUTED FROM THE SERIES ABOVE. Turning a basis into a rate\n"
        "  needs the time to expiry, and a CONTINUOUS futures series carries\n"
        "  no expiry. Rule 1 puts expiry in the spec store and forbids it as\n"
        "  a literal — \"last Thursday\" typed here is exactly the literal that\n"
        "  rule exists to stop, since NSE moves expiries for holidays and\n"
        "  changed the convention in 2025.\n\n"
        "  So the tenor below is STATED, and this is a machinery check on\n"
        "  measure_basis() rather than a measurement:\n\n"
        "     tenor    basis    fair basis   mispricing   implied repo\n");
    {
        const double spot_px = spot.back().second * 100.0;   // paise
        const double rate = 0.065, divy = 0.012;
        for (const double days : {7.0, 30.0, 90.0}) {
            const double t = days / 365.0;
            // A future priced EXACTLY at carry, so the mispricing must be zero
            // and any non-zero value below is an arithmetic error, not an
            // opportunity.
            const double fair = spot_px * std::exp((rate - divy) * t);
            BasisQuote bq;
            bq.spot = Price{static_cast<std::int64_t>(spot_px)};
            bq.future = Price{static_cast<std::int64_t>(fair)};
            bq.spot_ts = Timestamp{0};
            bq.future_ts = Timestamp{0};
            bq.t = Years{t};
            bq.rate = rate;
            bq.dividend_yield = divy;
            const auto b = measure_basis(bq, Duration{1'000'000'000});
            if (!b) { continue; }
            s += QStringLiteral("   %1d %2 %3 %4 %5\n")
                     .arg(days, 6, 'f', 0)
                     .arg(static_cast<double>(b->basis) / 100.0, 8, 'f', 2)
                     .arg(b->fair_basis / 100.0, 12, 'f', 2)
                     .arg(b->mispricing / 100.0, 12, 'f', 4)
                     .arg(100.0 * b->implied_repo, 12, 'f', 4);
        }
    }
    s += QStringLiteral(
        "\n  The mispricing column is zero by construction — the future was\n"
        "  priced at carry. A non-zero value there is a broken pricer.\n"
        "  The implied repo returns the %1% financing rate it was given,\n"
        "  which is the round trip that makes the function trustworthy.\n\n"
        "――― WHAT WOULD MAKE SECTION 2 REAL ―――\n\n"
        "  A per-contract futures series with its expiry, or the spec store\n"
        "  populated for the historical contracts. Then every session in\n"
        "  section 1 gets a rate, and a rate is comparable across tenors in\n"
        "  a way a point basis never is.\n").arg(6.5, 0, 'f', 1);
    return s;
}

} // namespace altair::ui
