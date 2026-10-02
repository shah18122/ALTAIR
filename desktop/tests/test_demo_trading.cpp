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
          "premiums,SYNTHETIC\nclock,\"VARIANCE: days / 252\"\nrule_touch,wait for a touch\n"
          "rule_strangle,sell both edges at 09:20\nslippage_pts,0.5\nNIFTY_lot_size,65\nBANKNIFTY_lot_size,30\n");
    write(o + "summary.csv", "rule,instrument,model,trades,gross_pnl,net_pnl,costs\n"
                             "touch,NIFTY,\"GARCH(1,1)\",2,100,-60,UNVERIFIED\n"
                             "touch,BANKNIFTY,EWMA,1,50,-30,UNVERIFIED\n"
                             "strangle,NIFTY,\"GARCH(1,1)\",1,400,300,UNVERIFIED\n");
    write(o + "trades_touch.csv",
          "date,instrument,model,rule,gross_pnl,expenses,net_pnl,costs\n"
          "2024-01-02,NIFTY,\"GARCH(1,1)\",touch,300.00,80.00,220.00,UNVERIFIED\n"
          "2024-01-03,NIFTY,\"GARCH(1,1)\",touch,-200.00,80.00,-280.00,UNVERIFIED\n"
          "2024-01-03,BANKNIFTY,EWMA,touch,50.00,80.00,-30.00,UNVERIFIED\n");
    write(o + "trades_strangle.csv",
          "date,instrument,model,rule,gross_pnl,expenses,net_pnl,costs\n"
          "2024-01-02,NIFTY,\"GARCH(1,1)\",strangle,400.00,100.00,300.00,UNVERIFIED\n");
    const QString p = root + QStringLiteral("/data/verified/pairs_futures/");
    write(p + "ratio.csv",
          "pair,ratio,days,min,min_date,max,max_date,last,last_date,p05,p50,p95,cap,days_above_cap,first_above,last_above\n"
          "NIFTY-BANKNIFTY,BANKNIFTY/NIFTY,10,0.6,2000-01-01,2.66,2019-07-05,2.40,2026-09-24,0.9,2.0,2.5,2.600,91,2019-03-26,2020-02-28\n");

    const QString v = root + QStringLiteral("/data/verified/vol_premium/");
    write(v + "summary.csv", "variant,instrument,trades,net_pnl\nalways,NIFTY,2,100\nhar-2,NIFTY,1,80\n");
    write(v + "trades_always.csv", "instrument,variant,entry_date,net_pnl\nNIFTY,always,2024-01-02,60\nNIFTY,always,2024-02-01,40\n");
    write(v + "trades_har-2.csv", "instrument,variant,entry_date,net_pnl\nNIFTY,har-2,2024-02-01,80\n");
    write(v + "trades_aa_odd.csv", "something,else\n1,2\n");

    const QString sa = root + QStringLiteral("/data/verified/resid_reversion/");
    write(sa + "summary.csv", "variant,trades,net_pnl\nmarket+sector,2,10\n");
    write(sa + "trades_market+sector.csv", "symbol,side,net_pnl\nINFY,long,5\nTCS,short,5\n");

    DemoTradingPage page(root);
    check(page.stat_arb_trades() == 2, "stat-arb: the market+sector trades are shown");
    check(page.vol_premium_trades() == 3, "vol premium: every variant's trades in one table; an unknown layout is left out");
    check(page.banner_text().contains(QStringLiteral("UNVERIFIED")) && page.banner_text().contains(QStringLiteral("VARIANCE")),
          "the banner carries the UNVERIFIED stamp and the clock the premiums ran on");
    check(page.rule_filter()->count() == 2 && page.rule_filter()->currentText() == QStringLiteral("touch"),
          "every rule the CLI ran is offered, first one shown");
    check(page.shown_trades() == 3 && page.shown_summary_rows() == 2 && page.totals_text().contains(QStringLiteral("3 trades"))
              && page.totals_text().contains(QStringLiteral("-\u2060₹90")),
          "a rule's trades and its per-model rows; net is the sum of the net column (220 - 280 - 30)");
    check(page.model_filter()->count() == 3, "model filter: All plus the two models that traded");
    page.model_filter()->setCurrentIndex(page.model_filter()->findText(QStringLiteral("GARCH(1,1)")));
    check(page.shown_trades() == 2 && page.totals_text().contains(QStringLiteral("2 trades"))
              && page.totals_text().contains(QStringLiteral("50.0% won")),
          "filtering by the model that took the trade keeps only its trades");
    page.instrument_filter()->setCurrentIndex(page.instrument_filter()->findText(QStringLiteral("BANKNIFTY")));
    check(page.shown_trades() == 0, "filters combine: GARCH placed no BANKNIFTY trade here");
    page.rule_filter()->setCurrentIndex(1);
    check(page.shown_summary_rows() == 1 && page.banner_text().contains(QStringLiteral("sell both edges")),
          "switching rule shows that rule's summary and description");
    page.instrument_filter()->setCurrentIndex(0);
    page.model_filter()->setCurrentIndex(0);
    check(page.shown_trades() == 1 && page.totals_text().contains(QStringLiteral("₹300")),
          "and loads that rule's own trades file");
    check(page.ratio_text().contains(QStringLiteral("REJECTED")) && page.ratio_text().contains(QStringLiteral("2019-07-05")),
          "the 2.6 cap hypothesis is shown rejected, with the day of the maximum");
}

void test_empty(const QString& root) {
    DemoTradingPage page(root);
    check(page.shown_trades() == 0 && page.banner_text().contains(QStringLiteral("Option demo")),
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
