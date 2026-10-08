// Tests for desktop/demo_trading_page.hpp -- fixtures only; no CLI is run.

#include "../demo_trading_page.hpp"
#include "../threshold_page.hpp"

#include <QApplication>
#include <QTableView>
#include <QTextBrowser>
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
          "rule_strangle,sell both edges at 09:20\nrule_strangle-stop2x,sell both edges with a 2x stop\n"
          "slippage_pts,0.5\nNIFTY_lot_size,65\nBANKNIFTY_lot_size,30\n");
    // An older output still holds the "touch" rule: it is no longer offered.
    write(o + "summary.csv", "rule,instrument,model,trades,gross_pnl,net_pnl,costs\n"
                             "touch,NIFTY,\"GARCH(1,1)\",5,10,-90,UNVERIFIED\n"
                             "strangle,NIFTY,\"GARCH(1,1)\",2,100,-60,UNVERIFIED\n"
                             "strangle,BANKNIFTY,EWMA,1,50,-30,UNVERIFIED\n"
                             "strangle-stop2x,NIFTY,\"GARCH(1,1)\",1,400,300,UNVERIFIED\n");
    write(o + "trades_touch.csv",
          "date,instrument,model,rule,gross_pnl,expenses,net_pnl,costs\n"
          "2024-01-02,NIFTY,\"GARCH(1,1)\",touch,10.00,100.00,-90.00,UNVERIFIED\n");
    write(o + "trades_strangle.csv",
          "date,instrument,model,rule,theta_pnl,move_pnl,iv_pnl,slippage_pnl,gross_pnl,expenses,net_pnl,costs,"
          "brokerage,stt,exchange_txn,sebi,stamp,ipft,gst\n"
          "2024-01-02,NIFTY,\"GARCH(1,1)\",strangle,350.00,-40.00,0.00,-10.00,300.00,80.00,220.00,UNVERIFIED,"
          "40.00,20.00,8.00,0.10,1.00,0.10,10.80\n"
          "2024-01-03,NIFTY,\"GARCH(1,1)\",strangle,100.00,-290.00,0.00,-10.00,-200.00,80.00,-280.00,UNVERIFIED,"
          "40.00,20.00,8.00,0.10,1.00,0.10,10.80\n"
          "2024-01-03,BANKNIFTY,EWMA,strangle,60.00,0.00,0.00,-10.00,50.00,80.00,-30.00,UNVERIFIED,"
          "40.00,20.00,8.00,0.10,1.00,0.10,10.80\n");
    write(o + "trades_strangle-stop2x.csv",
          "date,instrument,model,rule,gross_pnl,expenses,net_pnl,costs\n"
          "2024-01-02,NIFTY,\"GARCH(1,1)\",strangle-stop2x,400.00,100.00,300.00,UNVERIFIED\n");
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

    // altair_trader: two trips as app/trader_main.cpp writes them (the first is a real row of its run).
    const QString at = root + QStringLiteral("/data/verified/altair_trader/");
    write(at + "trades.csv",
          "date,index,side,lots,qty,entry_date,entry,exit_date,exit,days,rolls,gross,expenses,net,"
          "brokerage,stt,exchange_txn,sebi,stamp,ipft,gst,expected_bp,cost_bp,why_in,why_out,costs\n"
          "2016-02-17,NIFTY,short,1,65,2016-02-08,7387.25,2016-02-17,7108.45,7,0,18122.00,173.37,17948.63,"
          "40.00,96.03,16.30,0.94,9.24,0.47,10.39,-18.0,3.63,\"expected -18.0 bp over a 3.63 bp round trip\","
          "\"the expected move turned against the position\",verified\n"
          "2016-02-22,NIFTY,long,1,65,2016-02-17,7108.45,2016-02-22,7034.55,3,0,-4803.50,171.17,-4974.67,"
          "40.00,94.05,16.13,0.93,9.24,0.47,10.35,9.8,3.66,\"expected 9.8 bp over a 3.66 bp round trip\","
          "\"the expected move turned against the position\",verified\n");
    write(at + "days.csv", "date,index,ready,expected_bp,error_bp,lambda,move_bp,cost_bp,position,pnl,cum_gross\n"
                           "2016-02-08,NIFTY,1,-18.00,80.1,100,-12.3,3.63,-1,5000.00,5000.00\n");
    write(at + "weights.csv", "index,track,model,weight\nNIFTY,intercept,,2.100\nNIFTY,nifty_daily,\"Stack\",1.300\n");
    write(at + "summary.txt", "altair_trader -- one model over 161 model outputs\nNIFTY (lot 65 x 1)\n  buy and hold one lot\n");

    DemoTradingPage page(root);
    {
        int trades_tab = -1, how_tab = -1;
        for (int i = 0; i < page.tabs()->count(); ++i) {
            if (page.tabs()->tabText(i) == QStringLiteral("altair_trader · trades")) trades_tab = i;
            if (page.tabs()->tabText(i) == QStringLiteral("altair_trader · how it works")) how_tab = i;
        }
        check(trades_tab > 0 && how_tab > trades_tab, "altair_trader has its trades, days, weights and how-it-works tabs");
        page.tabs()->setCurrentIndex(trades_tab);
        check(page.stats_text().contains(QStringLiteral("altair_trader")) && page.stats_text().contains(QStringLiteral("2 trip(s)")),
              "its trips get the same ratios");
        int short_row = -1;
        for (int r = 0; r < 2 && short_row < 0; ++r)
            if (page.row_detail(trades_tab, r).contains(QStringLiteral("17948.63"))) short_row = r;
        const QString d = short_row >= 0 ? page.row_detail(trades_tab, short_row) : QString();
        check(d.contains(QStringLiteral("<b>Gross</b> = <b>18122.00</b>")) && d.contains(QStringLiteral("STT 96.03"))
                  && d.contains(QStringLiteral("= <b>173.37</b>")) && d.contains(QStringLiteral("= <b>17948.63</b>")),
              "a double-click on a trip walks gross, every expense head, and net");
        auto* how = page.findChild<QTextBrowser*>(QStringLiteral("traderSummary"));
        check(how != nullptr && how->toPlainText().contains(QStringLiteral("Step by step, every day"))
                  && how->toPlainText().contains(QStringLiteral("buy and hold one lot")),
              "how it works, with the latest run's summary under it");
        page.tabs()->setCurrentIndex(0);
    }
    check(page.stat_arb_trades() == 2, "stat-arb: the market+sector trades are shown");
    check(page.vol_premium_trades() == 3, "vol premium: every variant's trades in one table; an unknown layout is left out");
    check(page.banner_text().contains(QStringLiteral("UNVERIFIED")) && page.banner_text().contains(QStringLiteral("VARIANCE")),
          "the banner carries the UNVERIFIED stamp and the clock the premiums ran on");
    check(page.rule_filter()->count() == 2 && page.rule_filter()->currentText() == QStringLiteral("strangle")
              && page.rule_filter()->findText(QStringLiteral("touch")) < 0,
          "every rule the CLI ran is offered but the dropped touch rule, first one shown");
    check(page.shown_trades() == 3 && page.shown_summary_rows() == 2 && page.totals_text().contains(QStringLiteral("3 trades"))
              && page.totals_text().contains(QStringLiteral("-\u2060₹90")),
          "a rule's trades and its per-model rows; net is the sum of the net column (220 - 280 - 30)");
    check(page.model_filter()->count() == 3, "model filter: All plus the two models that traded");
    check(page.stats_text().contains(QStringLiteral("3 trip(s) over 2 day(s)")) && page.stats_text().contains(QStringLiteral("Sharpe"))
              && page.stats_text().contains(QStringLiteral("Sortino")) && page.stats_text().contains(QStringLiteral("profit factor")),
          "the ratios over the trades shown: Sharpe, Sortino, profit factor");
    {
        page.tabs()->setCurrentIndex(1);
        int row0 = -1;
        for (int r = 0; r < 3 && row0 < 0; ++r)
            if (page.row_detail(1, r).contains(QStringLiteral("-200.00"))) row0 = r;
        const QString d = row0 >= 0 ? page.row_detail(1, row0) : QString();
        check(d.contains(QStringLiteral("time decay 100.00")) && d.contains(QStringLiteral("brokerage 40.00"))
                  && d.contains(QStringLiteral("GST 10.80")) && d.contains(QStringLiteral("= <b>-280.00</b>")),
              "a double-click walks gross from its parts, expenses head by head, and net = gross - expenses");
        page.tabs()->setCurrentIndex(0);
    }
    page.model_filter()->setCurrentIndex(page.model_filter()->findText(QStringLiteral("GARCH(1,1)")));
    check(page.shown_trades() == 2 && page.totals_text().contains(QStringLiteral("2 trades"))
              && page.totals_text().contains(QStringLiteral("50.0% won")),
          "filtering by the model that took the trade keeps only its trades");
    page.instrument_filter()->setCurrentIndex(page.instrument_filter()->findText(QStringLiteral("BANKNIFTY")));
    check(page.shown_trades() == 0, "filters combine: GARCH placed no BANKNIFTY trade here");
    page.rule_filter()->setCurrentIndex(1);
    check(page.shown_summary_rows() == 1 && page.banner_text().contains(QStringLiteral("2x stop")),
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

void test_threshold(const QString& root) {
    const QString o = root + QStringLiteral("/data/verified/threshold/");
    write(o + "breakout_trades.csv",
          "n,side,entry_date,exit_date,days,entry,stop,exit,exit_reason,flip,mfe_pts,mae_pts,points,qty,rolls,gross_pnl,"
          "brokerage,stt,exchange_txn,sebi,stamp,ipft,gst,expenses,net_pnl,workbook_cost_pts,workbook_net_pts\n"
          "1,Long,2024-01-02,2024-01-04,2,47000.00,46800.00,47300.00,Reversal,,400.00,50.00,300.00,30,0,9000.00,"
          "40.00,282.00,25.00,1.00,21.00,1.00,12.00,382.00,8618.00,141.45,158.55\n"
          "2,Short,2024-01-04,2024-01-05,1,47300.00,47500.00,47450.00,Stop,Yes,10.00,160.00,-150.00,30,0,-4500.00,"
          "40.00,284.00,25.00,1.00,21.00,1.00,12.00,384.00,-4884.00,142.13,-292.13\n");
    write(o + "ratio_trades.csv",
          "n,side,entry_date,exit_date,days,gross_pnl,expenses,net_pnl,open\n"
          "1,Short ratio (short BANKNIFTY; long NIFTY),2024-02-01,2024-03-15,30,12000.00,900.00,11100.00,\n");
    write(o + "summary.txt", "THRESHOLD STRATEGIES -- paper only\n1. PREV-2-DAY BREAKOUT\n");
    write(root + "/threshold_strategy/extracted/tradingview_backtests.csv",
          "symbol,timeframe,total_trades,net_profit_inr,trades_listed,same_bar_trades,file\n"
          "NSE:SBIN1!,15 minutes,472,3512831.33,472,447,x.xlsx\n");
    ThresholdPage page(root);
    check(page.breakout_trades() == 2 && page.ratio_trades() == 1 && page.tradingview_rows() == 1,
          "Threshold: the breakout and ratio trades and the TradingView results load");
    check(page.how_text().contains(QStringLiteral("PREV-2-DAY BREAKOUT")) && page.how_text().contains(QStringLiteral("Waiting for rules")),
          "how they work: the rules, the latest run, and what waits for rules");
    auto* tv_note = page.findChild<QLabel*>(QStringLiteral("tradingviewNote"));
    check(tv_note != nullptr && tv_note->text().contains(QStringLiteral("447 of 472 trades (95 %)")),
          "the TradingView tab says how many trades open and close inside one bar");
    page.tabs()->setCurrentIndex(1);
    check(page.stats_text().contains(QStringLiteral("Sharpe")) && page.stats_text().contains(QStringLiteral("2 trip")),
          "the breakout tab's ratios: win rate, Sharpe, Sortino over its trades");
    check(DemoTradingPage::trade_detail_html(
              qobject_cast<QTableView*>(page.tabs()->widget(1))->model(), 0).contains(QStringLiteral("Expenses</b> = brokerage")),
          "double-click detail walks a breakout trade's expense heads, gross to net");
    page.tabs()->setCurrentIndex(0);
    check(page.stats_text().isEmpty(), "no trade ratios over the OHL record tab");
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    std::printf("Demo trading page\n");
    test_split();
    QTemporaryDir full, empty;
    test_page(full.path());
    test_empty(empty.path());
    QTemporaryDir thr;
    test_threshold(thr.path());
    std::printf("Demo trading page: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
