// desktop/threshold_page.hpp -- the Threshold strategies page.
//
// THIS PAGE PLACES NOTHING BY ITSELF. It holds:
//   * OHL -- the live engine's record of the open = high / open = low rule
//     (demo, and real only behind LIVE and its own switch);
//   * the owner's threshold strategies from threshold_strategy/, on paper:
//     the prev-2-day BANKNIFTY breakout (2D-H-L.xlsx) and the BANKNIFTY/NIFTY
//     ratio z-score (BNFNF), run by altair_threshold with real futures
//     expenses and written under data/verified/threshold/;
//   * the TradingView backtests in threshold_strategy/BACKTEST/, shown as
//     TradingView wrote them (threshold_strategy/extracted/tradingview_backtests.csv,
//     from research/tools/threshold_extract.py) -- not re-run here.

#pragma once

#include "demo_trading_page.hpp"
#include "helper_process.hpp"
#include "strategy_record.hpp"
#include "theme.hpp"
#include "trade_stats.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTabWidget>
#include <QTableView>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QWidget>

#include <limits>
#include <vector>

namespace altair::ui {

class ThresholdPage final : public QWidget {
public:
    /// `root` holds threshold_strategy/ and data/verified/ (the source tree).
    explicit ThresholdPage(QString root, QWidget* parent = nullptr) : QWidget(parent), root_(std::move(root)) {
        setObjectName(QStringLiteral("thresholdPage"));
        auto* v = new QVBoxLayout(this);
        auto* head = new QLabel(QStringLiteral(
            "<h3>Threshold strategies</h3>"
            "<b>OHL (open = high / open = low)</b> on futures. At 09:15:00 the open set in the pre-open is compared "
            "with the first second of trading: <b>open = high</b> → SELL one lot, stop-loss at high + 0.5 %; "
            "<b>open = low</b> → BUY one lot, stop-loss at low − 0.5 %. Once the trade is 1.5 % in profit a "
            "trailing stop follows 0.25 % behind the best price. Anything still open is squared off at 15:20. "
            "Demo by default. Real orders only while LIVE is on <b>and</b> its own switch (Auto: OHL) is on, "
            "within its caps.<br>"
            "<b>Your strategies (threshold_strategy/)</b> run on paper only: the 2-day high/low breakout and the "
            "BANKNIFTY/NIFTY ratio, with real futures expenses; your TradingView results are shown as they are."), this);
        head->setWordWrap(true);
        head->setTextFormat(Qt::RichText);
        v->addWidget(head);

        auto* controls = new QHBoxLayout;
        run_ = new QPushButton(QStringLiteral("Run altair_threshold"), this);
        run_->setObjectName(QStringLiteral("thresholdRun"));
        run_->setToolTip(QStringLiteral("Re-run the 2-day breakout and the ratio z-score on the dataset (paper only)"));
        auto* reread = new QPushButton(QStringLiteral("Re-read"), this);
        status_ = new QLabel(this);
        status_->setStyleSheet(QStringLiteral("color:%1;").arg(QString::fromLatin1(theme_token::kTextMuted)));
        status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        controls->addWidget(run_);
        controls->addWidget(reread);
        controls->addWidget(status_, 1);
        v->addLayout(controls);
        stats_ = new QLabel(this);
        stats_->setObjectName(QStringLiteral("thresholdStats"));
        stats_->setWordWrap(true);
        stats_->setTextFormat(Qt::RichText);
        v->addWidget(stats_);

        tabs_ = new QTabWidget(this);
        tabs_->setObjectName(QStringLiteral("thresholdTabs"));
        breakout_ = new demo_detail::CsvModel(this);
        ratio_ = new demo_detail::CsvModel(this);
        tradingview_ = new demo_detail::CsvModel(this);
        tabs_->addTab(new StrategyRecordPanel(QStringLiteral("OHL"), root_, this), QStringLiteral("OHL · live record"));
        tabs_->addTab(view(breakout_), QStringLiteral("2-day breakout · trades"));
        tabs_->addTab(view(ratio_), QStringLiteral("BNF/NF ratio · trades"));
        how_ = new QTextBrowser(this);
        how_->setObjectName(QStringLiteral("thresholdHow"));
        tabs_->addTab(how_, QStringLiteral("How they work · latest run"));
        {
            auto* w = new QWidget(this);
            auto* l = new QVBoxLayout(w);
            l->setContentsMargins(0, 0, 0, 0);
            tv_note_ = new QLabel(w);
            tv_note_->setObjectName(QStringLiteral("tradingviewNote"));
            auto* note = tv_note_;
            note->setWordWrap(true);
            note->setTextFormat(Qt::RichText);
            l->addWidget(note);
            l->addWidget(view(tradingview_));
            tabs_->addTab(w, QStringLiteral("TradingView backtests"));
        }
        v->addWidget(tabs_, 1);

        connect(run_, &QPushButton::clicked, this, [this] { run(); });
        connect(reread, &QPushButton::clicked, this, [this] { reload(); });
        connect(tabs_, &QTabWidget::currentChanged, this, [this](int) { update_stats(); });
        reload();
    }

    [[nodiscard]] QTabWidget* tabs() const noexcept { return tabs_; }
    [[nodiscard]] int breakout_trades() const { return breakout_->rowCount(); }
    [[nodiscard]] int ratio_trades() const { return ratio_->rowCount(); }
    [[nodiscard]] int tradingview_rows() const { return tradingview_->rowCount(); }
    [[nodiscard]] QString how_text() const { return how_->toPlainText(); }
    [[nodiscard]] QString stats_text() const { return stats_->text(); }

    void reload() {
        const QString out = root_ + QStringLiteral("/data/verified/threshold");
        breakout_->set(demo_detail::read_csv(out + QStringLiteral("/breakout_trades.csv")));
        ratio_->set(demo_detail::read_csv(out + QStringLiteral("/ratio_trades.csv")));
        tradingview_->set(demo_detail::read_csv(root_ + QStringLiteral("/threshold_strategy/extracted/tradingview_backtests.csv")));
        tv_note_->setText(tradingview_note(tradingview_->csv()));
        QFile f(out + QStringLiteral("/summary.txt"));
        const QString summary = f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        how_->setHtml(how_html(summary));
        update_stats();
    }

private:
    QTableView* view(demo_detail::CsvModel* m) {
        auto* s = new QSortFilterProxyModel(this);
        s->setSourceModel(m);
        s->setSortRole(Qt::UserRole);
        auto* tv = demo_detail::table_view(s, this);
        tv->setToolTip(QStringLiteral("Double-click a row: every column, the expense heads and gross to net"));
        connect(tv, &QTableView::doubleClicked, this, [this, tv](const QModelIndex& i) { show_row(tv, i.row()); });
        return tv;
    }

    void show_row(QTableView* tv, int row) {
        const QString html = DemoTradingPage::trade_detail_html(tv->model(), row);
        if (html.isEmpty()) return;
        QDialog d(this);
        d.setWindowTitle(QStringLiteral("Trade"));
        d.resize(760, 640);
        auto* lay = new QVBoxLayout(&d);
        auto* b = new QTextBrowser(&d);
        b->setHtml(html);
        lay->addWidget(b);
        auto* ok = new QDialogButtonBox(QDialogButtonBox::Close, &d);
        connect(ok, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        lay->addWidget(ok);
        d.exec();
    }

    /// Win rate, profit factor, Sharpe, Sortino, drawdown of the trades tab shown.
    void update_stats() {
        const int t = tabs_->currentIndex();
        const demo_detail::CsvModel* m = t == 1 ? breakout_ : t == 2 ? ratio_ : nullptr;
        if (m == nullptr || m->rowCount() == 0) { stats_->setText(QString()); return; }
        const auto& csv = m->csv();
        const int cd = csv.column(QStringLiteral("exit_date")), cg = csv.column(QStringLiteral("gross_pnl")),
                  ce = csv.column(QStringLiteral("expenses")), cn = csv.column(QStringLiteral("net_pnl"));
        if (cg < 0) { stats_->setText(QString()); return; }
        const double nan = std::numeric_limits<double>::quiet_NaN();
        std::vector<TradeStatRow> rows;
        for (const auto& r : csv.rows) {
            const auto num = [&](int c) {
                bool ok = false;
                const double v = c >= 0 && c < r.size() ? r[c].toDouble(&ok) : nan;
                return ok ? v : nan;
            };
            rows.push_back(TradeStatRow{cd >= 0 && cd < r.size() ? r[cd] : QString(), num(cg), num(ce), num(cn)});
        }
        stats_->setText(QStringLiteral("<b>%1, paper:</b> ").arg(t == 1 ? QStringLiteral("2-day breakout") : QStringLiteral("BNF/NF ratio"))
                        + trade_stats_html(trade_stats(rows)));
    }

    /// What to read before the TradingView figures, counted from them.
    [[nodiscard]] static QString tradingview_note(const demo_detail::Csv& csv) {
        QString t = QStringLiteral(
            "<b>As TradingView wrote them</b> (BACKTEST/, 15-minute bars): not re-run here. Read their settings before "
            "their results: order size, commission and slippage; a fraction column is TradingView's own (3.51 = 351 %).");
        const int cl = csv.column(QStringLiteral("trades_listed")), cs = csv.column(QStringLiteral("same_bar_trades"));
        long long listed = 0, same = 0;
        for (const auto& r : csv.rows) {
            if (cl >= 0 && cl < r.size()) listed += r[cl].toLongLong();
            if (cs >= 0 && cs < r.size()) same += r[cs].toLongLong();
        }
        if (listed > 0 && same > 0)
            t += QStringLiteral(
                     "<br><span style='color:#F0B429'><b>Read with care:</b> %1 of %2 trades (%3 %) enter and leave inside "
                     "one 15-minute bar. Inside a bar TradingView does not know whether the high or the low came first "
                     "(bar magnifier off), so those results rest on its guess; with slippage at 0 ticks as well, the win "
                     "rates and profit factors are likely flattered. Re-run them with the bar magnifier on and slippage "
                     "set before relying on them.</span>")
                     .arg(same)
                     .arg(listed)
                     .arg(QString::number(100.0 * static_cast<double>(same) / static_cast<double>(listed), 'f', 0));
        return t;
    }

    [[nodiscard]] static QString how_html(const QString& summary) {
        QString h = QStringLiteral(
            "<h3>2-day high / low breakout &mdash; BANKNIFTY</h3>"
            "<p>From <i>market bnf sheets/2D-H-L.xlsx</i>. <b>Long</b> when a day trades above the higher of the previous "
            "two days' highs, at that level (at the open when it gaps above). <b>Short</b> below the lower of the previous "
            "two lows. The stop is fixed at entry at the previous day's low (long) or high (short), and trails at each "
            "previous day's low / high from the day after entry; whichever is nearer fills. On an exit day the opposite "
            "breakout reverses the position (a flip). One position at a time; from flat, when both sides trigger, the "
            "day's close against its open picks the side.</p>"
            "<p>Checked against the workbook's own trade log, trade by trade (the numbers are in the run below). Paper: one "
            "lot of the BANKNIFTY future at today's lot size, priced at index levels, every fill and every month-end roll "
            "paying the expenses of config/charges.toml. Stops fill exactly at their level here; the run says how much "
            "slippage a fill would take the whole net away.</p>"
            "<h3>BANKNIFTY / NIFTY ratio &mdash; z-score</h3>"
            "<p>From <i>BNFNF</i> (the relative-value note). The ratio's z over 120 sessions: <b>short the ratio</b> "
            "(short BANKNIFTY, long NIFTY) above +1, <b>buy it</b> below &minus;1, <b>flat</b> inside 0.25. A position "
            "formed at a close earns to the next close. Measured as the note measures it (equal notional, 6 bps a change) "
            "and in rupees: one lot of BANKNIFTY against the NIFTY lots nearest equal notional, both legs' expenses and rolls "
            "priced.</p>"
            "<p><b>Waiting for rules</b> (not in the folder): Short Straddle, Nifty CE Buy on High Close, BNF CE Buy, Monthly "
            "SIP with Expiry, BNF, Wed + Mon Both, Tue + Thursday Both, Delta Hedge using Option.</p>");
        if (summary.isEmpty())
            h += QStringLiteral("<p><i>No run yet: press <b>Run altair_threshold</b>.</i></p>");
        else
            h += QStringLiteral("<h4>Latest run</h4><pre>%1</pre>").arg(summary.toHtmlEscaped());
        return h;
    }

    void run() {
        const QString exe = demo_detail::find_helper(QStringLiteral("altair_threshold"));
        if (exe.isEmpty()) {
            status_->setText(QStringLiteral("altair_threshold is not built (it needs tomlplusplus)."));
            return;
        }
        run_->setEnabled(false);
        status_->setText(QStringLiteral("running altair_threshold ..."));
        const auto started = helper_.start(exe, {}, root_, 10 * 60 * 1000, [this](HelperProcessResult r) {
            run_->setEnabled(true);
            if (!r.ran_to_completion()) {
                status_->setText(QStringLiteral("altair_threshold failed: %1").arg(r.detail));
                return;
            }
            status_->setText(r.exit_code == 0 ? QStringLiteral("altair_threshold finished")
                                              : QStringLiteral("altair_threshold exited %1: %2")
                                                    .arg(r.exit_code)
                                                    .arg(r.output.trimmed().section(QChar('\n'), -1)));
            reload();
        });
        if (!started) {
            run_->setEnabled(true);
            status_->setText(QStringLiteral("a run is still going"));
        }
    }

    QString root_;
    HelperProcess helper_;
    QPushButton* run_{};
    QLabel* status_{};
    QLabel* stats_{};
    QTabWidget* tabs_{};
    QTextBrowser* how_{};
    QLabel* tv_note_{};
    demo_detail::CsvModel* breakout_{};
    demo_detail::CsvModel* ratio_{};
    demo_detail::CsvModel* tradingview_{};
};

} // namespace altair::ui
