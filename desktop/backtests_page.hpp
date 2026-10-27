// desktop/backtests_page.hpp -- every backtest and every trade, in one place.
//
// Smit asked for "everything with trade logs". The trades already exist as
// CSV files: the demo-trading CLIs write data/verified/<run>/*trades*.csv,
// the threshold replication writes data/verified/threshold/, the models
// engine's paper book is data/live/paper/trades.csv and real strategy orders
// are data/live_orders/strategy_trades.csv. This page finds them all, lists
// them with their trade count and date, and shows the one you pick: every
// trade (sortable, searchable, double-click for the full row and its expense
// walk), the statistics (desktop/trade_stats.hpp) and the equity curve.
//
// Reads files; computes no backtest and places nothing. A run is started
// from its own page (Demo Trading, Threshold, the Terminal's models).
#pragma once

#include "demo_trading_page.hpp"
#include "strategy_record.hpp"
#include "trade_reason.hpp"
#include "theme.hpp"
#include "trade_stats.hpp"

#include <QDesktopServices>
#include <QDirIterator>
#include <QFileDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QSaveFile>
#include <QSplitter>
#include <QUrl>

#include <cmath>
#include <limits>
#include <vector>

namespace altair::ui {

/// One trade log found on disk.
struct BacktestLog {
    QString label;   ///< what the list shows
    QString path;    ///< absolute
    QString group;   ///< "Backtest", "Paper (live models)", "Real orders"
};

namespace backtests_detail {

/// The first column present, by name, case-insensitive; -1 when none.
[[nodiscard]] inline int find_column(const demo_detail::Csv& csv, std::initializer_list<const char*> names) {
    for (const char* n : names)
        for (int c = 0; c < csv.header.size(); ++c)
            if (csv.header[c].compare(QLatin1String(n), Qt::CaseInsensitive) == 0) return c;
    return -1;
}

[[nodiscard]] inline QString date_of(const QString& raw) {
    // "2026-10-24", "2026-10-24 09:20:00", "2026-10-24T09:20" -> the day.
    const QString t = raw.trimmed().left(10);
    return QDate::fromString(t, Qt::ISODate).isValid() ? t : QString{};
}

/// The rows as trade_stats wants them. Net from a net column, or gross minus
/// expenses; a row with neither stays unpriced.
[[nodiscard]] inline std::vector<TradeStatRow> stat_rows(const demo_detail::Csv& csv) {
    const int net = find_column(csv, {"net_pnl", "net", "pnl_net", "net_rupees", "net_inr", "net_rs_per_lot", "net_rs_per_unit", "net_rs", "net_per_trade"});
    const int gross = find_column(csv, {"gross_pnl", "gross", "pnl_gross", "gross_rs_per_unit", "pnl"});
    const int exp = find_column(csv, {"expenses", "costs", "cost", "charges"});
    const int date = find_column(csv, {"exit_date", "date", "day", "exit_day", "exit_time", "exit", "entry_date", "entry_time", "time"});
    std::vector<TradeStatRow> out;
    out.reserve(csv.rows.size());
    const auto value = [](const QStringList& row, int c) {
        bool ok = false;
        const double v = c >= 0 && c < row.size() ? row[c].toDouble(&ok) : 0.0;
        return ok ? v : std::numeric_limits<double>::quiet_NaN();
    };
    for (const auto& row : csv.rows) {
        TradeStatRow r;
        r.date = date >= 0 && date < row.size() ? date_of(row[date]) : QString{};
        const double g = value(row, gross), e = value(row, exp), n = value(row, net);
        r.gross = std::isfinite(g) ? g : (std::isfinite(n) ? n : 0.0);
        if (std::isfinite(n)) {
            r.net = n;
            r.expenses = std::isfinite(e) ? e : (std::isfinite(g) ? g - n : 0.0);
        } else if (std::isfinite(g) && std::isfinite(e)) {
            r.expenses = e;
            r.net = g - e;
        }
        out.push_back(r);
    }
    return out;
}

/// Cumulative gross and net by day, for the equity chart.
[[nodiscard]] inline std::vector<demo_detail::EquityChart::Point> equity(const std::vector<TradeStatRow>& rows) {
    std::map<QString, std::pair<double, double>> by_day;
    for (const auto& r : rows) {
        if (r.date.isEmpty()) continue;
        by_day[r.date].first += r.gross;
        if (std::isfinite(r.net)) by_day[r.date].second += r.net;
    }
    std::vector<demo_detail::EquityChart::Point> pts;
    double g = 0.0, n = 0.0;
    for (const auto& [d, v] : by_day) {
        g += v.first;
        n += v.second;
        pts.push_back({QDate::fromString(d, Qt::ISODate), g, n});
    }
    return pts;
}

} // namespace backtests_detail

/// Every trade log under `root`, backtests first (newest run first), then
/// the live paper book and real strategy orders.
[[nodiscard]] inline std::vector<BacktestLog> find_backtest_logs(const QString& root) {
    std::vector<BacktestLog> out;
    const QString verified = root + QStringLiteral("/data/verified");
    std::vector<std::pair<QDateTime, BacktestLog>> runs;
    QDirIterator it(verified, {QStringLiteral("*trade*.csv")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QFileInfo f(path);
        QString rel = QDir(verified).relativeFilePath(path);
        rel.chop(4);   // ".csv"
        // The forecast curriculum's logs are calls, not trades.
        if (rel.startsWith(QLatin1String("forecast_log/"))) continue;
        runs.push_back({f.lastModified(), {rel.replace(QLatin1Char('/'), QStringLiteral(" · ")), f.absoluteFilePath(),
                                           QStringLiteral("Backtest")}});
    }
    std::sort(runs.begin(), runs.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second.label < b.second.label;
    });
    for (auto& [t, log] : runs) out.push_back(std::move(log));
    const auto add = [&](const char* rel, const char* label, const char* group) {
        const QFileInfo f(root + QLatin1Char('/') + QLatin1String(rel));
        if (f.exists()) out.push_back({QString::fromUtf8(label), f.absoluteFilePath(), QString::fromUtf8(group)});
    };
    add(kDemoTradesFile, "Live models · paper trades", "Paper (live models)");
    add(kRealStrategyTradesFile, "Strategies · real orders", "Real orders");
    return out;
}

class BacktestsPage final : public QWidget {
public:
    /// `only`: show just the logs whose path contains one of these words
    /// (case-insensitive), for a strategy page's own trade tab; empty is all.
    explicit BacktestsPage(QString root, QWidget* parent = nullptr, QStringList only = {})
        : QWidget(parent), root_(std::move(root)), only_(std::move(only)) {
        setObjectName(only_.isEmpty() ? QStringLiteral("backtestsPage") : QStringLiteral("backtestsFiltered"));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(16, 14, 16, 12);
        v->setSpacing(10);

        auto* head = new QHBoxLayout;
        auto* title = new QLabel(QStringLiteral("Backtests · every trade"), this);
        set_role(title, QStringLiteral("title"));
        head->addWidget(title);
        head->addStretch();
        refresh_ = new QPushButton(QStringLiteral("Refresh"), this);
        refresh_->setToolTip(QStringLiteral("Look for new runs on disk"));
        head->addWidget(refresh_);
        v->addLayout(head);
        auto* note = new QLabel(QStringLiteral(
            "Every trade log on this PC: the backtests (Demo Trading, Threshold and the research CLIs write them to "
            "data/verified/), the live models' paper book and real strategy orders. Pick one to see each trade, its "
            "statistics after expenses and the equity curve. Double-click a trade for every column and the gross-to-net walk. "
            "Runs start from their own pages; this one only reads."), this);
        note->setWordWrap(true);
        set_role(note, QStringLiteral("note"));
        v->addWidget(note);

        auto* split = new QSplitter(Qt::Horizontal, this);
        list_ = new QListWidget(split);
        list_->setObjectName(QStringLiteral("backtestList"));
        list_->setMinimumWidth(240);
        split->addWidget(list_);

        auto* right = new QWidget(split);
        auto* rv = new QVBoxLayout(right);
        rv->setContentsMargins(0, 0, 0, 0);
        rv->setSpacing(8);
        heading_ = new QLabel(right);
        heading_->setTextFormat(Qt::RichText);
        heading_->setWordWrap(true);
        rv->addWidget(heading_);
        stats_ = new QLabel(right);
        stats_->setObjectName(QStringLiteral("backtestStats"));
        stats_->setTextFormat(Qt::RichText);
        stats_->setWordWrap(true);
        rv->addWidget(stats_);
        chart_ = new demo_detail::EquityChart(right);
        chart_->setMinimumHeight(170);
        chart_->setMaximumHeight(220);
        rv->addWidget(chart_);
        auto* bar = new QHBoxLayout;
        find_ = new QLineEdit(right);
        find_->setPlaceholderText(QStringLiteral("Search the trades (any column: symbol, date, reason…)"));
        find_->setClearButtonEnabled(true);
        bar->addWidget(find_, 1);
        shown_ = new QLabel(right);
        set_role(shown_, QStringLiteral("note"));
        bar->addWidget(shown_);
        export_ = new QPushButton(QStringLiteral("Export shown trades…"), right);
        open_folder_ = new QPushButton(QStringLiteral("Open folder"), right);
        bar->addWidget(export_);
        bar->addWidget(open_folder_);
        rv->addLayout(bar);
        model_ = new demo_detail::CsvModel(this);
        proxy_ = new QSortFilterProxyModel(this);
        proxy_->setSourceModel(model_);
        proxy_->setSortRole(Qt::UserRole);
        proxy_->setFilterKeyColumn(-1);
        proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
        table_ = demo_detail::table_view(proxy_, right);
        table_->setObjectName(QStringLiteral("backtestTrades"));
        table_->setAlternatingRowColors(true);
        rv->addWidget(table_, 1);
        split->addWidget(right);
        split->setStretchFactor(0, 1);
        split->setStretchFactor(1, 4);
        v->addWidget(split, 1);

        connect(refresh_, &QPushButton::clicked, this, [this] { rescan(); });
        connect(list_, &QListWidget::currentRowChanged, this, [this](int row) { open_log(row); });
        connect(find_, &QLineEdit::textChanged, this, [this](const QString& q) {
            proxy_->setFilterFixedString(q);
            update_shown();
        });
        connect(export_, &QPushButton::clicked, this, [this] { export_shown(); });
        connect(open_folder_, &QPushButton::clicked, this, [this] {
            if (const BacktestLog* log = current()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(log->path).absolutePath()));
        });
        connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
            const QModelIndex src = proxy_->mapToSource(index);
            QDialog d(this);
            d.setWindowTitle(QStringLiteral("Trade: why, fills, forecasts, every column"));
            auto* l = new QVBoxLayout(&d);
            auto* text = new QTextBrowser(&d);
            text->setObjectName(QStringLiteral("tradeReason"));
            text->setHtml(trade_html(src.row()));
            l->addWidget(text);
            d.resize(760, 680);
            d.exec();
        });
        rescan();
    }

    /// Look for logs on disk again, keeping the selection when it still exists.
    void rescan() {
        const QString keep = current() ? current()->path : QString{};
        logs_ = find_backtest_logs(root_);
        if (!only_.isEmpty())
            std::erase_if(logs_, [this](const BacktestLog& log) {
                const QString rel = QDir(root_).relativeFilePath(log.path);
                for (const QString& w : only_)
                    if (rel.contains(w, Qt::CaseInsensitive)) return false;
                return true;
            });
        const QSignalBlocker block(list_);
        list_->clear();
        int select = logs_.empty() ? -1 : 0;
        QString group;
        for (std::size_t i = 0; i < logs_.size(); ++i) {
            const auto& log = logs_[i];
            const QFileInfo f(log.path);
            auto* item = new QListWidgetItem(QStringLiteral("%1\n%2 · %3").arg(log.label, log.group,
                                                 f.lastModified().toString(QStringLiteral("dd MMM yyyy HH:mm"))), list_);
            item->setToolTip(log.path);
            if (log.path == keep) select = static_cast<int>(i);
        }
        if (logs_.empty()) {
            heading_->setText(only_.isEmpty()
                                  ? QStringLiteral("<b>No trade logs yet.</b> Run a backtest from <b>Demo Trading</b> or "
                                                   "<b>Threshold</b>, or let the live models paper-trade; their trades appear here.")
                                  : QStringLiteral("<b>No saved runs for this strategy yet.</b> Build and save one on the "
                                                   "<b>Study trades</b> tab, or run it from Demo Trading; it appears here and on "
                                                   "the Backtests page."));
            stats_->clear();
            model_->set({});
            chart_->set({}, QString{}, false);
            update_shown();
            return;
        }
        list_->setCurrentRow(select);
        open_log(select);
    }

    [[nodiscard]] int log_count() const noexcept { return static_cast<int>(logs_.size()); }
    /// The reasoning (desktop/trade_reason.hpp), then every column of trade `row`.
    [[nodiscard]] QString trade_html(int row) const {
        const auto& csv = model_->csv();
        if (row < 0 || row >= static_cast<int>(csv.rows.size())) return {};
        return trade_reason_html(root_, trade_context_from_row(csv.header, csv.rows[static_cast<std::size_t>(row)]))
               + QStringLiteral("<hr>") + DemoTradingPage::trade_detail_html(model_, row);
    }
    [[nodiscard]] int trade_count() const { return model_->rowCount(); }
    [[nodiscard]] QString stats_text() const { return stats_->text(); }
    bool select_log(int i) {
        if (i < 0 || i >= log_count()) return false;
        list_->setCurrentRow(i);
        return true;
    }

protected:
    void showEvent(QShowEvent* e) override {
        QWidget::showEvent(e);
        rescan();
    }

private:
    QString root_;
    QStringList only_;
    std::vector<BacktestLog> logs_;
    QListWidget* list_ = nullptr;
    QLabel* heading_ = nullptr;
    QLabel* stats_ = nullptr;
    QLabel* shown_ = nullptr;
    demo_detail::EquityChart* chart_ = nullptr;
    QLineEdit* find_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QPushButton* export_ = nullptr;
    QPushButton* open_folder_ = nullptr;
    demo_detail::CsvModel* model_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
    QTableView* table_ = nullptr;

    [[nodiscard]] const BacktestLog* current() const {
        const int i = list_ ? list_->currentRow() : -1;
        return i >= 0 && i < log_count() ? &logs_[static_cast<std::size_t>(i)] : nullptr;
    }

    void open_log(int row) {
        if (row < 0 || row >= log_count()) return;
        const BacktestLog& log = logs_[static_cast<std::size_t>(row)];
        auto csv = demo_detail::read_csv(log.path);
        const auto rows = backtests_detail::stat_rows(csv);
        const TradeStats s = trade_stats(rows);
        heading_->setText(QStringLiteral("<b style='color:#F0B765'>%1</b> <span style='color:#8B949E'>· %2 · <code>%3</code></span>")
                              .arg(log.label.toHtmlEscaped(), log.group.toHtmlEscaped(),
                                   QDir(root_).relativeFilePath(log.path).toHtmlEscaped()));
        stats_->setText(csv.rows.empty() ? QStringLiteral("No trades in this log.")
                        : QStringLiteral("<b>Net %1</b> · gross %2 · expenses %3<br>%4")
                              .arg(trade_stats_detail::money(s.net), trade_stats_detail::money(s.gross),
                                   trade_stats_detail::money(s.expenses), trade_stats_html(s)));
        chart_->set(backtests_detail::equity(rows), QStringLiteral("Cumulative P&L by day"), s.priced == s.trips);
        model_->set(std::move(csv));
        update_shown();
    }

    void update_shown() {
        shown_->setText(QStringLiteral("%1 of %2 trades").arg(proxy_->rowCount()).arg(model_->rowCount()));
        export_->setEnabled(proxy_->rowCount() > 0);
        open_folder_->setEnabled(current() != nullptr);
    }

    void export_shown() {
        const BacktestLog* log = current();
        if (log == nullptr) return;
        const QString suggested = QDir::home().filePath(QFileInfo(log->path).completeBaseName() + QStringLiteral("_export.csv"));
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export trades"), suggested,
                                                          QStringLiteral("CSV (*.csv)"));
        if (path.isEmpty()) return;
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        const auto quote = [](QString s) {
            if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"')) || s.contains(QLatin1Char('\n')))
                s = QLatin1Char('"') + s.replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"');
            return s;
        };
        QStringList header;
        for (int c = 0; c < proxy_->columnCount(); ++c) header << quote(proxy_->headerData(c, Qt::Horizontal).toString());
        f.write(header.join(QLatin1Char(',')).toUtf8() + '\n');
        for (int r = 0; r < proxy_->rowCount(); ++r) {
            QStringList cells;
            for (int c = 0; c < proxy_->columnCount(); ++c) cells << quote(proxy_->index(r, c).data().toString());
            f.write(cells.join(QLatin1Char(',')).toUtf8() + '\n');
        }
        (void)f.commit();
    }
};

} // namespace altair::ui
