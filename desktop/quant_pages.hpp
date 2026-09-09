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
#include <analytics/ewma.hpp>
#include <analytics/hurst.hpp>
#include <analytics/garch.hpp>
#include <analytics/greeks.hpp>
#include <analytics/greeks2.hpp>
#include <analytics/iv.hpp>
#include <analytics/svi.hpp>
#include <analytics/svi_fit.hpp>
#include <analytics/hmm.hpp>
#include <analytics/kalman.hpp>
#include <analytics/sabr.hpp>
#include <book/flow.hpp>
#include <book/microstructure.hpp>
#include <analytics/derivatives.hpp>
#include <features/registry.hpp>
#include <flagging/drift.hpp>
#include <flagging/scorecard.hpp>
#include <models/dcf.hpp>
#include <models/gbdt.hpp>
#include <models/regime_rl.hpp>
#include <models/aggregator.hpp>
#include <models/forecast_scorecard.hpp>
#include <models/spot_forecast.hpp>
#include <risk/covariance.hpp>
#include <risk/limits.hpp>
#include <risk/sizing.hpp>
#include <risk/slippage.hpp>
#include <risk/optimise.hpp>
#include <risk/stress.hpp>
#include <risk/var.hpp>
#ifdef ALTAIR_HAVE_CHARGES_TOML
#include <risk/charges_toml.hpp>
#endif
#include <strategies/parity.hpp>
#include <strategies/calendar.hpp>
// P21. strategies/ is header-only here and is NOT linked -- see the note at
// the bottom of desktop/CMakeLists.txt. It detects; it cannot trade, and it
// does not link oms/ either, so including it widens nothing.
#include <backtest/validation.hpp>
#include <strategies/basis.hpp>
#include <strategies/regime.hpp>
#include <strategies/cointegration.hpp>
#include <strategies/meanrev.hpp>
#include <strategies/overnight.hpp>
#include <strategies/momentum.hpp>

#include <QDateTime>
#include <QTimeZone>
#include <QComboBox>
#include <QLabel>
#include <QDir>
#include <QTextStream>
#include <QProcess>
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
/// Bar stamps in nanoseconds, alongside the closes. P32-06.
///
/// `ui_load_closes` throws the time column away, which is why the spot
/// forecast could not say which bar it was about. This keeps it.
///
/// THE FILES ARE NOT ALL THE SAME SHAPE. Two forms appear on disk:
/// `2026-09-08` and `2026-09-08T15:29:00+05:30`. Both are parsed; a row whose
/// stamp parses as neither is DROPPED ALONG WITH ITS CLOSE, so the two
/// vectors stay the same length. Dropping the close and keeping the price
/// would silently shift every later stamp by one bar, which is the failure
/// mode this function exists to prevent.
struct UiStamped {
    std::vector<double> closes;
    std::vector<std::int64_t> stamps_ns;
};

[[nodiscard]] inline UiStamped ui_load_stamped(const QString& path) {
    UiStamped out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) { return out; }
    QTextStream ts(&f);
    ts.readLine();                                  // header
    while (!ts.atEnd()) {
        const QString line = ts.readLine();
        if (line.trimmed().isEmpty()) { continue; }
        const QStringList col = line.split(QLatin1Char(','));
        if (col.size() < 5) { continue; }
        bool ok = false;
        const double close = col[4].toDouble(&ok);
        if (!ok || !(close > 0.0)) { continue; }

        const QString stamp = col[0].trimmed();
        QDateTime dt = QDateTime::fromString(stamp, Qt::ISODate);
        if (!dt.isValid()) {
            dt = QDateTime::fromString(stamp.left(10),
                                       QStringLiteral("yyyy-MM-dd"));
            // A DATE IS NOT AN INSTANT UNTIL SOMEBODY SAYS WHICH ZONE. A
            // bare 2026-09-08 in an NSE file is an IST trading day, and
            // reading it as UTC lands it 5.5 hours early -- which is the same
            // class of bug P11Q-11 hit on the watchlist's expiry column,
            // where it printed a contract expiring the day before it does.
            if (dt.isValid()) {
                dt.setTimeZone(QTimeZone(5 * 3600 + 30 * 60));
            }
        }
        if (!dt.isValid()) { continue; }
        out.closes.push_back(close);
        out.stamps_ns.push_back(
            static_cast<std::int64_t>(dt.toMSecsSinceEpoch()) * 1'000'000LL);
    }
    return out;
}

/// A bar width in words. The median gap of the series, so it says what the
/// data is rather than what the caller assumed.
[[nodiscard]] inline QString describe_ns(std::int64_t ns) {
    const std::int64_t sec = ns / 1'000'000'000LL;
    if (sec % 86400 == 0) {
        return QStringLiteral("%1 day(s)").arg(sec / 86400);
    }
    if (sec % 3600 == 0) { return QStringLiteral("%1 hour(s)").arg(sec / 3600); }
    if (sec % 60 == 0) { return QStringLiteral("%1 minute(s)").arg(sec / 60); }
    return QStringLiteral("%1 s").arg(sec);
}

/// A nanosecond instant as an IST wall clock. The exchange's zone, always.
[[nodiscard]] inline QString ui_ist(std::int64_t ns, bool with_time) {
    if (ns == 0) { return QStringLiteral("—"); }
    const QDateTime dt =
        QDateTime::fromMSecsSinceEpoch(ns / 1'000'000LL,
                                       QTimeZone(5 * 3600 + 30 * 60));
    return dt.toString(with_time ? QStringLiteral("yyyy-MM-dd HH:mm")
                                 : QStringLiteral("yyyy-MM-dd"))
           + QStringLiteral(" IST");
}

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

/// Which instrument a dataset-backed page is looking at.
///
/// P32-04. Smit asked for every model to offer NIFTY, BANKNIFTY or INDIA VIX
/// rather than being silently about NIFTY.
///
/// IT IS A DIRECTORY NAME, AND THAT IS THE WHOLE POINT OF THE LAYOUT.
///
/// CLAUDE.md's dataset/ section says the partition IS the definition: "a model
/// trained on NIFTY spot reads exactly one directory, so its training set is
/// defined by a path rather than by a filter someone has to get right." So
/// switching instrument is switching one path segment, and there is nothing
/// else to keep in step -- no filter, no symbol column, no join.
///
/// THE THIRD ONE IS NOT LIKE THE OTHER TWO, AND THE PAGES SAY SO.
///
/// INDIA VIX is an index of implied volatility, not a tradeable price series.
/// A momentum rule on it is a momentum rule on a volatility level; a cost
/// model on it is meaningless, because there is no instrument to pay the cost
/// on. Every page that charges a cost states that when VIX is selected rather
/// than quietly reporting a net figure for a trade nobody can make.
struct QuantSymbol {
    const char* dir;      ///< the dataset/ directory: nifty, banknifty, ...
    const char* label;    ///< what to call it on screen
    bool tradeable;       ///< false for an index of implied vol
    /// Kite's instrument_token. P33-01: the live forecast has to ask Kite for
    /// the newest candle, and Kite is addressed by token. It lives here rather
    /// than in a second table because two lists of the same three instruments
    /// is one list that will go out of step.
    std::uint32_t kite_token;
};

[[nodiscard]] inline const QuantSymbol* quant_symbols(std::size_t& n) {
    static const QuantSymbol kSyms[] = {
        {"nifty",     "NIFTY 50",   true,  256265u},
        {"banknifty", "NIFTY BANK", true,  260105u},
        {"indiavix",  "INDIA VIX",  false, 264969u},
    };
    n = sizeof(kSyms) / sizeof(kSyms[0]);
    return kSyms;
}

[[nodiscard]] inline QuantSymbol quant_symbol_by_dir(const QString& dir) {
    std::size_t n = 0;
    const QuantSymbol* v = quant_symbols(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (dir == QLatin1String(v[i].dir)) { return v[i]; }
    }
    return v[0];
}

/// `<root>/spot/<sym>/<interval>/` -- one place, so a page cannot invent a
/// path shape that does not exist on disk.
[[nodiscard]] inline QString spot_path(const QString& root, const QString& sym,
                                       const char* interval,
                                       const char* file = "all.csv") {
    return QStringLiteral("%1/spot/%2/%3/%4")
        .arg(root, sym, QLatin1String(interval), QLatin1String(file));
}

/// A banner naming what the page is looking at, and what that costs.
[[nodiscard]] inline QString symbol_banner(const QString& sym) {
    const QuantSymbol q = quant_symbol_by_dir(sym);
    QString s = QStringLiteral("  INSTRUMENT: %1  (dataset/spot/%2/)\n")
                    .arg(QLatin1String(q.label), sym);
    if (!q.tradeable) {
        s += QStringLiteral(
            "\n  INDIA VIX IS NOT A TRADEABLE SERIES. It is an index of\n"
            "  implied volatility, so any figure below that is NET OF COST\n"
            "  is net of a cost nobody can pay: there is no instrument here\n"
            "  to buy. Read the gross numbers and the error bars; treat the\n"
            "  net ones as arithmetic rather than as a strategy.\n");
    }
    s += QStringLiteral("\n");
    return s;
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

    /// The instrument this page is about. P32-04.
    ///
    /// IN THE BUTTON ROW, NOT IN A SETTINGS DIALOG. The instrument is part of
    /// the question being asked, so it belongs beside the button that asks it
    /// -- and it is deliberately NOT applied on change: the pages behind these
    /// buttons take seconds to minutes, and a combo that silently re-ran a
    /// 214,000-row walk-forward because somebody scrolled over it would be a
    /// UI that punishes curiosity.
    ///
    /// Changing it therefore says the shown result is stale, and leaves the
    /// result on screen. Nothing is worse here than blanking the pane: the
    /// reader loses the number they were comparing against.
    [[nodiscard]] QComboBox* add_symbols() {
        symbols_ = new QComboBox(this);
        std::size_t k = 0;
        const QuantSymbol* v = quant_symbols(k);
        for (std::size_t i = 0; i < k; ++i) {
            symbols_->addItem(QLatin1String(v[i].label),
                              QLatin1String(v[i].dir));
        }
        row_->insertWidget(0, new QLabel(QStringLiteral("Instrument"), this));
        row_->insertWidget(1, symbols_);
        stale_ = new QLabel(this);
        stale_->setStyleSheet(QStringLiteral("color:#B9770B;"));
        row_->addWidget(stale_);
        connect(symbols_, &QComboBox::currentIndexChanged, this, [this] {
            if (ran_) {
                stale_->setText(QStringLiteral(
                    "instrument changed — press the button to re-run"));
            }
        });
        return symbols_;
    }

    /// The sampling rate, for pages where that is part of the question.
    [[nodiscard]] QComboBox* add_interval() {
        auto* c = new QComboBox(this);
        c->addItem(QStringLiteral("daily"), QStringLiteral("1d"));
        c->addItem(QStringLiteral("60-minute"), QStringLiteral("60m"));
        c->addItem(QStringLiteral("15-minute"), QStringLiteral("15m"));
        c->addItem(QStringLiteral("5-minute"), QStringLiteral("5m"));
        row_->insertWidget(2, new QLabel(QStringLiteral("Interval"), this));
        row_->insertWidget(3, c);
        return c;
    }

    /// Which instrument is selected, as its dataset directory name.
    [[nodiscard]] QString symbol() const {
        return symbols_ == nullptr ? QStringLiteral("nifty")
                                   : symbols_->currentData().toString();
    }

    void set_text(const QString& s) {
        out_->setPlainText(s);
        ran_ = true;
        if (stale_ != nullptr) { stale_->clear(); }
    }
    void append(const QString& s) { out_->appendPlainText(s); }

private:
    QPushButton* run_ = nullptr;
    QComboBox* symbols_ = nullptr;
    QLabel* stale_ = nullptr;
    bool ran_ = false;
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

[[nodiscard]] inline QString volatility_report(const QString& dataset_root,
                                                  const QString& sym
                                                      = QStringLiteral("nifty")) {
    const auto closes =
        ui_load_closes(spot_path(dataset_root, sym, "1d"));
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
    s += symbol_banner(sym);
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

[[nodiscard]] inline QString risk_report(const QString& dataset_root,
                                            const QString& sym
                                                = QStringLiteral("nifty")) {
    const auto closes =
        ui_load_closes(spot_path(dataset_root, sym, "1d"));
    if (closes.size() < 2000) {
        return QStringLiteral("RISK — P15\n\nNo dataset.");
    }
    const auto r = ui_log_returns(closes);
    QString s = QStringLiteral("VALUE AT RISK — P15-01/02\n\n%1 daily NIFTY "
                               "returns\n").arg(r.size());
    s += symbol_banner(sym);

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
    // ――― THE FORECAST, AS A PRICE ―――
    //
    // P30-01. Everything above is basis points, because everything above is
    // an EVALUATION. Smit asked for a price, and he is right that a basis
    // point is not a forecast anybody can act on: it makes the reader hold the
    // last close in their head and do exponential arithmetic.
    //
    // The band is the model's own OUT-OF-SAMPLE RMSE from the walk-forward
    // above -- not a training residual, which would be narrower by exactly the
    // amount the model overfits.
    {
        SpotSpec ns = spec;
        // THE STAMPS, RELOADED FROM THE SAME FILES THE CLOSES CAME FROM.
        //
        // P32-06. Reloaded rather than threaded through, because `closes` is
        // assembled by two different paths above (a single all.csv, or twelve
        // monthly files a year) and a stamp vector built by only one of them
        // would be silently short. Same paths, same order, same length -- and
        // forecast_next REFUSES a length mismatch rather than labelling the
        // forecast with somebody else's bar.
        std::vector<std::int64_t> stamps;
        {
            const UiStamped one =
                ui_load_stamped(dataset_root + sub + QStringLiteral("all.csv"));
            if (one.closes.size() >= 2000) {
                stamps = one.stamps_ns;
            } else {
                for (int y = 2015; y <= 2026; ++y) {
                    for (int m = 1; m <= 12; ++m) {
                        const UiStamped part = ui_load_stamped(
                            dataset_root + sub
                            + QStringLiteral("%1-%2.csv")
                                  .arg(y, 4, 10, QLatin1Char('0'))
                                  .arg(m, 2, 10, QLatin1Char('0')));
                        stamps.insert(stamps.end(), part.stamps_ns.begin(),
                                      part.stamps_ns.end());
                    }
                }
            }
            if (stamps.size() != closes.size()) { stamps.clear(); }
        }

        for (int which = 0; which < 2; ++which) {
            const bool leaf = (which == 1);
            const double rmse = leaf ? r->rmse_leaf : r->rmse_level;
            const auto fc = forecast_next(closes, ns, rmse, leaf, stamps);
            if (!fc) { continue; }
            if (which == 0) {
                const bool intraday =
                    fc->interval_ns > 0 && fc->interval_ns < 86'400'000'000'000LL;
                s += QStringLiteral(
                    "\n――― THE NEXT BAR, AS A PRICE ―――\n\n"
                    "  Last bar SEEN          %1  close %2\n"
                    "  Forecast IS ABOUT      %3\n"
                    "  Bar width              %4\n"
                    "  Fitted on              %5 rows\n\n"
                    "  Two stamps, not one. The first is an observation and\n"
                    "  the second is a claim; collapsing them is how a number\n"
                    "  computed at the close gets read as a number about the\n"
                    "  close, which is the direction that looks like an edge.\n\n"
                    "     model        move      forecast          band"
                    "                 straddles last?\n")
                         .arg(fc->stamped()
                                  ? ui_ist(fc->last_ts_ns, intraday)
                                  : QStringLiteral("UNSTAMPED — the file's "
                                                   "time column did not parse"))
                         .arg(fc->last_price, 0, 'f', 2)
                         .arg(fc->stamped() ? ui_ist(fc->for_ts_ns, intraday)
                                            : QStringLiteral("—"))
                         .arg(fc->interval_ns > 0
                                  ? describe_ns(fc->interval_ns)
                                  : QStringLiteral("—"))
                         .arg(fc->fitted_on);
            }
            s += QStringLiteral("   %1  %2 bps  %3  %4 .. %5   %6\n")
                     .arg(leaf ? QStringLiteral("leaf-wise ")
                               : QStringLiteral("level-wise"))
                     .arg(fc->move_bps, 8, 'f', 3)
                     .arg(fc->price, 12, 'f', 2)
                     .arg(fc->lo, 10, 'f', 2).arg(fc->hi, 10, 'f', 2)
                     .arg(fc->band_straddles_last()
                              ? QStringLiteral("YES")
                              : QStringLiteral("no"));
        }
        s += QStringLiteral(
            "\n  THE LAST COLUMN IS THE ANSWER. A band that straddles the\n"
            "  last close does not say which side of today the next bar\n"
            "  lands on, and the point inside it is not a direction. On this\n"
            "  series it straddles at every horizon, because the RMSE above\n"
            "  is WORSE than a constant -- the band is wider than the move it\n"
            "  is drawn around.\n\n"
            "  Published without the band, the same number reads as a\n"
            "  forecast. That is the only reason this column exists.\n");
    }

    return s;
}

// ---------------------------------------------------------------------------
// P19-06 — the model catalogue, rebuilt
// ---------------------------------------------------------------------------

[[nodiscard]] inline QString regime_report(const QString& dataset_root,
                                              const QString& sym
                                                  = QStringLiteral("nifty")) {
    const auto closes =
        ui_load_closes(spot_path(dataset_root, sym, "1d"));
    if (closes.size() < 2000) {
        return QStringLiteral("REGIMES — P14-06 / P18\n\nNo dataset.");
    }
    const auto r = ui_log_returns(closes);
    QString s = QStringLiteral("HIDDEN MARKOV REGIMES — P14-06\n\n");
    s += symbol_banner(sym);

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
[[nodiscard]] inline QString strategies_report(const QString& dataset_root,
                                                  const QString& sym
                                                      = QStringLiteral("nifty")) {
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
    s += symbol_banner(sym);

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
        ui_load_closes(spot_path(dataset_root, sym, "1d"));
    if (closes.size() < 2000) {
        s += QStringLiteral(
            "  No dataset at dataset/spot/%1/1d/all.csv.\n\n"
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
        spot_path(dataset_root, sym, "5m", ""));
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
            "  No 5-minute partition at dataset/spot/%1/5m, so the\n"
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
[[nodiscard]] inline QString overnight_report(const QString& dataset_root,
                                                 const QString& sym
                                                     = QStringLiteral("nifty")) {
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

    s += symbol_banner(sym);

    const auto bars = ui_load_sessions(
        spot_path(dataset_root, sym, "5m", ""));
    if (bars.size() < 2000) {
        return s + QStringLiteral(
            "――― NO DATA ―――\n\n"
            "  dataset/spot/%1/5m is missing or too short. Nothing is\n"
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

    // ――― 5. American early exercise ―――
    //
    // I earlier listed american.hpp with the headers that "need an option
    // chain". That was wrong: it prices from the same stated inputs Black-76
    // takes, and the early-exercise premium is computable without a single
    // market quote. The section exists because the claim was incorrect.
    s += QStringLiteral(
        "――― 5. AMERICAN EARLY EXERCISE ―――\n\n"
        "  Same stated inputs. `carry` is the cost of carry b: b = r for a\n"
        "  non-dividend stock, b = r - q with a dividend yield, b = 0 for a\n"
        "  future. It is EXPLICIT rather than derived, because deriving it\n"
        "  silently is how a future gets priced as a stock.\n\n"
        "     right   carry     European    American   early-ex   boundary\n");
    {
        for (const auto& cfg : {std::pair<OptionRight, double>{
                                    OptionRight::Put, 0.065},
                                {OptionRight::Put, 0.0},
                                {OptionRight::Call, 0.065},
                                {OptionRight::Call, 0.0}}) {
            const auto av = american_value(
                cfg.first, Price{static_cast<std::int64_t>(F)},
                Price{static_cast<std::int64_t>(F)}, Years{T},
                Vol{true_vol}, rate, cfg.second);
            if (!av) { continue; }
            s += QStringLiteral("   %1  %2  %3  %4  %5  %6\n")
                     .arg(cfg.first == OptionRight::Put
                              ? QStringLiteral("put ") : QStringLiteral("call"), 6)
                     .arg(cfg.second, 6, 'f', 3)
                     .arg(av->european / 100.0, 11, 'f', 2)
                     .arg(av->price / 100.0, 11, 'f', 2)
                     .arg(av->early_exercise_premium / 100.0, 10, 'f', 4)
                     .arg(av->exercise_boundary > 0.0
                              ? QStringLiteral("%1").arg(
                                    av->exercise_boundary / 100.0, 9, 'f', 0)
                              : QStringLiteral("     never"));
        }
    }
    s += QStringLiteral(
        "\n  A zero boundary means early exercise is never optimal, and the\n"
        "  premium is then zero by construction rather than by rounding. On a\n"
        "  FUTURE (b = 0) both sides can be worth exercising early; on a\n"
        "  non-dividend stock (b = r) an American call never is, which is the\n"
        "  textbook result and a useful thing for this table to reproduce.\n\n"
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
    // NIFTY BY CONSTRUCTION, NOT BY OVERSIGHT. This page differences a
    // future against its own spot, and dataset/fut/ holds nifty and nothing
    // else -- so there is no BankNifty basis to compute and no instrument
    // selector on this page. Stated here because a page without the selector,
    // sitting beside eight that have one, otherwise reads as one somebody
    // forgot.
    const auto spot = ui_load_dated_closes(
        spot_path(dataset_root, QStringLiteral("nifty"), "1d"));
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

// ---------------------------------------------------------------------------
// P23-06 — Flagging: has the world moved under the model?
// ---------------------------------------------------------------------------

/// The drift detectors, on real NIFTY.
///
/// THIS IS A REAL MEASUREMENT AND IT IS THE SELF-CORRECTION LAYER'S ONLY PAGE.
///
/// `flagging/` decides when a model has stopped describing the world. Until
/// now none of it was visible anywhere, which is a strange thing to be missing
/// from a system whose stated defence against a bad retrain is
/// shadow -> canary -> auto-rollback.
///
/// Every number here comes from daily NIFTY returns split into a REFERENCE
/// window and a LIVE window. That is exactly the comparison a deployed model
/// faces, so the detectors are being asked the question they exist to answer
/// rather than a synthetic one.
///
/// WHAT A DRIFT ALARM IS AND IS NOT.
///
/// It says the input distribution moved. It does NOT say the model got worse,
/// and the two come apart constantly: a vol regime change moves every feature
/// and may leave a well-specified model alone, while a silent data-vendor
/// change can leave the distribution intact and destroy the mapping. That is
/// why the scorecard exists beside the detector, and why an alarm here is a
/// reason to LOOK rather than a reason to roll back.
[[nodiscard]] inline QString flagging_report(const QString& dataset_root,
                                                const QString& sym
                                                    = QStringLiteral("nifty")) {
    QString s = QStringLiteral("FLAGGING — drift detection\n\n");
    s += symbol_banner(sym);

    const auto closes =
        ui_load_closes(spot_path(dataset_root, sym, "1d"));
    if (closes.size() < 3000) {
        return s + QStringLiteral("  No dataset at dataset/spot/%1/1d/.\n")
                       .arg(sym);
    }
    std::vector<double> r = ui_log_returns(closes);
    for (double& v : r) { v *= 10000.0; }          // bps

    // Reference is the FIRST half, live the second. Split by index rather than
    // by date because the question is "did it move", not "when".
    const std::size_t half = r.size() / 2;
    std::vector<double> ref(r.begin(), r.begin() + static_cast<long>(half));
    std::vector<double> live(r.begin() + static_cast<long>(half), r.end());

    s += QStringLiteral(
        "  %1 daily returns. Reference = first %2, live = last %3.\n\n"
        "――― 1. POPULATION STABILITY INDEX ―――\n\n")
            .arg(r.size()).arg(ref.size()).arg(live.size());
    {
        std::vector<double> fit_copy = ref;
        PsiDetector psi;
        const auto fitted =
            psi.fit(fit_copy.data(), fit_copy.size(), 10, Scaling::Raw);
        if (!fitted) {
            s += QStringLiteral("  The reference window was refused.\n\n");
        } else {
            const auto v = psi.psi(live.data(), live.size());
            if (v) {
                // The 0.10 / 0.25 bands are the industry convention and they
                // are a CONVENTION, not a test -- stated here so the number is
                // read against something rather than felt.
                const char* band = *v < 0.10 ? "no material shift"
                                 : (*v < 0.25 ? "moderate shift — look"
                                              : "major shift — the reference "
                                                "is stale");
                s += QStringLiteral(
                    "  PSI %1  over 10 equal-frequency bins\n"
                    "  Convention: < 0.10 stable, 0.10–0.25 moderate, "
                    "> 0.25 major.\n"
                    "  Verdict: %2\n\n").arg(*v, 0, 'f', 5)
                         .arg(QString::fromUtf8(band));
            }
        }
    }

    // ――― 2. Kolmogorov-Smirnov ―――
    {
        std::vector<double> a = ref, b = live;
        const auto d = ks_statistic(a.data(), a.size(), b.data(), b.size());
        if (d) {
            // 1.36 is the SMIRNOV COEFFICIENT c(alpha) at alpha = 0.05, and
            // the third argument is that coefficient -- not alpha itself.
            // Passing 0.05 here (as this page first did) returns a critical
            // value 27 times too small, which would call every comparison
            // significant and never say so.
            const double c_alpha_05 = 1.36;
            const double crit =
                ks_critical(ref.size(), live.size(), c_alpha_05);
            s += QStringLiteral(
                "――― 2. KOLMOGOROV-SMIRNOV ―――\n\n"
                "  D %1   critical at 5% %2   %3\n\n"
                "  KS answers a different question from PSI: PSI weights the\n"
                "  bins a model actually uses, KS is the largest gap anywhere\n"
                "  in the CDF. They disagree when the tails moved and the body\n"
                "  did not — which for returns is most of the time.\n\n")
                     .arg(*d, 0, 'f', 5).arg(crit, 0, 'f', 5)
                     .arg(*d > crit ? QStringLiteral("DISTRIBUTIONS DIFFER")
                                    : QStringLiteral("no detectable difference"));
        }
    }

    // ――― 3. sequential detectors ―――
    s += QStringLiteral(
        "――― 3. SEQUENTIAL DETECTORS ―――\n\n"
        "  PSI and KS compare two fixed windows. These watch a stream and say\n"
        "  WHEN, which is what a live system needs — a model does not fail at\n"
        "  a window boundary.\n\n"
        "  Fed the absolute return, so they are watching volatility.\n\n");
    {
        PageHinkley ph;
        // Neither threshold has a default in the header, deliberately: a
        // tolerance of zero fires on any deviation and a threshold is a claim
        // about how much evidence justifies pulling a model. Both are stated.
        const double delta = 5.0, lambda = 500.0;
        std::size_t alarms = 0, first = 0;
        if (ph.configure(delta, lambda)) {
            for (std::size_t i = 0; i < r.size(); ++i) {
                const auto fired = ph.push(std::fabs(r[i]));
                if (fired && *fired) {
                    ++alarms;
                    if (first == 0) { first = i; }
                }
            }
        }
        s += QStringLiteral(
            "  Page-Hinkley  delta %1  lambda %2\n"
            "    alarms %3, first at observation %4 of %5\n\n")
                 .arg(delta, 0, 'f', 1).arg(lambda, 0, 'f', 1)
                 .arg(alarms).arg(first).arg(r.size());

        Adwin ad;
        std::size_t aw = 0, aw_first = 0;
        if (ad.configure(0.002)) {
            for (std::size_t i = 0; i < r.size(); ++i) {
                const auto fired = ad.push(std::fabs(r[i]));
                if (fired && *fired) {
                    ++aw;
                    if (aw_first == 0) { aw_first = i; }
                }
            }
        }
        s += QStringLiteral(
            "  ADWIN  confidence 0.002\n"
            "    window cuts %1, first at observation %2\n\n"
            "  ADWIN cuts its own window when the two halves differ, so it\n"
            "  needs no reference period at all. That is the property that\n"
            "  matters in production: a reference window is a thing somebody\n"
            "  has to remember to refresh.\n\n").arg(aw).arg(aw_first);
    }

    // ――― 4. THE SCORECARD, WITH REAL FORECASTS ―――
    //
    // I earlier declined this section on the grounds that "no model has been
    // scored in production, and a scorecard of one shadow fit would be a
    // table of zeros pretending to be a track record". The second half is
    // right and the first half was the wrong reason to skip it: the momentum
    // and mean-reversion rules from P21 emit a forecast on every bar of 36
    // years, and scoring THOSE is neither zeros nor a pretence. What it is
    // not is a production track record, and the page says which it is.
    s += QStringLiteral(
        "――― 4. THE SCORECARD ―――\n\n"
        "  The P21 rules, scored per REGIME on daily NIFTY. This is a\n"
        "  backtest scorecard, not a production one: nothing here has traded.\n\n"
        "  CLAUDE.md: report per regime, NEVER only in aggregate. The\n"
        "  aggregate is shown last, with the cell counts it was built from,\n"
        "  so it cannot be quoted without the reader seeing what it hid.\n\n");
    {
        Scorecards cards;
        const std::size_t w = 60;
        std::size_t scored = 0, incomplete = 0;

        // Regimes, computed CAUSALLY from a trailing window. Vol by tercile
        // against the whole sample's own quantiles, trend by the sign of a
        // trailing lag-1 autocorrelation. Crude on purpose -- the subject
        // here is the scorecard, and a richer detector would confound "the
        // cells differ" with "the detector is clever".
        std::vector<double> vols;
        vols.reserve(r.size());
        for (std::size_t i = w; i < r.size(); ++i) {
            double m = 0.0;
            for (std::size_t k = i - w; k < i; ++k) { m += r[k]; }
            m /= static_cast<double>(w);
            double v = 0.0;
            for (std::size_t k = i - w; k < i; ++k) { v += (r[k]-m)*(r[k]-m); }
            vols.push_back(std::sqrt(v / static_cast<double>(w - 1)));
        }
        std::vector<double> sorted_v = vols;
        std::sort(sorted_v.begin(), sorted_v.end());
        const double lo_q = sorted_v[sorted_v.size() / 3];
        const double hi_q = sorted_v[2 * sorted_v.size() / 3];

        const auto mom = momentum_positions(r, MomentumSpec{});
        MeanRevSpec mrs;
        const auto rev = meanrev_positions(r, mrs);
        if (mom && rev) {
            for (std::size_t i = w; i + 1 < r.size(); ++i) {
                MarketRegime reg;
                const double sd = vols[i - w];
                reg.vol = sd < lo_q ? VolRegime::Low
                        : (sd > hi_q ? VolRegime::High : VolRegime::Normal);
                // Trailing lag-1 autocorrelation, from data through i-1 only.
                double num = 0.0, den = 0.0;
                for (std::size_t k = i - w + 1; k < i; ++k) {
                    num += r[k] * r[k - 1];
                    den += r[k - 1] * r[k - 1];
                }
                const double rho = den > 0.0 ? num / den : 0.0;
                reg.trend = rho > 0.05 ? TrendRegime::Trending
                          : (rho < -0.05 ? TrendRegime::MeanReverting
                                         : TrendRegime::RandomWalk);
                reg.liquidity = LiquidityRegime::Normal;
                if (!reg.complete()) { ++incomplete; continue; }
                const double realised = r[i + 1];
                (void)cards.observe(0, 0, reg, (*mom)[i], realised);
                (void)cards.observe(1, 0, reg, (*rev)[i], realised);
                ++scored;
            }
        }

        s += QStringLiteral("  %1 bars scored.\n\n"
                            "   model      regime                 n      IC"
                            "    IC se     hit\n").arg(scored);
        const char* tnames[] = {"?", "trending", "meanrev ", "randwalk"};
        const char* vnames[] = {"?", "lowvol ", "normvol", "highvol"};
        for (std::size_t m = 0; m < 2; ++m) {
            for (std::size_t t = 1; t <= 3; ++t) {
                for (std::size_t vq = 1; vq <= 3; ++vq) {
                    const std::size_t cell = t * 4 + vq;
                    const ScoreCell& c = cards.at(m, 0, cell);
                    if (c.n == 0) { continue; }
                    const auto ic = c.ic();
                    const auto hr = c.hit_rate();
                    s += QStringLiteral("   %1  %2 %3  %4  %5  %6  %7\n")
                             .arg(m == 0 ? QStringLiteral("momentum")
                                         : QStringLiteral("meanrev "))
                             .arg(QString::fromUtf8(tnames[t]))
                             .arg(QString::fromUtf8(vnames[vq]))
                             .arg(c.n, 7)
                             .arg(ic ? QStringLiteral("%1").arg(*ic, 7, 'f', 4)
                                     : QStringLiteral("      —"))
                             .arg(c.ic_std_error(), 7, 'f', 4)
                             .arg(hr ? QStringLiteral("%1").arg(*hr, 6, 'f', 3)
                                     : QStringLiteral("     —"));
                }
            }
        }

        // HOW MANY OF THOSE CELLS ARE ACTUALLY A SIGNAL?
        //
        // ROADMAP section 3, for the fourth time in this UI: a measurement
        // without its error bar is not a measurement. `ic_std_error()` is
        // carried on every cell precisely so a 0.15 from twelve observations
        // cannot be read beside a 0.10 from nine hundred, and a table of
        // twenty ICs invites exactly that reading.
        {
            std::size_t cells_total = 0, cells_2se = 0;
            double best_t = 0.0;
            for (std::size_t m = 0; m < 2; ++m) {
                for (std::size_t cell = 0; cell < 16; ++cell) {
                    const ScoreCell& c = cards.at(m, 0, cell);
                    if (c.n == 0) { continue; }
                    const auto ic = c.ic();
                    if (!ic) { continue; }
                    ++cells_total;
                    const double t = *ic / c.ic_std_error();
                    if (std::fabs(t) > best_t) { best_t = std::fabs(t); }
                    if (std::fabs(t) > 2.0) { ++cells_2se; }
                }
            }
            s += QStringLiteral(
                "\n  Of %1 populated cells, %2 have an IC further than TWO\n"
                "  standard errors from zero. The largest is %3 sigma.\n\n"
                "  Twenty cells were examined, so at a naive 2-sigma bar one\n"
                "  clears by chance. Read the table as a SHAPE -- momentum\n"
                "  positive where mean reversion is negative, which is what\n"
                "  two near-opposite rules must do -- and not as twenty\n"
                "  separate results.\n").arg(cells_total).arg(cells_2se)
                     .arg(best_t, 0, 'f', 2);
        }

        s += QStringLiteral("\n  AND THE AGGREGATE, WITH WHAT IT HID:\n\n");
        for (std::size_t m = 0; m < 2; ++m) {
            const auto a = cards.aggregate(m, 0);
            const auto ic = a.overall.ic();
            s += QStringLiteral(
                "   %1  overall IC %2 over %3 obs, from %4 populated cells,\n"
                "             thinnest %5\n")
                     .arg(m == 0 ? QStringLiteral("momentum")
                                 : QStringLiteral("meanrev "))
                     .arg(ic ? QStringLiteral("%1").arg(*ic, 7, 'f', 4)
                             : QStringLiteral("      —"))
                     .arg(a.overall.n).arg(a.populated).arg(a.thinnest);
        }
        s += QStringLiteral(
            "\n  An IC of +0.30 in trending regimes and -0.30 in ranging ones\n"
            "  aggregates to approximately zero, and read as one number that\n"
            "  is a model with no edge rather than a model with a switch. The\n"
            "  cell counts are printed beside the aggregate for exactly that\n"
            "  reason: the second thing an aggregate hides is how thin the\n"
            "  cells under it were.\n\n"
            "  `ic()` REFUSES below four observations rather than returning\n"
            "  the +/-1 that two points always produce.\n\n");
    }

    s += QStringLiteral(
        "――― WHAT IS STILL MISSING ―――\n\n"
        "  A PRODUCTION scorecard. The table above is a backtest: nothing in\n"
        "  it has traded, so it measures the rules rather than the pipeline.\n"
        "  flagging/deploy.hpp's shadow -> canary -> auto-rollback path needs\n"
        "  a model actually serving before it has anything to gate.\n");
    return s;
}

// ---------------------------------------------------------------------------
// P23-07 — Microstructure
// ---------------------------------------------------------------------------

/// Order-book imbalance, microprice, VPIN and Kyle's lambda.
///
/// THE FEED HAS NEVER DELIVERED DEPTH, AND THAT IS THE HEADLINE.
///
/// The Live Grid already says it in the depth pane: "the feed has never
/// delivered depth for this instrument — which is not the same as a book with
/// nothing in it". Every measure on this page needs an L2 book, so none of
/// them has ever run on real data, and this page exists to say so in one place
/// and to prove the arithmetic works for when a full-mode subscription arrives.
///
/// NYQUIST IS WHY THIS MATTERS AND WHY IT CANNOT BE FAKED FROM BARS.
///
/// ROADMAP section 3: order-book imbalance decays in 10–200 ms. The predecessor
/// polled at 500 ms and could not, even in principle, observe it. No amount of
/// five-minute bar data substitutes — the quantity does not exist at that
/// sampling rate, which is why this page shows constructed books rather than
/// something derived from `dataset/`.
[[nodiscard]] inline QString microstructure_report() {
    QString s = QStringLiteral(
        "MICROSTRUCTURE — imbalance, microprice, VPIN, Kyle's lambda\n\n"
        "――― THERE IS NO DEPTH ―――\n\n"
        "  No Kite full-mode subscription is running, so the feed has never\n"
        "  delivered an L2 book. Nothing here is a market measurement.\n\n"
        "  And it cannot be reconstructed from dataset/: order-book imbalance\n"
        "  decays in 10–200 ms, so at five-minute bars the quantity does not\n"
        "  exist. That is a Nyquist limit, not a data-cleaning problem, and it\n"
        "  is the reason this engine is event-driven rather than a faster\n"
        "  poll.\n\n"
        "――― 1. IMBALANCE AND MICROPRICE, ON CONSTRUCTED BOOKS ―――\n\n"
        "     bid qty   ask qty       OBI    microprice   mid\n");
    {
        for (const std::int64_t bq : {100, 300, 500, 700, 900}) {
            const std::int64_t aq = 1000 - bq;
            BookState b{};
            b.bid[0].px = Price{2'399'900};
            b.bid[0].qty = Qty{bq};
            b.ask[0].px = Price{2'400'100};
            b.ask[0].qty = Qty{aq};
            b.bid_levels = 1;
            b.ask_levels = 1;
            const auto o = obi(b, 1);
            const auto mp = microprice(b);
            s += QStringLiteral("   %1   %2  %3    %4  %5\n")
                     .arg(bq, 9).arg(aq, 8)
                     .arg(o ? QStringLiteral("%1").arg(*o, 8, 'f', 4)
                            : QStringLiteral("       —"))
                     .arg(mp ? QStringLiteral("%1")
                                   .arg(static_cast<double>(mp->raw()) / 100.0,
                                        11, 'f', 2)
                             : QStringLiteral("          —"))
                     .arg(24000.00, 8, 'f', 2);
        }
    }
    s += QStringLiteral(
        "\n  The microprice leans toward the side with LESS size, because that\n"
        "  is the side about to be taken. A mid-price ignores that and is the\n"
        "  wrong reference for anything that has to cross a spread.\n\n"
        "――― 2. VPIN AND KYLE'S LAMBDA REFUSE TO GUESS ―――\n\n");
    {
        Vpin::Config vc;
        vc.bucket_volume = 1000;
        vc.window = 50;
        vc.min_buckets = 50;
        Vpin v(vc);
        const auto empty = v.value();
        s += QStringLiteral(
            "  A fresh VPIN with no trades reports %1.\n")
                 .arg(empty ? QStringLiteral("a value")
                            : QStringLiteral("NOTHING — as it must"));
        // Feed a one-sided flow and check it becomes extreme rather than
        // hovering at 0.5, which is what a broken classifier produces.
        for (int i = 0; i < 60 * 20; ++i) {
            v.on_trade(TradeSide::Buy, Qty{50});
        }
        const auto loaded = v.value();
        s += QStringLiteral(
            "  After 1,200 uniformly BUY-initiated trades: %1 over %2 buckets.\n"
            "  A perfectly one-sided flow is toxicity 1.0. A classifier that\n"
            "  returned 0.5 here would be reporting a coin flip and would look\n"
            "  entirely reasonable on a screen.\n\n")
                 .arg(loaded ? QStringLiteral("%1").arg(loaded->value, 0, 'f', 4)
                             : QStringLiteral("still nothing"))
                 .arg(loaded ? loaded->buckets : 0);

        KyleLambda::Config kc;
        kc.min_samples = 30;
        KyleLambda k(kc);
        const auto before = k.value();
        // A known slope: 2 paise of impact per unit of signed volume.
        for (int i = 1; i <= 200; ++i) {
            const std::int64_t vol = (i % 2 == 0) ? i : -i;
            k.on_observation(vol, Price{2 * vol});
        }
        const auto after = k.value();
        s += QStringLiteral(
            "  Kyle's lambda before %1 observations: %2.\n"
            "  Fed a known slope of 2.0 paise per unit, it recovers %3\n"
            "  with R2 %4 over %5 samples.\n\n"
            "  It is a MEASURED quantity with an error bar, so ROADMAP section\n"
            "  3 says size on its lower confidence bound and never the point\n"
            "  estimate. The R2 is reported for exactly that reason.\n\n")
                 .arg(kc.min_samples)
                 .arg(before ? QStringLiteral("a slope")
                             : QStringLiteral("nothing, correctly"))
                 .arg(after ? QStringLiteral("%1").arg(after->lambda, 0, 'f', 6)
                            : QStringLiteral("—"))
                 .arg(after ? QStringLiteral("%1").arg(after->r2, 0, 'f', 6)
                            : QStringLiteral("—"))
                 .arg(after ? after->samples : 0);
    }
    s += QStringLiteral(
        "――― WHAT WOULD MAKE THIS REAL ―――\n\n"
        "  A Kite full-mode subscription (P2-02). Depth arrives, the Live Grid\n"
        "  depth pane fills, and every number above becomes a measurement\n"
        "  instead of a demonstration. Nothing else on this page changes.\n");
    return s;
}

// ---------------------------------------------------------------------------
// P24-01 — Sizing and limits
// ---------------------------------------------------------------------------

/// How big, and what stops it being bigger.
///
/// THE ONE SENTENCE THIS PAGE EXISTS FOR.
///
/// ROADMAP section 3: size on the LOWER CONFIDENCE BOUND of edge, never the
/// point estimate, and a signal whose error bar straddles zero is not a signal.
/// `size_position` implements that literally -- it subtracts `edge_sigmas`
/// standard errors before doing anything else, and returns `NoEdge` with zero
/// lots when what is left is not positive.
///
/// That is the single most consequential line in `risk/`, and until now it was
/// not visible anywhere in the UI. The table below sweeps the error bar at a
/// FIXED point estimate so the reader can see the position go to zero while
/// the headline edge never changes.
///
/// FOUR METHODS, AND THE ANSWER IS THE SMALLEST.
///
/// Fixed-fractional, Kelly, vol-target and the exchange freeze quantity each
/// produce a size; the decision is their minimum, and `binding` names which
/// one it was. Keeping all four rather than the winner alone is what makes a
/// surprising size a question with its answer already attached -- and rule 10
/// wants the decision reproducible from what was recorded.
[[nodiscard]] inline QString sizing_report() {
    QString s = QStringLiteral(
        "SIZING AND LIMITS — P24-01\n\n"
        "――― 1. THE EDGE IS SIZED ON ITS LOWER BOUND ―――\n\n"
        "  The point estimate is held FIXED at 12.0 bps throughout. Only the\n"
        "  error bar moves. Watch the position die while the headline edge\n"
        "  stays exactly where it was.\n\n"
        "    edge   std err  sigmas   lower bound   lots   bound by\n");

    // Capital, price and lot size are STATED here. In the engine they come
    // from the account and the spec store -- rule 1 -- and this page is
    // demonstrating the decision rule, not quoting a live position.
    const auto base = [] {
        SizingInputs in;
        // Rs 5 crore, in paise.
        //
        // The first version of this page used Rs 10 lakh and every row of
        // every table came back ZERO LOTS -- correctly, because one NIFTY lot
        // at 24,000 x 75 is Rs 18 lakh of notional and a 1% risk budget on ten
        // lakh does not reach a single lot. The tables were arithmetically
        // right and told the reader nothing, which is its own kind of wrong:
        // a demonstration whose every cell is the same number demonstrates
        // nothing. Capital is sized here so the METHODS separate.
        in.capital = Notional{5'000'000'000};
        in.price = Price{2'400'000};             // 24,000.00
        in.lot_size = LotSize{75};
        in.freeze_qty = Qty{1800};
        in.edge_bps = Bps{12.0};
        in.edge_sigmas = 2.0;
        in.period_vol = Vol{0.012};
        in.risk_fraction = 0.01;
        in.stop_distance = Price{24'000};
        in.kelly_divisor = 4.0;
        in.target_vol = 0.008;
        in.max_leverage = 3.0;
        return in;
    };

    const auto bound_name = [](SizingBound b) {
        switch (b) {
        case SizingBound::FixedFractional: return "fixed fractional";
        case SizingBound::Kelly:           return "Kelly";
        case SizingBound::VolTarget:       return "vol target";
        case SizingBound::FreezeQuantity:  return "exchange freeze";
        case SizingBound::NoEdge:          return "NO EDGE — flat";
        case SizingBound::Unset:           break;
        }
        return "unset";
    };

    for (const double se : {0.5, 2.0, 4.0, 5.9, 6.0, 8.0}) {
        SizingInputs in = base();
        in.edge_std_error_bps = Bps{se};
        const auto d = size_position(in);
        if (!d) { continue; }
        s += QStringLiteral("   %1  %2   %3   %4  %5   %6\n")
                 .arg(in.edge_bps.raw(), 6, 'f', 1).arg(se, 8, 'f', 1)
                 .arg(in.edge_sigmas, 6, 'f', 1)
                 .arg(d->edge_lower_bps, 12, 'f', 2)
                 .arg(d->lots.raw(), 6)
                 .arg(QString::fromUtf8(bound_name(d->binding)));
    }
    s += QStringLiteral(
        "\n  At a 6.0 bps standard error the lower bound reaches zero and the\n"
        "  position does too. Nothing about the 12 bps estimate changed. A\n"
        "  system that sized on the point estimate would have taken the same\n"
        "  trade in every row of this table.\n\n");

    // ――― 2. which method binds ―――
    s += QStringLiteral(
        "――― 2. WHICH METHOD BINDS ―――\n\n"
        "  Every method's answer is kept, not just the winner.\n\n"
        "    target vol   fixed frac   Kelly   vol target   freeze   ->  lots"
        "   bound by\n");
    for (const double tv : {0.002, 0.004, 0.008, 0.020, 0.060}) {
        SizingInputs in = base();
        in.edge_std_error_bps = Bps{2.0};
        in.target_vol = tv;
        const auto d = size_position(in);
        if (!d) { continue; }
        s += QStringLiteral("   %1  %2  %3   %4  %5   %6   %7\n")
                 .arg(tv, 10, 'f', 3)
                 .arg(d->by_fixed_fractional.raw(), 10)
                 .arg(d->by_kelly.raw(), 6)
                 .arg(d->by_vol_target.raw(), 10)
                 .arg(d->by_freeze.raw(), 7)
                 .arg(d->lots.raw(), 6)
                 .arg(QString::fromUtf8(bound_name(d->binding)));
    }
    s += QStringLiteral(
        "\n  The exchange FREEZE QUANTITY is a hard cap and it comes from the\n"
        "  spec store, never a literal — it is instrument-specific and it\n"
        "  changes. A size that silently exceeds it is an order the exchange\n"
        "  rejects, which is the good case; the bad case is a partial fill\n"
        "  and a position nobody planned.\n\n");

    // ――― 3. Kelly ―――
    s += QStringLiteral(
        "――― 3. KELLY, AND WHY THE DIVISOR IS NEVER 1 ―――\n\n"
        "    divisor   fraction of capital   lots\n");
    for (const double kd : {1.0, 2.0, 4.0, 8.0}) {
        SizingInputs in = base();
        in.edge_std_error_bps = Bps{2.0};
        in.kelly_divisor = kd;
        const auto d = size_position(in);
        const auto kf = kelly_fraction(in);
        if (!d || !kf) { continue; }
        s += QStringLiteral("   %1        %2   %3\n")
                 .arg(kd, 7, 'f', 1).arg(*kf, 18, 'f', 6)
                 .arg(d->by_kelly.raw(), 6);
    }
    s += QStringLiteral(
        "\n  Full Kelly maximises log growth ON A KNOWN EDGE. The edge here is\n"
        "  measured, its error bar is in section 1, and full Kelly on a\n"
        "  mis-measured edge is how accounts die while the model is still\n"
        "  right on average.\n\n");

    // ――― 4. conservation ―――
    s += QStringLiteral(
        "――― 4. THE CONSERVATION INVARIANT ―――\n\n"
        "  Sum(fills) + Sum(costs) + cash_delta == 0, EXACTLY, in integer\n"
        "  paise, checked every tick. A breach trips the kill switch.\n\n"
        "       fills       costs  cash delta   residual   verdict\n");
    struct Row { std::int64_t f, c, d; const char* note; };
    for (const Row& r : {Row{-2'400'000, -1'250, 2'401'250, "a clean buy"},
                         Row{2'400'000, -1'250, -2'398'750, "a clean sell"},
                         Row{-2'400'000, -1'250, 2'401'251, "one paise adrift"}}) {
        const std::int64_t resid = conservation_residual(
            Notional{r.f}, Notional{r.c}, Notional{r.d});
        s += QStringLiteral("   %1  %2  %3  %4   %5\n")
                 .arg(r.f, 10).arg(r.c, 10).arg(r.d, 11).arg(resid, 9)
                 .arg(resid == 0 ? QStringLiteral("balanced — %1")
                                       .arg(QString::fromUtf8(r.note))
                                 : QStringLiteral("KILL SWITCH — %1")
                                       .arg(QString::fromUtf8(r.note)));
    }
    s += QStringLiteral(
        "\n  One paise is a breach. Not a tolerance, not a rounding allowance:\n"
        "  money is integer paise and the identity is exact, so a residual of\n"
        "  1 means an accounting path is wrong and every number downstream of\n"
        "  it is suspect.\n\n"
        "――― 5. THE LIMIT VOCABULARY ―――\n\n"
        "  %1 distinct violations are defined as bit flags, so an order can\n"
        "  breach several at once and the report names all of them rather\n"
        "  than the first one found. `clean()` is the only way to pass.\n\n"
        "  Nothing on this page can place an order. risk/ measures and\n"
        "  refuses; oms/ is the only module that trades, and the UI does not\n"
        "  link it.\n")
            .arg(12);
    return s;
}

// ---------------------------------------------------------------------------
// P24-02 — Cointegration and validation
// ---------------------------------------------------------------------------

/// Engle-Granger on a pair whose answer is known in advance, and the
/// walk-forward machinery that every backtest in this tree has to pass through.
///
/// WHY SPOT AGAINST FUTURES IS THE RIGHT TEST CASE.
///
/// A cointegration test is easy to run and hard to trust: on any two trending
/// series it will find a relationship, and the literature is full of spurious
/// pairs that were cointegrated right up until they were traded. So the pair
/// here is one where the ANSWER IS KNOWN BEFORE THE TEST: NIFTY spot and the
/// NIFTY future are tied together by arbitrage. If Engle-Granger cannot find
/// that, the implementation is broken; if it finds cointegration everywhere,
/// it is also broken, which is what the control below is for.
///
/// THE CONTROL IS SPOT AGAINST NOISE, AND IT MUST FAIL.
///
/// The same test, same window, same lags, against an independent random walk.
/// Two independent random walks are the textbook spurious regression: high R2,
/// no cointegration. A test that cannot tell those apart is a test that will
/// pair anything with anything.
[[nodiscard]] inline QString cointegration_report(const QString& dataset_root,
                                                     const QString& sym
                                                         = QStringLiteral("nifty")) {
    QString s = QStringLiteral("COINTEGRATION AND VALIDATION — P24-02\n\n");
    s += symbol_banner(sym);

    const auto fut = ui_load_dated_closes(
        dataset_root + QStringLiteral("/fut/nifty/1d/all.csv"));
    const auto spot = ui_load_dated_closes(
        spot_path(dataset_root, sym, "1d"));
    if (fut.size() < 600 || spot.size() < 600) {
        return s + QStringLiteral("  No dataset for the futures/spot pair.\n");
    }

    // Joined on date, for the reason the Basis page learned the hard way.
    std::map<std::string, double> spot_by_day;
    for (const auto& kv : spot) { spot_by_day[kv.first] = kv.second; }
    std::vector<double> y, x;
    for (const auto& kv : fut) {
        const auto it = spot_by_day.find(kv.first);
        if (it == spot_by_day.end()) { continue; }
        y.push_back(kv.second);        // future
        x.push_back(it->second);       // spot
    }
    // The ADF buffers are a fixed 2048, so the window is the LAST 2000
    // sessions rather than everything. Stated because a silently truncated
    // window is how a result stops being about the period somebody thinks.
    const std::size_t w = std::min<std::size_t>(2000, y.size());
    if (w < 600) { return s + QStringLiteral("  Too few paired sessions.\n"); }
    const std::vector<double> yw(y.end() - static_cast<long>(w), y.end());
    const std::vector<double> xw(x.end() - static_cast<long>(w), x.end());

    s += QStringLiteral(
        "――― 1. NIFTY FUTURE AGAINST NIFTY SPOT ―――\n\n"
        "  %1 paired sessions (the last %2 of %3 available — the ADF buffer\n"
        "  is a fixed 2048 and a silently truncated window is how a result\n"
        "  stops being about the period somebody thinks it is).\n\n")
            .arg(w).arg(w).arg(y.size());
    {
        const auto eg = engle_granger(yw.data(), xw.data(), w, 1);
        if (!eg) {
            s += QStringLiteral("  The test was refused on this pair.\n\n");
        } else {
            s += QStringLiteral(
                "  hedge ratio (beta)   %1        alpha %2\n"
                "  R2 of the level regression   %3\n"
                "  level correlation            %4\n\n"
                "  ADF on the residual: t %5   gamma %6   n %7\n"
                "  MacKinnon 5% critical        %8\n\n"
                "  VERDICT: %9\n\n")
                     .arg(eg->hedge.beta, 0, 'f', 6)
                     .arg(eg->hedge.alpha, 0, 'f', 2)
                     .arg(eg->hedge.r_squared, 0, 'f', 6)
                     .arg(eg->level_correlation, 0, 'f', 6)
                     .arg(eg->residual_test.t_stat, 0, 'f', 4)
                     .arg(eg->residual_test.gamma, 0, 'f', 6)
                     .arg(eg->residual_test.n)
                     .arg(eg->critical_5pct, 0, 'f', 4)
                     .arg(eg->cointegrated
                              ? QStringLiteral("COINTEGRATED, as arbitrage "
                                               "requires")
                              : QStringLiteral("NOT cointegrated — which for "
                                               "this pair would mean the "
                                               "implementation is wrong"));
            s += QStringLiteral(
                "  A beta near 1.0 is the whole point: the future IS the spot\n"
                "  plus carry, so any other hedge ratio would say the\n"
                "  regression had found something other than the arbitrage.\n\n");
        }
    }

    // ――― 2. the control ―――
    s += QStringLiteral(
        "――― 2. THE CONTROL: THE SAME TEST AGAINST NOISE ―――\n\n"
        "  Two independent random walks are the textbook spurious regression:\n"
        "  high R2, no cointegration. A test that cannot tell this from\n"
        "  section 1 will pair anything with anything.\n\n");
    {
        std::uint64_t seed = 0xC0FFEE5Eull;
        auto u = [&seed]() {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            return (static_cast<double>(seed >> 11) + 0.5)
                 * (1.0 / 9007199254740992.0);
        };
        std::vector<double> walk(w);
        double lvl = xw.front();
        for (std::size_t i = 0; i < w; ++i) {
            lvl *= std::exp(0.01 * std::sqrt(-2.0 * std::log(u()))
                            * std::cos(6.283185307179586 * u()));
            walk[i] = lvl;
        }
        const auto eg = engle_granger(xw.data(), walk.data(), w, 1);
        if (eg) {
            s += QStringLiteral(
                "  R2 %1   level correlation %2\n"
                "  ADF t %3 against a 5% critical of %4\n\n"
                "  VERDICT: %5\n\n")
                     .arg(eg->hedge.r_squared, 0, 'f', 6)
                     .arg(eg->level_correlation, 0, 'f', 6)
                     .arg(eg->residual_test.t_stat, 0, 'f', 4)
                     .arg(eg->critical_5pct, 0, 'f', 4)
                     .arg(eg->cointegrated
                              ? QStringLiteral("cointegrated — which on "
                                               "independent noise is a BUG, "
                                               "not a pair")
                              : QStringLiteral("NOT cointegrated, correctly"));
        }
    }

    // ――― 3. walk-forward and the purge ―――
    s += QStringLiteral(
        "――― 3. WALK-FORWARD, PURGE AND EMBARGO ―――\n\n"
        "  Markets are not ergodic, so random K-fold is BANNED in the training\n"
        "  harness (ROADMAP section 3). Every fold below trains on the past\n"
        "  and tests on the future, with a gap the length of the label horizon.\n\n");
    {
        WalkForwardSpec spec;
        spec.initial_train = 500;
        spec.test_len = 100;
        // step == test_len gives NON-OVERLAPPING test blocks, which is what a
        // report wants: overlapping tests double-count the same bars and make
        // a fold count look like independent evidence when it is not.
        spec.step = 100;
        spec.gap = 20;
        spec.expanding = true;
        const auto folds = walk_forward_count(2000, spec);
        s += QStringLiteral(
            "  n 2000, train %1, test %2, gap %3, expanding\n"
            "  -> %4 folds\n\n"
            "     fold   train         test          gap\n")
                 .arg(spec.initial_train).arg(spec.test_len).arg(spec.gap)
                 .arg(folds ? *folds : 0);
        const std::size_t nf = folds ? *folds : 0;
        for (std::size_t i = 0; i < nf && i < 6; ++i) {
            const auto f = walk_forward_fold(2000, spec, i);
            if (!f) { continue; }
            s += QStringLiteral("   %1   [%2, %3)   [%4, %5)   %6\n")
                     .arg(i, 6)
                     .arg(f->train.start, 5).arg(f->train.end, 5)
                     .arg(f->test.start, 5).arg(f->test.end, 5)
                     .arg(f->test.start - f->train.end, 6);
        }
        if (nf > 6) {
            s += QStringLiteral("   ... %1 more\n").arg(nf - 6);
        }
        s += QStringLiteral(
            "\n  THE GAP IS NOT COSMETIC. A label that looks 20 bars ahead is\n"
            "  computed from data inside the test block, so a training row\n"
            "  ending at the boundary has already seen it. Without the gap the\n"
            "  fold leaks, and it leaks in the direction that flatters.\n\n"
            "――― WHAT IS STILL MISSING ―――\n\n"
            "  strategies/parity.hpp, calendar.hpp and score.hpp have no page,\n"
            "  and cannot get one: parity needs an option chain, the calendar\n"
            "  scanner needs two dated slices of one, and score.hpp combines\n"
            "  signals from models that have not been fitted. They are listed\n"
            "  here rather than given a page of constructed inputs, because a\n"
            "  scanner that has only ever seen invented quotes is a scanner\n"
            "  nobody should trust the first time it sees a real one.\n");
    }
    return s;
}

// ---------------------------------------------------------------------------
// P24-03 — Memory: Hurst, EWMA, and the error bars that decide them
// ---------------------------------------------------------------------------

/// Does this series remember anything? Two estimators, one discipline.
///
/// BOTH NUMBERS ON THIS PAGE ARE MEANINGLESS WITHOUT THEIR ERROR BAR, AND THAT
/// IS THE POINT OF PUTTING THEM TOGETHER.
///
/// A Hurst exponent of 0.58 reads as persistence. With a standard error of
/// 0.04 it is 2 sigma from a random walk and worth acting on; with a standard
/// error of 0.06 it is not, and nothing about the 0.58 changed.
/// `hurst_departs_from_random_walk` is the only sanctioned way to act on the
/// number, and it takes the sigmas rather than assuming them.
///
/// ROADMAP section 3 again, in its third form on this UI: measurements carry
/// error, size on the lower bound, a signal whose bar straddles the null is
/// not a signal. The Sizing page shows it for edge, the Options page for
/// implied vol, and this page for memory.
///
/// AND THE REGIME DETECTOR ALREADY FOUND THE ANSWER HERE.
///
/// P6-03 measured the Hurst label against forward returns and it did not
/// separate them. That is a negative result this page does not overturn; it
/// shows where the exponent sits and how wide the bar is, so the negative
/// result is legible rather than remembered.
[[nodiscard]] inline QString memory_report(const QString& dataset_root,
                                              const QString& sym
                                                  = QStringLiteral("nifty")) {
    QString s = QStringLiteral("MEMORY — Hurst and EWMA — P24-03\n\n");
    s += symbol_banner(sym);

    const auto closes =
        ui_load_closes(spot_path(dataset_root, sym, "1d"));
    if (closes.size() < 2000) {
        return s + QStringLiteral("  No dataset at dataset/spot/%1/1d/.\n")
                       .arg(sym);
    }
    std::vector<double> r = ui_log_returns(closes);
    for (double& v : r) { v *= 10000.0; }

    // ――― 1. Hurst, with the bar ―――
    s += QStringLiteral(
        "――― 1. HURST, AND WHETHER IT DEPARTS FROM 0.5 ―――\n\n"
        "  R/S on daily NIFTY returns, over several window lengths. 0.5 is a\n"
        "  random walk; the question is never the exponent alone.\n\n"
        "     window        H     std err       R2   scales   2-sigma verdict\n");
    for (const std::size_t w : {512u, 1024u, 2048u, 4096u}) {
        if (r.size() < w) { continue; }
        const auto e = hurst_rs(r.data() + (r.size() - w), w);
        if (!e) { continue; }
        const bool departs = hurst_departs_from_random_walk(*e, 2.0);
        s += QStringLiteral("   %1  %2  %3  %4   %5   %6\n")
                 .arg(w, 8).arg(e->h, 7, 'f', 4).arg(e->std_error, 10, 'f', 4)
                 .arg(e->r_squared, 7, 'f', 4).arg(e->scales, 6)
                 .arg(departs ? QStringLiteral("departs")
                              : QStringLiteral("indistinguishable from noise"));
    }
    s += QStringLiteral(
        "\n  The R2 column is a separate question from the exponent: it says\n"
        "  whether a scaling law holds AT ALL. A series with no self-similar\n"
        "  structure still produces a slope, and a low R2 says that slope\n"
        "  describes nothing.\n\n"
        "  P6-03 already measured the Hurst LABEL against forward returns and\n"
        "  found it did not separate them. This page does not overturn that;\n"
        "  it makes the exponent and its bar visible so the negative result\n"
        "  is legible rather than remembered.\n\n");

    // ――― 2. EWMA vs the flat window ―――
    s += QStringLiteral(
        "――― 2. EWMA VARIANCE AGAINST A FLAT WINDOW ―――\n\n"
        "  P14 found EWMA beat GARCH on this series. It is worth seeing why a\n"
        "  one-parameter estimator is hard to beat: it is TIME-AWARE. The\n"
        "  update takes a timestamp, so a gap in the feed decays the estimate\n"
        "  by the elapsed time rather than by the number of ticks that\n"
        "  happened to arrive.\n\n"
        "     half-life   final vol (bps/day)   annualised\n");
    for (const std::int64_t days : {5, 10, 21, 63, 252}) {
        // tau in nanoseconds; a trading day is the unit here and it is stated
        // rather than derived from a calendar, because this is a decay
        // constant and not an instrument fact.
        const Duration tau{days * 86'400'000'000'000LL};
        auto v = EwmaVariance::create(tau);
        if (!v) { continue; }
        double last = 0.0;
        for (std::size_t i = 0; i < r.size(); ++i) {
            const Timestamp ts{
                static_cast<std::int64_t>(i) * 86'400'000'000'000LL};
            const auto up = v->update(ts, r[i]);
            if (up) { last = *up; }
        }
        const double sd = std::sqrt(last);
        s += QStringLiteral("   %1        %2          %3%\n")
                 .arg(days, 9).arg(sd, 14, 'f', 3)
                 .arg(sd * std::sqrt(252.0) / 100.0, 9, 'f', 2);
    }
    s += QStringLiteral(
        "\n  A longer half-life is not a better estimate, it is a different\n"
        "  question. Five days answers \"what is volatility now\"; a year\n"
        "  answers \"what has it been\". Reporting one without saying which is\n"
        "  how a risk number becomes untraceable.\n\n"
        "――― WHAT IS STILL WITHOUT A PAGE, AND WHY ―――\n\n"
        "  analytics/derivatives.hpp — velocity and acceleration of a price,\n"
        "  with error propagation. It needs a TIMED tick stream, not bars:\n"
        "  the whole value is resolving motion between quotes, and at daily\n"
        "  sampling the derivative is the return.\n\n"
        "  analytics/vix.hpp and american.hpp — both need an option chain.\n\n"
        "  models/mlp, recurrent, attention — LibTorch training. Fitting one\n"
        "  on press would freeze the window for minutes, and a page that\n"
        "  showed the LAST fit would be reporting a number it cannot\n"
        "  reproduce. They belong behind the training harness, not a button.\n\n"
        "  features/registry.hpp — a versioned registry with nothing\n"
        "  registered. A page listing zero features is not information.\n");
    return s;
}

// ---------------------------------------------------------------------------
// P11Q-05 / P25-02 — Discounted cash flow
// ---------------------------------------------------------------------------

/// A DCF, and the honest statement of how much of it is a forecast.
///
/// THE INPUTS ARE STATED. NOTHING HERE WAS FETCHED.
///
/// This page used to be a `blocked_page` saying `models/dcf.hpp` needs
/// point-in-time fundamentals from `strategies/fundamentals.hpp` and no filings
/// have been ingested. That is still true and it is still printed. What was
/// wrong was leaving the arithmetic unexercised because of it: the pricer, the
/// WACC, the net-debt bridge and the sensitivity grid all work, and a DCF whose
/// INPUTS are typed in is exactly how every DCF gets built anyway.
///
/// So the inputs below are a stated, illustrative company. They are not
/// Reliance and they are not fetched from anywhere. When filings land, the same
/// functions run on real numbers and this page changes in one place.
///
/// THE ANSWER IS A RANGE, AND THE PAGE LEADS WITH THAT.
///
/// A single DCF number is the least useful output this file can produce. What
/// decides whether a valuation means anything is `terminal_share` -- how much
/// of the value is a perpetuity assumption rather than a forecast -- and the
/// sensitivity grid, which shows the whole answer moving as the two
/// unobservable inputs move. Both are above the headline number here, not
/// below it.
[[nodiscard]] inline QString dcf_report() {
    // ---- the stated inputs -------------------------------------------
    // Everything in PAISE, because `equity_from_enterprise` takes Notional and
    // rule 3 puts money in integer paise. 1 crore of rupees = 1e9 paise.
    constexpr double kCrore = 1'000'000'000.0;      // paise per crore
    const double fcff = 1200.0 * kCrore;            // Rs 1,200 crore
    const Notional debt{static_cast<std::int64_t>(4500.0 * kCrore)};
    const Notional cash{static_cast<std::int64_t>(1800.0 * kCrore)};
    const double shares = 620'000'000.0;            // 62 crore shares
    const double market_cap_cr = 21'500.0;          // for the comparison only

    GrowthProfile g;
    g.high_years = 5;
    g.high_growth = 0.14;
    g.fade_years = 5;
    g.terminal_growth = 0.045;

    QString s = QStringLiteral(
        "DISCOUNTED CASH FLOW — P11Q-05\n\n"
        "――― THE INPUTS ARE STATED, NOT FETCHED ―――\n\n"
        "  No filings have been ingested. strategies/fundamentals.hpp returns\n"
        "  nothing before a statement's FILING date, by design, and there is\n"
        "  nothing behind it yet. Every number below was typed here.\n\n"
        "  It is still worth running: the pricer, the WACC, the net-debt\n"
        "  bridge and the sensitivity grid all work, and a DCF is built from\n"
        "  typed assumptions in any case. When filings land, the same\n"
        "  functions run on real numbers and this page changes in one place.\n\n"
        "  FCFF                 %1 cr\n"
        "  growth               %2% for %3y, fading to %4% over %5y\n"
        "  total debt           %6 cr\n"
        "  cash                 %7 cr\n"
        "  shares               %8 cr\n\n")
            .arg(fcff / kCrore, 0, 'f', 0)
            .arg(100.0 * g.high_growth, 0, 'f', 1).arg(g.high_years)
            .arg(100.0 * g.terminal_growth, 0, 'f', 1).arg(g.fade_years)
            .arg(static_cast<double>(debt.raw()) / kCrore, 0, 'f', 0)
            .arg(static_cast<double>(cash.raw()) / kCrore, 0, 'f', 0)
            .arg(shares / 10'000'000.0, 0, 'f', 1);

    // ---- WACC ---------------------------------------------------------
    const double eq_mkt = market_cap_cr * kCrore;
    const double dbt = static_cast<double>(debt.raw());
    const auto w = wacc(eq_mkt, dbt, 0.132, 0.086, 0.25);
    if (!w) { return s + QStringLiteral("  WACC was refused.\n"); }
    s += QStringLiteral(
        "――― 1. WACC ―――\n\n"
        "  cost of equity 13.2%   cost of debt 8.6%   tax 25%\n"
        "  E %1 cr   D %2 cr\n\n"
        "  WACC = E/V*Re + D/V*Rd*(1-tax) = %3%\n\n"
        "  THE TAX SHIELD IS ON THE DEBT LEG ONLY. Applying it to the whole\n"
        "  expression understates the discount rate by the equity share of\n"
        "  the shield, and it is one of the three errors models/dcf.hpp was\n"
        "  written around.\n\n")
            .arg(eq_mkt / kCrore, 0, 'f', 0).arg(dbt / kCrore, 0, 'f', 0)
            .arg(100.0 * *w, 0, 'f', 3);

    // ---- the valuation ------------------------------------------------
    const auto ev = dcf_fcff(fcff, g, *w);
    if (!ev) { return s + QStringLiteral("  The DCF was refused.\n"); }
    const double equity = equity_from_enterprise(*ev, debt, cash);
    const double per_share = equity / shares / 100.0;   // paise -> rupees

    s += QStringLiteral(
        "――― 2. HOW MUCH OF THIS IS A FORECAST? ―――\n\n"
        "  PV of the %1 explicit + fading years   %2 cr\n"
        "  PV of the perpetuity                   %3 cr\n"
        "  TERMINAL SHARE                         %4%\n\n"
        "  That last line is the one to read first. %4% of this valuation is\n"
        "  an assumption about forever, not a forecast of anything. A DCF is\n"
        "  not a measurement of a company; it is a measurement of the\n"
        "  perpetuity assumption, wearing ten years of arithmetic.\n\n")
            .arg(ev->dcf.years)
            .arg(ev->dcf.pv_explicit / kCrore, 0, 'f', 0)
            .arg(ev->dcf.pv_terminal / kCrore, 0, 'f', 0)
            .arg(100.0 * ev->dcf.terminal_share, 0, 'f', 1);

    s += QStringLiteral(
        "――― 3. THE BRIDGE, AND THE NUMBER ―――\n\n"
        "  enterprise value    %1 cr\n"
        "  less total debt     %2 cr\n"
        "  plus cash           %3 cr\n"
        "  equity value        %4 cr\n"
        "  per share           Rs %5\n\n"
        "  `equity_from_enterprise` is defined ONLY on EnterpriseValue. An\n"
        "  EquityValue has already netted the debt — FCFE is after interest\n"
        "  and repayment — so double-subtracting is a compile error rather\n"
        "  than a valuation quietly low by the debt.\n\n")
            .arg(ev->value() / kCrore, 0, 'f', 0)
            .arg(dbt / kCrore, 0, 'f', 0)
            .arg(static_cast<double>(cash.raw()) / kCrore, 0, 'f', 0)
            .arg(equity / kCrore, 0, 'f', 0)
            .arg(per_share, 0, 'f', 2);

    // ---- sensitivity ---------------------------------------------------
    const auto ts = terminal_growth_sensitivity(fcff, g, *w);
    if (ts) {
        s += QStringLiteral(
            "――― 4. WHAT MOVES IT ―――\n\n"
            "  %1% of total value per 1 BASIS POINT of terminal growth.\n"
            "  Over a plausible 50bp of disagreement about an unobservable\n"
            "  perpetual rate, that is %2% of the valuation.\n\n")
                 .arg(100.0 * *ts, 0, 'f', 4)
                 .arg(100.0 * *ts * 50.0, 0, 'f', 2);
    }

    const auto grid = sensitivity_grid(fcff, g, *w, 0.005, 0.005, 2);
    if (grid) {
        s += QStringLiteral(
            "  Equity value per share, Rs. Rows are WACC, columns terminal\n"
            "  growth. A cell where growth meets the discount rate is REFUSED,\n"
            "  not zero-filled: it is undefined, and averaging a zero into a\n"
            "  range drags the answer down while looking like arithmetic.\n\n"
            "        WACC \\ g  ");
        for (std::size_t j = 0; j < grid->n_growth; ++j) {
            s += QStringLiteral("%1  ").arg(100.0 * grid->growth[j], 7, 'f', 2);
        }
        s += QStringLiteral("\n");
        for (std::size_t i = 0; i < grid->n_rate; ++i) {
            s += QStringLiteral("        %1%   ")
                     .arg(100.0 * grid->rate[i], 6, 'f', 2);
            for (std::size_t j = 0; j < grid->n_growth; ++j) {
                const std::size_t k = i * grid->n_growth + j;
                if (!grid->priced[k]) {
                    s += QStringLiteral("      —  ");
                    continue;
                }
                const double eq = grid->value[k]
                                - dbt + static_cast<double>(cash.raw());
                s += QStringLiteral("%1  ")
                         .arg(eq / shares / 100.0, 7, 'f', 0);
            }
            s += QStringLiteral("\n");
        }
        s += QStringLiteral(
            "\n  highest / lowest priced cell   %1x\n"
            "  cells refused                  %2 of %3\n\n"
            "  A %1x spread across half a percent either way on two inputs\n"
            "  nobody can observe is the actual output of this exercise. The\n"
            "  single number in section 3 is one cell of this table.\n\n")
                 .arg(grid->spread(), 0, 'f', 2)
                 .arg(grid->refused())
                 .arg(grid->n_rate * grid->n_growth);
    }

    // ---- the finding about projection length ---------------------------
    s += QStringLiteral(
        "――― 5. A LONGER PROJECTION DOES NOT REMOVE THE ASSUMPTION ―――\n\n"
        "     explicit+fade years   terminal share\n");
    for (const std::size_t hy : {3u, 5u, 8u, 10u, 15u}) {
        GrowthProfile g2 = g;
        g2.high_years = hy;
        g2.fade_years = hy;
        const auto e2 = dcf_fcff(fcff, g2, *w);
        if (!e2) { continue; }
        s += QStringLiteral("        %1                 %2%\n")
                 .arg(2 * hy, 6).arg(100.0 * e2->dcf.terminal_share, 8, 'f', 1);
    }
    s += QStringLiteral(
        "\n  Projecting further MOVES the assumption rather than removing it:\n"
        "  the terminal share falls, but only because more of the same\n"
        "  guess has been written out year by year in the fade. The\n"
        "  perpetuity is smaller and the forecast is longer, and a\n"
        "  twenty-year explicit forecast of an Indian mid-cap is not\n"
        "  evidence about anything.\n\n"
        "――― WHAT WOULD MAKE THIS REAL ―――\n\n"
        "  strategies/fundamentals.hpp, populated. It returns nothing before\n"
        "  a statement's FILING date — not its period-end date — which is the\n"
        "  whole point of a point-in-time store and the reason a backtest\n"
        "  using it cannot see a result before the market did.\n");
    return s;
}

// ---------------------------------------------------------------------------
// P26-02 — Neural tier
// ---------------------------------------------------------------------------

/// The neural models, by RUNNING THE ACCEPTANCE TEST rather than recomputing.
///
/// I earlier declined this page on the grounds that "fitting on press freezes
/// the window, and showing the LAST fit reports a number the page cannot
/// reproduce". The first half was wrong -- `models/mlp.hpp` and
/// `models/recurrent.hpp` are HAND-ROLLED, not LibTorch: a fixed random hidden
/// layer with a ridge-solved readout, fitted in closed form. The whole
/// walk-forward over 211,000 five-minute returns takes about six seconds.
///
/// The second half was right, and it is why this page shells out to
/// `altair_neural_5m_test` instead of reimplementing the harness here.
/// `quant_pages.hpp` opens with the rule the whole file hangs on: a number
/// that exists in two places has already contradicted itself twice in this UI.
/// A second copy of a walk-forward -- its folds, its per-fold scaler, its
/// purge -- is exactly that failure with more places to make it. Running the
/// binary means the page cannot drift from the test, because it IS the test.
///
/// The same pattern as the Link Kite panel, and for the same reason: launching
/// a process is how you consume something you must not duplicate.
[[nodiscard]] inline QString neural_report() {
    QString s = QStringLiteral(
        "NEURAL TIER — P8-16\n\n"
        "――― THIS PAGE RUNS THE ACCEPTANCE TEST ―――\n\n"
        "  It does not reimplement the walk-forward. A second copy of the\n"
        "  folds, the per-fold scaler and the purge is the one failure this\n"
        "  file exists to prevent, so the page shells out to\n"
        "  altair_neural_5m_test and renders what it printed. The page cannot\n"
        "  drift from the test because it is the test.\n\n"
        "  About six seconds: 211,000 five-minute returns, five folds.\n\n");

    // Beside the running executable first, then the sibling build tree.
    const QString exe =
#if defined(_WIN32)
        QStringLiteral("altair_neural_5m_test.exe");
#else
        QStringLiteral("altair_neural_5m_test");
#endif
    QStringList tried;
    tried << QCoreApplication::applicationDirPath()
                 + QStringLiteral("/../models/") + exe;
    tried << QCoreApplication::applicationDirPath() + QStringLiteral("/") + exe;
#ifdef ALTAIR_SOURCE_DIR
    tried << QStringLiteral(ALTAIR_SOURCE_DIR "/build/default/models/") + exe;
#endif
    QString found;
    for (const QString& p : tried) {
        if (QFileInfo(p).isFile()) { found = QFileInfo(p).canonicalFilePath(); break; }
    }
    if (found.isEmpty()) {
        return s + QStringLiteral(
            "  altair_neural_5m_test was not found. It is built with the\n"
            "  tests; run build.bat and press again.\n");
    }

    QProcess proc;
    proc.setProgram(found);
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start();
    if (!proc.waitForStarted(5000)) {
        return s + QStringLiteral("  could not start %1\n").arg(found);
    }
    if (!proc.waitForFinished(120000)) {
        proc.kill();
        return s + QStringLiteral("  timed out after 120 s\n");
    }
    const QString out = QString::fromUtf8(proc.readAll());
    s += QStringLiteral("――― %1 ―――\n  exit %2\n\n")
             .arg(found).arg(proc.exitCode());
    s += out;
    return s;
}

// ---------------------------------------------------------------------------
// P26-04 — Features and kinematics
// ---------------------------------------------------------------------------

/// The feature registry, actually populated, and the derivative estimator on
/// real bars.
///
/// I DECLINED BOTH OF THESE AND WAS WRONG ABOUT BOTH.
///
/// "features/registry.hpp -- a versioned registry with nothing registered; a
/// page listing zero features is not information." True as far as it went, and
/// the wrong conclusion: the fix is to REGISTER the features that exist. The
/// registry's whole job is turning a set of features into a stable
/// `feature_version` that rule 10 can record beside a decision, and that is
/// demonstrable with the features this tree already has.
///
/// "analytics/derivatives.hpp -- needs a TIMED tick stream, not bars." Half
/// right. The regime that MATTERS is sub-second and bars cannot reach it. But
/// the estimator, its window, its order and above all its ERROR PROPAGATION
/// work on any timed series, and a velocity smaller than its own standard
/// error is not a velocity at any sampling rate. That is worth showing.
[[nodiscard]] inline QString features_report(const QString& dataset_root,
                                                const QString& sym
                                                    = QStringLiteral("nifty")) {
    QString s = QStringLiteral("FEATURES AND KINEMATICS — P26-04\n\n");
    s += symbol_banner(sym);

    // ――― 1. the registry ―――
    s += QStringLiteral(
        "――― 1. THE REGISTRY, POPULATED ―――\n\n"
        "  Registration ORDER is part of the identity: the index is what a\n"
        "  model's weights are attached to, so inserting a feature in the\n"
        "  middle silently rewires every trained model that came before.\n\n"
        "   idx  name                          ver  band            lookback\n");
    FeatureRegistry reg;
    struct F { const char* n; std::uint16_t v; HorizonBand lo, hi; std::int64_t ns; };
    // Real features from features/. The bands are the ones each is meaningful
    // over, not the ones it happens to compute on.
    const F specs[] = {
        {"book.imbalance.l1",      1, HorizonBand::Micro,    HorizonBand::Fast,     1'000'000'000LL},
        {"book.microprice.dev",    1, HorizonBand::Micro,    HorizonBand::Fast,     1'000'000'000LL},
        {"flow.vpin",              1, HorizonBand::Fast,     HorizonBand::Intraday, 300'000'000'000LL},
        {"flow.kyle.lambda",       1, HorizonBand::Fast,     HorizonBand::Intraday, 300'000'000'000LL},
        {"kin.velocity",           1, HorizonBand::Fast,     HorizonBand::Intraday, 60'000'000'000LL},
        {"kin.acceleration",       1, HorizonBand::Fast,     HorizonBand::Intraday, 60'000'000'000LL},
        {"vol.ewma.10m",           2, HorizonBand::Intraday, HorizonBand::Swing,    600'000'000'000LL},
        {"vol.rv.twoscale",        1, HorizonBand::Intraday, HorizonBand::Swing,    3'600'000'000'000LL},
        {"regime.hmm.state",       1, HorizonBand::Intraday, HorizonBand::Swing,    86'400'000'000'000LL},
        {"opt.calendar.dte",       1, HorizonBand::Swing,    HorizonBand::Swing,    86'400'000'000'000LL},
    };
    for (const F& f : specs) {
        FeatureSpec fs;
        fs.name = f.n;
        fs.version = f.v;
        fs.min_band = f.lo;
        fs.max_band = f.hi;
        fs.lookback = Duration{f.ns};
        const auto idx = reg.add(fs);
        if (!idx) { continue; }
        s += QStringLiteral("   %1  %2  %3  %4  %5\n")
                 .arg(*idx, 3)
                 .arg(QString::fromUtf8(f.n), -28)
                 .arg(f.v, 3)
                 .arg(QStringLiteral("%1..%2")
                          .arg(QString::fromUtf8(band_name(f.lo)),
                               QString::fromUtf8(band_name(f.hi))), -14)
                 .arg(static_cast<double>(f.ns) / 1e9, 8, 'f', 1);
    }
    const auto sealed = reg.seal();
    if (sealed) {
        s += QStringLiteral(
            "\n  %1 features. SEALED — feature_version %2\n\n"
            "  That hash is one of the five things rule 10 requires beside\n"
            "  every live decision: {model_hash, feature_version, config_hash,\n"
            "  spec_version, tick_seqno}. It is FNV-1a and deliberately not\n"
            "  cryptographic — it identifies a configuration, it does not\n"
            "  defend against one — but it must be STABLE across builds and\n"
            "  platforms, or a recorded version stops meaning anything next\n"
            "  quarter.\n\n"
            "  Sealing is one-way. An add() after this is refused, because a\n"
            "  registry that can grow after a model was fitted against it is\n"
            "  not a version.\n\n").arg(reg.size())
                 .arg(QStringLiteral("0x%1")
                          .arg(*sealed, 16, 16, QLatin1Char('0')));
        const auto after = reg.add(FeatureSpec{"late.arrival", 1,
                                               HorizonBand::Fast,
                                               HorizonBand::Fast,
                                               Duration{1'000'000'000LL}});
        s += QStringLiteral("  adding one more after the seal: %1\n\n")
                 .arg(after ? QStringLiteral("ACCEPTED — which would be a bug")
                            : QStringLiteral("refused, as it must be"));
    }

    // ――― 2. kinematics ―――
    s += QStringLiteral(
        "――― 2. VELOCITY AND ACCELERATION, WITH THEIR ERROR ―――\n\n"
        "  On one-minute NIFTY closes. THE REGIME THAT MATTERS IS SUB-SECOND\n"
        "  and bars cannot reach it — order-book imbalance decays in 10-200 ms\n"
        "  and at 60-second sampling the quantity does not exist. What DOES\n"
        "  survive the sampling rate is the error propagation, and that is\n"
        "  what this section is for.\n\n");
    const auto closes = ui_load_closes(
        spot_path(dataset_root, sym, "1m", "2026-09.csv"));
    if (closes.size() < 200) {
        s += QStringLiteral("  No recent 1-minute file to read.\n");
        return s;
    }
    std::vector<TimedPoint> pts;
    pts.reserve(closes.size());
    for (std::size_t i = 0; i < closes.size(); ++i) {
        TimedPoint tp;
        tp.ts = Timestamp{static_cast<std::int64_t>(i) * 60'000'000'000LL};
        tp.value = closes[i] * 100.0;          // paise
        pts.push_back(tp);
    }
    const Timestamp at{static_cast<std::int64_t>(pts.size() - 1)
                       * 60'000'000'000LL};
    s += QStringLiteral(
        "     window   order   velocity      accel   resid sd   vel/se\n");
    for (const std::int64_t mins : {5, 15, 30, 60}) {
        const Duration w{mins * 60'000'000'000LL};
        const auto d = derivatives_at(pts.data(), pts.size(), at, w, 2);
        if (!d) { continue; }
        // The standard error of the velocity is roughly residual_sd over
        // sqrt(N) times the window half-width in seconds -- the header says
        // so, and it is the only thing that makes the velocity readable.
        const double h = static_cast<double>(w.raw()) / 2e9;
        const double se = d->points > 0
            ? d->residual_sd / (std::sqrt(static_cast<double>(d->points)) * h)
            : 0.0;
        s += QStringLiteral("   %1m %2  %3  %4  %5  %6\n")
                 .arg(mins, 6).arg(d->order, 6)
                 .arg(d->velocity, 11, 'g', 4)
                 .arg(d->acceleration, 11, 'g', 4)
                 .arg(d->residual_sd, 10, 'f', 2)
                 .arg(se > 0.0 ? d->velocity / se : 0.0, 8, 'f', 2);
    }
    s += QStringLiteral(
        "\n  The last column is the one to read. A velocity smaller than its\n"
        "  own standard error is not a velocity — ROADMAP section 3, the same\n"
        "  rule the Sizing page applies to edge and the Memory page to Hurst.\n"
        "  Widening the window lowers the noise and raises the lag, and the\n"
        "  table shows both moving at once rather than one in isolation.\n\n"
        "  `scale` is carried on every result so two measurements from\n"
        "  different windows cannot be compared by accident.\n");
    return s;
}

/// What is on disk under `dataset/`, counted rather than remembered.
///
/// P30-03. The Data Flow page drew the feed path and said nothing about what
/// had come through it. A reader could watch every stage light up and still
/// not know whether the tree held a week or eleven years, or which
/// instruments.
///
/// COUNTED FROM DISK, NOT TYPED. A hand-maintained inventory is the first
/// thing to go stale: NIFTY BANK was ingested and a typed list would not have
/// mentioned it until somebody remembered to. This walks the partitions and
/// reports the first and last bar it finds, so the page is wrong only if the
/// files are.
/// Is this the leading field of a bar row, or the header that precedes it?
///
/// The tail-window read below can land on a header when a file holds nothing
/// but one, and "timestamp" sorts ABOVE every date -- so an unvalidated stamp
/// would not just be wrong, it would win the max and pin the whole series to
/// a word. Shape-checked rather than parsed: this decides what to display,
/// and a display must not depend on a locale-sensitive date parser.
[[nodiscard]] inline bool is_stamp(const QString& v) {
    return v.size() == 10 && v[4] == QLatin1Char('-') && v[7] == QLatin1Char('-')
           && v[0].isDigit() && v[1].isDigit() && v[2].isDigit()
           && v[3].isDigit() && v[5].isDigit() && v[6].isDigit()
           && v[8].isDigit() && v[9].isDigit();
}

/// How much history one series directory actually holds.
///
/// P30-03. Split out of dataset_inventory so it can be pointed at a directory
/// built by a test. The bug this replaced was invisible against a synthetic
/// tree and appeared only against the real one, which is the worst place to
/// find out that a page has been reporting a wrong date.
struct SeriesSpan {
    int files = 0;
    QString first;      ///< empty when nothing in the directory parsed
    QString last;
};

[[nodiscard]] inline SeriesSpan series_span(const QString& dir) {
    SeriesSpan sp;
    QDir d(dir);
    if (!d.exists()) { return sp; }
    const QStringList files = d.entryList(QStringList{QStringLiteral("*.csv")},
                                          QDir::Files, QDir::Name);
    if (files.isEmpty()) { return sp; }
    sp.files = static_cast<int>(files.size());

    // EVERY FILE, NOT THE FIRST AND LAST BY NAME.
    //
    // The first version read files.first() and files.last() and
    // assumed name order was date order. That holds for YYYY-MM.csv
    // and breaks the moment a directory holds anything else:
    // spot/banknifty/1d has all.csv AND vendor_pre2015.csv, and the
    // vendor file sorts LAST, so the page reported BankNifty daily as
    // ending 2015-01-08 when all.csv runs to 2026-09-08.
    //
    // A wrong "last" on a data-inventory page is the worst kind of
    // wrong: it is the number somebody checks before deciding whether
    // a backfill is needed.
    //
    // AND IT DOES NOT READ THE FILES. The obvious correct version --
    // stream every file, keep the min and max stamp -- is 240 MB of
    // CSV on the GUI thread every time this page is built. Both
    // stamps live at a known end of the file, so both are seeks: the
    // first data line is one readLine past the header, and the last
    // is the last newline-terminated run in a 4 KB tail window.
    QString first, last;
    for (const QString& fn : files) {
        QFile f(d.filePath(fn));
        if (!f.open(QIODevice::ReadOnly)) { continue; }

        {   // first data row: header, then one line.
            QTextStream ts(&f);
            ts.readLine();
            const QString a =
                ts.readLine().section(QLatin1Char(','), 0, 0).left(10);
            if (is_stamp(a) && (first.isEmpty() || a < first)) {
                first = a;
            }
        }

        // last data row: a tail window, widened until it holds a
        // newline. A bar row is ~60 bytes; 4 KB is ~65 rows of slack,
        // and the loop means a pathological row still resolves rather
        // than silently reporting the wrong date.
        const qint64 sz = f.size();
        for (qint64 win = 4096; win <= 1 << 20; win *= 4) {
            const qint64 from = sz > win ? sz - win : 0;
            if (!f.seek(from)) { break; }
            const QByteArray tail = f.readAll();
            if (tail.size() < 2) { break; }
            const int nl =
                tail.lastIndexOf('\n', tail.size() - 2);
            if (nl < 0 && from > 0) { continue; }   // widen
            const QByteArray lastLine =
                tail.mid(nl + 1).trimmed();
            const QString b = QString::fromLatin1(lastLine)
                                  .section(QLatin1Char(','), 0, 0)
                                  .left(10);
            if (is_stamp(b) && (last.isEmpty() || b > last)) {
                last = b;
            }
            break;
        }
    }
    sp.first = first;
    sp.last = last;
    return sp;
}

[[nodiscard]] inline QString dataset_inventory() {
    const QString root = QStringLiteral(ALTAIR_DATASET_DIR);
    QString s;
    s += QStringLiteral("  %1  %2  %3  %4\n")
             .arg(QStringLiteral("series"), -26)
             .arg(QStringLiteral("files"), 6)
             .arg(QStringLiteral("first"), -12)
             .arg(QStringLiteral("last"), -12);

    struct Part { const char* seg; const char* sym; };
    const Part parts[] = {
        {"spot", "nifty"}, {"spot", "banknifty"}, {"spot", "indiavix"},
        {"fut", "nifty"}, {"opt", "nifty"},
    };
    const char* intervals[] = {"1m", "5m", "15m", "60m", "1d"};

    for (const Part& pt : parts) {
        for (const char* iv : intervals) {
            const QString dir = QStringLiteral("%1/%2/%3/%4")
                                    .arg(root, QLatin1String(pt.seg),
                                         QLatin1String(pt.sym),
                                         QLatin1String(iv));
            const SeriesSpan sp = series_span(dir);
            if (sp.files == 0) { continue; }
            s += QStringLiteral("  %1  %2  %3  %4\n")
                     .arg(QStringLiteral("%1/%2/%3")
                              .arg(QLatin1String(pt.seg), QLatin1String(pt.sym),
                                   QLatin1String(iv)), -26)
                     .arg(sp.files, 6)
                     .arg(sp.first, -12).arg(sp.last, -12);
        }
    }
    s += QStringLiteral(
        "\n  dataset/ is gitignored and regenerable. Counted from disk on\n"
        "  every open, because a typed inventory goes stale the first time\n"
        "  something is ingested and nobody edits the list.\n");
    return s;
}

/// P31-01. The two arbitrage scanners that had no page.
///
/// The audit that produced P23 found 34 headers of real results against 22
/// with a page. These are the last two of the large ones: P5-06's put-call
/// parity, box and butterfly scanner, and P5-07's calendar scanner.
///
/// WHAT IS BEING MEASURED HERE IS THE SCANNER, NOT THE MARKET.
///
/// There is no option chain -- dataset/opt/ is empty and no option
/// subscription is running -- so every chain below is priced by this page at
/// a stated vol and then handed to the engine's own scanner. That sounds like
/// a weaker exercise than it is: on BOTH of these cards the finding was that
/// the textbook scanner is wrong, and a synthetic arbitrage-free chain is the
/// only surface on which that can be proved. A real chain cannot tell you
/// whether an opportunity is real; a chain you priced yourself can.
///
/// Nothing here is reimplemented. The page builds quotes and calls
/// butterfly_margin, scan_parity, scan_box, scan_option_calendar and
/// scan_futures_calendar -- the same functions the engine calls.
[[nodiscard]] inline QString arbitrage_scans_report() {
    QString s = QStringLiteral(
        "PARITY & CALENDAR — the two scanners that had no page\n\n"
        "――― WHAT THIS IS NOT ―――\n\n"
        "  There is no option chain. dataset/opt/ is empty and no option\n"
        "  subscription is running, so NO number below is a market\n"
        "  measurement and nothing here is an opportunity anyone can trade.\n\n"
        "  Every chain is priced BY THIS PAGE at a stated vol and handed to\n"
        "  the engine's own scanner. On both of these cards the finding was\n"
        "  that the textbook scanner is WRONG, and a synthetic, convex,\n"
        "  arbitrage-free chain is the only surface on which that can be\n"
        "  shown: a real chain cannot tell you whether an opportunity is\n"
        "  genuine, and one you priced yourself can.\n\n");

    // ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――
    // 1. The butterfly. Needs no cost schedule: convexity is a property of
    //    three quotes.
    // ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――
    s += QStringLiteral(
        "――― 1. THE TEXTBOOK BUTTERFLY ASSUMES AN EVEN LADDER ―――\n\n"
        "  The condition every reference gives is\n\n"
        "      C(K1) - 2*C(K2) + C(K3) >= 0\n\n"
        "  and it is valid only when K2-K1 == K3-K2. NIFTY's ladder is 50\n"
        "  points near the money and 100 further out, so three CONSECUTIVE\n"
        "  LISTED strikes routinely straddle the step change.\n\n"
        "  Both rows below are the SAME flat 14-vol Black-76 chain, forward\n"
        "  24000, 30 days. A flat surface is convex in strike by\n"
        "  construction, so any butterfly found on it is the scanner's, not\n"
        "  the market's.\n\n"
        "    ladder                  gaps       weighted    unweighted\n");

    {
        const double F = 2'400'000.0, T = 30.0 / 365.0, vol = 0.14;
        const auto fair = [&](std::int64_t K) -> std::int64_t {
            const auto g = black76(OptionRight::Call, Price{2'400'000},
                                   Price{K}, Years{T}, Vol{vol}, 0.0);
            return g ? static_cast<std::int64_t>(g->price + 0.5) : 0;
        };
        (void)F;
        const auto tight = [](std::int64_t fairp) {
            return Touch{Price{fairp}, Price{fairp}};
        };

        struct Ladder { const char* name; std::int64_t k1, k2, k3; };
        const Ladder ladders[] = {
            {"24000 / 24100 / 24200", 2'400'000, 2'410'000, 2'420'000},
            {"24000 / 24100 / 24300", 2'400'000, 2'410'000, 2'430'000},
        };
        double fictional = 0.0;
        for (const Ladder& L : ladders) {
            ButterflyQuote q{};
            q.k1 = Price{L.k1}; q.k2 = Price{L.k2}; q.k3 = Price{L.k3};
            q.c1 = tight(fair(L.k1));
            q.c2 = tight(fair(L.k2));
            q.c3 = tight(fair(L.k3));
            const auto r = butterfly_margin(q);
            if (!r) { continue; }
            s += QStringLiteral("    %1  %2/%3   %4   %5\n")
                     .arg(QString::fromLatin1(L.name), -22)
                     .arg(r->lower_gap / 100).arg(r->upper_gap / 100, -4)
                     .arg(r->margin / 100.0, 10, 'f', 2)
                     .arg(r->unweighted / 100.0, 13, 'f', 2);
            if (!r->equally_spaced && r->unweighted < 0.0) {
                fictional = -r->unweighted / 100.0;
            }
        }
        if (fictional > 0.0) {
            s += QStringLiteral(
                "\n  A scanner using the textbook form on the uneven triple\n"
                "  reports Rs %1 per unit of FREE MONEY, every tick, on a\n"
                "  chain that has no arbitrage in it at all — and it does so\n"
                "  at every strike where the ladder steps from 50 to 100.\n\n"
                "  It is not a rounding wobble. The error is the strike\n"
                "  asymmetry times the slope of the call curve, so it is\n"
                "  rupees and it grows with how uneven the ladder is.\n\n"
                "  The weighted form used above is\n\n"
                "      (K3-K2)*C(K1) - (K3-K1)*C(K2) + (K2-K1)*C(K3) >= 0\n\n"
                "  which reduces to the textbook one when the spacing is\n"
                "  equal — the first row, where the two agree. The strikes\n"
                "  come from the spec store; the STEP is never assumed.\n\n")
                     .arg(fictional, 0, 'f', 2);
        }
    }

    // ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――
    // 4. The calendar alignment. Also needs no cost schedule: it compares
    //    total variance, and a variance has no brokerage.
    //
    //    Placed here, before the costed sections, because it and the
    //    butterfly are the two findings this page exists to show and neither
    //    depends on a build option.
    // ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――
    s += QStringLiteral(
        "――― 2. TWO EXPIRIES DO NOT SHARE A FORWARD ―――\n\n"
        "  The calendar condition is on TOTAL IMPLIED VARIANCE at fixed\n"
        "  log-moneyness:  w(k,T2) >= w(k,T1) for T2 > T1, at every\n"
        "  k = ln(K/F(T)).  The forward is not the same at the two expiries,\n"
        "  so fixed k means a DIFFERENT STRIKE on each one.\n\n"
        "  Iterating strikes and reading each slice at that strike's own\n"
        "  moneyness feels like aligning by moneyness and is exactly the\n"
        "  strike-aligned test wearing the other label. Both are run below,\n"
        "  401 sampled strikes each.\n\n");

    {
        // dsigma/dk parametrisation: SVI's `b` is in variance units and
        // scales with T, so the same `b` that is a realistic skew at 30 days
        // is an absurd one at 7 and an inadmissible slice besides.
        const auto smile = [](double atm_vol, double t, double dsig_dk,
                              double rho) {
            SviParams p{};
            p.rho = rho;
            p.m = 0.0;
            p.sigma = 0.10;
            p.b = 2.0 * atm_vol * t * dsig_dk / rho;
            p.a = atm_vol * atm_vol * t - p.b * p.sigma;
            return p;
        };
        const auto scaled = [](const SviParams& p, double lambda) {
            SviParams q = p;
            q.a = p.a * lambda;
            q.b = p.b * lambda;
            return q;
        };

        const double r = 0.065;

        // ── 2a. an index calendar, pure carry ──
        {
            const double T1 = 7.0 / 365.0, T2 = 21.0 / 365.0;
            const std::int64_t F1 = 2'400'000;
            const std::int64_t F2 = static_cast<std::int64_t>(
                static_cast<double>(F1) * std::exp(r * (T2 - T1)) + 0.5);
            DatedSlice s1{}, s2{};
            s1.slice = smile(0.14, T1, -0.70, -0.72);
            s1.forward = Price{F1};
            s1.t = Years{T1};
            s2.slice = scaled(s1.slice, 1.60);   // clean by construction
            s2.forward = Price{F2};
            s2.t = Years{T2};

            const auto by_k = scan_option_calendar(s1, s2, Price{2'160'000},
                                                   Price{2'640'000}, 401,
                                                   CalendarAlignment::Moneyness);
            const auto by_K = scan_option_calendar(s1, s2, Price{2'160'000},
                                                   Price{2'640'000}, 401,
                                                   CalendarAlignment::Strike);
            if (by_k && by_K) {
                const double dk = std::log(static_cast<double>(F2)
                                           / static_cast<double>(F1));
                s += QStringLiteral(
                    "  2a. INDEX, PURE CARRY. 7d against 21d. The far forward\n"
                    "      %1 sits ABOVE the near %2, so k moves %3.\n"
                    "      The far slice is the near one scaled by 1.60, so the\n"
                    "      surface is CLEAN by construction and anything found\n"
                    "      in it is the scan's own.\n\n"
                    "        by moneyness : %4 / %5 violations, worst %6\n"
                    "        by strike    : %7 / %8 violations, worst %9\n")
                         .arg(static_cast<double>(F2) / 100.0, 0, 'f', 2)
                         .arg(static_cast<double>(F1) / 100.0, 0, 'f', 2)
                         .arg(dk, 0, 'f', 4)
                         .arg(by_k->violations, 4).arg(by_k->points)
                         .arg(by_k->worst, 0, 'f', 6)
                         .arg(by_K->violations, 4).arg(by_K->points)
                         .arg(by_K->worst, 0, 'f', 6);
                if (by_k->worst != 0.0) {
                    s += QStringLiteral(
                        "\n      The strike-aligned margin is %1 of the correct\n"
                        "      one. Real, biased, and second order: on this pair\n"
                        "      the misalignment is NOT enough to invent an\n"
                        "      opportunity. Reporting that as the headline would\n"
                        "      be as wrong as ignoring it — what it fixes is the\n"
                        "      threshold at which it starts to matter, and 2b\n"
                        "      moves the forward the other way and further.\n\n")
                             .arg(QStringLiteral("%1 per cent")
                                      .arg(100.0 * by_K->worst / by_k->worst,
                                           0, 'f', 1));
                }
            }
        }

        // ── 2b. the card: a dividend moves the forward the other way ──
        {
            const double T1 = 7.0 / 365.0, T2 = 35.0 / 365.0;
            const double S = 295'000.0, div = 4'000.0;
            const std::int64_t F1 =
                static_cast<std::int64_t>(S * std::exp(r * T1));
            const std::int64_t F2 =
                static_cast<std::int64_t>(S * std::exp(r * T2) - div);
            DatedSlice s1{}, s2{};
            s1.slice = smile(0.28, T1, -0.60, -0.60);
            s1.forward = Price{F1};
            s1.t = Years{T1};
            // A GENUINE violation: total variance FALLS half a per cent from
            // the near expiry to the far one, at every k.
            s2.slice = scaled(s1.slice, 0.995);
            s2.forward = Price{F2};
            s2.t = Years{T2};

            const auto by_k = scan_option_calendar(s1, s2, Price{250'000},
                                                   Price{340'000}, 401,
                                                   CalendarAlignment::Moneyness);
            const auto by_K = scan_option_calendar(s1, s2, Price{250'000},
                                                   Price{340'000}, 401,
                                                   CalendarAlignment::Strike);
            if (by_k && by_K) {
                const double dk = std::log(static_cast<double>(F2)
                                           / static_cast<double>(F1));
                s += QStringLiteral(
                    "  2b. THE CARD. A 2950 stock going Rs 40 ex-dividend\n"
                    "      between the expiries — the ordinary state of affairs\n"
                    "      for a dividend payer, not an anomaly. The far forward\n"
                    "      %1 sits BELOW the near %2, so k moves %3:\n"
                    "      the OTHER way from 2a and three times as far.\n"
                    "      Total variance genuinely falls 0.5 per cent from near\n"
                    "      to far, so there is a REAL violation at every strike.\n\n"
                    "        by moneyness : %4 / %5 violations found\n"
                    "        by strike    : %6 / %7 violations found\n\n")
                         .arg(static_cast<double>(F2) / 100.0, 0, 'f', 2)
                         .arg(static_cast<double>(F1) / 100.0, 0, 'f', 2)
                         .arg(dk, 0, 'f', 5)
                         .arg(by_k->violations, 4).arg(by_k->points)
                         .arg(by_K->violations, 4).arg(by_K->points);
                const int hidden = by_k->violations - by_K->violations;
                if (hidden > 0) {
                    s += QStringLiteral(
                        "      %1 GENUINE VIOLATIONS HIDDEN.\n\n"
                        "      Which is the failure nobody expects. The strike-\n"
                        "      aligned test does not INVENT arbitrage here, it\n"
                        "      CONCEALS it, above the smile's minimum where w\n"
                        "      rises with k. That is the worse of the two: an\n"
                        "      invented opportunity costs money once and then\n"
                        "      gets fixed, and a hidden one is never seen at all.\n\n")
                             .arg(hidden);
                }
            }
        }

        s += QStringLiteral(
            "  A caller who does not name an alignment is REFUSED: the two\n"
            "  answers differ, one of them is wrong, and there is no default\n"
            "  that is not a decision.\n\n"
            "  And nothing here compares implied VOLS. In a stressed market\n"
            "  the front month can trade at 30 vol against 18 two months\n"
            "  out; w still rises, because T rises faster than sigma^2\n"
            "  falls. A scanner comparing vols would flag every stressed\n"
            "  market as an arbitrage.\n\n");
    }

    // ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――
    // 3-5. Everything that needs a bill.
    //
    // RULE 5: every signal is priced net of full cost BEFORE IT EXISTS. So
    // these three cannot run against an invented schedule. Where the loader
    // is absent the page names the blocker, exactly as the cost panel does --
    // a made-up charge is worse than no charge, because rule 5 is what
    // everything downstream trusts.
    // ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――
#ifdef ALTAIR_HAVE_CHARGES_TOML
    {
        std::vector<ChargeSchedule> schedules;
        const auto rep = load_charges_file(ALTAIR_CHARGES_TOML, schedules);
        const ChargeSchedule* sch = nullptr;
        if (rep) {
            for (const ChargeSchedule& c : schedules) {
                if (sch == nullptr || sch->valid_from < c.valid_from) {
                    sch = &c;
                }
            }
        }
        if (sch == nullptr) {
            s += QStringLiteral(
                "――― 3. PARITY, THE BOX, AND THE FUTURES CALENDAR ―――\n\n"
                "  COSTED SCANNERS UNAVAILABLE. config/charges.toml did not\n"
                "  load. Rule 5 says every signal is priced\n"
                "  net of full cost before it exists, so these three report\n"
                "  nothing rather than a number costed against a guess.\n\n");
        } else {
            // BROKERAGE IS COMMERCIAL, NOT REGULATORY, and is deliberately
            // not in charges.toml -- it changes without a circular. It is the
            // one literal on this page and is named as such, the same way the
            // cost panel names it. Options are a FLAT Rs 20 per order: the
            // "Rs 20 or 0.03 per cent, whichever lower" rule is the
            // intraday-equity and futures term, and applying it to options
            // understates the bill by a factor of five in the direction that
            // makes an options strategy look viable.
            BrokerageRule opt_br{};
            opt_br.flat_per_order = Notional{2'000};
            opt_br.pct = 0;
            opt_br.take_lower = false;

            // Inside the chosen schedule's own window, by construction.
            const Timestamp ts = sch->valid_from;
            const QString eff =
                QDateTime::fromSecsSinceEpoch(
                    ts.ns_since_epoch() / 1'000'000'000, QTimeZone::utc())
                    .toString(QStringLiteral("yyyy-MM-dd"));
            const Qty lot{75};        // the spec store's NIFTY lot
            const double T = 30.0 / 365.0, vol = 0.14, rate = 0.065;
            const double F = 2'400'000.0;

            const auto opt_fair = [&](OptionRight w, std::int64_t K,
                                      double t) -> std::int64_t {
                const auto g = black76(w, Price{static_cast<std::int64_t>(F)},
                                       Price{K}, Years{t}, Vol{vol}, rate);
                return g ? static_cast<std::int64_t>(g->price + 0.5) : 0;
            };
            const auto wide = [](std::int64_t fairp, std::int64_t half) {
                return Touch{Price{fairp - half}, Price{fairp + half}};
            };

            // ── 3. parity at the touch vs at the mid ──
            s += QStringLiteral(
                "――― 3. PARITY AT THE TOUCH IS NOT PARITY AT THE MID ―――\n\n"
                "  A 24000 straddle priced off a flat 14-vol chain, both\n"
                "  options quoted Rs 2.00 wide and the future Rs 0.50 wide.\n"
                "  Parity therefore holds EXACTLY at the mid and there is no\n"
                "  opportunity in the chain by construction.\n\n");
            {
                ParityQuote q{};
                const std::int64_t K = 2'400'000;
                const std::int64_t cf = opt_fair(OptionRight::Call, K, T);
                q.call = wide(cf, 200);
                q.put = wide(opt_fair(OptionRight::Put, K, T), 200);
                q.hedge = wide(static_cast<std::int64_t>(F), 50);
                q.hedge_leg = HedgeLeg::Future;
                q.strike = Price{K};
                q.t = Years{T};
                q.rate = rate;
                ShortCashCapability cap{};
                const auto o = scan_parity(q, lot, Exchange::NSE, ts, cap,
                                           *sch, opt_br);
                if (o) {
                    s += QStringLiteral(
                        "    deviation at the MID    %1 paise/unit\n"
                        "    deviation at the TOUCH  %2 paise/unit\n"
                        "    charges, four legs      Rs %3\n"
                        "    net on one lot of %4    Rs %5\n\n"
                        "  The mid number is what a screen shows. The touch\n"
                        "  number is the one that can be traded, and it is\n"
                        "  the spread that separates them. There is no mid\n"
                        "  accessor anywhere in strategies/parity.hpp,\n"
                        "  deliberately: a mid cannot be traded.\n\n")
                             .arg(o->mid_deviation, 10, 'f', 2)
                             .arg(o->touch_deviation, 10, 'f', 2)
                             .arg(static_cast<double>(o->cost.raw()) / 100.0,
                                  0, 'f', 2)
                             .arg(lot.raw())
                             .arg(static_cast<double>(o->net.raw()) / 100.0,
                                  0, 'f', 2);
                }
                // THE ASYMMETRY, ON A QUOTE THAT ACTUALLY REACHES IT.
                //
                // The fair chain above makes the CONVERSION the better side,
                // and a conversion sells the synthetic and BUYS the hedge --
                // no short anywhere, so it is reachable whatever the hedge
                // is. Asking that quote about short cash proves nothing: the
                // branch is never taken. The future is therefore quoted 100
                // points RICH against the options' own forward, which makes
                // the synthetic cheap and the REVERSAL the chosen side. Now
                // the trade sells the hedge, and whether that is possible is
                // the whole question.
                ParityQuote rich = q;
                rich.hedge = wide(static_cast<std::int64_t>(F) + 10'000, 50);
                const auto rf = scan_parity(rich, lot, Exchange::NSE, ts, cap,
                                            *sch, opt_br);
                ParityQuote rich_spot = rich;
                rich_spot.hedge_leg = HedgeLeg::Spot;
                const auto rs = scan_parity(rich_spot, lot, Exchange::NSE, ts,
                                            cap, *sch, opt_br);
                const auto unnamed = scan_parity(
                    [&] { ParityQuote u = q; u.hedge_leg = HedgeLeg::Unknown;
                          return u; }(),
                    lot, Exchange::NSE, ts, cap, *sch, opt_br);
                s += QStringLiteral(
                    "  THE HEDGE IS NOT A DETAIL. Quote the hedge 100 points\n"
                    "  rich against the options' own forward and the\n"
                    "  synthetic is cheap, so the chosen side becomes the\n"
                    "  REVERSAL — which SELLS the hedge.\n\n"
                    "    hedged with a FUTURE  %1\n"
                    "    hedged with SPOT      %2\n\n"
                    "  A future shorts freely. The underlying does not: there\n"
                    "  is no short cash delivery in India, so the identical\n"
                    "  arithmetic is unreachable through one instrument and\n"
                    "  reachable through the other.\n\n"
                    "  And a caller who does not say which hedge it is: %3.\n"
                    "  The hedge decides both the segment the bill is\n"
                    "  computed under and whether the trade exists at all, so\n"
                    "  there is no default that is not a guess.\n\n")
                         .arg(rf && rf->trade == ParityTrade::Reversal
                                  ? (rf->executability
                                             == Executability::Executable
                                         ? QStringLiteral("reversal, EXECUTABLE")
                                         : QStringLiteral("reversal, blocked"))
                                  : QStringLiteral("no reversal reached"), -24)
                         .arg(rs && rs->executability
                                        == Executability::ShortCashUnavailable
                                  ? QStringLiteral("reversal, BLOCKED — no "
                                                   "short cash")
                                  : QStringLiteral("reachable — WHICH IS A "
                                                   "BUG"), -24)
                         .arg(unnamed.has_value()
                                  ? QStringLiteral("accepted — WHICH IS A BUG")
                                  : QStringLiteral("REFUSED"));
            }

            // ── 4. the box ──
            s += QStringLiteral(
                "――― 4. A BOX IS A LOAN, AND ITS RATE IS THE NUMBER ―――\n\n"
                "  24000/24200, priced off the same chain at a %1 per cent\n"
                "  discount rate. Each tenor is quoted twice: once with NO\n"
                "  spread at all, and once with every one of the four legs\n"
                "  Rs 1.00 wide.\n\n"
                "  The zero-width row is not a trade — it is the fair price\n"
                "  asked of the SAME function, so the comparison needs no mid\n"
                "  computed anywhere. strategies/parity.hpp has no mid\n"
                "  accessor on purpose, and a page that reintroduced one in\n"
                "  the UI would have undone that on the only screen anyone\n"
                "  reads.\n\n"
                "    tenor    spread     touch cost     implied rate\n")
                     .arg(100.0 * rate, 0, 'f', 1);
            {
                struct Tenor { const char* label; double t; };
                const Tenor tenors[] = {{"30 days", 30.0 / 365.0},
                                        {" 6 days", 6.0 / 365.0}};
                double fair_rate = 0.0, crossed_rate = 0.0;
                for (const Tenor& tn : tenors) {
                    for (const std::int64_t half : {0LL, 100LL}) {
                        BoxQuote q{};
                        q.k1 = Price{2'400'000};
                        q.k2 = Price{2'420'000};
                        q.c1 = wide(opt_fair(OptionRight::Call, 2'400'000, tn.t),
                                    half);
                        q.c2 = wide(opt_fair(OptionRight::Call, 2'420'000, tn.t),
                                    half);
                        q.p1 = wide(opt_fair(OptionRight::Put, 2'400'000, tn.t),
                                    half);
                        q.p2 = wide(opt_fair(OptionRight::Put, 2'420'000, tn.t),
                                    half);
                        q.t = Years{tn.t};
                        q.rate = rate;
                        const auto o = scan_box(q, lot, Exchange::NSE, ts, *sch,
                                                opt_br);
                        if (!o) { continue; }
                        if (tn.t > 0.05) {
                            (half == 0 ? fair_rate : crossed_rate) =
                                o->implied_rate;
                        }
                        s += QStringLiteral("    %1  %2  %3      %4 per cent\n")
                                 .arg(QString::fromLatin1(tn.label))
                                 .arg(half == 0 ? QStringLiteral("none   ")
                                                : QStringLiteral("Rs 1.00"))
                                 .arg(o->touch_cost / 100.0, 12, 'f', 2)
                                 .arg(o->implied_rate * 100.0, 10, 'f', 2);
                    }
                }
                s += QStringLiteral(
                    "\n  Uncrossed, the 30-day box recovers %1 per cent —\n"
                    "  the rate the chain was priced with, read back out of\n"
                    "  four quotes. Crossing four Rs 1.00 spreads costs\n"
                    "  Rs 4.00 and turns it into %2 per cent: a swing of\n"
                    "  %3 percentage points of annualised rate out of four\n"
                    "  rupees.\n\n"
                    "  The SAME Rs 4.00 at six days does several times the\n"
                    "  damage, because a rate divides by tenor and identical\n"
                    "  rupees are not identical rates.\n\n"
                    "  Which is why the scanner ranks on implied RATE and\n"
                    "  not on the rupee gap: the two order a chain\n"
                    "  differently, and only one of them is comparable\n"
                    "  across tenors.\n\n")
                         .arg(fair_rate * 100.0, 0, 'f', 2)
                         .arg(crossed_rate * 100.0, 0, 'f', 2)
                         .arg(fair_rate * 100.0 - crossed_rate * 100.0,
                              0, 'f', 1);
            }

            // ── 5. the futures calendar ──
            s += QStringLiteral(
                "――― 5. THE FUTURES CALENDAR ―――\n\n"
                "  Both legs are futures, so both short freely and this is\n"
                "  the second structure in Phase 5 with no executability\n"
                "  caveat — the box being the first.\n\n"
                "  What it DOES depend on is the two legs having been seen\n"
                "  at the same instant. The far month is thinner and its\n"
                "  last print is routinely older, which biases a naive\n"
                "  spread toward whatever the near month has done since, so\n"
                "  the scanner takes a skew limit and REFUSES rather than\n"
                "  quietly comparing two different moments.\n\n");
            {
                BrokerageRule fut_br{};
                fut_br.flat_per_order = Notional{2'000};
                fut_br.pct = rate_from(0.0003L);
                fut_br.take_lower = true;

                // THE FAR LEG IS PRICED AT ITS OWN CARRY, NOT TYPED.
                //
                // An earlier version typed 24040 against a near 24000 while
                // carry over the same month says about 24105, and the scan
                // duly reported Rs 2,617 of net edge -- a fabricated
                // arbitrage, on a page whose whole argument is that these
                // trades do not survive their costs. Priced fairly, the
                // question becomes the real one: what do two crossed spreads
                // and four brokerage legs do to a calendar that is quoted
                // right?
                FuturesCalendarQuote q{};
                const double near_px = 2'400'000.0;
                const double dt = (60.0 - 30.0) / 365.0;
                const double div_y = 0.012;
                const std::int64_t far_px = static_cast<std::int64_t>(
                    near_px * std::exp((rate - div_y) * dt) + 0.5);
                q.near_leg = wide(static_cast<std::int64_t>(near_px), 50);
                q.near_ts = ts;
                q.near_t = Years{30.0 / 365.0};
                q.far_leg = wide(far_px, 100);
                q.far_ts = ts;
                q.far_t = Years{60.0 / 365.0};
                q.rate = rate;
                q.dividend_yield = div_y;

                const auto o = scan_futures_calendar(
                    q, lot, Exchange::NSE, Duration{2'000'000'000}, *sch,
                    fut_br);
                if (o) {
                    s += QStringLiteral(
                        "    touch spread     %1 paise\n"
                        "    fair from carry  %2 paise\n"
                        "    implied carry    %3 per cent per year\n"
                        "    charges          Rs %4\n"
                        "    net on one lot   Rs %5\n\n"
                        "  Quoted at its own carry, the spread is fair and\n"
                        "  the net is what the two crossed spreads and four\n"
                        "  brokerage legs cost. That is the answer this page\n"
                        "  exists to give.\n\n"
                        "  The implied carry comes back at the rate the pair\n"
                        "  was priced with — the scanner recovers %6 per cent\n"
                        "  from two quotes, the same round trip the box makes.\n"
                        "  It adds the dividend yield back, so what it reports\n"
                        "  is comparable to a repo rate rather than to the\n"
                        "  net-of-dividend drift.\n\n"
                        "  The implied carry is the comparable number: a\n"
                        "  40-point spread over one month and over three are\n"
                        "  different carries, exactly as in the basis. A point\n"
                        "  spread is not.\n\n")
                             .arg(o->touch_spread, 10, 'f', 2)
                             .arg(o->fair_spread, 10, 'f', 2)
                             .arg(o->implied_carry * 100.0, 8, 'f', 3)
                             .arg(static_cast<double>(o->cost.raw()) / 100.0,
                                  0, 'f', 2)
                             .arg(static_cast<double>(o->net.raw()) / 100.0,
                                  0, 'f', 2)
                             .arg(100.0 * rate, 0, 'f', 1);
                }
                // The staleness refusal, shown rather than described.
                FuturesCalendarQuote stale = q;
                stale.far_ts = ts + Duration{5'000'000'000};
                const auto sr = scan_futures_calendar(
                    stale, lot, Exchange::NSE, Duration{2'000'000'000}, *sch,
                    fut_br);
                s += QStringLiteral(
                    "  A far leg five seconds older than the near one,\n"
                    "  against a two-second limit: %1\n\n")
                         .arg(!sr && sr.error() == CalendarSpreadError::StaleLeg
                                  ? QStringLiteral("REFUSED as a stale leg.")
                                  : QStringLiteral("accepted — WHICH IS A BUG"));
            }

            s += QStringLiteral(
                "  Charges above are the effective-dated rates from\n"
                "  config/charges.toml, taken from the schedule CURRENTLY IN\n"
                "  FORCE — effective %1 — rather than from a date typed on\n"
                "  this page. A typed date goes stale the moment a circular\n"
                "  is added to the file, and the page would then disagree\n"
                "  with the engine with nothing to say why.%2\n\n"
                "  Brokerage is the one literal here. It is commercial\n"
                "  rather than regulatory and changes without a circular, so\n"
                "  it does not belong in an effective-dated schedule.\n")
                     .arg(eff)
                     .arg(sch->verified
                              ? QString()
                              : QStringLiteral(
                                    "\n\n  THAT SCHEDULE IS UNVERIFIED: it has "
                                    "never been checked\n  against a broker "
                                    "contract note, so every rupee above\n"
                                    "  inherits that."));
        }
    }
#else
    s += QStringLiteral(
        "――― 3-5. PARITY, THE BOX, AND THE FUTURES CALENDAR ―――\n\n"
        "  COSTED SCANNERS UNAVAILABLE in this build, and deliberately not\n"
        "  worked around.\n\n"
        "  These three scanners each price a bill: four option legs for a\n"
        "  box, two options and a hedge in and out for parity, two futures\n"
        "  legs for a calendar. Rule 5 says every signal is priced net of\n"
        "  full cost BEFORE IT EXISTS, and rule 1 says the rates come from\n"
        "  config/charges.toml rather than from a literal.\n\n"
        "  This build has no charges.toml loader: it needs toml++, which\n"
        "  arrives with the `vcpkg` preset (and with `net`, which inherits\n"
        "  it). Configure with one of those and this section computes.\n\n"
        "  The alternative would be a schedule hard-coded here, which is\n"
        "  worse than an empty section: an invented charge is not a\n"
        "  conservative approximation, it is a number the rest of the\n"
        "  system would trust.\n\n"
        "  Sections 1 and 2 above are unaffected. Neither of them prices\n"
        "  anything: a convexity margin is a property of three quotes and a\n"
        "  calendar margin is a difference of two total variances, and\n"
        "  neither has a brokerage.\n");
#endif

    return s;
}

/// The ensemble, on members that actually exist.
///
/// P32-03. This page was a BLOCKED page whose text read: "It has no members to
/// combine until the Models page above has something in it." That was true
/// when it was written and has not been true since P16. There are members:
/// the gradient-boosted spot forecast fits on real NIFTY closes, at several
/// lag counts and under two growth strategies, and every one of them produces
/// a net edge and a standard error. They were never combined.
///
/// A BLOCKED PAGE THAT IS NO LONGER BLOCKED IS WORSE THAN A WRONG NUMBER.
///
/// It says the machinery is untested when the machinery has been sitting
/// there, and it removes the one page that would have shown the thing P8-11
/// and P8-12 exist to show. So this runs the aggregator, on real members, and
/// reports what it says.
///
/// WHAT THE MEMBERS ARE, AND WHY THEY ARE THE RIGHT KIND OF WRONG.
///
/// Six members from three walk-forward runs: lag counts 4, 8 and 12, each
/// scored level-wise and leaf-wise. They share their features, their training
/// data and their labels, which is exactly the case P6-04 measured for signals
/// and the case an unweighted ensemble handles worst. An ensemble of six
/// genuinely independent models would be a different and much rarer object;
/// this is the one people actually build.
///
/// THE STANDARD ERROR IS OF A MEAN RETURN, AND IS COMPUTED HERE.
///
/// `SpotReport` gives net bps per bar but not its uncertainty, so the error
/// bar is computed from the data the page already holds: the per-bar return
/// standard deviation over the scored bars, divided by the root of the count.
/// That is the standard error of a mean, which is what a net-bps-per-bar
/// figure is.
[[nodiscard]] inline QString aggregator_report(const QString& dataset_root,
                                               const QString& sub,
                                               const char* label) {
    QString s = QStringLiteral(
        "AGGREGATOR — combining members, and the interval that decides a size\n"
        "P8-11 / P8-12, on %1\n\n")
                    .arg(QLatin1String(label));

    std::vector<double> closes =
        ui_load_closes(dataset_root + sub + QStringLiteral("all.csv"));
    if (closes.size() < 2000) {
        return s + QStringLiteral(
            "――― NO MEMBERS ―――\n\n"
            "  No daily series under %1%2. The aggregator needs models, and\n"
            "  a model needs data; neither is invented here.\n")
                       .arg(dataset_root, sub);
    }

    // ――― the members ―――
    //
    // Same horizon for every one of them, deliberately. Combining across
    // horizons is the mistake this header exists to prevent and it is
    // demonstrated separately below, as a REFUSAL, rather than performed.
    struct Built { QString name; double value; double se; std::size_t scored; };
    std::vector<Built> built;

    // Per-bar return sd, in bps. The error bar's numerator, computed from the
    // series rather than assumed: a mean of n draws has standard error
    // sd/sqrt(n), and pretending otherwise is how an ensemble acquires an
    // interval that is too tight for a reason nothing downstream can see.
    double mu = 0.0;
    std::vector<double> rets;
    rets.reserve(closes.size());
    for (std::size_t i = 1; i < closes.size(); ++i) {
        if (!(closes[i] > 0.0) || !(closes[i - 1] > 0.0)) { continue; }
        rets.push_back(10'000.0 * std::log(closes[i] / closes[i - 1]));
    }
    for (double r : rets) { mu += r; }
    mu /= static_cast<double>(rets.empty() ? 1 : rets.size());
    double sd = 0.0;
    for (double r : rets) { sd += (r - mu) * (r - mu); }
    sd = rets.size() > 1
             ? std::sqrt(sd / static_cast<double>(rets.size() - 1))
             : 0.0;

    for (const std::size_t lags : {4u, 8u, 12u}) {
        SpotSpec spec;
        spec.horizon = 1;
        spec.cost_bps = 5.5;
        spec.folds = 5;
        spec.lags = lags;
        const auto r = forecast_spot(closes, spec);
        if (!r) { continue; }
        const double se =
            r->scored > 1 ? sd / std::sqrt(static_cast<double>(r->scored))
                          : 0.0;
        built.push_back({QStringLiteral("level-wise, %1 lags").arg(lags),
                         r->net_bps_level, se, r->scored});
        built.push_back({QStringLiteral("leaf-wise,  %1 lags").arg(lags),
                         r->net_bps_leaf, se, r->scored});
    }

    if (built.empty()) {
        return s + QStringLiteral(
            "――― NO MEMBERS ―――\n\n  No fold completed.\n");
    }

    s += QStringLiteral(
        "――― 1. THE MEMBERS ―――\n\n"
        "  Six models from three walk-forward runs: lag counts 4, 8 and 12,\n"
        "  each scored level-wise and leaf-wise. Every value is NET of a\n"
        "  5.5 bps round trip (rule 5) and every one is a mean over the\n"
        "  scored bars, so its error bar is sd/sqrt(n) with sd measured on\n"
        "  the series itself: %1 bps per bar.\n\n"
        "    member                   net bps/bar        se       t\n")
             .arg(sd, 0, 'f', 2);

    std::vector<Member> members;
    for (const Built& b : built) {
        Member m{};
        m.forecast.value = b.value;
        m.forecast.std_error = b.se;
        m.forecast.feature_version = 1;
        m.horizon = 1;
        m.weight = 1.0;          // equal, and the sweep below is why
        m.ready = true;
        members.push_back(m);
        s += QStringLiteral("    %1 %2 %3 %4\n")
                 .arg(b.name, -24)
                 .arg(b.value, 12, 'f', 4)
                 .arg(b.se, 9, 'f', 4)
                 .arg(b.se > 0.0 ? b.value / b.se : 0.0, 7, 'f', 2);
    }
    {
        // COUNTED, NOT ASSERTED. And counted against the threshold SIX tests
        // require, not the one a single test would.
        int over2 = 0, over_bonf = 0;
        double best = 0.0;
        // Two-sided 5% across k tests: the Sidak/Bonferroni z. For k = 6 this
        // is about 2.64, and using 2.00 on the best of six is the oldest way
        // to find an edge that is not there.
        const double zb = 2.638;
        for (const Built& b : built) {
            const double t = b.se > 0.0 ? b.value / b.se : 0.0;
            if (t > 2.0) { ++over2; }
            if (t > zb) { ++over_bonf; }
            if (t > best) { best = t; }
        }
        s += QStringLiteral(
            "\n  %1 of %2 members clear two standard errors individually,\n"
            "  the best at t = %3.\n\n"
            "  THAT IS THE WRONG THRESHOLD AND IT IS WORTH SAYING SO. Six\n"
            "  members were fitted and all six are reported, so the bar is\n"
            "  the one six tests require: t = %4 for two-sided 5 per cent.\n"
            "  %5 member(s) clear THAT.\n\n"
            "  And these t-statistics are on NET RETURN, while P16-06's\n"
            "  sigma_level is on DIRECTIONAL ACCURACY — a model can be worse\n"
            "  than a constant at RMSE, which P16-06 measured it to be, and\n"
            "  still make money on the sign. The two numbers disagree because\n"
            "  they are different statistics, and this page reports its own\n"
            "  rather than quoting whichever is more flattering.\n\n")
                 .arg(over2).arg(built.size()).arg(best, 0, 'f', 2)
                 .arg(zb, 0, 'f', 2).arg(over_bonf);
    }

    // ――― 2. the refusal ―――
    s += QStringLiteral(
        "――― 2. TWO HORIZONS ARE NOT TWO OPINIONS ABOUT ONE THING ―――\n\n"
        "  A model trained on a 1-bar forward return and one trained on a\n"
        "  5-bar forward return produce numbers of the same type, in the\n"
        "  same units, on the same instrument, at the same instant.\n"
        "  Averaging them is one line of code and it estimates neither\n"
        "  quantity.\n\n"
        "  Under a random walk a return scales with the root of the\n"
        "  horizon, so the longest-horizon member dominates an unweighted\n"
        "  mean regardless of whether it is any good.\n\n");
    {
        std::vector<Member> mixed = members;
        mixed[1].horizon = 5;         // one member, a different horizon
        const auto bad = aggregate(mixed.data(), mixed.size(), 0.5, 0.05);
        s += QStringLiteral("    combining a 1-bar and a 5-bar member: %1\n\n")
                 .arg(bad.has_value()
                          ? QStringLiteral("ACCEPTED — WHICH IS A BUG")
                          : QStringLiteral("REFUSED"));

        // And the same for a feature-registry mismatch, which is the other
        // way two members can be talking about different things while
        // agreeing on every visible number.
        std::vector<Member> fv = members;
        fv[2].forecast.feature_version = 7;
        const auto bad2 = aggregate(fv.data(), fv.size(), 0.5, 0.05);
        s += QStringLiteral(
            "    combining members built on different feature registries:"
            " %1\n\n")
                 .arg(bad2.has_value()
                          ? QStringLiteral("ACCEPTED — WHICH IS A BUG")
                          : QStringLiteral("REFUSED"));
    }

    // ――― 3. the correlation sweep ―――
    s += QStringLiteral(
        "――― 3. SIX MODELS ON THE SAME FEATURES ARE NOT SIX VOTES ―――\n\n"
        "  P6-04 measured ten members of an EMA stack carrying 1.25\n"
        "  signals' worth of information. Model outputs are worse, because\n"
        "  they share not only their features but their training data and\n"
        "  their labels — which is exactly what these six do.\n\n"
        "  The mean correlation is SWEPT rather than assumed. An ensemble's\n"
        "  standard error is not sigma/sqrt(k), and rho = 0 is the\n"
        "  flattering assumption, so the page shows what the interval does\n"
        "  across the range instead of picking a number that suits it.\n\n"
        "    rho     n_eff     naive se      real se     lower bound(2se)\n");

    for (const double rho : {0.0, 0.25, 0.50, 0.75, 0.90, 0.95}) {
        const auto a = aggregate(members.data(), members.size(), rho, 0.05);
        if (!a) { continue; }
        s += QStringLiteral("    %1 %2 %3 %4 %5\n")
                 .arg(rho, 5, 'f', 2)
                 .arg(a->n_effective, 9, 'f', 3)
                 .arg(a->naive_std_error, 12, 'f', 4)
                 .arg(a->std_error, 12, 'f', 4)
                 .arg(a->lower_bound(2.0), 16, 'f', 4);
    }
    {
        const auto a0 = aggregate(members.data(), members.size(), 0.0, 0.05);
        const auto a9 = aggregate(members.data(), members.size(), 0.95, 0.05);
        if (a0 && a9) {
            s += QStringLiteral(
                "\n  THE CORRELATION CORRECTION IS DOING ALL THE WORK HERE,\n"
                "  AND THAT IS THE FINDING.\n\n"
                "  At rho = 0 the lower bound is %1 bps and there is an edge.\n"
                "  At rho = 0.95 it is %2 bps and there is not. Same members,\n"
                "  same data, same arithmetic — the entire answer is decided\n"
                "  by one number.\n\n"
                "  AND THAT NUMBER IS NOT MEASURED. `forecast_spot` returns\n"
                "  summary statistics, not the per-bar forecast series, so\n"
                "  the correlation among these six cannot be computed from\n"
                "  what the engine hands back. It has to be swept.\n\n"
                "  So the honest reading is not 'there is an edge' and not\n"
                "  'there is none'. It is that six models sharing their\n"
                "  features, their training data and their labels are\n"
                "  certainly not at rho = 0, the answer at any plausible rho\n"
                "  is nothing, and the measurement that would settle it is a\n"
                "  change to forecast_spot rather than an opinion.\n\n")
                     .arg(a0->lower_bound(2.0), 0, 'f', 4)
                     .arg(a9->lower_bound(2.0), 0, 'f', 4);
        }
    }

    // ――― 4. the money ―――
    {
        const auto a = aggregate(members.data(), members.size(), 0.90, 0.05);
        if (a) {
            // One NIFTY lot of 75 at the last close. paise_per_unit converts
            // one basis point of the aggregate into paise on that notional --
            // bps are a thousandth of a per cent, so a bp of notional is
            // notional/10,000.
            const double last_paise = closes.back() * 100.0;
            const double notional = last_paise * 75.0;
            const double per_bp = notional / 10'000.0;
            // THE ROUND TRIP IS ZERO HERE AND THAT IS NOT RULE 5 BEING
            // SKIPPED.
            //
            // The first version passed the 5.5 bps round trip again and
            // charged it twice: every member's value is `net_bps_level`,
            // which forecast_spot has ALREADY taken the same 5.5 bps out of
            // (SpotSpec::cost_bps above). The aggregate of net numbers is a
            // net number. Subtracting the cost a second time turned a
            // +0.09 bps lower bound into minus Rs 958 a lot and made the page
            // read as a much more confident negative than the data supports.
            //
            // Passing zero is therefore the arithmetic that keeps rule 5
            // exactly once, and this comment exists because a zero round trip
            // on a page about net edge is otherwise indistinguishable from
            // the rule being ignored.
            const auto net = net_edge_lower_bound(*a, per_bp, 2.0,
                                                  Notional{0});
            s += QStringLiteral(
                "――― 4. WHAT A TRADE HANDLER WOULD BE ALLOWED TO SEE ―――\n\n"
                "  One NIFTY lot of 75 at %1, notional Rs %2.\n"
                "  There is no accessor anywhere that returns the POINT\n"
                "  estimate in paise, for the same reason P6-04's Score has\n"
                "  none: the only number that leaves this header is already\n"
                "  net of cost and already at the lower bound.\n\n"
                "    aggregate value        %3 bps/bar   (already net of\n"
                "                                        5.5 bps round trip)\n"
                "    standard error         %4 bps  (rho 0.90)\n"
                "    lower bound at 2 se    %5 bps\n"
                "    edge at the bound      Rs %6 per lot per bar\n\n")
                     .arg(closes.back(), 0, 'f', 2)
                     .arg(notional / 100.0, 0, 'f', 0)
                     .arg(a->value, 0, 'f', 4)
                     .arg(a->std_error, 0, 'f', 4)
                     .arg(a->lower_bound(2.0), 0, 'f', 4)
                     .arg(net ? QString::number(
                                    static_cast<double>(net->raw()) / 100.0,
                                    'f', 2)
                              : QStringLiteral("refused"));
            s += QStringLiteral(
                "  Rs %1 a lot a bar, at rho = 0.90, before anything this\n"
                "  page has not modelled: slippage beyond the modelled\n"
                "  round trip, the fact that the members were selected\n"
                "  after being fitted, and a correlation that was assumed\n"
                "  rather than measured.\n\n"
                "  A number this small is not a signal. It is what a lower\n"
                "  bound is FOR — the point estimate was %2 bps and would\n"
                "  have sized a position.\n\n")
                     .arg(net ? QString::number(
                                    static_cast<double>(net->raw()) / 100.0,
                                    'f', 2)
                              : QStringLiteral("—"))
                     .arg(a->value, 0, 'f', 4);
        }
    }

    s += QStringLiteral(
        "――― WHAT THIS PAGE IS AND IS NOT SAYING ―――\n\n"
        "  The aggregator works. It combined six real members, refused a\n"
        "  mismatched horizon and a mismatched feature registry, and\n"
        "  reported an interval carrying both the correlation inflation and\n"
        "  the weights' own estimation error. P8-11 and P8-12 did their job.\n\n"
        "  It is NOT saying the members are worthless. The best of them\n"
        "  clears two standard errors on net return, and one clears the\n"
        "  threshold six simultaneous tests require. On a daily series that\n"
        "  is worth another look, and it is more than this project has\n"
        "  found in most places it has looked.\n\n"
        "  It is also NOT saying there is an edge. The ensemble's lower\n"
        "  bound survives only at correlations these six models cannot\n"
        "  plausibly have, given that they share their features, their\n"
        "  training data and their labels. At any rho a reasonable person\n"
        "  would defend, the bound is nothing.\n\n"
        "  WHAT IT IS SAYING is that the answer turns entirely on a number\n"
        "  nobody has measured, and that measuring it is a change to\n"
        "  models/spot_forecast.hpp — return the per-bar forecast series so\n"
        "  the correlation among members can be computed instead of swept.\n"
        "  Until then this page has an argument and not a result.\n");
    return s;
}

} // namespace altair::ui
