// desktop/demo_trading_page.hpp -- paper trades from the band and pairs demos.
//
// THIS PAGE PLACES NOTHING. It runs two research CLIs and shows what they
// wrote under data/verified/:
//   * altair_band_option_demo -- each band model's 09:20 forecast; the first
//     touch of an edge sells one lot of the option at the first strike past
//     it, bought back at 15:20 (strategies/band_option_fade.hpp).
//   * altair_pairs_futures -- pairs hedged with futures, long one leg and
//     short the other in whole lots, walked forward (strategies/pairs_futures.hpp).
// Option premiums are SYNTHETIC (Black-76 at INDIA VIX); the dataset has no
// option-chain history. Expenses come from config/charges.toml only; while
// that schedule is UNVERIFIED the CLIs refuse to price them unless asked,
// and then every expense and net figure carries the UNVERIFIED stamp, which
// this page shows in red rather than dropping.

#pragma once

#include "helper_process.hpp"
#include "theme.hpp"

#include <QAbstractTableModel>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTabWidget>
#include <QTableView>
#include <QTextStream>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace altair::ui {

namespace demo_detail {

/// One CSV line: commas, double quotes, doubled quotes inside them.
[[nodiscard]] inline QStringList split_csv(const QString& line) {
    QStringList out;
    QString cell;
    bool quoted = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (quoted) {
            if (c == QChar('"')) {
                if (i + 1 < line.size() && line[i + 1] == QChar('"')) { cell += c; ++i; }
                else { quoted = false; }
            } else {
                cell += c;
            }
        } else if (c == QChar('"')) {
            quoted = true;
        } else if (c == QChar(',')) {
            out << cell;
            cell.clear();
        } else {
            cell += c;
        }
    }
    out << cell;
    return out;
}

struct Csv {
    QStringList header;
    std::vector<QStringList> rows;
    [[nodiscard]] int column(const QString& name) const { return static_cast<int>(header.indexOf(name)); }
};

[[nodiscard]] inline Csv read_csv(const QString& path) {
    Csv out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    bool first = true;
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.trimmed().isEmpty()) continue;
        QStringList cells = split_csv(line);
        if (first) { out.header = std::move(cells); first = false; continue; }
        while (cells.size() < out.header.size()) cells << QString{};
        out.rows.push_back(std::move(cells));
    }
    return out;
}

[[nodiscard]] inline QString cell(const Csv& csv, const QStringList& row, const QString& name) {
    const int c = csv.column(name);
    return c >= 0 && c < row.size() ? row[c] : QString{};
}

[[nodiscard]] inline QString rupees(double v) {
    // U+2060 WORD JOINER: a wrapped line never splits the sign from the amount.
    return (v < 0.0 ? QStringLiteral("-\u2060₹%L1") : QStringLiteral("₹%L1")).arg(std::fabs(v), 0, 'f', 0);
}

/// A read-only table over a CSV; numbers sort as numbers.
class CsvModel final : public QAbstractTableModel {
public:
    using QAbstractTableModel::QAbstractTableModel;

    void set(Csv csv) {
        beginResetModel();
        csv_ = std::move(csv);
        endResetModel();
    }
    [[nodiscard]] const Csv& csv() const noexcept { return csv_; }

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(csv_.rows.size());
    }
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(csv_.header.size());
    }
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid()) return {};
        const QStringList& row = csv_.rows[static_cast<std::size_t>(index.row())];
        const QString text = index.column() < row.size() ? row[index.column()] : QString{};
        if (role == Qt::DisplayRole) return text;
        if (role == Qt::UserRole) {
            bool ok = false;
            const double v = text.toDouble(&ok);
            return ok ? QVariant(v) : QVariant(text);
        }
        if (role == Qt::ForegroundRole) {
            const QString& name = csv_.header[index.column()];
            if (text == QLatin1String("UNVERIFIED")) return QColor(QStringLiteral("#F85149"));
            if (name.endsWith(QLatin1String("pnl")) || name == QLatin1String("net_per_trade")) {
                bool ok = false;
                const double v = text.toDouble(&ok);
                if (ok && v != 0.0) return QColor(v > 0.0 ? QStringLiteral("#3FB950") : QStringLiteral("#F85149"));
            }
        }
        if (role == Qt::TextAlignmentRole) {
            bool ok = false;
            (void)text.toDouble(&ok);
            return ok ? QVariant(Qt::AlignRight | Qt::AlignVCenter) : QVariant(Qt::AlignLeft | Qt::AlignVCenter);
        }
        return {};
    }
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation o, int role) const override {
        if (o != Qt::Horizontal || role != Qt::DisplayRole || section >= csv_.header.size()) return {};
        return csv_.header[section];
    }

private:
    Csv csv_;
};

/// Rows whose named columns equal the chosen values ("" = any).
class ColumnFilter final : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void set_filter(std::vector<std::pair<int, QString>> filter) {
        filter_ = std::move(filter);
        invalidateFilter();
    }

protected:
    [[nodiscard]] bool filterAcceptsRow(int row, const QModelIndex& parent) const override {
        for (const auto& [column, want] : filter_) {
            if (column < 0 || want.isEmpty()) continue;
            if (sourceModel()->index(row, column, parent).data().toString() != want) return false;
        }
        return true;
    }

private:
    std::vector<std::pair<int, QString>> filter_;
};

/// Cumulative gross and net, summed across whatever the filter keeps.
class EquityChart final : public QWidget {
public:
    using QWidget::QWidget;

    struct Point { QDate date; double gross = 0.0, net = 0.0; };

    void set(std::vector<Point> points, QString title, bool net_verified) {
        points_ = std::move(points);
        title_ = std::move(title);
        net_verified_ = net_verified;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor(QString::fromLatin1(theme_token::kCanvas)));
        p.setPen(QColor(QString::fromLatin1(theme_token::kText)));
        p.drawText(QRect(12, 6, width() - 24, 20), Qt::AlignLeft | Qt::AlignVCenter, title_);
        if (points_.size() < 2) {
            p.setPen(QColor(QString::fromLatin1(theme_token::kTextMuted)));
            p.drawText(rect(), Qt::AlignCenter, QStringLiteral("No trades to plot. Run the option demo."));
            return;
        }
        double lo = 0.0, hi = 0.0;
        for (const Point& q : points_) {
            lo = std::min({lo, q.gross, q.net});
            hi = std::max({hi, q.gross, q.net});
        }
        if (!(hi > lo)) hi = lo + 1.0;
        const QRectF area(104.0, 32.0, std::max(10.0, width() - 120.0), std::max(10.0, height() - 60.0));
        const auto y_of = [&](double v) { return area.bottom() - (v - lo) / (hi - lo) * area.height(); };
        const auto x_of = [&](std::size_t i) {
            return area.left() + static_cast<double>(i) / static_cast<double>(points_.size() - 1) * area.width();
        };
        p.setPen(QPen(QColor(QString::fromLatin1(theme_token::kBorder)), 1.0));
        p.drawRect(area);
        p.setPen(QPen(QColor(QString::fromLatin1(theme_token::kBorderHi)), 1.0, Qt::DashLine));
        p.drawLine(QPointF(area.left(), y_of(0.0)), QPointF(area.right(), y_of(0.0)));
        p.setPen(QColor(QString::fromLatin1(theme_token::kTextMuted)));
        for (const double v : {hi, 0.0, lo}) {
            if (v == 0.0 && (y_of(hi) > y_of(0.0) - 18.0 || y_of(lo) < y_of(0.0) + 18.0)) continue;   // would overlap
            p.drawText(QRectF(0.0, y_of(v) - 9.0, 98.0, 18.0), Qt::AlignRight | Qt::AlignVCenter, rupees(v));
        }
        p.drawText(QRectF(area.left(), area.bottom() + 4.0, 120.0, 18.0), Qt::AlignLeft,
                   points_.front().date.toString(Qt::ISODate));
        p.drawText(QRectF(area.right() - 120.0, area.bottom() + 4.0, 120.0, 18.0), Qt::AlignRight,
                   points_.back().date.toString(Qt::ISODate));

        const auto line = [&](auto value, const QColor& colour) {
            QPainterPath path;
            path.moveTo(x_of(0), y_of(value(points_[0])));
            for (std::size_t i = 1; i < points_.size(); ++i) path.lineTo(x_of(i), y_of(value(points_[i])));
            p.setPen(QPen(colour, 1.6));
            p.drawPath(path);
        };
        line([](const Point& q) { return q.gross; }, QColor(QString::fromLatin1(theme_token::kLink)));
        line([](const Point& q) { return q.net; },
             net_verified_ ? QColor(QString::fromLatin1(theme_token::kAccent)) : QColor(QStringLiteral("#F85149")));
        p.setPen(QColor(QString::fromLatin1(theme_token::kLink)));
        p.drawText(QRectF(area.right() - 330.0, 8.0, 100.0, 18.0), Qt::AlignRight, QStringLiteral("— gross"));
        p.setPen(net_verified_ ? QColor(QString::fromLatin1(theme_token::kAccent)) : QColor(QStringLiteral("#F85149")));
        p.drawText(QRectF(area.right() - 220.0, 8.0, 220.0, 18.0), Qt::AlignRight,
                   net_verified_ ? QStringLiteral("— net of expenses") : QStringLiteral("— net (UNVERIFIED expenses)"));
    }

private:
    std::vector<Point> points_;
    QString title_;
    bool net_verified_ = false;
};

[[nodiscard]] inline QTableView* table_view(QAbstractItemModel* model, QWidget* parent) {
    auto* view = new QTableView(parent);
    view->setModel(model);
    view->setSortingEnabled(true);
    view->sortByColumn(0, Qt::AscendingOrder);   // oldest trade first
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->verticalHeader()->setVisible(false);
    view->horizontalHeader()->setStretchLastSection(true);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view->horizontalHeader()->setResizeContentsPrecision(200);   // sample rows: 27k trades
    return view;
}

[[nodiscard]] inline QString find_helper(const QString& name) {
#if defined(_WIN32)
    const QString exe = name + QStringLiteral(".exe");
#else
    const QString exe = name;
#endif
    const QString app = QCoreApplication::applicationDirPath();
    QStringList tried{app + QStringLiteral("/") + exe, app + QStringLiteral("/../app/") + exe,
                      app + QStringLiteral("/../Helpers/") + exe, app + QStringLiteral("/../../net/app/") + exe};
#ifdef ALTAIR_SOURCE_DIR
    tried << QStringLiteral(ALTAIR_SOURCE_DIR "/build/net/app/") + exe;
#endif
    for (const QString& path : tried) {
        const QFileInfo file(path);
        if (file.exists() && file.isFile()) return file.canonicalFilePath();
    }
    return {};
}

} // namespace demo_detail

class DemoTradingPage final : public QWidget {
public:
    /// `root` holds data/verified/; empty means the source tree.
    explicit DemoTradingPage(QString root = {}, QWidget* parent = nullptr)
        : QWidget(parent), root_dir_(std::move(root)) {
        if (root_dir_.isEmpty()) {
#ifdef ALTAIR_SOURCE_DIR
            root_dir_ = QStringLiteral(ALTAIR_SOURCE_DIR);
#else
            root_dir_ = QCoreApplication::applicationDirPath();
#endif
        }
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(14, 12, 14, 8);
        v->setSpacing(6);

        auto* head = new QLabel(QStringLiteral(
            "<b>DEMO TRADING</b> &nbsp;·&nbsp; paper trades only: nothing here is sent to a broker. "
            "One lot each. Band models sell options at the edges their 09:20 forecast set, under the rule "
            "you pick, and buy them back by 15:20; pairs go long one future and short the other; the vol premium "
            "sells the monthly straddle delta hedged when implied vol beats a HAR forecast; stat-arb trades every NIFTY 50 "
            "stock's residual against the market and its sector."), this);
        head->setWordWrap(true);
        v->addWidget(head);

        banner_ = new QLabel(this);
        banner_->setWordWrap(true);
        banner_->setTextFormat(Qt::RichText);
        v->addWidget(banner_);

        auto* controls = new QHBoxLayout;
        // One selector and one button: each entry is a research CLI that writes under data/verified/.
        run_choice_ = new QComboBox(this);
        run_choice_->addItem(QStringLiteral("Option demo (band models)"), QStringLiteral("altair_band_option_demo"));
        run_choice_->addItem(QStringLiteral("Pairs with futures"), QStringLiteral("altair_pairs_futures"));
        run_choice_->addItem(QStringLiteral("Vol premium (hedged straddle)"), QStringLiteral("altair_vol_premium"));
        run_choice_->addItem(QStringLiteral("Stat-arb (NIFTY 50 residuals)"), QStringLiteral("altair_resid_reversion"));
        run_ = new QPushButton(QStringLiteral("Run"), this);
        unverified_ = new QCheckBox(QStringLiteral("Price UNVERIFIED expenses"), this);
        unverified_->setToolTip(QStringLiteral("Pass --unverified-costs: price expenses from config/charges.toml although "
                                               "it is not verified. Every expense and net figure is then stamped UNVERIFIED."));
        auto* reread = new QPushButton(QStringLiteral("Re-read"), this);
        status_ = new QLabel(this);
        status_->setStyleSheet(QStringLiteral("color:%1;").arg(QString::fromLatin1(theme_token::kTextMuted)));
        status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        controls->addWidget(run_choice_);
        controls->addWidget(run_);
        controls->addWidget(unverified_);
        controls->addWidget(reread);
        controls->addWidget(status_, 1);
        v->addLayout(controls);

        auto* filters = new QHBoxLayout;
        rule_ = new QComboBox(this);
        rule_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        instrument_ = new QComboBox(this);
        model_ = new QComboBox(this);
        model_->setMinimumContentsLength(22);
        model_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        model_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        totals_ = new QLabel(this);
        totals_->setWordWrap(true);
        filters->addWidget(new QLabel(QStringLiteral("Rule"), this));
        filters->addWidget(rule_);
        filters->addWidget(new QLabel(QStringLiteral("Instrument"), this));
        filters->addWidget(instrument_);
        filters->addWidget(new QLabel(QStringLiteral("Model"), this));
        filters->addWidget(model_);
        filters->addStretch(1);
        v->addLayout(filters);
        v->addWidget(totals_);

        tabs_ = new QTabWidget(this);
        summary_ = new demo_detail::CsvModel(this);
        trades_ = new demo_detail::CsvModel(this);
        pairs_summary_ = new demo_detail::CsvModel(this);
        pairs_trades_ = new demo_detail::CsvModel(this);
        pairs_windows_ = new demo_detail::CsvModel(this);
        vrp_summary_ = new demo_detail::CsvModel(this);
        sa_summary_ = new demo_detail::CsvModel(this);
        sa_trades_ = new demo_detail::CsvModel(this);
        vrp_trades_ = new demo_detail::CsvModel(this);
        summary_filter_ = new demo_detail::ColumnFilter(this);
        summary_filter_->setSourceModel(summary_);
        summary_filter_->setSortRole(Qt::UserRole);
        trade_filter_ = new demo_detail::ColumnFilter(this);
        trade_filter_->setSourceModel(trades_);
        trade_filter_->setSortRole(Qt::UserRole);
        const auto sorted = [this](QAbstractItemModel* m) {
            auto* s = new QSortFilterProxyModel(this);
            s->setSourceModel(m);
            s->setSortRole(Qt::UserRole);
            return s;
        };
        equity_ = new demo_detail::EquityChart(this);
        ratio_ = new QLabel(this);
        ratio_->setWordWrap(true);
        ratio_->setTextFormat(Qt::RichText);
        ratio_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        ratio_->setContentsMargins(12, 12, 12, 12);
        const auto view = [this](QAbstractItemModel* m) {
            views_.push_back(demo_detail::table_view(m, this));
            return views_.back();
        };
        tabs_->addTab(view(summary_filter_), QStringLiteral("Options · by model"));
        tabs_->addTab(view(trade_filter_), QStringLiteral("Options · trades"));
        tabs_->addTab(equity_, QStringLiteral("Options · equity"));
        tabs_->addTab(view(sorted(pairs_summary_)), QStringLiteral("Pairs · summary"));
        tabs_->addTab(view(sorted(pairs_trades_)), QStringLiteral("Pairs · trades"));
        tabs_->addTab(view(sorted(pairs_windows_)), QStringLiteral("Pairs · windows"));
        tabs_->addTab(view(sorted(vrp_summary_)), QStringLiteral("Vol premium · summary"));
        tabs_->addTab(view(sorted(vrp_trades_)), QStringLiteral("Vol premium · trades"));
        tabs_->addTab(view(sorted(sa_summary_)), QStringLiteral("Stat-arb · summary"));
        tabs_->addTab(view(sorted(sa_trades_)), QStringLiteral("Stat-arb · trades"));
        tabs_->addTab(ratio_, QStringLiteral("BANKNIFTY/NIFTY ratio"));
        v->addWidget(tabs_, 1);

        connect(run_, &QPushButton::clicked, this, [this] { run(run_choice_->currentData().toString()); });
        connect(reread, &QPushButton::clicked, this, [this] { reload(); });
        connect(rule_, &QComboBox::currentIndexChanged, this, [this](int) { load_rule(); });
        connect(instrument_, &QComboBox::currentIndexChanged, this, [this](int) { refilter(); });
        connect(model_, &QComboBox::currentIndexChanged, this, [this](int) { refilter(); });
        reload();
    }

    [[nodiscard]] QString banner_text() const { return banner_->text(); }
    [[nodiscard]] QString totals_text() const { return totals_->text(); }
    [[nodiscard]] QString ratio_text() const { return ratio_->text(); }
    [[nodiscard]] int shown_trades() const { return trade_filter_->rowCount(); }
    [[nodiscard]] QComboBox* model_filter() const { return model_; }
    [[nodiscard]] QComboBox* instrument_filter() const { return instrument_; }
    [[nodiscard]] QComboBox* rule_filter() const { return rule_; }
    [[nodiscard]] int shown_summary_rows() const { return summary_filter_->rowCount(); }
    [[nodiscard]] int vol_premium_trades() const { return vrp_trades_->rowCount(); }
    [[nodiscard]] int stat_arb_trades() const { return sa_trades_->rowCount(); }

private:
    [[nodiscard]] QString out_dir(const QString& name) const {
        return root_dir_ + QStringLiteral("/data/verified/") + name;
    }

    void reload() {
        const QString opt = out_dir(QStringLiteral("band_option_demo"));
        const auto meta = demo_detail::read_csv(opt + QStringLiteral("/meta.csv"));
        meta_.clear();
        for (const auto& row : meta.rows) {
            if (row.size() >= 2) meta_[row[0]] = row[1];
        }
        have_meta_ = !meta.rows.empty();
        summary_->set(demo_detail::read_csv(opt + QStringLiteral("/summary.csv")));
        pairs_summary_->set(demo_detail::read_csv(out_dir(QStringLiteral("pairs_futures")) + QStringLiteral("/summary.csv")));
        pairs_trades_->set(demo_detail::read_csv(out_dir(QStringLiteral("pairs_futures")) + QStringLiteral("/trades.csv")));
        pairs_windows_->set(demo_detail::read_csv(out_dir(QStringLiteral("pairs_futures")) + QStringLiteral("/windows.csv")));
        {
            // Every variant's trades in one table; they share a header and carry a variant column.
            const QString vdir = out_dir(QStringLiteral("vol_premium"));
            vrp_summary_->set(demo_detail::read_csv(vdir + QStringLiteral("/summary.csv")));
            demo_detail::Csv all;
            const QStringList files = QDir(vdir).entryList({QStringLiteral("trades_*.csv")}, QDir::Files, QDir::Name);
            for (const QString& f : files) {
                auto one = demo_detail::read_csv(vdir + QLatin1Char('/') + f);
                if (all.header.isEmpty() && one.column(QStringLiteral("variant")) >= 0) all.header = one.header;
                if (one.header != all.header) continue;   // a layout this page does not know
                for (auto& row : one.rows) all.rows.push_back(std::move(row));
            }
            vrp_trades_->set(std::move(all));
        }
        sa_summary_->set(demo_detail::read_csv(out_dir(QStringLiteral("resid_reversion")) + QStringLiteral("/summary.csv")));
        sa_trades_->set(demo_detail::read_csv(out_dir(QStringLiteral("resid_reversion")) + QStringLiteral("/trades_market+sector.csv")));
        const QString costs = meta_.count(QStringLiteral("costs")) ? meta_[QStringLiteral("costs")] : QString{};
        costs_verified_ = costs == QLatin1String("verified");

        // Rules in the order the CLI ran them; an output from before rules
        // existed has one unnamed rule and a single trades.csv.
        const auto& sc = summary_->csv();
        const int rc = sc.column(QStringLiteral("rule"));
        QStringList rules;
        if (rc >= 0) {
            for (const auto& row : sc.rows) {
                if (rc < row.size() && !rules.contains(row[rc])) rules << row[rc];
            }
        }
        {
            const QString keep = rule_->currentText();
            const QSignalBlocker block(rule_);
            rule_->clear();
            rule_->addItems(rules);
            const int at = rule_->findText(keep);
            rule_->setCurrentIndex(at >= 0 ? at : 0);
        }
        rule_->setEnabled(rules.size() > 1);
        reload_ratio();
        load_rule();
    }

    void load_rule() {
        const QString opt = out_dir(QStringLiteral("band_option_demo"));
        const QString rule = rule_->currentText();
        trades_->set(demo_detail::read_csv(opt + (rule.isEmpty() ? QStringLiteral("/trades.csv")
                                                                  : QStringLiteral("/trades_%1.csv").arg(rule))));
        summary_filter_->set_filter({{summary_->csv().column(QStringLiteral("rule")), rule}});
        auto& m = meta_;
        if (!have_meta_) {
            banner_->setText(QStringLiteral("No option demo output yet in <code>%1</code>. Choose <b>Option demo</b> and press <b>Run</b>.")
                                 .arg(opt.toHtmlEscaped()));
            banner_->setStyleSheet(QStringLiteral("color:%1;").arg(QString::fromLatin1(theme_token::kTextMuted)));
        } else {
            const QString costs = m[QStringLiteral("costs")];
            const QString colour = costs_verified_ ? QStringLiteral("#3FB950") : QStringLiteral("#F85149");
            const QString text = m.count(QStringLiteral("rule_") + rule) ? m[QStringLiteral("rule_") + rule]
                                                                         : m[QStringLiteral("rule")];
            const QString clock = m.count(QStringLiteral("clock")) ? m[QStringLiteral("clock")]
                                                                   : QStringLiteral("CALENDAR: time to expiry / 365 days");
            banner_->setText(QStringLiteral("<span style='color:%1'><b>Expenses: %2.</b> %3</span><br>"
                                            "<b>Premiums:</b> %4 &nbsp;·&nbsp; <b>Clock:</b> %5<br><b>Rule:</b> %6 &nbsp;·&nbsp; "
                                            "slippage %7 pt each way &nbsp;·&nbsp; lot NIFTY %8, BANKNIFTY %9")
                                 .arg(colour, costs.toHtmlEscaped(), m[QStringLiteral("cost_note")].toHtmlEscaped(),
                                      m[QStringLiteral("premiums")].toHtmlEscaped(), clock.toHtmlEscaped(),
                                      text.toHtmlEscaped(), m[QStringLiteral("slippage_pts")],
                                      m[QStringLiteral("NIFTY_lot_size")], m[QStringLiteral("BANKNIFTY_lot_size")]));
            banner_->setStyleSheet({});
        }
        fill_combo(instrument_, trades_->csv(), QStringLiteral("instrument"));
        fill_combo(model_, trades_->csv(), QStringLiteral("model"));
        refilter();
        for (auto* v : views_) v->resizeColumnsToContents();
    }

    static void fill_combo(QComboBox* box, const demo_detail::Csv& csv, const QString& column) {
        const QString keep = box->currentText();
        const int c = csv.column(column);
        QStringList values;
        if (c >= 0) {
            for (const auto& row : csv.rows) {
                if (c < row.size() && !values.contains(row[c])) values << row[c];
            }
        }
        values.sort();
        const QSignalBlocker block(box);
        box->clear();
        box->addItem(QStringLiteral("All"));
        box->addItems(values);
        const int at = box->findText(keep);
        box->setCurrentIndex(at >= 0 ? at : 0);
    }

    void refilter() {
        const auto& csv = trades_->csv();
        const auto want = [](const QComboBox* box) { return box->currentIndex() > 0 ? box->currentText() : QString{}; };
        const QString inst = want(instrument_), model = want(model_);
        trade_filter_->set_filter({{csv.column(QStringLiteral("instrument")), inst},
                                   {csv.column(QStringLiteral("model")), model}});

        // Trades sorted by date, summed, cumulated.
        std::map<QDate, std::pair<double, double>> by_day;
        long n = 0, wins = 0;
        double gross = 0.0, expenses = 0.0, net = 0.0;
        bool have_net = false;
        for (const auto& row : csv.rows) {
            if (!inst.isEmpty() && demo_detail::cell(csv, row, QStringLiteral("instrument")) != inst) continue;
            if (!model.isEmpty() && demo_detail::cell(csv, row, QStringLiteral("model")) != model) continue;
            const QDate d = QDate::fromString(demo_detail::cell(csv, row, QStringLiteral("date")), Qt::ISODate);
            const double g = demo_detail::cell(csv, row, QStringLiteral("gross_pnl")).toDouble();
            bool net_ok = false;
            const double nn = demo_detail::cell(csv, row, QStringLiteral("net_pnl")).toDouble(&net_ok);
            have_net = have_net || net_ok;
            ++n;
            gross += g;
            net += net_ok ? nn : g;
            expenses += demo_detail::cell(csv, row, QStringLiteral("expenses")).toDouble();
            if ((net_ok ? nn : g) > 0.0) ++wins;
            auto& slot = by_day[d];
            slot.first += g;
            slot.second += net_ok ? nn : g;
        }
        std::vector<demo_detail::EquityChart::Point> points;
        double cg = 0.0, cn = 0.0;
        for (const auto& [d, v] : by_day) {
            cg += v.first;
            cn += v.second;
            points.push_back({d, cg, cn});
        }
        const QString who = model.isEmpty() ? QStringLiteral("every model") : model;
        const QString where = inst.isEmpty() ? QStringLiteral("NIFTY + BANKNIFTY") : inst;
        equity_->set(std::move(points), QStringLiteral("Cumulative P&L, 1 lot per trade · %1 · %2").arg(who, where),
                     costs_verified_);
        if (n == 0) {
            totals_->setText(QStringLiteral("no option trades loaded"));
            return;
        }
        const QString stamp = costs_verified_ ? QString{} : QStringLiteral(" <span style='color:#F85149'>UNVERIFIED</span>");
        totals_->setText(have_net
            ? QStringLiteral("<nobr>%1 trades</nobr> · <nobr>%2% won</nobr> · <nobr>gross %3</nobr> · "
                             "<nobr>expenses %4</nobr> · <nobr><b>net %5</b>%6</nobr>")
                  .arg(n).arg(100.0 * static_cast<double>(wins) / static_cast<double>(n), 0, 'f', 1)
                  .arg(demo_detail::rupees(gross), demo_detail::rupees(expenses), demo_detail::rupees(net), stamp)
            : QStringLiteral("<nobr>%1 trades</nobr> · <nobr>gross %2</nobr> · expenses not priced (charges schedule unverified)")
                  .arg(n).arg(demo_detail::rupees(gross)));
    }

    void reload_ratio() {
        const auto csv = demo_detail::read_csv(out_dir(QStringLiteral("pairs_futures")) + QStringLiteral("/ratio.csv"));
        if (csv.rows.empty()) {
            ratio_->setText(QStringLiteral("No ratio output yet. Choose <b>Pairs with futures</b> and press <b>Run</b>."));
            return;
        }
        QString html;
        for (const auto& row : csv.rows) {
            const auto c = [&](const char* name) { return demo_detail::cell(csv, row, QString::fromLatin1(name)); };
            const double cap = c("cap").toDouble();
            const long above = c("days_above_cap").toLong();
            const QString verdict = cap > 0.0
                ? (above > 0 ? QStringLiteral("<span style='color:#F85149'><b>REJECTED</b></span>: it closed above %1 on %2 day(s), %3 to %4.")
                                   .arg(c("cap"), QString::number(above), c("first_above"), c("last_above"))
                             : QStringLiteral("<span style='color:#3FB950'><b>Held</b></span> on every day of the sample (that is not a guarantee)."))
                : QString{};
            html += QStringLiteral("<h3>%1 · %2</h3>"
                                   "<p>Hypothesis: the ratio never exceeds %3. %4</p>"
                                   "<p>Max %5 on %6 · min %7 on %8 · last %9 (%10) · %11 days.<br>"
                                   "5th / 50th / 95th percentile: %12 / %13 / %14.</p>"
                                   "<p style='color:%15'>A level ratio that drifts over decades is not stationary; a "
                                   "fixed cap is not a trading signal. The pairs model trades the cointegration "
                                   "residual instead, re-estimated each window.</p>")
                        .arg(c("pair"), c("ratio"), c("cap"), verdict, c("max"), c("max_date"), c("min"), c("min_date"),
                             c("last"), c("last_date"), c("days"))
                        .arg(c("p05"), c("p50"), c("p95"), QString::fromLatin1(theme_token::kTextMuted));
        }
        ratio_->setText(html);
    }

    void run(const QString& name) {
        const QString exe = demo_detail::find_helper(name);
        if (exe.isEmpty()) {
            status_->setText(QStringLiteral("%1 is not built (it needs tomlplusplus).").arg(name));
            return;
        }
        QStringList args;
        if (unverified_->isChecked()) args << QStringLiteral("--unverified-costs");
        const QString vix_log = root_dir_ + QStringLiteral("/data/verified/forecast_log/india_vix_daily.csv");
        if (name == QLatin1String("altair_vol_premium") && QFileInfo::exists(vix_log)) {
            args << QStringLiteral("--vix-log") << vix_log;   // adds the VIX-direction veto variant
        }
        run_->setEnabled(false);
        status_->setText(QStringLiteral("running %1 ...").arg(name));
        const auto started = helper_.start(exe, args, root_dir_, 30 * 60 * 1000, [this, name](HelperProcessResult r) {
            run_->setEnabled(true);
            if (!r.ran_to_completion()) {
                status_->setText(QStringLiteral("%1 failed: %2").arg(name, r.detail));
                return;
            }
            status_->setText(r.exit_code == 0
                ? QStringLiteral("%1 finished").arg(name)
                : QStringLiteral("%1 exited %2: %3").arg(name).arg(r.exit_code)
                      .arg(r.output.trimmed().section(QChar('\n'), -1)));
            reload();
        });
        if (!started) {
            run_->setEnabled(true);
            status_->setText(QStringLiteral("another run is still going"));
        }
    }

    QString root_dir_;
    bool costs_verified_ = false;
    bool have_meta_ = false;
    std::map<QString, QString> meta_;
    HelperProcess helper_;
    QLabel* banner_{};
    QLabel* status_{};
    QLabel* totals_{};
    QLabel* ratio_{};
    QComboBox* run_choice_{};
    QPushButton* run_{};
    QCheckBox* unverified_{};
    QComboBox* rule_{};
    QComboBox* instrument_{};
    QComboBox* model_{};
    QTabWidget* tabs_{};
    demo_detail::CsvModel* summary_{};
    demo_detail::CsvModel* trades_{};
    demo_detail::CsvModel* pairs_summary_{};
    demo_detail::CsvModel* pairs_trades_{};
    demo_detail::CsvModel* pairs_windows_{};
    demo_detail::CsvModel* vrp_summary_{};
    demo_detail::CsvModel* vrp_trades_{};
    demo_detail::CsvModel* sa_summary_{};
    demo_detail::CsvModel* sa_trades_{};
    demo_detail::ColumnFilter* trade_filter_{};
    demo_detail::ColumnFilter* summary_filter_{};
    demo_detail::EquityChart* equity_{};
    std::vector<QTableView*> views_;
};

} // namespace altair::ui
