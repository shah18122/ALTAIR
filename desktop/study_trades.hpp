// desktop/study_trades.hpp -- the trades behind a study page's numbers.
//
// The Strategies and Overnight Gap pages report averages: bps a bar, a t, a
// hit rate. Smit asked for every trade behind them. This rebuilds the trade
// list from the SAME rule and the SAME data the page's report uses
// (strategies/momentum.hpp, meanrev.hpp, overnight.hpp on dataset/), one row
// per round trip: when it went in and out, at what close, why, gross, cost and
// net. Nothing here changes a strategy; it only reads its positions.
//
// Units. A round trip's money is in rupees PER ONE UNIT of the index (one
// NIFTY point is Rs 1 a unit), so a lot is the row times the lot size. The
// cost is the page's own: `cost_bps` per unit of turnover, a full flip being
// two units.
//
// A built list can be saved to data/verified/study/, where the Backtests page
// lists it with the rest.
#pragma once

#include "quant_pages.hpp"
#include "theme.hpp"
#include "trade_reason.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QTextBrowser>
#include <QDir>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QTableWidget>
#include <QThreadPool>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace altair::ui {

/// One round trip of a study.
struct StudyTrip {
    QString entry_day, exit_day, side;   ///< side: "long" or "short"
    double size = 0.0;                    ///< units of the index at entry (vol-scaled rules vary inside the trip)
    double entry = 0.0, exit = 0.0;       ///< closes (or the open, for the overnight exit)
    int bars = 0;                         ///< bars held
    double gross_bps = 0.0, cost_bps = 0.0, net_bps = 0.0;   ///< summed over the bars, per unit of position
    double gross_rs = 0.0, cost_rs = 0.0, net_rs = 0.0;      ///< per one unit of the index
    QString why_in, why_out;
};

/// Round trips out of a position series.
///
/// `pos[i]` is held INTO bar i and earns the move from close i-1 to close i,
/// as strategies/momentum.hpp's evaluate() books it. A trip is a run of one
/// sign; a flip closes one trip and opens the next on the same bar. Turnover
/// inside a trip (a vol-scaled rule resizing) is charged to that trip, and the
/// turnover of a flip is split: closing the old side to the old trip, opening
/// the new side to the new one. Summed, the trips equal evaluate()'s totals.
[[nodiscard]] inline std::vector<StudyTrip> trips_from_positions(const std::vector<std::string>& days,
                                                                 const std::vector<double>& closes,
                                                                 const std::vector<double>& pos, double cost_bps,
                                                                 const QString& why_in, const QString& why_out) {
    std::vector<StudyTrip> out;
    if (closes.size() != days.size() || pos.size() + 1 != closes.size()) return out;
    StudyTrip cur;
    bool open = false;
    double prev = 0.0;
    const auto sgn = [](double p) { return p > 1e-12 ? 1 : (p < -1e-12 ? -1 : 0); };
    const auto close_trip = [&](std::size_t i, double exit_turn) {
        // Exit at close i-1: the last bar the position was held into is i-1.
        cur.exit_day = QString::fromStdString(days[i]);
        cur.exit = closes[i];
        cur.cost_bps += exit_turn * cost_bps;
        cur.cost_rs += exit_turn * cost_bps * 1e-4 * closes[i];
        cur.net_bps = cur.gross_bps - cur.cost_bps;
        cur.net_rs = cur.gross_rs - cur.cost_rs;
        cur.why_out = why_out;
        out.push_back(cur);
        open = false;
    };
    for (std::size_t i = 0; i < pos.size(); ++i) {
        const double p = pos[i];
        // Bar i of `pos` is the move from closes[i] to closes[i+1].
        const double c0 = closes[i], c1 = closes[i + 1];
        const int s = sgn(p), ps = sgn(prev);
        if (open && s != ps) close_trip(i, std::fabs(prev));
        if (s != 0 && (!open)) {
            cur = StudyTrip{};
            open = true;
            cur.entry_day = QString::fromStdString(days[i]);
            cur.entry = c0;
            cur.side = s > 0 ? QStringLiteral("long") : QStringLiteral("short");
            cur.size = std::fabs(p);
            cur.why_in = why_in;
            cur.cost_bps += std::fabs(p) * cost_bps;
            cur.cost_rs += std::fabs(p) * cost_bps * 1e-4 * c0;
        } else if (open) {
            const double turn = std::fabs(p - prev);   // a resize inside the trip
            cur.cost_bps += turn * cost_bps;
            cur.cost_rs += turn * cost_bps * 1e-4 * c0;
        }
        if (open) {
            cur.gross_bps += p * 10000.0 * std::log(c1 / c0);
            cur.gross_rs += p * (c1 - c0);
            ++cur.bars;
        }
        prev = p;
    }
    if (open) {
        // Still held at the end of the data: marked at the last close, with no
        // exit cost, since nothing was sold (evaluate() charges none either).
        close_trip(closes.size() - 1, 0.0);
        out.back().why_out = QStringLiteral("still open at the last close in the data (marked there, no exit cost)");
    }
    return out;
}

/// The studies a trade list can be built for.
enum class Study { Momentum, MeanReversion, Overnight };

[[nodiscard]] inline QString study_name(Study s) {
    switch (s) {
    case Study::Momentum: return QStringLiteral("Momentum (60-day, vol-scaled)");
    case Study::MeanReversion: return QStringLiteral("Mean reversion (60-day z, in 1.5 / out 0.5)");
    case Study::Overnight: return QStringLiteral("Overnight gap (close to 09:15 open, every night)");
    }
    return {};
}

[[nodiscard]] inline QString study_slug(Study s) {
    switch (s) {
    case Study::Momentum: return QStringLiteral("momentum");
    case Study::MeanReversion: return QStringLiteral("mean_reversion");
    case Study::Overnight: return QStringLiteral("overnight_gap");
    }
    return {};
}

/// Every round trip of `study` on `sym` (a dataset/spot directory name).
/// Empty when the data is missing or too short. Pure: safe on any thread.
[[nodiscard]] inline std::vector<StudyTrip> study_trips(const QString& dataset_root, const QString& sym, Study study,
                                                        double cost_bps) {
    if (study == Study::Overnight) {
        const auto bars = ui_load_sessions(spot_path(dataset_root, sym, "5m", ""));
        std::vector<StudyTrip> out;
        if (bars.size() < 100) return out;
        out.reserve(bars.size());
        for (std::size_t i = 1; i < bars.size(); ++i) {
            const SessionBar& p = bars[i - 1];
            const SessionBar& b = bars[i];
            if (!(p.close > 0.0) || !(b.open > 0.0)) continue;
            StudyTrip t;
            t.entry_day = QString::fromStdString(p.day);
            t.exit_day = QString::fromStdString(b.day);
            t.side = QStringLiteral("long");
            t.size = 1.0;
            t.entry = p.close;
            t.exit = b.open;
            t.bars = 1;
            t.gross_bps = 10000.0 * std::log(b.open / p.close);
            t.cost_bps = 2.0 * cost_bps;
            t.net_bps = t.gross_bps - t.cost_bps;
            t.gross_rs = b.open - p.close;
            t.cost_rs = cost_bps * 1e-4 * (p.close + b.open);
            t.net_rs = t.gross_rs - t.cost_rs;
            t.why_in = QStringLiteral("bought at the close (the rule takes every night)");
            t.why_out = QStringLiteral("sold into the 09:15 open auction");
            out.push_back(std::move(t));
        }
        return out;
    }
    const auto dated = ui_load_dated_closes(spot_path(dataset_root, sym, "1d"));
    if (dated.size() < 300) return {};
    std::vector<std::string> days;
    std::vector<double> closes, r_bps;
    days.reserve(dated.size());
    closes.reserve(dated.size());
    for (const auto& [d, c] : dated) {
        days.push_back(d);
        closes.push_back(c);
    }
    for (std::size_t i = 1; i < closes.size(); ++i) r_bps.push_back(10000.0 * std::log(closes[i] / closes[i - 1]));
    if (study == Study::Momentum) {
        const auto pos = momentum_positions(r_bps, MomentumSpec{});
        if (!pos) return {};
        return trips_from_positions(days, closes, *pos, cost_bps,
                                    QStringLiteral("trailing 60-day return turned this way (sized to 100 bps a day of risk)"),
                                    QStringLiteral("trailing 60-day return changed sign"));
    }
    const auto pos = meanrev_positions(r_bps, MeanRevSpec{});
    if (!pos) return {};
    return trips_from_positions(days, closes, *pos, cost_bps,
                                QStringLiteral("price moved beyond 1.5 standard deviations of its 60-day mean; faded it"),
                                QStringLiteral("back within 0.5 standard deviations of the mean"));
}

/// The trades as CSV, in the columns the Backtests page reads.
[[nodiscard]] inline QByteArray study_trips_csv(const std::vector<StudyTrip>& trips) {
    QByteArray out = "entry_date,exit_date,side,size_units,entry,exit,bars,gross_bps,cost_bps,net_bps,"
                     "gross_rs_per_unit,costs,net_rs_per_unit,why_in,why_out\n";
    const auto q = [](QString s) { return QLatin1Char('"') + s.replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"'); };
    for (const auto& t : trips) {
        out += QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12,%13,%14,%15\n")
                   .arg(t.entry_day, t.exit_day, t.side, QString::number(t.size, 'f', 3), QString::number(t.entry, 'f', 2),
                        QString::number(t.exit, 'f', 2))
                   .arg(t.bars)
                   .arg(QString::number(t.gross_bps, 'f', 2), QString::number(t.cost_bps, 'f', 2),
                        QString::number(t.net_bps, 'f', 2), QString::number(t.gross_rs, 'f', 2),
                        QString::number(t.cost_rs, 'f', 2), QString::number(t.net_rs, 'f', 2), q(t.why_in), q(t.why_out))
                   .toUtf8();
    }
    return out;
}

/// One line of figures over a trade list, in bps a unit and rupees a unit.
[[nodiscard]] inline QString study_summary_html(const std::vector<StudyTrip>& trips) {
    if (trips.empty()) return QStringLiteral("No trades.");
    int wins = 0;
    double net_bps = 0.0, gross_bps = 0.0, cost_bps = 0.0, net_rs = 0.0, best = -1e300, worst = 1e300, won = 0.0, lost = 0.0;
    for (const auto& t : trips) {
        if (t.net_bps > 0.0) { ++wins; won += t.net_bps; } else { lost -= t.net_bps; }
        net_bps += t.net_bps;
        gross_bps += t.gross_bps;
        cost_bps += t.cost_bps;
        net_rs += t.net_rs;
        best = std::max(best, t.net_bps);
        worst = std::min(worst, t.net_bps);
    }
    const double n = static_cast<double>(trips.size());
    const auto col = [](double v) { return v >= 0.0 ? QStringLiteral("#3FB950") : QStringLiteral("#F85149"); };
    return QStringLiteral("<b>%1 round trips</b> (%2 to %3) · won %4% · gross %5 bps · costs %6 bps · "
                          "<b>net <span style='color:%7'>%8 bps</span></b> (mean %9 a trip) · net Rs %10 a unit · "
                          "best %11, worst %12 bps · profit factor %13")
        .arg(trips.size())
        .arg(trips.front().entry_day, trips.back().exit_day)
        .arg(100.0 * wins / n, 0, 'f', 1)
        .arg(gross_bps, 0, 'f', 0)
        .arg(cost_bps, 0, 'f', 0)
        .arg(col(net_bps))
        .arg(net_bps, 0, 'f', 0)
        .arg(net_bps / n, 0, 'f', 2)
        .arg(net_rs, 0, 'f', 0)
        .arg(best, 0, 'f', 0)
        .arg(worst, 0, 'f', 0)
        .arg(lost > 0.0 ? QString::number(won / lost, 'f', 2) : QStringLiteral("—"));
}

/// The trade list for a study page: pick the rule and instrument, build it
/// (off the UI thread), read every trip, save it for the Backtests page.
class StudyTradesPanel final : public QWidget {
public:
    StudyTradesPanel(QString dataset_root, QString root, std::vector<Study> studies, QWidget* parent = nullptr)
        : QWidget(parent), dataset_(std::move(dataset_root)), root_(std::move(root)) {
        setObjectName(QStringLiteral("studyTrades"));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 8, 0, 0);
        auto* note = new QLabel(QStringLiteral(
            "Every round trip behind this page's figures: the same rule on the same dataset/ series, one row per trade "
            "(when in and out, at what close, why, gross, cost and net). Money is per one unit of the index; multiply by the "
            "lot size for a lot. <b>Save</b> puts the list in data/verified/study/, and the Backtests page lists it."), this);
        note->setWordWrap(true);
        note->setTextFormat(Qt::RichText);
        set_role(note, QStringLiteral("note"));
        v->addWidget(note);
        auto* row = new QHBoxLayout;
        study_ = new QComboBox(this);
        study_->setObjectName(QStringLiteral("studyChoice"));
        for (const Study s : studies) study_->addItem(study_name(s), static_cast<int>(s));
        row->addWidget(study_, 1);
        symbol_ = new QComboBox(this);
        std::size_t k = 0;
        const QuantSymbol* syms = quant_symbols(k);
        for (std::size_t i = 0; i < k; ++i) symbol_->addItem(QLatin1String(syms[i].label), QLatin1String(syms[i].dir));
        row->addWidget(symbol_);
        row->addWidget(new QLabel(QStringLiteral("cost / unit turnover"), this));
        cost_ = new QDoubleSpinBox(this);
        cost_->setRange(0.0, 100.0);
        cost_->setDecimals(1);
        cost_->setSuffix(QStringLiteral(" bps"));
        cost_->setValue(5.5);
        row->addWidget(cost_);
        build_ = new QPushButton(QStringLiteral("Build trade list"), this);
        set_variant(build_, QStringLiteral("primary"));
        build_->setObjectName(QStringLiteral("studyBuild"));
        row->addWidget(build_);
        save_ = new QPushButton(QStringLiteral("Save for Backtests"), this);
        save_->setEnabled(false);
        row->addWidget(save_);
        v->addLayout(row);
        summary_ = new QLabel(QStringLiteral("Not built yet."), this);
        summary_->setObjectName(QStringLiteral("studySummary"));
        summary_->setTextFormat(Qt::RichText);
        summary_->setWordWrap(true);
        v->addWidget(summary_);
        table_ = new QTableWidget(0, 12, this);
        table_->setObjectName(QStringLiteral("studyTable"));
        table_->setHorizontalHeaderLabels({QStringLiteral("In"), QStringLiteral("Out"), QStringLiteral("Side"),
                                           QStringLiteral("Units"), QStringLiteral("Entry"), QStringLiteral("Exit"),
                                           QStringLiteral("Bars"), QStringLiteral("Gross bps"), QStringLiteral("Cost bps"),
                                           QStringLiteral("Net bps"), QStringLiteral("Why in"), QStringLiteral("Why out")});
        table_->verticalHeader()->hide();
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setAlternatingRowColors(true);
        table_->horizontalHeader()->setStretchLastSection(true);
        v->addWidget(table_, 1);
        QObject::connect(build_, &QPushButton::clicked, this, [this] { build(); });
        QObject::connect(save_, &QPushButton::clicked, this, [this] { (void)save(); });
        table_->setToolTip(QStringLiteral("Double-click a trade: why it went in and out, and what the forecasts said that day"));
        QObject::connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int r, int) {
            const QString html = trip_html(r);
            if (html.isEmpty()) return;
            QDialog d(this);
            d.setWindowTitle(QStringLiteral("Study trade"));
            auto* l = new QVBoxLayout(&d);
            auto* b = new QTextBrowser(&d);
            b->setHtml(html);
            l->addWidget(b);
            d.resize(720, 560);
            d.exec();
        });
    }

    /// The reasoning of the trade on row `r` (newest first), as the double-click shows it.
    [[nodiscard]] QString trip_html(int r) const {
        if (r < 0 || r >= static_cast<int>(trips_.size())) return {};
        const auto& t = trips_[trips_.size() - 1 - static_cast<std::size_t>(r)];
        return trade_reason_html(root_, TradeContext{study_name(built_), built_symbol_.toUpper(), t.entry_day, t.entry_day,
                                                     t.exit_day, t.why_in, t.why_out})
               + QStringLiteral("<p><b>Gross</b> %1 bps (Rs %2 a unit) − <b>cost</b> %3 bps (Rs %4) = <b>net %5 bps</b> (Rs %6), "
                                "%7 bars at up to %8 units.</p>")
                     .arg(QString::number(t.gross_bps, 'f', 1), QString::number(t.gross_rs, 'f', 2),
                          QString::number(t.cost_bps, 'f', 1), QString::number(t.cost_rs, 'f', 2),
                          QString::number(t.net_bps, 'f', 1), QString::number(t.net_rs, 'f', 2))
                     .arg(t.bars)
                     .arg(t.size, 0, 'f', 2);
    }

    /// Build the list for the selected study. Runs off the UI thread; `done`
    /// (tests) is called on the UI thread when the table is filled.
    void build(std::function<void()> done = {}) {
        if (busy_) return;
        busy_ = true;
        build_->setEnabled(false);
        summary_->setText(QStringLiteral("Building…"));
        const QString ds = dataset_, sym = symbol_->currentData().toString();
        const Study s = static_cast<Study>(study_->currentData().toInt());
        const double cost = cost_->value();
        QPointer<StudyTradesPanel> self(this);
        QThreadPool::globalInstance()->start([self, ds, sym, s, cost, done] {
            auto trips = study_trips(ds, sym, s, cost);
            QMetaObject::invokeMethod(qApp, [self, trips = std::move(trips), s, sym, done]() mutable {
                if (self == nullptr) return;
                self->show_trips(std::move(trips), s, sym);
                if (done) done();
            }, Qt::QueuedConnection);
        });
    }

    /// Write the shown list to data/verified/study/<study>_<symbol>_trades.csv.
    /// Returns the path, or empty when there is nothing to save or it failed.
    QString save() {
        if (trips_.empty()) return {};
        const QString dir = root_ + QStringLiteral("/data/verified/study");
        if (!QDir().mkpath(dir)) return {};
        const QString path = dir + QStringLiteral("/%1_%2_trades.csv").arg(study_slug(built_), built_symbol_);
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return {};
        f.write(study_trips_csv(trips_));
        if (!f.commit()) return {};
        summary_->setText(summary_->text() + QStringLiteral("<br>Saved to <code>%1</code>.")
                                                 .arg(QDir(root_).relativeFilePath(path).toHtmlEscaped()));
        return path;
    }

    [[nodiscard]] int trip_count() const noexcept { return static_cast<int>(trips_.size()); }
    [[nodiscard]] QString summary_text() const { return summary_->text(); }
    [[nodiscard]] QComboBox* study_box() const noexcept { return study_; }

private:
    void show_trips(std::vector<StudyTrip> trips, Study s, const QString& sym) {
        busy_ = false;
        build_->setEnabled(true);
        trips_ = std::move(trips);
        built_ = s;
        built_symbol_ = sym;
        save_->setEnabled(!trips_.empty());
        summary_->setText(trips_.empty()
                              ? QStringLiteral("No trades: dataset/spot/%1 is missing or too short for this rule.").arg(sym.toHtmlEscaped())
                              : QStringLiteral("<b>%1</b> · %2<br>%3").arg(study_name(s).toHtmlEscaped(), sym.toUpper().toHtmlEscaped(),
                                                                           study_summary_html(trips_)));
        table_->setUpdatesEnabled(false);
        table_->setRowCount(static_cast<int>(trips_.size()));
        for (int i = 0; i < static_cast<int>(trips_.size()); ++i) {
            const auto& t = trips_[trips_.size() - 1 - static_cast<std::size_t>(i)];   // newest first
            const QStringList cells{t.entry_day, t.exit_day, t.side, QString::number(t.size, 'f', 2),
                                    QString::number(t.entry, 'f', 2), QString::number(t.exit, 'f', 2), QString::number(t.bars),
                                    QString::number(t.gross_bps, 'f', 1), QString::number(t.cost_bps, 'f', 1),
                                    QString::number(t.net_bps, 'f', 1), t.why_in, t.why_out};
            for (int c = 0; c < cells.size(); ++c) {
                auto* it = new QTableWidgetItem(cells[c]);
                if (c == 9) it->setForeground(QColor(t.net_bps >= 0.0 ? 0x3FB950 : 0xF85149));
                table_->setItem(i, c, it);
            }
        }
        table_->setUpdatesEnabled(true);
    }

    QString dataset_, root_;
    QComboBox* study_ = nullptr;
    QComboBox* symbol_ = nullptr;
    QDoubleSpinBox* cost_ = nullptr;
    QPushButton* build_ = nullptr;
    QPushButton* save_ = nullptr;
    QLabel* summary_ = nullptr;
    QTableWidget* table_ = nullptr;
    std::vector<StudyTrip> trips_;
    Study built_ = Study::Momentum;
    QString built_symbol_;
    bool busy_ = false;
};

} // namespace altair::ui
