// desktop/daily_findings_page.hpp -- one trading day, everything ALTAIR found.
//
// Smit's rule is that findings are logged every day, trade or no trade. This
// page puts one day on one screen, from the files the system already writes:
//
//   What the forecasts said    data/verified/forecast_log/: every model's call
//                              for NIFTY, BANKNIFTY and India VIX, the price it
//                              forecast, what happened, right or wrong;
//   What was traded            data/live/paper/trades.csv (paper) and
//                              data/live_orders/strategy_trades.csv (real);
//   What each model decided    data/live/paper/decisions.csv: how many
//                              decisions and the last one, so a model that
//                              stood aside says why;
//   The day's logs             data/logs/ files whose name carries the date.
//
// "Save findings" writes the day as Markdown to data/logs/findings/<date>.md.
// Reads files and writes only that report; places nothing.
#pragma once

#include "strategy_record.hpp"
#include "theme.hpp"
#include "trade_reason.hpp"

#include <QComboBox>
#include <QDate>
#include <QDesktopServices>
#include <QDirIterator>
#include <QHBoxLayout>
#include <QLabel>
#include <QMap>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <map>
#include <vector>

namespace altair::ui {

namespace findings_detail {

using trade_reason_detail::cell;
using trade_reason_detail::rows;
using trade_reason_detail::stamp;

/// The forecast tracks this page reports, in order: NIFTY, BANKNIFTY, VIX.
[[nodiscard]] inline QStringList tracks(const QString& root) {
    const QDir dir(root + QStringLiteral("/data/verified/forecast_log"));
    QStringList nifty, bank, vix;
    for (const QString& f : dir.entryList({QStringLiteral("*.csv")}, QDir::Files, QDir::Name)) {
        const QString n = f.toLower();
        if (n.startsWith(QLatin1String("banknifty"))) bank << dir.filePath(f);
        else if (n.startsWith(QLatin1String("nifty"))) nifty << dir.filePath(f);
        else if (n.startsWith(QLatin1String("india_vix")) || n.startsWith(QLatin1String("indiavix"))) vix << dir.filePath(f);
    }
    return nifty + bank + vix;
}

/// The days in a CSV's time-like `column`, read line by line.
[[nodiscard]] inline QSet<QString> days_in(const QString& path, const char* column) {
    QSet<QString> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    const QStringList head = csv_split(in.readLine());
    const int c = head.indexOf(QString::fromLatin1(column));
    if (c < 0) return out;
    QString last;
    while (!in.atEnd()) {
        const QString line = in.readLine();
        // The day leads the line in every file this page reads; only a new
        // day is split and checked.
        if (c == 0 && line.size() >= 10 && line.left(10) == last) continue;
        const QStringList r = csv_split(line);
        if (c >= r.size()) continue;
        const QString d = stamp(r[c]).left(10);
        if (d == last) continue;
        if (QDate::fromString(d, Qt::ISODate).isValid()) {
            out.insert(d);
            last = d;
        }
    }
    return out;
}

} // namespace findings_detail

/// Every day something was recorded, newest first.
[[nodiscard]] inline QStringList findings_days(const QString& root) {
    using namespace findings_detail;
    QSet<QString> all;
    for (const QString& t : tracks(root)) all.unite(days_in(t, "time"));
    all.unite(days_in(root + QLatin1Char('/') + QLatin1String(kDemoTradesFile), "date"));
    all.unite(days_in(root + QLatin1Char('/') + QLatin1String(kRealStrategyTradesFile), "date"));
    all.unite(days_in(root + QStringLiteral("/data/live/paper/decisions.csv"), "time"));
    QStringList out(all.begin(), all.end());
    std::sort(out.begin(), out.end(), std::greater<>());
    return out;
}

/// What the forecasts said on `date`: one table per track, every model.
[[nodiscard]] inline QString findings_forecasts_html(const QString& root, const QString& date, QString* markdown = nullptr) {
    using namespace findings_detail;
    QString h;
    for (const QString& path : tracks(root)) {
        const auto rs = rows(path, [&date](const QStringList& head, const QStringList& r) {
            return stamp(cell(head, r, "time")).startsWith(date);
        }, 600);
        if (rs.size() < 2) continue;
        const QStringList& head = rs.front();
        const QString track = QFileInfo(path).completeBaseName();
        int right = 0, scored = 0;
        double net = 0.0;
        QString body;
        for (std::size_t i = 1; i < rs.size(); ++i) {
            const auto& r = rs[i];
            const QString result = cell(head, r, "result");
            if (result == QLatin1String("RIGHT") || result == QLatin1String("WRONG")) {
                ++scored;
                if (result == QLatin1String("RIGHT")) ++right;
            }
            net += cell(head, r, "net_bp").toDouble();
            const QString colour = result == QLatin1String("RIGHT") ? QStringLiteral("#3FB950")
                                   : result == QLatin1String("WRONG") ? QStringLiteral("#F85149")
                                                                       : QStringLiteral("#8B949E");
            body += QStringLiteral("<tr><td>%1</td><td>%2</td><td align='right'>%3</td><td align='right'>%4</td>"
                                   "<td align='right'>%5</td><td>%6</td><td>%7</td><td style='color:%8'><b>%9</b></td>"
                                   "<td align='right'>%10</td></tr>")
                        .arg(stamp(cell(head, r, "time")).mid(11, 5).toHtmlEscaped(), cell(head, r, "model").toHtmlEscaped(),
                             cell(head, r, "last_price").toHtmlEscaped(), cell(head, r, "forecast_price").toHtmlEscaped(),
                             cell(head, r, "next_price").toHtmlEscaped(), cell(head, r, "call").toHtmlEscaped(),
                             cell(head, r, "moved").toHtmlEscaped(), colour, result.isEmpty() ? QStringLiteral("—") : result.toHtmlEscaped())
                        .arg(cell(head, r, "net_bp").toHtmlEscaped());
        }
        const QString line = QStringLiteral("%1: %2 calls, %3 of %4 scored right, net %5 bp after costs")
                                 .arg(track).arg(rs.size() - 1).arg(right).arg(scored).arg(net, 0, 'f', 1);
        if (markdown != nullptr) *markdown += QStringLiteral("- ") + line + QLatin1Char('\n');
        h += QStringLiteral("<p><b>%1</b></p><table border='1' cellspacing='0' cellpadding='3'><tr><th>At</th><th>Model</th>"
                            "<th>Last</th><th>Forecast</th><th>Actual next</th><th>Call</th><th>Moved</th><th>Result</th>"
                            "<th>Net bp</th></tr>%2</table>")
                 .arg(line.toHtmlEscaped(), body);
    }
    if (h.isEmpty()) {
        if (markdown != nullptr) *markdown += QStringLiteral("- No forecasts were logged this day.\n");
        return QStringLiteral("<p><i>No forecasts logged this day. The forecast curriculum writes data/verified/forecast_log/ "
                              "when it runs (Demo Trading › Run all, or the nightly retrain).</i></p>");
    }
    return h;
}

/// The trades of `date` in a trades file (the live ledger's columns).
[[nodiscard]] inline QString findings_trades_html(const QString& path, const QString& date, const QString& label,
                                                  QString* markdown = nullptr) {
    using namespace findings_detail;
    const auto rs = rows(path, [&date](const QStringList& head, const QStringList& r) {
        return cell(head, r, "date").startsWith(date);
    }, 500);
    if (rs.size() < 2) {
        if (markdown != nullptr) *markdown += QStringLiteral("- %1: no trades.\n").arg(label);
        return QStringLiteral("<p><b>%1</b>: no trades this day.</p>").arg(label.toHtmlEscaped());
    }
    const QStringList& head = rs.front();
    double gross = 0.0, net = 0.0;
    int wins = 0;
    QString body;
    for (std::size_t i = 1; i < rs.size(); ++i) {
        const auto& r = rs[i];
        const double g = cell(head, r, "gross").toDouble(), n = cell(head, r, "net").toDouble();
        gross += g;
        net += n;
        if (n > 0.0) ++wins;
        body += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td align='right'>%4</td><td>%5 → %6</td>"
                               "<td align='right' style='color:%7'>%8</td><td>%9</td><td>%10</td></tr>")
                    .arg(cell(head, r, "model").toHtmlEscaped(), cell(head, r, "symbol").toHtmlEscaped(),
                         cell(head, r, "side").toHtmlEscaped(), cell(head, r, "qty").toHtmlEscaped(),
                         cell(head, r, "entry_time").mid(11, 5).toHtmlEscaped(), cell(head, r, "exit_time").mid(11, 5).toHtmlEscaped(),
                         n >= 0.0 ? QStringLiteral("#3FB950") : QStringLiteral("#F85149"), QString::number(n, 'f', 2))
                    .arg(cell(head, r, "why_in").toHtmlEscaped(), cell(head, r, "why_out").toHtmlEscaped());
    }
    const int k = static_cast<int>(rs.size()) - 1;
    const QString line = QStringLiteral("%1: %2 round trips, %3 won, gross Rs %4, net Rs %5 after expenses")
                             .arg(label).arg(k).arg(wins).arg(gross, 0, 'f', 2).arg(net, 0, 'f', 2);
    if (markdown != nullptr) *markdown += QStringLiteral("- ") + line + QLatin1Char('\n');
    return QStringLiteral("<p><b>%1</b></p><table border='1' cellspacing='0' cellpadding='3'><tr><th>Model</th><th>Symbol</th>"
                          "<th>Side</th><th>Qty</th><th>In → out</th><th>Net Rs</th><th>Why in</th><th>Why out</th></tr>%2</table>")
        .arg(line.toHtmlEscaped(), body);
}

/// Each model's decisions on `date`: how many, and the last one in its words.
[[nodiscard]] inline QString findings_decisions_html(const QString& root, const QString& date, QString* markdown = nullptr) {
    using namespace findings_detail;
    const auto rs = rows(root + QStringLiteral("/data/live/paper/decisions.csv"), [&date](const QStringList& head, const QStringList& r) {
        return stamp(cell(head, r, "time")).startsWith(date);
    }, 20000);
    if (rs.size() < 2) {
        if (markdown != nullptr) *markdown += QStringLiteral("- The models engine logged no decisions.\n");
        return QStringLiteral("<p><i>The models engine logged no decisions this day (it runs with the feed, 09:15–15:30).</i></p>");
    }
    const QStringList& head = rs.front();
    std::map<QString, std::pair<int, QStringList>> by_model;   // count, last row
    for (std::size_t i = 1; i < rs.size(); ++i) {
        auto& e = by_model[cell(head, rs[i], "model")];
        ++e.first;
        e.second = rs[i];
    }
    QString body;
    for (const auto& [model, e] : by_model) {
        const QString last = cell(head, e.second, "decision");
        body += QStringLiteral("<tr><td><b>%1</b></td><td align='right'>%2</td><td>%3</td><td>%4</td></tr>")
                    .arg(model.toHtmlEscaped())
                    .arg(e.first)
                    .arg(stamp(cell(head, e.second, "time")).mid(11, 5).toHtmlEscaped(), last.toHtmlEscaped());
        if (markdown != nullptr)
            *markdown += QStringLiteral("- %1: %2 decisions; last: %3\n").arg(model).arg(e.first).arg(last);
    }
    return QStringLiteral("<table border='1' cellspacing='0' cellpadding='3'><tr><th>Model</th><th>Decisions</th><th>Last at</th>"
                          "<th>Last decision (why it traded, or why not)</th></tr>%1</table>")
        .arg(body);
}

/// data/logs/ files whose path carries `date`, relative to root.
[[nodiscard]] inline QStringList findings_logs(const QString& root, const QString& date) {
    QStringList out;
    const QString base = root + QStringLiteral("/data/logs");
    QString compact = date;
    compact.remove(QLatin1Char('-'));
    QDirIterator it(base, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        const QString rel = QDir(root).relativeFilePath(p);
        if (rel.contains(date) || rel.contains(compact)) out << rel;
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// The whole day, as HTML for the page and Markdown for the saved report.
struct DayFindings {
    QString html, markdown;
};

[[nodiscard]] inline DayFindings day_findings(const QString& root, const QString& date) {
    DayFindings f;
    QString& md = f.markdown;
    md = QStringLiteral("# ALTAIR findings, %1\n\n## Forecasts\n").arg(date);
    QString h = QStringLiteral("<h2>%1</h2>").arg(QDate::fromString(date, Qt::ISODate).toString(QStringLiteral("dddd d MMMM yyyy")));
    h += QStringLiteral("<h3>What the forecasts said, and what happened</h3>") + findings_forecasts_html(root, date, &md);
    md += QStringLiteral("\n## Trades\n");
    h += QStringLiteral("<h3>What was traded</h3>")
         + findings_trades_html(root + QLatin1Char('/') + QLatin1String(kDemoTradesFile), date, QStringLiteral("Paper (models engine)"), &md)
         + findings_trades_html(root + QLatin1Char('/') + QLatin1String(kRealStrategyTradesFile), date, QStringLiteral("Real orders (FYERS)"), &md);
    md += QStringLiteral("\n## Decisions\n");
    h += QStringLiteral("<h3>What each model decided</h3>") + findings_decisions_html(root, date, &md);
    const QStringList logs = findings_logs(root, date);
    md += QStringLiteral("\n## Logs\n");
    h += QStringLiteral("<h3>The day's logs</h3>");
    if (logs.isEmpty()) {
        h += QStringLiteral("<p><i>No file under data/logs/ carries this date.</i></p>");
        md += QStringLiteral("- none\n");
    } else {
        h += QStringLiteral("<ul>");
        for (const QString& l : logs) {
            h += QStringLiteral("<li><a href='%1'>%2</a></li>")
                     .arg(QUrl::fromLocalFile(root + QLatin1Char('/') + l).toString(), l.toHtmlEscaped());
            md += QStringLiteral("- %1\n").arg(l);
        }
        h += QStringLiteral("</ul>");
    }
    f.html = h;
    return f;
}

class DailyFindingsPage final : public QWidget {
public:
    explicit DailyFindingsPage(QString root, QWidget* parent = nullptr) : QWidget(parent), root_(std::move(root)) {
        setObjectName(QStringLiteral("dailyFindings"));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(16, 14, 16, 12);
        v->setSpacing(10);
        auto* head = new QHBoxLayout;
        auto* title = new QLabel(QStringLiteral("Daily findings"), this);
        set_role(title, QStringLiteral("title"));
        head->addWidget(title);
        head->addStretch();
        head->addWidget(new QLabel(QStringLiteral("Day"), this));
        day_ = new QComboBox(this);
        day_->setObjectName(QStringLiteral("findingsDay"));
        day_->setMinimumWidth(130);
        head->addWidget(day_);
        auto* refresh = new QPushButton(QStringLiteral("Refresh"), this);
        head->addWidget(refresh);
        save_ = new QPushButton(QStringLiteral("Save findings"), this);
        set_variant(save_, QStringLiteral("primary"));
        save_->setToolTip(QStringLiteral("Write this day as Markdown to data/logs/findings/<date>.md"));
        head->addWidget(save_);
        v->addLayout(head);
        auto* note = new QLabel(QStringLiteral(
            "One trading day on one screen: every forecast model's call for NIFTY, BANKNIFTY and India VIX against what "
            "happened, every paper and real trade, what each live model decided (and so why it stood aside), and that "
            "day's logs. Read from the files ALTAIR writes; nothing here trades."), this);
        note->setWordWrap(true);
        set_role(note, QStringLiteral("note"));
        v->addWidget(note);
        view_ = new QTextBrowser(this);
        view_->setObjectName(QStringLiteral("findingsView"));
        view_->setOpenLinks(false);
        v->addWidget(view_, 1);
        status_ = new QLabel(this);
        set_role(status_, QStringLiteral("note"));
        v->addWidget(status_);
        QObject::connect(refresh, &QPushButton::clicked, this, [this] { rescan(); });
        QObject::connect(day_, &QComboBox::currentTextChanged, this, [this] { show_day(); });
        QObject::connect(save_, &QPushButton::clicked, this, [this] { (void)save(); });
        QObject::connect(view_, &QTextBrowser::anchorClicked, this, [](const QUrl& u) { QDesktopServices::openUrl(u); });
    }

    /// The days on disk again, keeping the one shown when it is still there.
    void rescan() {
        const QString keep = day_->currentText();
        QStringList days = findings_days(root_);
        const QString today = QDate::currentDate().toString(Qt::ISODate);
        if (!days.contains(today)) days.prepend(today);   // today is always there, even before anything is logged
        const QSignalBlocker block(day_);
        day_->clear();
        day_->addItems(days);
        const int i = day_->findText(keep);
        day_->setCurrentIndex(i >= 0 ? i : 0);
        show_day();
    }

    /// Write the shown day to data/logs/findings/<date>.md; the path, or empty.
    QString save() {
        const QString date = day_->currentText();
        if (date.isEmpty()) return {};
        const QString dir = root_ + QStringLiteral("/data/logs/findings");
        if (!QDir().mkpath(dir)) return {};
        const QString path = dir + QLatin1Char('/') + date + QStringLiteral(".md");
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return {};
        f.write(day_findings(root_, date).markdown.toUtf8());
        if (!f.commit()) return {};
        status_->setText(QStringLiteral("Saved %1").arg(QDir(root_).relativeFilePath(path)));
        return path;
    }

    [[nodiscard]] QString text() const { return view_->toPlainText(); }
    [[nodiscard]] QComboBox* day_box() const noexcept { return day_; }

protected:
    void showEvent(QShowEvent* e) override {
        QWidget::showEvent(e);
        rescan();
    }

private:
    void show_day() {
        const QString date = day_->currentText();
        if (date.isEmpty()) return;
        view_->setHtml(day_findings(root_, date).html);
        status_->clear();
    }

    QString root_;
    QComboBox* day_ = nullptr;
    QPushButton* save_ = nullptr;
    QTextBrowser* view_ = nullptr;
    QLabel* status_ = nullptr;
};

} // namespace altair::ui
