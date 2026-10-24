// desktop/trade_stats.hpp and the strategy record panel on numbers known in
// advance: win rate, profit factor, expectancy, Sharpe, Sortino and drawdown
// over the daily net; an unpriced trip counted in gross only; a netted
// cross-exchange pair laid out as its two fills with every expense head (the
// BSE leg on BSE's rate) and gross walked to net; and the panel reading a
// demo record, showing the ratios and the detail a double-click opens.
//
// No check description here may contain the substring "F" "AIL" joined.

#include "../strategy_record.hpp"
#include "../trade_stats.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "BAD ", what);
    if (!ok) ++failures;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    using namespace altair::ui;
    std::printf("trade statistics and expense detail\n");

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<TradeStatRow> rows{
        {QStringLiteral("2026-10-01"), 110.0, 10.0, 100.0},
        {QStringLiteral("2026-10-01"), -40.0, 10.0, -50.0},
        {QStringLiteral("2026-10-02"), 210.0, 10.0, 200.0},
        {QStringLiteral("2026-10-03"), -20.0, 10.0, -30.0},
        {QStringLiteral("2026-10-03"), 15.0, nan, nan},
    };
    const TradeStats s = trade_stats(rows);
    check(s.trips == 5 && s.priced == 4 && s.wins == 2 && s.losses == 2 && std::fabs(s.win_rate - 0.5) < 1e-12,
          "five trips, four priced: two won, two lost, win rate 50 %");
    check(std::fabs(s.gross - 275.0) < 1e-9 && std::fabs(s.expenses - 40.0) < 1e-9 && std::fabs(s.net - 220.0) < 1e-9,
          "gross counts every trip (275); expenses and net only the priced ones (40, 220)");
    check(std::fabs(s.profit_factor - 300.0 / 80.0) < 1e-12 && std::fabs(s.expectancy - 55.0) < 1e-12
              && std::fabs(s.avg_win - 150.0) < 1e-12 && std::fabs(s.avg_loss + 40.0) < 1e-12,
          "profit factor 300 / 80, expectancy 55 a trip, average win 150, average loss -40");
    // Daily net: 50, 200, -30.
    const double mean = 220.0 / 3.0;
    const double sd = std::sqrt(((50 - mean) * (50 - mean) + (200 - mean) * (200 - mean) + (-30 - mean) * (-30 - mean)) / 2.0);
    const double down = std::sqrt(30.0 * 30.0 / 3.0);
    check(s.days == 3 && std::fabs(s.sharpe - mean / sd * std::sqrt(252.0)) < 1e-9
              && std::fabs(s.sortino - mean / down * std::sqrt(252.0)) < 1e-9,
          "Sharpe and Sortino over the daily net, annualised by sqrt(252)");
    check(std::fabs(s.max_drawdown - 30.0) < 1e-9, "max drawdown of the cumulative daily net: 250 down to 220 = 30");
    check(trade_stats_html(s).contains(QStringLiteral("Sortino")) && trade_stats_html(s).contains(QStringLiteral("unpriced")),
          "the line names the ratios and says one trip was unpriced");

#if ALTAIR_HAVE_CHARGES_TOML
    const QDate day(2026, 10, 5);
    const auto nse = fill_charges(ChargeKind::Equity, true, 100, 1000.0, day, false, QStringLiteral("nse"));
    const auto bse = fill_charges(ChargeKind::Equity, true, 100, 1000.0, day, true, QStringLiteral("bse"));
    check(nse.priced && bse.priced && bse.exchange > nse.exchange
              && std::fabs(nse.total - (nse.brokerage + nse.stt + nse.exchange + nse.sebi + nse.stamp + nse.ipft + nse.gst)) < 0.02,
          "a fill's heads add to its total, and the BSE leg pays BSE's exchange rate");
    const QString d = trip_detail_html(QStringLiteral("2026-10-05"), QStringLiteral("RELIANCE BSE->NSE"), QStringLiteral("long"), 70,
                                       998.60, 1000.00, 98.0, 29.98, 68.02, QStringLiteral("14 bp gap"),
                                       QStringLiteral("netted by the clearing corporation"));
    check(d.contains(QStringLiteral("buy 70 RELIANCE on BSE at 998.60")) && d.contains(QStringLiteral("sell 70 RELIANCE on NSE at 1000.00"))
              && d.contains(QStringLiteral("STT")) && d.contains(QStringLiteral("no exit fill")),
          "a netted pair is its two fills -- buy on BSE, sell on NSE -- with every head, and no exit fill");
#else
    std::printf("  (no charges.toml loader in this preset: the expense heads are not checked)\n");
#endif

    // The panel on a demo record.
    QTemporaryDir root;
    QDir().mkpath(root.path() + QStringLiteral("/data/live/paper"));
    QFile f(root.path() + QStringLiteral("/data/live/paper/trades.csv"));
    if (f.open(QIODevice::WriteOnly)) {
        f.write("date,model,symbol,token,side,qty,entry_time,entry,exit_time,exit,gross,expenses,net,why_in,why_out,source,costs\n"
                "2026-10-05,\"Cross-exchange arbitrage\",RELIANCE BSE->NSE,2,long,70,2026-10-05 10:00:01,998.60,2026-10-05 10:00:01,"
                "1000.00,98.00,29.98,68.02,\"14 bp gap\",\"the pair is netted by the clearing corporation\",SIM,VERIFIED\n"
                "2026-10-06,\"Cross-exchange arbitrage\",INFY NSE->BSE,4,long,40,2026-10-06 11:00:01,1500.00,2026-10-06 11:00:01,"
                "1501.00,40.00,18.00,22.00,\"7 bp gap\",\"the pair is netted by the clearing corporation\",SIM,VERIFIED\n");
        f.close();
    }
    StrategyRecordPanel panel(QStringLiteral("Cross-exchange arbitrage"), root.path());
    panel.refresh();
    check(panel.trips_table()->rowCount() == 2 && panel.trips_table()->item(0, 2)->text() == QStringLiteral("INFY NSE->BSE"),
          "the panel lists each pair as one row, newest first");
    check(panel.stats_text().contains(QStringLiteral("Sharpe")) && panel.stats_text().contains(QStringLiteral("2 trip(s) over 2 day(s)")),
          "and the ratios under the totals");
    check(panel.trip_detail(1).contains(QStringLiteral("buy 70 RELIANCE on BSE")) && panel.trip_detail(1).contains(QStringLiteral("Gross")),
          "a double-click opens the pair's fills and gross-to-net");

    std::printf("%s\n", failures == 0 ? "all trade statistics checks passed" : "trade statistics checks did not pass");
    return failures == 0 ? 0 : 1;
}
