// desktop/message_bar.hpp on files written as the live engine writes them: a
// demo fill and a netted pair become DEMO lines, a risk-gate refusal an RMS
// line, the engine's own note an ENGINE line, a model's routine "gate shut"
// nothing; only today's lines on the first look, then only what is appended;
// a line still being written waits for its newline.
//
// No check description here may contain the substring "F" "AIL" joined.

#include "../message_bar.hpp"

#include <QApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "BAD ", what);
    if (!ok) ++failures;
}

void append(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    if (f.open(QIODevice::Append)) f.write(bytes);
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    using namespace altair::ui;
    std::printf("message bar\n");

    const QString fill = QStringLiteral(
        "2026-10-07 10:00:01,\"Cross-exchange arbitrage\",RELIANCE,2,buy,70,998.60,14.99,1,\"14 bp gap, BSE ask under NSE bid\",LIVE,VERIFIED");
    const QString d = message_bar::demo_fill(fill);
    check(d.startsWith(QStringLiteral("10:00:01  DEMO Cross-exchange arbitrage: BUY 70 RELIANCE @ 998.60"))
              && d.contains(QStringLiteral("expenses 14.99")),
          "a model's fill is a DEMO line: time, model, side, quantity, symbol, price and expenses");
    const QString netted = message_bar::demo_fill(QStringLiteral(
        "2026-10-07 10:00:01,\"Cross-exchange arbitrage\",RELIANCE,2,sell,70,998.60,0.00,1,\"netted: pair with NSE\",SIM,VERIFIED"));
    check(netted.contains(QStringLiteral("DEMO (SIM)")) && netted.contains(QStringLiteral("netted (no exit fill)")),
          "a SIM fill says SIM, and a netted leg says it had no exit fill");
    check(message_bar::demo_fill(QStringLiteral("time,model,symbol,token,side,qty,price,expenses,at_quote,reason,source,costs")).isEmpty(),
          "the header is not a message");
    const QString rms = message_bar::rms_decision(QStringLiteral(
        "2026-10-07 10:05:00,1,\"OHL threshold\",\"Could not buy: risk: gross notional 5,10,000 over 5,00,000\""));
    check(rms == QStringLiteral("10:05:00  RMS OHL threshold: Could not buy: risk: gross notional 5,10,000 over 5,00,000"),
          "a refusal by the risk gate is an RMS line, with its limit");
    check(message_bar::rms_decision(QStringLiteral("2026-10-07 15:20:00,1,engine,\"15:20 square-off: exits submitted\""))
              .contains(QStringLiteral("ENGINE: 15:20 square-off")),
          "the engine's own note is an ENGINE line");
    check(message_bar::rms_decision(QStringLiteral(
              "2026-10-07 10:15:00,1,\"Direction AR(2)\",\"Gate shut: worth -5.2 bp after costs\"")).isEmpty(),
          "a model's routine gate-shut is not a message");

    QTemporaryDir dir;
    const QString fills = dir.path() + QStringLiteral("/fills.csv");
    const QString today = message_bar::today_ist();
    append(fills, QByteArray("time,model,symbol,token,side,qty,price,expenses,at_quote,reason,source,costs\n")
                      + "2020-01-01 10:00:00,\"Old\",X,1,buy,1,1.00,0.10,1,\"old\",SIM,VERIFIED\n"
                      + (today + QStringLiteral(" 10:00:01,\"Arb\",RELIANCE,2,buy,70,998.60,14.99,1,\"gap\",SIM,VERIFIED\n")).toUtf8());
    MessageBar bar;
    bar.poll(dir.path());
    check(bar.lines() == 1 && bar.toPlainText().contains(QStringLiteral("DEMO (SIM) Arb: BUY 70 RELIANCE")),
          "the first look shows today's fills only");
    append(fills, (today + QStringLiteral(" 10:00:02,\"Arb\",RELIANCE,3,sell,70,1000.00,15.10,1,\"gap\",SIM,VE")).toUtf8());
    bar.poll(dir.path());
    check(bar.lines() == 1, "a line still being written waits for its newline");
    append(fills, QByteArray("RIFIED\n"));
    bar.poll(dir.path());
    check(bar.lines() == 2 && bar.toPlainText().contains(QStringLiteral("SELL 70 RELIANCE @ 1000.00")),
          "and shows once it is whole, without the lines already shown");
    append(dir.path() + QStringLiteral("/decisions.csv"),
           (QStringLiteral("time,ns,model,decision\n") + today
            + QStringLiteral(" 10:06:00,1,\"Pairs\",\"Could not enter: risk: position limit (20)\"\n")).toUtf8());
    bar.poll(dir.path());
    check(bar.lines() == 3 && bar.toPlainText().contains(QStringLiteral("RMS Pairs: Could not enter: risk: position limit (20)")),
          "a refusal appended to decisions.csv reaches the bar");
    bar.add(QStringLiteral("10:07:00  EXECUTED BUY 65 NIFTY26OCTFUT @ 24010.00"));
    check(bar.lines() == 4, "and the Terminal's own order lines go to the same bar");

    std::printf("%s\n", failures == 0 ? "all message bar checks passed" : "message bar checks did not pass");
    return failures == 0 ? 0 : 1;
}
