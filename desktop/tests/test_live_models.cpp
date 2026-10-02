// Acceptance tests for desktop/live_models.hpp: the live models panel.
//
// The engine's files are written by hand into a temporary root (the engine
// itself is tested in live/tests/test_engine.cpp); the prices come over the
// real bus into the real client, so the tick-by-tick marking is what is
// tested, not a stub of it.
//
// No check description here may contain the substring "F" "AIL" joined.

#include "../live_models.hpp"

#include <server/price_bus.hpp>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include <boost/asio/io_context.hpp>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

template <typename F>
void pump(altair::PriceBus& bus, int ms, F done) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        bus.poll();
        QCoreApplication::processEvents();
        if (done()) { return; }
    }
}

void write(const QString& path, const QByteArray& body) {
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(body);
}

QString cell(QTableWidget* t, int r, int c) { return t->item(r, c) ? t->item(r, c)->text() : QString(); }

}  // namespace

int main(int argc, char** argv) {
    using namespace altair;
    using namespace altair::ui;
    QApplication app(argc, argv);
    std::printf("live models panel\n");

    QTemporaryDir root;
    QDir(root.path()).mkpath(QStringLiteral("data/live/paper"));
    // 2026-10-01 11:00 IST
    write(root.path() + QStringLiteral("/data/live/engine_state.json"), R"json({
  "engine_ns": 1790832600000000000, "source": "SIM", "stale": false,
  "note": "Expenses priced from an UNVERIFIED config/charges.toml (--unverified-costs).",
  "models": [
    {"name": "Strangle 80% NIFTY", "family": "option selling", "state": "in position", "signal": "short A + B",
     "reason": "Sold at 09:20.", "fields": [["band at 09:20", "22883.5 – 23198.6"]]},
    {"name": "Direction 10:15 AR(2)", "family": "direction + magnitude gate", "state": "done today", "signal": "UP to the close",
     "reason": "Gate shut: the call is worth -2.4 bp after costs.", "fields": [["calibrated q", "51.0 %"]]},
    {"name": "Stat-arb NIFTY 50", "family": "stat-arb", "state": "abstaining", "signal": "",
     "reason": "No history: 0 of 50 stocks have daily closes in data/pairs/", "fields": []}
  ],
  "positions": [
    {"model": "Strangle 80% NIFTY", "symbol": "NIFTY26O0623250CE", "token": 1001, "side": -1, "qty": 65, "lot": 65,
     "entry": 81.90, "entry_ns": 1790826600000000000, "entry_expenses": 30.50, "carry": false, "why_in": "09:20 band"},
    {"model": "Pairs BANKNIFTY/NIFTY", "symbol": "NIFTY26OCTFUT", "token": 1002, "side": 1, "qty": 130, "lot": 65,
     "entry": 23100.00, "entry_ns": 1790826600000000000, "entry_expenses": null, "carry": true, "why_in": "z -2.3"}
  ]
}
)json");
    write(root.path() + QStringLiteral("/data/live/paper/trades.csv"),
          "date,model,symbol,token,side,qty,entry_time,entry,exit_time,exit,gross,expenses,net,why_in,why_out,source,costs\n"
          "2026-09-30,\"Strangle 80% NIFTY\",NIFTY26O0623300CE,9,short,65,2026-09-30 09:20:00,70.00,2026-09-30 15:20:00,50.00,1300.00,60.00,1240.00,\"09:20 band\",\"15:20 square-off\",SIM,UNVERIFIED\n"
          "2026-10-01,\"Strangle 80% NIFTY\",NIFTY26O0622900PE,8,short,65,2026-10-01 09:20:00,61.20,2026-10-01 10:40:00,122.40,-3978.00,55.96,-4033.96,\"09:20 band, a, comma\",\"premium doubled: stop at 10:40\",SIM,UNVERIFIED\n"
          "2026-10-01,\"Direction 10:15 AR(2)\",NIFTY26OCTFUT,1002,long,65,2026-10-01 10:15:00,23100.00,2026-10-01 15:20:00,23150.00,3250.00,,,\"q 56 %\",\"15:20 square-off\",SIM,UNPRICED\n");

    boost::asio::io_context io;
    PriceBus bus(io, 0);
    PriceClient client;
    client.start(QStringLiteral("127.0.0.1"), bus.port());
    pump(bus, 3000, [&] { return client.connected() && bus.clients() == 1; });
    const auto quote = [&](quint32 tok, std::int64_t bid, std::int64_t ask, std::int64_t ltp) {
        QuotePayload q;
        q.token = tok; q.flags = kQuoteHasTop | kQuoteSimulated; q.bid = bid; q.ask = ask; q.bid_qty = 65; q.ask_qty = 65;
        bus.publish_quote(q, 1);
        PricePayload p;
        p.token = tok; p.flags = kPriceSimulated; p.last_paise = ltp;
        bus.publish(kTopicTrades, p, nullptr, nullptr, 1);
    };
    quote(1001, 9000, 9050, 9020);         // the short call: marked at the ask, 90.50
    quote(1002, 2312000, 2312020, 2312010);   // the long future: marked at the bid, 23120.00
    pump(bus, 3000, [&] { const LivePrice* f = client.price(1002); return client.price(1001) != nullptr && f != nullptr && f->has_quote; });

    LiveModelsPanel panel(&client, root.path());
    panel.resize(1300, 700);
    panel.show();
    panel.reload();
    QCoreApplication::processEvents();

    auto* m = panel.models_table();
    check(m->rowCount() == 3 && cell(m, 0, 0) == QStringLiteral("Strangle 80% NIFTY") && cell(m, 0, 2) == QStringLiteral("in position"),
          "every model has a row with its state");
    check(cell(m, 1, 4) == QStringLiteral("Gate shut: the call is worth -2.4 bp after costs."), "and says why it is not acting");
    check(cell(m, 2, 2) == QStringLiteral("abstaining") && cell(m, 2, 4).startsWith(QStringLiteral("No history")),
          "a model without its inputs says so");

    auto* p = panel.positions_table();
    check(p->rowCount() == 2, "both open positions are listed");
    check(cell(p, 0, 5) == QStringLiteral("90.50") && cell(p, 0, 6) == QStringLiteral("−⁠559"),
          "a short is marked at the ask: (81.90 - 90.50) x 65 = -559");
    check(cell(p, 1, 5) == QStringLiteral("23120.00") && cell(p, 1, 6) == QStringLiteral("2,600"),
          "a long is marked at the bid: (23120 - 23100) x 130 = 2,600");
    check(cell(p, 1, 7) == QStringLiteral("—") && cell(p, 1, 8) == QStringLiteral("carried"),
          "unpriced entry expenses are a dash, and a carried position says so");

    // A new tick re-marks it without a new state file.
    quote(1002, 2313000, 2313020, 2313010);
    pump(bus, 1500, [&] { const LivePrice* f = client.price(1002); return f != nullptr && f->quote.bid == 2313000; });
    pump(bus, 400, [] { return false; });
    check(cell(p, 1, 6) == QStringLiteral("3,900"), "the next tick re-marks the position (23130 - 23100) x 130");

    auto* t = panel.trades_table();
    check(t->rowCount() == 3 && cell(t, 0, 1) == QStringLiteral("Direction 10:15 AR(2)"), "trades newest first");
    check(cell(t, 1, 12) == QStringLiteral("premium doubled: stop at 10:40") && cell(t, 1, 11) == QStringLiteral("−⁠4,034"),
          "a quoted reason with a comma survives, and net is shown");
    check(cell(t, 0, 10) == QStringLiteral("—") && cell(t, 0, 13) == QStringLiteral("SIM · UNPRICED"),
          "an unpriced trade shows no expense and says UNPRICED");

    auto* b = panel.by_model_table();
    int strangle_row = -1;
    for (int r = 0; r < b->rowCount(); ++r) if (cell(b, r, 0) == QStringLiteral("Strangle 80% NIFTY")) strangle_row = r;
    check(strangle_row >= 0 && cell(b, strangle_row, 1) == QStringLiteral("2")
              && cell(b, strangle_row, 4) == QStringLiteral("−⁠2,794") && cell(b, strangle_row, 6) == QStringLiteral("−⁠4,034"),
          "P&L by model: all days net (1,240 - 4,034) and today's net");
    check(panel.status_text().contains(QStringLiteral("SIM")) && panel.status_text().contains(QStringLiteral("11:00:00")),
          "the status says SIM and the engine's clock");

    std::printf("%s\n", failures == 0 ? "all live models checks passed" : "live models checks did not pass");
    return failures == 0 ? 0 : 1;
}
