// desktop/strategy_record.hpp -- a strategy's P&L record, demo and real, with
// and without expenses.
//
// DEMO is what the models engine paper-traded: data/live/paper/trades.csv, one
// row per round trip (gross, expenses, net), written by live/ledger.hpp.
// REAL is what the order router filled at FYERS for that strategy:
// data/live_orders/strategy_trades.csv, the same columns, written by
// oms/live_router.hpp when both legs of a round trip have filled.
//
// The panel reads files; it places nothing.

#pragma once

#include "trade_stats.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTextBrowser>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace altair::ui {

inline constexpr const char* kDemoTradesFile = "data/live/paper/trades.csv";
inline constexpr const char* kRealStrategyTradesFile = "data/live_orders/strategy_trades.csv";

/// One round trip as both files write it.
struct StrategyTrip {
    QString date, model, symbol, side, entry_time, exit_time, why_in, why_out;
    qint64 qty = 0;
    double entry = 0.0, exit = 0.0, gross = 0.0, expenses = 0.0, net = 0.0;
};

struct StrategyTotals {
    int trips = 0, wins = 0, unpriced = 0;   ///< unpriced: no expenses, so left out of the net
    double gross = 0.0, expenses = 0.0, net = 0.0;
};

namespace strategy_record_detail {

/// One CSV line: commas, double quotes, doubled quotes inside them.
[[nodiscard]] inline QStringList split(const QString& line) {
    QStringList out;
    QString cell;
    bool quoted = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < line.size() && line[i + 1] == QLatin1Char('"')) { cell += c; ++i; }
                else quoted = false;
            } else {
                cell += c;
            }
        } else if (c == QLatin1Char('"')) {
            quoted = true;
        } else if (c == QLatin1Char(',')) {
            out << cell;
            cell.clear();
        } else {
            cell += c;
        }
    }
    out << cell;
    return out;
}

} // namespace strategy_record_detail

/// Whether a model's name belongs to the strategy: exactly the name, or the
/// name followed by a space (variants such as "OHL NIFTY").
[[nodiscard]] inline bool strategy_matches(const QString& model, const QString& strategy) {
    return model == strategy || model.startsWith(strategy + QLatin1Char(' '));
}

/// The round trips of `strategy` in a trades file (date,model,symbol,token,
/// side,qty,entry_time,entry,exit_time,exit,gross,expenses,net,why_in,why_out,...).
[[nodiscard]] inline std::vector<StrategyTrip> read_strategy_trips(const QString& path, const QString& strategy) {
    std::vector<StrategyTrip> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    const QStringList head = strategy_record_detail::split(in.readLine());
    const auto col = [&head](const char* name) { return head.indexOf(QString::fromLatin1(name)); };
    const int c_date = col("date"), c_model = col("model"), c_sym = col("symbol"), c_side = col("side"), c_qty = col("qty"),
              c_et = col("entry_time"), c_e = col("entry"), c_xt = col("exit_time"), c_x = col("exit"),
              c_g = col("gross"), c_ex = col("expenses"), c_n = col("net"), c_wi = col("why_in"), c_wo = col("why_out");
    if (c_model < 0 || c_g < 0 || c_n < 0) return out;
    while (!in.atEnd()) {
        const QStringList r = strategy_record_detail::split(in.readLine());
        const auto at = [&r](int c) { return c >= 0 && c < r.size() ? r[c] : QString(); };
        if (!strategy_matches(at(c_model), strategy)) continue;
        StrategyTrip t;
        t.date = at(c_date);
        t.model = at(c_model);
        t.symbol = at(c_sym);
        t.side = at(c_side);
        t.qty = at(c_qty).toLongLong();
        t.entry_time = at(c_et);
        t.entry = at(c_e).toDouble();
        t.exit_time = at(c_xt);
        t.exit = at(c_x).toDouble();
        // Blank when a fill could not be priced: no expenses, so no net.
        const auto num = [&at](int c) {
            bool ok = false;
            const double v = at(c).trimmed().toDouble(&ok);
            return ok ? v : std::numeric_limits<double>::quiet_NaN();
        };
        t.gross = at(c_g).toDouble();
        t.expenses = num(c_ex);
        t.net = num(c_n);
        t.why_in = at(c_wi);
        t.why_out = at(c_wo);
        out.push_back(std::move(t));
    }
    return out;
}

[[nodiscard]] inline StrategyTotals strategy_totals(const std::vector<StrategyTrip>& trips) {
    StrategyTotals t;
    for (const auto& r : trips) {
        ++t.trips;
        t.gross += r.gross;
        if (!std::isfinite(r.net) || !std::isfinite(r.expenses)) { ++t.unpriced; continue; }
        if (r.net > 0) ++t.wins;
        t.expenses += r.expenses;
        t.net += r.net;
    }
    return t;
}

[[nodiscard]] inline QString strategy_money(double v) {
    return (v < 0 ? QStringLiteral("−") : QString()) + QStringLiteral("Rs ") + QString::number(std::abs(v), 'f', 2);
}

[[nodiscard]] inline QString strategy_totals_html(const StrategyTotals& t, const QString& label) {
    const auto colour = [](double v) { return v > 0 ? QStringLiteral("#7EE787") : v < 0 ? QStringLiteral("#F85149") : QStringLiteral("#C9D1D9"); };
    return QStringLiteral("<b>%1</b> · %2 round trip(s), %3 won · <b>without expenses</b> "
                          "<span style='color:%4'>%5</span> · expenses %6 · <b>with expenses</b> "
                          "<span style='color:%7'><b>%8</b></span>%9")
        .arg(label).arg(t.trips).arg(t.wins)
        .arg(colour(t.gross), strategy_money(t.gross), strategy_money(t.expenses), colour(t.net), strategy_money(t.net),
             t.unpriced > 0 ? QStringLiteral(" · <span style='color:#F0B429'>%1 with unpriced expenses (left out of the net)</span>")
                                  .arg(t.unpriced)
                            : QString());
}

/// The record of one strategy: the engine's reading of it, what it holds and
/// every round trip, demo or real, gross and net.
class StrategyRecordPanel final : public QWidget {
public:
    StrategyRecordPanel(QString strategy, QString root, QWidget* parent = nullptr)
        : QWidget(parent), strategy_(std::move(strategy)), root_(std::move(root)) {
        setObjectName(QStringLiteral("strategyRecord"));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        state_ = new QLabel(this);
        state_->setObjectName(QStringLiteral("strategyState"));
        state_->setWordWrap(true);
        state_->setTextFormat(Qt::RichText);
        v->addWidget(state_);
        fields_ = table({QStringLiteral("Reading"), QStringLiteral("Value")});
        fields_->setMaximumHeight(170);
        v->addWidget(fields_);
        auto* held_title = new QLabel(QStringLiteral("<b>Open positions</b>"), this);
        v->addWidget(held_title);
        held_ = table({QStringLiteral("Model"), QStringLiteral("Symbol"), QStringLiteral("Side"), QStringLiteral("Qty"),
                       QStringLiteral("Entry"), QStringLiteral("Why")});
        held_->setMaximumHeight(150);
        v->addWidget(held_);
        auto* bar = new QHBoxLayout;
        bar->addWidget(new QLabel(QStringLiteral("<b>P&amp;L record</b>"), this));
        mode_ = new QComboBox(this);
        mode_->setObjectName(QStringLiteral("strategyRecordMode"));
        mode_->addItems({QStringLiteral("Demo (paper)"), QStringLiteral("Real (FYERS)")});
        bar->addWidget(mode_);
        bar->addStretch();
        v->addLayout(bar);
        totals_ = new QLabel(this);
        totals_->setObjectName(QStringLiteral("strategyTotals"));
        totals_->setTextFormat(Qt::RichText);
        totals_->setWordWrap(true);
        v->addWidget(totals_);
        stats_ = new QLabel(this);
        stats_->setObjectName(QStringLiteral("strategyStats"));
        stats_->setTextFormat(Qt::RichText);
        stats_->setWordWrap(true);
        v->addWidget(stats_);
        trips_ = table({QStringLiteral("Date"), QStringLiteral("Model"), QStringLiteral("Symbol"), QStringLiteral("Side"),
                        QStringLiteral("Qty"), QStringLiteral("In"), QStringLiteral("Entry"), QStringLiteral("Out"),
                        QStringLiteral("Exit"), QStringLiteral("Gross"), QStringLiteral("Expenses"), QStringLiteral("Net"),
                        QStringLiteral("Why in"), QStringLiteral("Why out")});
        trips_->setToolTip(QStringLiteral("Double-click a round trip: its fills, every expense head, gross to net"));
        v->addWidget(trips_, 1);
        QObject::connect(trips_, &QTableWidget::cellDoubleClicked, this, [this](int r, int) { show_trip(r); });
        QObject::connect(mode_, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
        timer_.setInterval(2000);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] { if (isVisible()) refresh(); });
        timer_.start();
        refresh();
    }

    void set_root(const QString& root) { root_ = root; refresh(); }
    void set_real(bool real) { mode_->setCurrentIndex(real ? 1 : 0); refresh(); }
    [[nodiscard]] QTableWidget* trips_table() const noexcept { return trips_; }
    [[nodiscard]] QTableWidget* held_table() const noexcept { return held_; }
    [[nodiscard]] QString totals_text() const { return totals_->text(); }
    [[nodiscard]] QString stats_text() const { return stats_->text(); }
    /// The detail of the trip shown on row `r` (newest first), as the double-click shows it.
    [[nodiscard]] QString trip_detail(int r) const {
        if (r < 0 || r >= static_cast<int>(shown_.size())) return {};
        const auto& t = shown_[static_cast<std::size_t>(r)];
        return trip_detail_html(t.date, t.symbol, t.side, t.qty, t.entry, t.exit, t.gross, t.expenses, t.net, t.why_in, t.why_out);
    }
    [[nodiscard]] QString state_text() const { return state_->text(); }

    void refresh() {
        // The engine's reading and positions.
        QFile f(root_ + QStringLiteral("/data/live/engine_state.json"));
        const QJsonObject st = f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject{};
        const bool fresh = QFileInfo::exists(f.fileName())
                           && QFileInfo(f.fileName()).lastModified().secsTo(QDateTime::currentDateTime()) < 30;
        QStringList lines;
        fields_->setRowCount(0);
        for (const QJsonValue mv : st.value(QStringLiteral("models")).toArray()) {
            const QJsonObject m = mv.toObject();
            const QString name = m.value(QStringLiteral("name")).toString();
            if (!strategy_matches(name, strategy_)) continue;
            lines << QStringLiteral("<b>%1</b>: %2 — %3").arg(name.toHtmlEscaped(),
                                                             m.value(QStringLiteral("state")).toString().toHtmlEscaped(),
                                                             m.value(QStringLiteral("reason")).toString().toHtmlEscaped());
            for (const QJsonValue fv : m.value(QStringLiteral("fields")).toArray()) {
                const QJsonArray kv = fv.toArray();
                if (kv.size() != 2) continue;
                const int r = fields_->rowCount();
                fields_->insertRow(r);
                fields_->setItem(r, 0, new QTableWidgetItem(kv.at(0).toString()));
                fields_->setItem(r, 1, new QTableWidgetItem(kv.at(1).toString()));
            }
        }
        if (lines.isEmpty())
            state_->setText(QStringLiteral("<span style='color:#E3B341'>%1 has not reported yet: it starts by itself with the "
                                           "models engine once the feed streams (09:15–15:30).</span>").arg(strategy_.toHtmlEscaped()));
        else
            state_->setText(QStringLiteral("<span style='color:%1'><b>%2</b></span> · %3<br>%4")
                                .arg(fresh ? QStringLiteral("#7EE787") : QStringLiteral("#E3B341"),
                                     fresh ? QStringLiteral("RUNNING") : QStringLiteral("ENGINE NOT UPDATING"),
                                     st.value(QStringLiteral("source")).toString() == QLatin1String("SIM")
                                         ? QStringLiteral("SIM prices") : QStringLiteral("live prices"),
                                     lines.join(QStringLiteral("<br>"))));
        held_->setRowCount(0);
        for (const QJsonValue pv : st.value(QStringLiteral("positions")).toArray()) {
            const QJsonObject p = pv.toObject();
            if (!strategy_matches(p.value(QStringLiteral("model")).toString(), strategy_)) continue;
            const int r = held_->rowCount();
            held_->insertRow(r);
            held_->setItem(r, 0, new QTableWidgetItem(p.value(QStringLiteral("model")).toString()));
            held_->setItem(r, 1, new QTableWidgetItem(p.value(QStringLiteral("symbol")).toString()));
            held_->setItem(r, 2, new QTableWidgetItem(p.value(QStringLiteral("side")).toInt() > 0 ? QStringLiteral("BUY") : QStringLiteral("SELL")));
            held_->setItem(r, 3, new QTableWidgetItem(QString::number(p.value(QStringLiteral("qty")).toInt())));
            held_->setItem(r, 4, new QTableWidgetItem(QString::number(p.value(QStringLiteral("entry")).toDouble(), 'f', 2)));
            held_->setItem(r, 5, new QTableWidgetItem(p.value(QStringLiteral("why_in")).toString()));
        }
        // The record.
        const bool real = mode_->currentIndex() == 1;
        const auto trips = read_strategy_trips(
            root_ + QLatin1Char('/') + QLatin1String(real ? kRealStrategyTradesFile : kDemoTradesFile), strategy_);
        totals_->setText(strategy_totals_html(strategy_totals(trips), real ? QStringLiteral("REAL") : QStringLiteral("DEMO")));
        std::vector<TradeStatRow> rows;
        for (const auto& t : trips) rows.push_back(TradeStatRow{t.date, t.gross, t.expenses, t.net});
        stats_->setText(trade_stats_html(trade_stats(rows)));
        shown_.assign(trips.rbegin(), trips.rend());
        trips_->setRowCount(static_cast<int>(trips.size()));
        for (int i = 0; i < static_cast<int>(trips.size()); ++i) {
            const auto& t = trips[static_cast<std::size_t>(trips.size()) - 1 - static_cast<std::size_t>(i)];   // newest first
            const QStringList cells{t.date, t.model, t.symbol, t.side, QString::number(t.qty), t.entry_time,
                                    QString::number(t.entry, 'f', 2), t.exit_time, QString::number(t.exit, 'f', 2),
                                    QString::number(t.gross, 'f', 2),
                                    std::isfinite(t.expenses) ? QString::number(t.expenses, 'f', 2) : QStringLiteral("unpriced"),
                                    std::isfinite(t.net) ? QString::number(t.net, 'f', 2) : QStringLiteral("unpriced"), t.why_in,
                                    t.why_out};
            for (int c = 0; c < cells.size(); ++c) {
                auto* it = new QTableWidgetItem(cells[c]);
                if (c == 9 || c == 11)
                    it->setForeground(QColor(c == 9 ? (t.gross >= 0 ? 0x7EE787 : 0xF85149) : (t.net >= 0 ? 0x7EE787 : 0xF85149)));
                trips_->setItem(i, c, it);
            }
        }
    }

private:
    void show_trip(int r) {
        const QString html = trip_detail(r);
        if (html.isEmpty()) return;
        QDialog d(this);
        d.setWindowTitle(QStringLiteral("Round trip"));
        d.resize(900, 420);
        auto* v = new QVBoxLayout(&d);
        auto* b = new QTextBrowser(&d);
        b->setObjectName(QStringLiteral("tripDetail"));
        b->setHtml(html);
        v->addWidget(b);
        auto* ok = new QDialogButtonBox(QDialogButtonBox::Close, &d);
        QObject::connect(ok, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        v->addWidget(ok);
        d.exec();
    }

    QTableWidget* table(const QStringList& head) {
        auto* t = new QTableWidget(0, static_cast<int>(head.size()), this);
        t->setHorizontalHeaderLabels(head);
        t->verticalHeader()->hide();
        t->setEditTriggers(QAbstractItemView::NoEditTriggers);
        t->setSelectionBehavior(QAbstractItemView::SelectRows);
        t->horizontalHeader()->setStretchLastSection(true);
        t->setAlternatingRowColors(true);
        return t;
    }

    QString strategy_;
    QString root_;
    QLabel* state_ = nullptr;
    QLabel* totals_ = nullptr;
    QLabel* stats_ = nullptr;
    std::vector<StrategyTrip> shown_;   ///< the trips as shown, newest first
    QComboBox* mode_ = nullptr;
    QTableWidget* fields_ = nullptr;
    QTableWidget* held_ = nullptr;
    QTableWidget* trips_ = nullptr;
    QTimer timer_;
};

} // namespace altair::ui
