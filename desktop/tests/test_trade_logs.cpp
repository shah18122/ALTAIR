// Trades on the strategy pages: round trips rebuilt from a study's positions
// add up to its evaluation, the overnight list, the reasoning a trade opens
// with, the filtered saved-runs tab and the tabs themselves. Temporary files
// only; no dataset/ and no network.

#include "../trade_log_tabs.hpp"

#include <QApplication>
#include <QThreadPool>
#include <QTemporaryDir>
#include <QTextBrowser>

#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}
void write(const QString& path, const QByteArray& text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(text);
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    using namespace altair::ui;

    // ---- round trips from positions -------------------------------------
    {
        std::vector<std::string> days;
        std::vector<double> closes;
        double c = 100.0;
        for (int i = 0; i < 400; ++i) {
            days.push_back(QDate(2020, 1, 1).addDays(i).toString(Qt::ISODate).toStdString());
            closes.push_back(c);
            c *= 1.0 + 0.01 * std::sin(0.13 * i);
        }
        std::vector<double> r;
        for (std::size_t i = 1; i < closes.size(); ++i) r.push_back(10000.0 * std::log(closes[i] / closes[i - 1]));
        std::vector<double> pos(r.size(), 0.0);
        for (std::size_t i = 10; i < pos.size(); ++i) pos[i] = std::sin(0.05 * static_cast<double>(i)) > 0.2 ? 1.5 : (std::sin(0.05 * static_cast<double>(i)) < -0.2 ? -0.8 : 0.0);
        const auto trips = trips_from_positions(days, closes, pos, 5.5, QStringLiteral("in"), QStringLiteral("out"));
        const auto e = altair::evaluate(pos, r, 5.5, 250.0);
        double gross = 0.0, net = 0.0;
        bool alternate = true;
        for (std::size_t i = 0; i < trips.size(); ++i) {
            gross += trips[i].gross_bps;
            net += trips[i].net_bps;
            if (i > 0 && trips[i].entry_day < trips[i - 1].exit_day) alternate = false;
        }
        const double n = static_cast<double>(r.size());
        check(!trips.empty() && e.has_value(), "a position series gives round trips");
        check(e && std::abs(gross / n - e->gross_bps) < 1e-9 && std::abs(net / n - e->net_bps) < 1e-9,
              "the trips add up to the study's own evaluation, gross and net of cost");
        check(alternate, "a trip never starts before the one before it ended");
        check(!trips.empty() && trips.front().side == QStringLiteral("long") && trips.front().why_in == QStringLiteral("in"),
              "side and reason are carried");
    }

    QTemporaryDir root;
    // ---- overnight study on a small synthetic 5m partition ------------------
    {
        QByteArray csv = "time,open,high,low,close,volume\n";
        for (int d = 0; d < 120; ++d) {
            const QString day = QDate(2024, 1, 1).addDays(d).toString(Qt::ISODate);
            const double open = 100.0 + d, close = 100.5 + d;
            csv += QStringLiteral("%1T09:15:00+05:30,%2,%2,%2,%2,0\n").arg(day).arg(open).toUtf8();
            csv += QStringLiteral("%1T15:25:00+05:30,%2,%2,%2,%2,0\n").arg(day).arg(close).toUtf8();
        }
        write(root.filePath("dataset/spot/nifty/5m/2024-01.csv"), csv);
        const auto trips = study_trips(root.filePath("dataset"), QStringLiteral("nifty"), Study::Overnight, 5.0);
        check(trips.size() == 119, "overnight: one trip a night");
        check(!trips.empty() && std::abs(trips[0].gross_rs - 0.5) < 1e-9 && std::abs(trips[0].cost_bps - 10.0) < 1e-12
                  && trips[0].entry_day == QStringLiteral("2024-01-01") && trips[0].exit_day == QStringLiteral("2024-01-02"),
              "overnight: bought at the close, sold at the next open, two sides of cost");
        check(study_trips(root.filePath("dataset"), QStringLiteral("banknifty"), Study::Momentum, 5.5).empty(),
              "no data: no trades, rather than invented ones");
        const QByteArray out = study_trips_csv(trips);
        check(out.startsWith("entry_date,exit_date,side") && out.count('\n') == 120, "the list as CSV, one line a trip");
    }

    // ---- the reasoning a trade opens with -----------------------------------
    {
        write(root.filePath("data/live/paper/decisions.csv"),
              "time,ns,model,decision\n"
              "2026-10-24 08:40:00,1,OHL,too early\n"
              "2026-10-24 09:05:00,2,OHL,watching: open 24100\n"
              "2026-10-24 09:20:00,3,OHL,open = low: buy\n"
              "2026-10-24 09:21:00,4,Vol band (HAR),not mine\n"
              "2026-10-24 15:20:00,5,OHL,square off\n"
              "2026-10-24 15:40:00,6,OHL,after the exit\n");
        write(root.filePath("data/live/paper/fills.csv"),
              "time,model,symbol,token,side,qty,price,expenses,at_quote,reason,source,costs\n"
              "2026-10-24 09:20:01,OHL,NIFTY FUT,1,BUY,75,24110,20,24109/24111,open = low,SIM,\n"
              "2026-10-24 15:20:01,OHL,NIFTY FUT,1,SELL,75,24150,20,24149/24151,square off,SIM,\n");
        write(root.filePath("data/verified/forecast_log/nifty_daily.csv"),
              "time,stage,learned_days,model,last_price,next_price,forecast_price,p_up,call,moved,result,net_bp\n"
              "2026-10-24 15:30:00,1,500,\"ARMA(1,1)\",24100,24200,24150,0.6,UP,UP,RIGHT,4\n"
              "2026-10-24 15:30:00,1,500,HAR,24100,24200,24080,0.4,DOWN,UP,WRONG,-4\n"
              "2026-10-23 15:30:00,1,500,HAR,24000,24100,24080,0.4,DOWN,UP,WRONG,-4\n");
        write(root.filePath("data/verified/forecast_log/banknifty_daily.csv"),
              "time,stage,learned_days,model,last_price,next_price,forecast_price,p_up,call,moved,result,net_bp\n"
              "2026-10-24 15:30:00,1,500,HAR,51000,51100,51050,0.6,UP,UP,RIGHT,4\n");
        write(root.filePath("data/verified/forecast_log/india_vix_daily.csv"),
              "time,stage,learned_days,model,last_price,next_price,forecast_price,p_up,call,moved,result,net_bp\n"
              "2026-10-24T15:30:00+05:30,1,500,HAR,12.1,12.4,12.3,,UP,UP,RIGHT,\n");
        const TradeContext t{QStringLiteral("OHL"), QStringLiteral("NIFTY FUT"), QStringLiteral("2026-10-24"),
                             QStringLiteral("2026-10-24 09:20:01"), QStringLiteral("2026-10-24 15:20:01"),
                             QStringLiteral("open = low"), QStringLiteral("square off")};
        const QString h = trade_reason_html(root.path(), t);
        check(h.contains(QStringLiteral("open = low")) && h.contains(QStringLiteral("square off")), "why in and why out lead");
        check(h.contains(QStringLiteral("watching: open 24100")) && h.contains(QStringLiteral("open = low: buy"))
                  && !h.contains(QStringLiteral("too early")) && !h.contains(QStringLiteral("after the exit"))
                  && !h.contains(QStringLiteral("not mine")),
              "the model's own decisions from 30 minutes before entry to the exit, no other model's");
        check(h.contains(QStringLiteral("24109/24111")) && h.contains(QStringLiteral("24149/24151")), "both fills, with their quotes");
        check(h.contains(QStringLiteral("nifty_daily")) && h.contains(QStringLiteral("1 up · 1 down"))
                  && h.contains(QStringLiteral("1 / 2 right")) && h.contains(QStringLiteral("india_vix_daily"))
                  && !h.contains(QStringLiteral("banknifty_daily")),
              "that day's NIFTY and VIX forecasts, scored; not BANKNIFTY's");
        const TradeContext b = trade_context_from_row({QStringLiteral("entry_date"), QStringLiteral("exit_date"),
                                                       QStringLiteral("variant"), QStringLiteral("reason")},
                                                      {QStringLiteral("2026-10-24"), QStringLiteral("2026-10-31"),
                                                       QStringLiteral("hedged"), QStringLiteral("IV >= RV")});
        check(b.date == QStringLiteral("2026-10-24") && b.model == QStringLiteral("hedged") && b.why_in == QStringLiteral("IV >= RV"),
              "a backtest row's own column names are understood");
        check(trade_reason_html(root.path(), b).contains(QStringLiteral("nifty_daily")), "a backtest trade gets that day's forecasts too");
    }

    // ---- saved runs filtered, and the tabs -----------------------------------
    {
        write(root.filePath("data/verified/pairs_futures/trades.csv"), "entry_date,exit_date,net_pnl\n2026-10-20,2026-10-21,5\n");
        write(root.filePath("data/verified/study/overnight_gap_nifty_trades.csv"),
              "entry_date,exit_date,net_rs_per_unit\n2026-10-20,2026-10-21,5\n");
        BacktestsPage only(root.path(), nullptr, {QStringLiteral("overnight")});
        check(only.log_count() == 1, "a strategy's Saved runs tab lists only its own runs");
        BacktestsPage all(root.path());
        check(all.log_count() == 2, "the Backtests page still lists every run");

        auto* study = new QWidget;
        QTabWidget* tabs = with_trade_logs(study, {{Study::Overnight}, {QStringLiteral("OHL"), QStringLiteral("Desk trader")},
                                                   {QStringLiteral("overnight")}, {}},
                                           root.filePath("dataset"), root.path());
        check(tabs->count() == 4 && tabs->widget(0) == study && tabs->tabText(1) == QStringLiteral("Study trades")
                  && tabs->tabText(2) == QStringLiteral("Paper && real trades") && tabs->tabText(3) == QStringLiteral("Saved runs"),
              "Study, Study trades, Paper & real trades, Saved runs");
        auto* panel = dynamic_cast<StudyTradesPanel*>(tabs->findChild<QWidget*>(QStringLiteral("studyTrades")));
        bool done = false;
        if (panel != nullptr) panel->build([&done] { done = true; });
        QThreadPool::globalInstance()->waitForDone(10000);
        for (int i = 0; i < 50 && !done; ++i) QApplication::processEvents();
        check(done && panel->trip_count() == 119 && panel->summary_text().contains(QStringLiteral("119 round trips")),
              "Build fills the table off the UI thread");
        check(panel != nullptr && !panel->save().isEmpty()
                  && QFile::exists(root.filePath("data/verified/study/overnight_gap_nifty_trades.csv")),
              "Save puts it where the Backtests page finds it");
        auto* record = dynamic_cast<StrategyRecordPanel*>(tabs->findChild<QWidget*>(QStringLiteral("strategyRecord")));
        check(record != nullptr && record->strategy() == QStringLiteral("OHL"), "the paper tab opens on the first strategy");
        auto* pick = tabs->findChild<QComboBox*>(QStringLiteral("tradeModelPick"));
        if (pick != nullptr) pick->setCurrentIndex(1);
        check(record != nullptr && record->strategy() == QStringLiteral("Desk trader"), "the strategy picker switches the record");
        QTabWidget* bare = with_trade_logs(new QWidget, {{}, {}, {QStringLiteral("basis")}, {}}, {}, root.path());
        check(bare->count() == 2, "a page with no rule and no engine model gets only its saved runs");
        delete tabs;
        delete bare;
    }

    std::printf("%s\n", failures == 0 ? "all trade log checks passed" : "trade log checks did not pass");
    return failures == 0 ? 0 : 1;
}
