// desktop/demo_reports.hpp -- Trade History, Expense & Margin and RMS for the
// DEMO books, beside the same three for the real FYERS account (Alt+F6).
//
// The demo books are the Terminal's manual paper book
// (data/live/paper/manual_trades.csv) and every model the engine
// paper-trades (data/live/paper/fills.csv, trades.csv, margin.csv,
// open_positions.csv). Read from the files the engine and the paper book
// write; the pages trade nothing.

#pragma once

#include <QCheckBox>
#include <QDate>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFile>
#include <QHash>
#include <QTimeZone>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <limits>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace altair::ui {

namespace demo_reports_detail {

/// Rows of a CSV with a header, as name -> value maps (quoted cells allowed).
[[nodiscard]] inline std::vector<QHash<QString, QString>> read_csv(const QString& path) {
    std::vector<QHash<QString, QString>> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    const auto split = [](const QString& line) {
        QStringList cells;
        QString cell;
        bool quoted = false;
        for (qsizetype i = 0; i < line.size(); ++i) {
            const QChar c = line[i];
            if (quoted) {
                if (c == QLatin1Char('"')) {
                    if (i + 1 < line.size() && line[i + 1] == QLatin1Char('"')) { cell += c; ++i; }
                    else quoted = false;
                } else cell += c;
            } else if (c == QLatin1Char('"')) quoted = true;
            else if (c == QLatin1Char(',')) { cells << cell; cell.clear(); }
            else cell += c;
        }
        cells << cell;
        return cells;
    };
    const QStringList head = split(in.readLine());
    while (!in.atEnd()) {
        const QStringList c = split(in.readLine());
        if (c.size() < 2) continue;
        QHash<QString, QString> row;
        for (int i = 0; i < head.size() && i < c.size(); ++i) row.insert(head[i], c[i]);
        out.push_back(std::move(row));
    }
    return out;
}

[[nodiscard]] inline QString ist_time(qint64 ns) {
    return QDateTime::fromMSecsSinceEpoch(ns / 1'000'000, QTimeZone::utc()).addSecs(19800).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}
[[nodiscard]] inline QString money(double v) { return QString::number(v, 'f', 2); }

[[nodiscard]] inline QTableWidget* table(QWidget* parent, const QStringList& head, const char* name) {
    auto* t = new QTableWidget(0, static_cast<int>(head.size()), parent);
    t->setObjectName(QString::fromLatin1(name));
    t->setHorizontalHeaderLabels(head);
    t->verticalHeader()->hide();
    t->verticalHeader()->setDefaultSectionSize(22);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setAlternatingRowColors(true);
    t->horizontalHeader()->setStretchLastSection(true);
    t->setSortingEnabled(false);
    return t;
}

inline void fill(QTableWidget* t, const std::vector<QStringList>& rows) {
    t->setRowCount(static_cast<int>(rows.size()));
    for (int r = 0; r < static_cast<int>(rows.size()); ++r)
        for (int c = 0; c < rows[static_cast<std::size_t>(r)].size() && c < t->columnCount(); ++c) {
            auto* it = t->item(r, c);
            if (it == nullptr) { it = new QTableWidgetItem; t->setItem(r, c, it); }
            const QString& text = rows[static_cast<std::size_t>(r)][c];
            if (it->text() != text) it->setText(text);
        }
}

} // namespace demo_reports_detail

/// Every demo fill, newest first: the manual paper book and the models.
struct DemoFill {
    QString time, book, symbol, side;
    qint64 qty = 0;
    double price = 0.0, expenses = 0.0;
};

[[nodiscard]] inline std::vector<DemoFill> read_demo_fills(const QString& paper_dir, bool today_only, const QString& today) {
    using namespace demo_reports_detail;
    std::vector<DemoFill> out;
    for (const auto& r : read_csv(paper_dir + QStringLiteral("/fills.csv"))) {
        DemoFill f;
        f.time = r.value(QStringLiteral("time"));
        f.book = r.value(QStringLiteral("model"));
        f.symbol = r.value(QStringLiteral("symbol"));
        f.side = r.value(QStringLiteral("side")).toUpper().left(1) == QLatin1String("B") ? QStringLiteral("BUY") : QStringLiteral("SELL");
        f.qty = r.value(QStringLiteral("qty")).toLongLong();
        f.price = r.value(QStringLiteral("price")).toDouble();
        f.expenses = r.value(QStringLiteral("expenses")).toDouble();
        out.push_back(std::move(f));
    }
    for (const auto& r : read_csv(paper_dir + QStringLiteral("/manual_trades.csv"))) {
        DemoFill f;
        f.time = ist_time(r.value(QStringLiteral("ns")).toLongLong());
        f.book = QStringLiteral("Manual (paper)");
        f.symbol = r.value(QStringLiteral("symbol"));
        f.side = r.value(QStringLiteral("side")) == QLatin1String("B") ? QStringLiteral("BUY") : QStringLiteral("SELL");
        f.qty = r.value(QStringLiteral("qty")).toLongLong();
        f.price = r.value(QStringLiteral("price")).toDouble() / 100.0;
        f.expenses = r.value(QStringLiteral("expenses")).toDouble();
        out.push_back(std::move(f));
    }
    if (today_only)
        out.erase(std::remove_if(out.begin(), out.end(), [&today](const DemoFill& f) { return !f.time.startsWith(today); }), out.end());
    std::sort(out.begin(), out.end(), [](const DemoFill& a, const DemoFill& b) { return a.time > b.time; });
    return out;
}

/// The demo books' totals: per book, fills, turnover, expenses, and the round
/// trips' gross and net; margin from the engine's latest estimate.
struct DemoBookTotals {
    int fills = 0, trips = 0;
    double turnover = 0, expenses = 0, gross = 0, net = 0, margin = std::numeric_limits<double>::quiet_NaN();
};

[[nodiscard]] inline std::map<QString, DemoBookTotals> demo_totals(const QString& paper_dir, bool today_only, const QString& today) {
    using namespace demo_reports_detail;
    std::map<QString, DemoBookTotals> out;
    for (const auto& f : read_demo_fills(paper_dir, today_only, today)) {
        auto& t = out[f.book];
        ++t.fills;
        t.turnover += f.price * static_cast<double>(f.qty);
        t.expenses += f.expenses;
    }
    for (const auto& r : read_csv(paper_dir + QStringLiteral("/trades.csv"))) {
        if (today_only && !r.value(QStringLiteral("date")).startsWith(today)) continue;
        auto& t = out[r.value(QStringLiteral("model"))];
        ++t.trips;
        t.gross += r.value(QStringLiteral("gross")).toDouble();
        t.net += r.value(QStringLiteral("net")).toDouble();
    }
    for (const auto& r : read_csv(paper_dir + QStringLiteral("/margin.csv"))) {
        auto it = out.find(r.value(QStringLiteral("model")));
        if (it != out.end()) it->second.margin = r.value(QStringLiteral("margin_estimate")).toDouble();   // the last line wins
    }
    return out;
}

/// Trade History · Demo.
class DemoTradeHistory final : public QWidget {
public:
    explicit DemoTradeHistory(QString paper_dir, QWidget* parent = nullptr) : QWidget(parent), dir_(std::move(paper_dir)) {
        setObjectName(QStringLiteral("demoTradeHistory"));
        auto* v = new QVBoxLayout(this);
        auto* bar = new QHBoxLayout;
        today_ = new QCheckBox(QStringLiteral("Today only"), this);
        today_->setChecked(true);
        bar->addWidget(new QLabel(QStringLiteral("<b>DEMO</b> · every paper fill: the manual paper book (F1/F2 in PAPER) and the models"), this));
        bar->addStretch();
        bar->addWidget(today_);
        v->addLayout(bar);
        table_ = demo_reports_detail::table(this, {QStringLiteral("Time (IST)"), QStringLiteral("Book"), QStringLiteral("Symbol"),
                                                   QStringLiteral("Side"), QStringLiteral("Qty"), QStringLiteral("Price"),
                                                   QStringLiteral("Expenses")}, "demoTrades");
        v->addWidget(table_, 1);
        QObject::connect(today_, &QCheckBox::toggled, this, [this] { refresh(); });
        timer_.setInterval(3000);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] { if (isVisible()) refresh(); });
        timer_.start();
    }
    void set_dir(const QString& d) { dir_ = d; refresh(); }
    void set_today(const QString& iso) { day_ = iso; refresh(); }
    [[nodiscard]] QTableWidget* table() const noexcept { return table_; }
    void refresh() {
        std::vector<QStringList> rows;
        for (const auto& f : read_demo_fills(dir_, today_->isChecked(), day())) {
            rows.push_back({f.time, f.book, f.symbol, f.side, QString::number(f.qty), demo_reports_detail::money(f.price),
                            demo_reports_detail::money(f.expenses)});
        }
        demo_reports_detail::fill(table_, rows);
    }

protected:
    void showEvent(QShowEvent* e) override { refresh(); QWidget::showEvent(e); }

private:
    [[nodiscard]] QString day() const { return day_.isEmpty() ? QDate::currentDate().toString(Qt::ISODate) : day_; }
    QString dir_, day_;
    QCheckBox* today_ = nullptr;
    QTableWidget* table_ = nullptr;
    QTimer timer_;
};

/// Expense & Margin · Demo.
class DemoExpenseMargin final : public QWidget {
public:
    explicit DemoExpenseMargin(QString paper_dir, QWidget* parent = nullptr) : QWidget(parent), dir_(std::move(paper_dir)) {
        setObjectName(QStringLiteral("demoExpenseMargin"));
        auto* v = new QVBoxLayout(this);
        auto* bar = new QHBoxLayout;
        today_ = new QCheckBox(QStringLiteral("Today only"), this);
        today_->setChecked(true);
        bar->addWidget(new QLabel(QStringLiteral("<b>DEMO</b> · per book: what the fills cost (brokerage, STT, exchange, SEBI, "
                                                 "stamp, GST) and the round trips with and without them"), this));
        bar->addStretch();
        bar->addWidget(today_);
        v->addLayout(bar);
        table_ = demo_reports_detail::table(this, {QStringLiteral("Book"), QStringLiteral("Fills"), QStringLiteral("Turnover"),
                                                   QStringLiteral("Expenses"), QStringLiteral("Round trips"),
                                                   QStringLiteral("P&L without expenses"), QStringLiteral("P&L with expenses"),
                                                   QStringLiteral("Margin (engine estimate)")}, "demoExpense");
        v->addWidget(table_, 1);
        QObject::connect(today_, &QCheckBox::toggled, this, [this] { refresh(); });
        timer_.setInterval(3000);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] { if (isVisible()) refresh(); });
        timer_.start();
    }
    void set_dir(const QString& d) { dir_ = d; refresh(); }
    void set_today(const QString& iso) { day_ = iso; refresh(); }
    [[nodiscard]] QTableWidget* table() const noexcept { return table_; }
    void refresh() {
        using demo_reports_detail::money;
        std::vector<QStringList> rows;
        DemoBookTotals all;
        for (const auto& [book, t] : demo_totals(dir_, today_->isChecked(), day())) {
            rows.push_back({book, QString::number(t.fills), money(t.turnover), money(t.expenses), QString::number(t.trips),
                            money(t.gross), money(t.net), std::isfinite(t.margin) ? money(t.margin) : QStringLiteral("—")});
            all.fills += t.fills; all.trips += t.trips; all.turnover += t.turnover; all.expenses += t.expenses;
            all.gross += t.gross; all.net += t.net;
        }
        rows.push_back({QStringLiteral("TOTAL"), QString::number(all.fills), money(all.turnover), money(all.expenses),
                        QString::number(all.trips), money(all.gross), money(all.net), QString()});
        demo_reports_detail::fill(table_, rows);
    }

protected:
    void showEvent(QShowEvent* e) override { refresh(); QWidget::showEvent(e); }

private:
    [[nodiscard]] QString day() const { return day_.isEmpty() ? QDate::currentDate().toString(Qt::ISODate) : day_; }
    QString dir_, day_;
    QCheckBox* today_ = nullptr;
    QTableWidget* table_ = nullptr;
    QTimer timer_;
};

/// RMS · Demo: what the demo books hold, marked to the stream, against a
/// day-loss limit of your own.
class DemoRms final : public QWidget {
public:
    DemoRms(QString paper_dir, std::function<double(quint32)> ltp, QWidget* parent = nullptr)
        : QWidget(parent), dir_(std::move(paper_dir)), ltp_(std::move(ltp)) {
        setObjectName(QStringLiteral("demoRms"));
        auto* v = new QVBoxLayout(this);
        auto* bar = new QHBoxLayout;
        max_loss_ = new QDoubleSpinBox(this);
        max_loss_->setRange(0, 1e8);
        max_loss_->setDecimals(0);
        max_loss_->setValue(50000);
        max_loss_->setPrefix(QStringLiteral("Rs "));
        warn_ = new QDoubleSpinBox(this);
        warn_->setRange(10, 100);
        warn_->setValue(80);
        warn_->setSuffix(QStringLiteral(" %"));
        bar->addWidget(new QLabel(QStringLiteral("<b>DEMO</b> · day-loss limit"), this));
        bar->addWidget(max_loss_);
        bar->addWidget(new QLabel(QStringLiteral("warn at"), this));
        bar->addWidget(warn_);
        bar->addStretch();
        v->addLayout(bar);
        state_ = new QLabel(this);
        state_->setObjectName(QStringLiteral("demoRmsState"));
        state_->setTextFormat(Qt::RichText);
        v->addWidget(state_);
        table_ = demo_reports_detail::table(this, {QStringLiteral("Book"), QStringLiteral("Symbol"), QStringLiteral("Side"),
                                                   QStringLiteral("Qty"), QStringLiteral("Entry"), QStringLiteral("LTP"),
                                                   QStringLiteral("MTM"), QStringLiteral("Why")}, "demoRmsPositions");
        v->addWidget(table_, 1);
        timer_.setInterval(2000);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] { if (isVisible()) refresh(); });
        timer_.start();
    }
    void set_dir(const QString& d) { dir_ = d; refresh(); }
    void set_today(const QString& iso) { day_ = iso; refresh(); }
    [[nodiscard]] QTableWidget* table() const noexcept { return table_; }
    [[nodiscard]] QString state_text() const { return state_->text(); }
    void refresh() {
        using namespace demo_reports_detail;
        std::vector<QStringList> rows;
        double mtm = 0.0;
        // The models' open positions.
        for (const auto& r : read_csv(dir_ + QStringLiteral("/open_positions.csv"))) {
            const int side = r.value(QStringLiteral("side")).toInt();
            const qint64 qty = r.value(QStringLiteral("qty")).toLongLong();
            const double entry = r.value(QStringLiteral("entry")).toDouble();
            const double px = ltp_ ? ltp_(r.value(QStringLiteral("token")).toUInt()) : 0.0;
            const double m = px > 0 ? (px - entry) * static_cast<double>(side * qty) : 0.0;
            mtm += m;
            rows.push_back({r.value(QStringLiteral("model")), r.value(QStringLiteral("symbol")), side > 0 ? QStringLiteral("BUY") : QStringLiteral("SELL"),
                            QString::number(qty), money(entry), px > 0 ? money(px) : QStringLiteral("—"), px > 0 ? money(m) : QStringLiteral("—"),
                            r.value(QStringLiteral("why_in"))});
        }
        // The manual paper book: net per instrument from its fills.
        struct Net { QString symbol; qint64 qty = 0; double cost = 0; };
        std::map<quint32, Net> manual;
        for (const auto& r : read_csv(dir_ + QStringLiteral("/manual_trades.csv"))) {
            Net& n = manual[r.value(QStringLiteral("token")).toUInt()];
            n.symbol = r.value(QStringLiteral("symbol"));
            const qint64 q = r.value(QStringLiteral("qty")).toLongLong() * (r.value(QStringLiteral("side")) == QLatin1String("B") ? 1 : -1);
            n.qty += q;
            n.cost += static_cast<double>(q) * r.value(QStringLiteral("price")).toDouble() / 100.0;
        }
        double manual_realised = 0.0;
        for (const auto& [tok, n] : manual) {
            const double px = ltp_ ? ltp_(tok) : 0.0;
            if (n.qty == 0) { manual_realised += -n.cost; continue; }
            const double avg = n.cost / static_cast<double>(n.qty);
            const double m = px > 0 ? (px - avg) * static_cast<double>(n.qty) : 0.0;
            mtm += m;
            rows.push_back({QStringLiteral("Manual (paper)"), n.symbol, n.qty > 0 ? QStringLiteral("BUY") : QStringLiteral("SELL"),
                            QString::number(std::llabs(n.qty)), money(avg), px > 0 ? money(px) : QStringLiteral("—"),
                            px > 0 ? money(m) : QStringLiteral("—"), QString()});
        }
        fill(table_, rows);
        double realised = manual_realised;
        const QString today = day_.isEmpty() ? QDate::currentDate().toString(Qt::ISODate) : day_;
        for (const auto& r : read_csv(dir_ + QStringLiteral("/trades.csv")))
            if (r.value(QStringLiteral("date")).startsWith(today)) realised += r.value(QStringLiteral("net")).toDouble();
        const double day = realised + mtm;
        const double used = max_loss_->value() > 0 && day < 0 ? -day / max_loss_->value() * 100.0 : 0.0;
        const QString status = used >= 100.0 ? QStringLiteral("<b style='color:#F85149'>STOP — day loss limit reached</b>")
                             : used >= warn_->value() ? QStringLiteral("<b style='color:#E3B341'>WARN</b>")
                                                      : QStringLiteral("<b style='color:#7EE787'>OK</b>");
        state_->setText(QStringLiteral("%1 · realised today (net of expenses) Rs %2 · open MTM Rs %3 · day Rs %4 · %5 % of the limit used")
                            .arg(status, money(realised), money(mtm), money(day)).arg(used, 0, 'f', 1));
    }

protected:
    void showEvent(QShowEvent* e) override { refresh(); QWidget::showEvent(e); }

private:
    QString dir_, day_;
    std::function<double(quint32)> ltp_;
    QDoubleSpinBox* max_loss_ = nullptr;
    QDoubleSpinBox* warn_ = nullptr;
    QLabel* state_ = nullptr;
    QTableWidget* table_ = nullptr;
    QTimer timer_;
};

} // namespace altair::ui
