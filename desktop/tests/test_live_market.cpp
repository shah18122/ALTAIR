// Acceptance tests for desktop/live_market.hpp: the live market watch.
//
// THE REAL BUS, THE REAL CLIENT, THE REAL WIDGET. Frames are published on a
// server::PriceBus exactly as altair_price_service publishes them -- trades,
// quotes and books -- and the checks read what the market watch, the depth
// table, the time-and-sales tape and the live option chain then SHOW.
//
// No check description here may contain the substring "F" "AIL" joined.

#include "../greek_watch.hpp"
#include "../live_market.hpp"

#include <server/price_bus.hpp>

#include <QApplication>
#include <QElapsedTimer>

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

using altair::ui::LiveRow;

LiveRow row(quint32 tok, const char* sym, const char* under, const char* kind, const char* group, qint64 expiry,
            double strike, qint64 lot) {
    LiveRow r;
    r.token = tok; r.symbol = QString::fromLatin1(sym); r.fyers = QStringLiteral("NSE:") + r.symbol;
    r.underlying = QString::fromLatin1(under); r.kind = QString::fromLatin1(kind); r.group = QString::fromLatin1(group);
    r.expiry_day = expiry; r.strike = strike; r.lot = lot;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace altair;
    using namespace altair::ui;
    QApplication app(argc, argv);
    std::printf("live market watch\n");

    boost::asio::io_context io;
    PriceBus bus(io, 0);
    if (!bus.ok()) { std::printf("  could not bind\n"); return 1; }
    PriceClient client;
    client.set_read_buffer(1 << 20);
    client.start(QStringLiteral("127.0.0.1"), bus.port());
    pump(bus, 3000, [&] { return client.connected() && bus.clients() == 1; });
    check(client.connected(), "the client connects to the bus");

    const qint64 opt_exp = live_detail::parse_day(QStringLiteral("2026-10-06"));
    const qint64 fut_exp = live_detail::parse_day(QStringLiteral("2026-10-27"));
    LiveMarketWatch page(&client, QStringLiteral("/nonexistent"));
    page.resize(1400, 800);
    page.set_rows_for_test({row(256265, "NIFTY 50", "NIFTY", "index", "Indices", 0, 0, 1),
                            row(12468226, "NIFTY26OCTFUT", "NIFTY", "future", "Futures", fut_exp, 0, 65),
                            row(1001, "NIFTY26O0624000CE", "NIFTY", "call", "NIFTY options", opt_exp, 24000, 65),
                            row(1002, "NIFTY26O0624000PE", "NIFTY", "put", "NIFTY options", opt_exp, 24000, 65)});
    page.show();

    // 2026-10-01 11:00 IST, simulated.
    const std::int64_t t0 = (live_detail::parse_day(QStringLiteral("2026-10-01")) * 86400 + 5 * 3600 + 30 * 60) * 1'000'000'000LL;
    const auto quote = [&](quint32 tok, std::int64_t prev, std::int64_t bid, std::int64_t ask) {
        QuotePayload q;
        q.token = tok;
        q.flags = kQuoteHasPrevClose | kQuoteSimulated | (bid > 0 ? kQuoteHasTop : 0);
        q.prev_close = prev; q.bid = bid; q.ask = ask; q.bid_qty = 650; q.ask_qty = 1300;
        bus.publish_quote(q, t0);
    };
    const auto trade = [&](quint32 tok, std::int64_t px, std::int64_t qty, std::int64_t ts) {
        PricePayload p;
        p.token = tok;
        // An index has no volume and no OI: the bits stay clear for it.
        p.flags = tok == 256265 ? std::uint16_t{kPriceSimulated} : std::uint16_t(kPriceSimulated | kPriceHasVolume | kPriceHasOi);
        p.last_paise = px; p.last_qty = qty; p.volume = 1000 + qty; p.oi = 50000; p.exchange_ts_ns = ts;
        bus.publish(kTopicTrades, p, nullptr, nullptr, ts);
    };
    quote(256265, 2390000, 0, 0);
    trade(256265, 2398000, 0, t0);
    quote(12468226, 2400000, 2401230, 2401250);
    trade(12468226, 2401200, 65, t0 + 1'000'000);
    trade(12468226, 2401250, 130, t0 + 2'000'000);   // an uptick
    // The options expire on 6 October, the future on 27 October: the chain
    // carries the future's mid (24012.40) back 21 days at 6.5 %, a forward of
    // about 23922.8. Priced at 13 % vol on that forward with 5.19 days left,
    // the 24000 call is ~109.8 and the put ~186.8 -- so both invert to ~13 %.
    quote(1001, 15000, 10950, 11010);
    trade(1001, 10980, 65, t0 + 3'000'000);
    quote(1002, 20000, 18650, 18710);
    trade(1002, 18680, 65, t0 + 4'000'000);
    PricePayload book;
    book.token = 12468226;
    book.flags = kPriceHasBook | kPriceSimulated;
    book.depth_levels = 5;   // the HSM and Kite books; the FYERS 50-level book is below
    PriceLevel bids[kMaxDepthLevels]{}, asks[kMaxDepthLevels]{};
    for (std::size_t i = 0; i < kMaxDepthLevels; ++i) {
        bids[i] = PriceLevel{2401230 - 10 * static_cast<std::int64_t>(i), 650 * static_cast<std::int64_t>(i + 1), static_cast<std::uint32_t>(i + 1), 0};
        asks[i] = PriceLevel{2401250 + 10 * static_cast<std::int64_t>(i), 1300, 2, 0};
    }
    bus.publish(kTopicBook, book, bids, asks, t0 + 5'000'000);
    pump(bus, 3000, [&] {
        const LivePrice* p = client.price(1002);
        return p != nullptr && p->trades >= 1 && client.price(12468226) != nullptr && client.price(12468226)->levels == 5;
    });
    pump(bus, 400, [] { return false; });   // let the 100 ms painter and the 250 ms side panel run

    auto* m = page.model();
    const int fr = m->row_of(12468226);
    check(fr == 1, "the future has its row");
    const QString ltp = m->data(m->index(fr, LiveWatchModel::Ltp), Qt::DisplayRole).toString();
    check(ltp == QStringLiteral("24012.50 ▲"), "LTP shows the last trade with an up-arrow after an uptick");
    check(m->data(m->index(fr, LiveWatchModel::Chg), Qt::DisplayRole).toString() == QStringLiteral("+12.50"),
          "change is against the previous close");
    check(m->data(m->index(fr, LiveWatchModel::Bid), Qt::DisplayRole).toString() == QStringLiteral("24012.30")
              && m->data(m->index(fr, LiveWatchModel::AskQty), Qt::DisplayRole).toString() == QStringLiteral("1,300"),
          "best bid and ask size from the quote topic");
    check(m->data(m->index(fr, LiveWatchModel::Ltq), Qt::DisplayRole).toString() == QStringLiteral("130"),
          "last traded quantity");
    check(m->data(m->index(fr, LiveWatchModel::Ltp), Qt::BackgroundRole).isValid(), "a moved price is lit");
    const int ir = m->row_of(256265);
    check(m->data(m->index(ir, LiveWatchModel::Bid), Qt::DisplayRole).toString() == QStringLiteral("—")
              && m->data(m->index(ir, LiveWatchModel::Volume), Qt::DisplayRole).toString() == QStringLiteral("—"),
          "an index shows a dash for bid and volume -- absent, not zero");
    check(m->data(m->index(fr, LiveWatchModel::Trades), Qt::DisplayRole).toString() == QStringLiteral("2"),
          "every trade is counted");

    page.select_token(12468226);
    pump(bus, 400, [] { return false; });
    auto* depth = page.depth();
    check(depth->rowCount() == 6 && depth->item(0, 2) && depth->item(0, 2)->text() == QStringLiteral("24012.30")
              && depth->item(4, 3) && depth->item(4, 3)->text() == QStringLiteral("24012.90"),
          "depth shows five levels a side");
    check(depth->item(5, 1) && depth->item(5, 1)->text() == QStringLiteral("9,750"), "and the bid total");
    {
        // The FYERS 50-level book: every level shows, the total under them.
        PricePayload deep = book;
        deep.depth_levels = static_cast<std::uint16_t>(kMaxDepthLevels);
        bus.publish(kTopicBook, deep, bids, asks, t0 + 6'000'000);
        pump(bus, 3000, [&] { return client.price(12468226) != nullptr && client.price(12468226)->levels == kMaxDepthLevels; });
        pump(bus, 400, [] { return false; });
        check(depth->rowCount() == 51 && depth->item(49, 2) && depth->item(49, 2)->text() == QStringLiteral("24007.40")
                  && depth->item(49, 3) && depth->item(49, 3)->text() == QStringLiteral("24017.40"),
              "a 50-level book shows all fifty levels a side");
        check(depth->item(50, 0) && depth->item(50, 0)->text() == QStringLiteral("Total"), "with the totals under the fiftieth");
    }
    auto* tape = page.tape();
    check(tape->rowCount() == 2 && tape->item(0, 1) && tape->item(0, 1)->text() == QStringLiteral("24012.50")
              && tape->item(1, 1)->text() == QStringLiteral("24012.00"),
          "time and sales lists every trade, newest first");
    check(tape->item(0, 0) && tape->item(0, 0)->text() == QStringLiteral("11:00:00.002"), "stamped in IST to the millisecond");

    auto* chain = page.chain();
    chain->refresh();
    auto* g = chain->grid();
    check(g->rowCount() == 1, "the chain has the one streamed strike");
    const QString ce_iv = g->item(0, LiveChainView::CIv) ? g->item(0, LiveChainView::CIv)->text() : QString();
    const QString pe_iv = g->item(0, LiveChainView::PIv) ? g->item(0, LiveChainView::PIv)->text() : QString();
    std::printf("    chain IV: CE %s, PE %s\n", qPrintable(ce_iv), qPrintable(pe_iv));
    check(!ce_iv.isEmpty() && ce_iv != QStringLiteral("—") && !pe_iv.isEmpty() && pe_iv != QStringLiteral("—"),
          "IV is inverted from the market price on both sides");
    check(std::fabs(ce_iv.toDouble() - pe_iv.toDouble()) < 1.0 && std::fabs(ce_iv.toDouble() - 13.0) < 1.0,
          "and a call and put at one strike agree, off the forward carried to the options' expiry");
    const QString cd = g->item(0, LiveChainView::CDelta) ? g->item(0, LiveChainView::CDelta)->text() : QString();
    check(cd.toDouble() > 0.4 && cd.toDouble() < 0.6, "an at-the-money call's delta is near one half");

    // GREEK WATCH: the 24000 call from the chain, every column from the stream.
    {
        GreekWatchWindow gw(&client);
        GreekLeg leg;
        leg.c.token = 1001; leg.c.symbol = QStringLiteral("NIFTY26O0624000CE"); leg.c.strike = 24000;
        leg.c.expiry_day = opt_exp; leg.c.lot = 65; leg.c.call = true;
        leg.under = QStringLiteral("NIFTY"); leg.fut = 12468226; leg.fut_expiry = fut_exp; leg.spot = 256265;
        GreekLeg put = leg;
        put.c.token = 1002; put.c.symbol = QStringLiteral("NIFTY26O0624000PE"); put.c.call = false;
        check(gw.add(leg) && gw.add(put) && !gw.add(leg), "a strike joins Greek Watch once");
        gw.refresh();
        const GreekRow r = greek_row(&client, gw.legs()[0], 0.065);
        std::printf("    Greek Watch CE: IV %.2f %%, delta %.3f, gamma %.6f, vega %.2f, theta %.2f\n",
                    r.iv * 100.0, r.delta, r.gamma, r.vega, r.theta);
        check(std::fabs(r.iv - 0.13) < 0.01 && r.delta > 0.4 && r.delta < 0.6 && r.gamma > 0 && r.vega > 0 && r.theta < 0,
              "IV, delta, gamma, vega and theta fill from the live prices");
        check(gw.grid()->item(0, GreekWatchWindow::Units)->text() == QStringLiteral("65")
                  && gw.grid()->item(0, GreekWatchWindow::DVal)->text().toDouble() > 20.0,
              "one lot by default, and the value columns use it");
        gw.grid()->item(1, GreekWatchWindow::Units)->setText(QStringLiteral("-65"));
        gw.refresh();
        auto* sum = gw.summary();
        check(sum->rowCount() == 1 && sum->item(0, GreekWatchWindow::SLegs)->text() == QStringLiteral("2")
                  && sum->item(0, GreekWatchWindow::SUnits)->text() == QStringLiteral("0"),
              "the summary adds the legs up per underlying and expiry (long call, short put)");
        const double dval = sum->item(0, GreekWatchWindow::SDVal)->text().toDouble();
        check(dval > 50.0 && dval < 75.0, "long call + short put is about one lot of delta: a synthetic future");
    }

    // ONLY ROWS ON SCREEN REPAINT. Hide the watch: a tick repaints nothing.
    {
        auto* model = page.model();
        page.set_view(LiveMarketWatch::View::Chain);
        QApplication::processEvents();
        trade(12468226, 2401300, 65, t0 + 7'000'000);
        pump(bus, 600, [] { return false; });
        check(model->last_flush_rows() == 0, "with the watch off screen a tick repaints no row");
        page.set_view(LiveMarketWatch::View::Watch);
        QApplication::processEvents();
        trade(12468226, 2401350, 65, t0 + 8'000'000);
        pump(bus, 2000, [&] { return model->last_flush_rows() > 0; });
        check(model->last_flush_rows() == 1, "on screen, the one row that ticked repaints, and only it");
    }

    check(page.status_text().contains(QStringLiteral("SIM")), "a simulated stream is labelled SIM, never LIVE");
    check(!page.status_text().contains(QStringLiteral("● LIVE")), "and not LIVE");

    std::printf("%s\n", failures == 0 ? "all live market checks passed" : "live market checks did not pass");
    return failures == 0 ? 0 : 1;
}
