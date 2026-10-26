// The Backtests page: finds every trade log, reads it, and adds it up.

#include "../backtests_page.hpp"

#include <QApplication>
#include <QTemporaryDir>

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
    QTemporaryDir root;

    BacktestsPage empty(root.path());
    check(empty.log_count() == 0, "no logs on disk: none listed, and the page says how to make one");

    write(root.filePath("data/verified/pairs_futures/trades.csv"),
          "entry_date,exit_date,symbol,gross_pnl,expenses,net_pnl,why_in\n"
          "2026-10-20,2026-10-21,NIFTY,1000,100,900,z>2\n"
          "2026-10-21,2026-10-22,NIFTY,-500,100,-600,z>2\n");
    write(root.filePath("data/verified/vol_premium/trades_hedged.csv"),
          "date,net_rs_per_lot,why_entered\n2026-10-20,250,IV>RV\n");
    write(root.filePath("data/verified/forecast_log/nifty_trades_calls.csv"), "date,call\n2026-10-20,up\n");
    write(root.filePath(kDemoTradesFile), "date,model,net\n2026-10-24,AR,10\n");

    const auto logs = find_backtest_logs(root.path());
    check(logs.size() == 3, "backtests and the live paper book found; forecast calls are not trades");
    check(!logs.empty() && logs.back().group == QStringLiteral("Paper (live models)"), "live paper trades listed after the backtests");

    BacktestsPage page(root.path());
    check(page.log_count() == 3, "the page lists them");
    int pairs = -1;
    for (int i = 0; i < static_cast<int>(logs.size()); ++i)
        if (logs[static_cast<std::size_t>(i)].label.contains(QStringLiteral("pairs_futures"))) pairs = i;
    check(page.select_log(pairs) && page.trade_count() == 2, "picking a log shows every trade");
    check(page.stats_text().contains(QStringLiteral("Net Rs 300.00")) && page.stats_text().contains(QStringLiteral("50.0 %")),
          "net after expenses and the win rate are added up");

    const auto csv = demo_detail::read_csv(root.filePath("data/verified/vol_premium/trades_hedged.csv"));
    const auto rows = backtests_detail::stat_rows(csv);
    check(rows.size() == 1 && rows[0].net == 250.0 && rows[0].date == QStringLiteral("2026-10-20"),
          "a per-lot net column and a date column are understood");

    std::printf("%s\n", failures == 0 ? "all backtests page checks passed" : "backtests page checks did not pass");
    return failures == 0 ? 0 : 1;
}
