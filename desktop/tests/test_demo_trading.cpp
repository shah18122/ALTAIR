// Tests for desktop/demo_trading_page.hpp -- fixtures only; no CLI is run.

#include "../demo_trading_page.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

namespace {

using namespace altair::ui;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

void write(const QString& path, const char* text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    (void)f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(text);
}

void test_split() {
    const auto cells = demo_detail::split_csv(QStringLiteral(R"(a,"b, c","say ""hi""",,e)"));
    check(cells == QStringList({"a", "b, c", "say \"hi\"", "", "e"}),
          "quoted commas and doubled quotes split as one cell each");
}

void test_page(const QString& root) {
    const QString o = root + QStringLiteral("/data/verified/band_option_demo/");
    write(o + "meta.csv",
          "key,value\ncosts,UNVERIFIED\ncost_note,\"priced anyway, UNVERIFIED\"\n"
          "premiums,SYNTHETIC\nrule,fade\nslippage_pts,0.5\nNIFTY_lot_size,65\nBANKNIFTY_lot_size,30\n");
    write(o + "summary.csv", "instrument,model,trades,gross_pnl,net_pnl,costs\nNIFTY,\"GARCH(1,1)\",2,100,-60,UNVERIFIED\n");
    write(o + "trades.csv",
          "date,instrument,model,option,gross_pnl,expenses,net_pnl,costs\n"
          "2024-01-02,NIFTY,\"GARCH(1,1)\",CE,300.00,80.00,220.00,UNVERIFIED\n"
          "2024-01-03,NIFTY,\"GARCH(1,1)\",PE,-200.00,80.00,-280.00,UNVERIFIED\n"
          "2024-01-03,BANKNIFTY,EWMA,CE,50.00,80.00,-30.00,UNVERIFIED\n");
    const QString p = root + QStringLiteral("/data/verified/pairs_futures/");
    write(p + "ratio.csv",
          "pair,ratio,days,min,min_date,max,max_date,last,last_date,p05,p50,p95,cap,days_above_cap,first_above,last_above\n"
          "NIFTY-BANKNIFTY,BANKNIFTY/NIFTY,10,0.6,2000-01-01,2.66,2019-07-05,2.40,2026-09-24,0.9,2.0,2.5,2.600,91,2019-03-26,2020-02-28\n");

    DemoTradingPage page(root);
    check(page.banner_text().contains(QStringLiteral("UNVERIFIED")), "the costs banner carries the UNVERIFIED stamp");
    check(page.shown_trades() == 3 && page.totals_text().contains(QStringLiteral("3 trades"))
              && page.totals_text().contains(QStringLiteral("-\u2060₹90")),
          "all trades shown; net is the sum of the net column (220 - 280 - 30)");
    check(page.model_filter()->count() == 3, "model filter: All plus the two models that traded");
    page.model_filter()->setCurrentIndex(page.model_filter()->findText(QStringLiteral("GARCH(1,1)")));
    check(page.shown_trades() == 2 && page.totals_text().contains(QStringLiteral("2 trades"))
              && page.totals_text().contains(QStringLiteral("50.0% won")),
          "filtering by the model that took the trade keeps only its trades");
    page.instrument_filter()->setCurrentIndex(page.instrument_filter()->findText(QStringLiteral("BANKNIFTY")));
    check(page.shown_trades() == 0, "filters combine: GARCH placed no BANKNIFTY trade here");
    check(page.ratio_text().contains(QStringLiteral("REJECTED")) && page.ratio_text().contains(QStringLiteral("2019-07-05")),
          "the 2.6 cap hypothesis is shown rejected, with the day of the maximum");
}

void test_empty(const QString& root) {
    DemoTradingPage page(root);
    check(page.shown_trades() == 0 && page.banner_text().contains(QStringLiteral("Run option demo")),
          "no output yet: says how to make it rather than showing zeros");
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    std::printf("Demo trading page\n");
    test_split();
    QTemporaryDir full, empty;
    test_page(full.path());
    test_empty(empty.path());
    std::printf("Demo trading page: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
