// Acceptance tests for desktop/obi_page.hpp: the Order Book Imbalance page.
//
// The books and prints come over the real bus into the real client, so what
// is tested is that the page sees EVERY frame (a burst the grid sees once),
// classifies an execution as an execution and a cancel as a cancel, draws the
// ladder and readings from it, and asks for the 50-level book in the file
// the feed reads.
//
// No check description here may contain the substring "F" "AIL" joined.

#include "../obi_page.hpp"

#include <server/price_bus.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include <boost/asio/io_context.hpp>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "BAD ", what);
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

}  // namespace

int main(int argc, char** argv) {
    using namespace altair;
    using namespace altair::ui;
    QApplication app(argc, argv);
    std::printf("order book imbalance page\n");
    simple_mode::use_settings(false);

    QTemporaryDir root;
    QDir(root.path()).mkpath(QStringLiteral("data/live/paper"));
    write(root.path() + QStringLiteral("/data/live/universe.csv"),
          "token,fyers,symbol,underlying,kind,expiry,strike,lot,tick,group,depth\n"
          "1,NSE:NIFTY50-INDEX,NIFTY 50,NIFTY,index,,0,1,0.05,Indices,0\n"
          "102,NSE:NIFTY26OCT24000PE,NIFTY26OCT24000PE,NIFTY,put,2026-10-27,24000,75,0.05,NIFTY options,5\n"
          "101,NSE:NIFTY26OCT24000CE,NIFTY26OCT24000CE,NIFTY,call,2026-10-27,24000,75,0.05,NIFTY options,5\n"
          "13,NSE:NIFTY26OCTFUT,NIFTY26OCTFUT,NIFTY,future,2026-10-27,0,75,0.05,Futures,5\n"
          "14,NSE:BANKNIFTY26OCTFUT,BANKNIFTY26OCTFUT,BANKNIFTY,future,2026-10-27,0,35,0.05,Futures,5\n");

    boost::asio::io_context io;
    PriceBus bus(io, 0);
    PriceClient client;
    client.start(QStringLiteral("127.0.0.1"), bus.port());
    pump(bus, 3000, [&] { return client.connected() && bus.clients() == 1; });

    const std::int64_t t0 = 1'790'832'600'000'000'000LL;   // 2026-10-01 11:00 IST
    std::int64_t volume = 10'000;
    const auto book = [&](std::uint32_t tok, std::int64_t bid, std::int64_t ask, std::int64_t bq, std::int64_t aq,
                          std::int64_t ns, std::int64_t ask0 = -1) {
        PricePayload p;
        p.token = tok;
        p.flags = kPriceHasBook | kPriceSimulated;
        p.depth_levels = 5;
        PriceLevel b[kMaxDepthLevels]{}, a[kMaxDepthLevels]{};
        for (int i = 0; i < 5; ++i) {
            b[i] = PriceLevel{bid - 5 * i, bq, 3, 0};
            a[i] = PriceLevel{ask + 5 * i, aq, 2, 0};
        }
        if (ask0 >= 0) a[0].qty = ask0;
        bus.publish(kTopicBook, p, b, a, ns);
    };
    const auto print = [&](std::uint32_t tok, std::int64_t px, std::int64_t qty, std::int64_t ns) {
        volume += qty;
        PricePayload p;
        p.token = tok;
        p.flags = kPriceHasVolume | kPriceSimulated;
        p.last_paise = px;
        p.last_qty = qty;
        p.volume = volume;
        p.exchange_ts_ns = ns;
        bus.publish(kTopicTrades, p, nullptr, nullptr, ns);
    };

    book(13, 2400000, 2400005, 1000, 1000, t0);
    print(13, 2400000, 0, t0);   // the day's volume so far
    pump(bus, 3000, [&] { const LivePrice* p = client.price(13); return p != nullptr && p->levels == 5 && p->has_volume; });

    OrderBookImbalancePage page(&client, root.path());
    page.resize(1500, 900);
    page.show();
    QCoreApplication::processEvents();

    auto* under = page.findChild<QComboBox*>(QStringLiteral("flowUnderlying"));
    auto* contract = page.findChild<QComboBox*>(QStringLiteral("flowContract"));
    check(under != nullptr && under->count() == 2 && under->itemText(0) == QStringLiteral("NIFTY")
              && under->itemText(1) == QStringLiteral("BANKNIFTY"),
          "the underlyings, NIFTY first; an index (no order book) is not offered");
    check(contract != nullptr && contract->count() == 3 && contract->itemData(0).toUInt() == 13
              && contract->itemData(1).toUInt() == 101 && contract->itemData(2).toUInt() == 102,
          "its contracts: the future, then each strike's call and put");
    check(page.token() == 13 && page.engine() != nullptr && page.engine()->signals().valid,
          "the future is opened with the book as it stands");
    check(page.engine()->config().tick == 5, "the tick comes from the instrument (0.05 = 5 paise)");

    // A burst: twenty books in one read. The grid hears of it once; the page's engine sees every one.
    const std::uint64_t before = page.engine()->signals().books;
    for (int i = 1; i <= 20; ++i) book(13, 2400000, 2400005, 1000 + 10 * i, 1000, t0 + i * 1'000'000);
    pump(bus, 3000, [&] { return page.engine()->signals().books >= before + 20; });
    check(page.engine()->signals().books == before + 20, "every book of a burst reaches the engine, not one per burst");

    // A buyer lifts 400 off the ask: executed, not cancelled.
    print(13, 2400005, 400, t0 + 30'000'000);
    book(13, 2400000, 2400005, 1200, 1000, t0 + 31'000'000, 600);
    pump(bus, 3000, [&] { return page.engine()->signals().ask_exec_drop >= 400; });
    {
        const auto& s = page.engine()->signals();
        check(s.buy_exec == 400 && s.ask_exec_drop == 400 && s.ask_cancel == 0, "400 bought at the ask: executed off the ask");
        bool buy = false, taken = false;
        for (const auto& e : page.events()) {
            buy = buy || (e.kind == order_flow_detail::BuyExec && e.qty == 400);
            taken = taken || (e.kind == order_flow_detail::AskTaken && e.qty == 400);
        }
        check(buy && taken, "and the tape lists the buy and the size it took");
    }
    // 300 pulled from the second ask with no print: a cancel, once it has waited settle_ns.
    book(13, 2400000, 2400005, 1200, 1000, t0 + 40'000'000, 600);
    {
        PricePayload p;
        p.token = 13; p.flags = kPriceHasBook | kPriceSimulated; p.depth_levels = 5;
        PriceLevel b[kMaxDepthLevels]{}, a[kMaxDepthLevels]{};
        for (int i = 0; i < 5; ++i) { b[i] = PriceLevel{2400000 - 5 * i, 1200, 3, 0}; a[i] = PriceLevel{2400005 + 5 * i, 1000, 2, 0}; }
        a[0].qty = 600;
        a[1].qty = 700;
        bus.publish(kTopicBook, p, b, a, t0 + 41'000'000);
    }
    book(13, 2400000, 2400005, 1200, 1000, t0 + 900'000'000, 600);   // later: the drop has settled
    pump(bus, 3000, [&] { return page.engine()->signals().total.ask_cancel >= 300; });
    check(page.engine()->signals().total.ask_cancel == 300, "300 pulled with no print is a cancel");

    page.repaint_now();
    check(page.ladder()->bid_rows() == 5 && page.ladder()->ask_rows() == 5, "the ladder draws the five levels a side");
    check(page.gauges()->gauges().size() == 8 && page.gauges()->gauges()[1].name == QStringLiteral("OBI 5")
              && std::fabs(page.gauges()->gauges()[1].v - page.engine()->signals().obi_short) < 1e-12,
          "the readings show the engine's OBI 5");
    check(page.head_text().contains(QStringLiteral("NIFTY26OCTFUT")) && page.head_text().contains(QStringLiteral("micro"))
              && page.head_text().contains(QStringLiteral("SIM")),
          "the headline names the instrument, its microprice, and that the stream is simulated");
    check(page.counts_text().contains(QStringLiteral("buy 400")) && page.counts_text().contains(QStringLiteral("cancelled: ask 300")),
          "the counters split executed from cancelled");
    {
        auto* tape = page.tape();
        bool cancel_row = false;
        for (int r = 0; r < tape->rowCount(); ++r)
            cancel_row = cancel_row || (tape->item(r, 1) && tape->item(r, 1)->text() == QStringLiteral("Ask cancelled"));
        check(tape->rowCount() >= 3 && cancel_row, "the flow tape lists them, newest first");
        auto* adds = page.findChild<QCheckBox*>(QStringLiteral("flowShowAdds"));
        const int without = tape->rowCount();
        adds->setChecked(true);
        page.repaint_now();
        check(tape->rowCount() > without, "adds are listed on request");
    }
    check(page.samples() > 0, "the charts have points");

    // The formulas, worked with the book the engine read: bids 1,200 a level, asks 600 / 1,000 x4.
    {
        const auto& s = page.engine()->signals();
        const QString t = page.imbalance_text();
        check(page.tabs()->currentIndex() == 0 && page.tabs()->tabText(0).startsWith(QStringLiteral("Imbalance")),
              "the page opens on the formulas");
        check(t.contains(QStringLiteral("(1,200 − 600) / (1,200 + 600)")) && std::fabs(s.obi1 - 1.0 / 3.0) < 1e-12,
              "OBI at the touch is shown with its numbers: (1,200 - 600) / (1,200 + 600) = +0.33");
        check(t.contains(QStringLiteral("(6,000 − 4,600) / (6,000 + 4,600)")), "the five-level sums are shown");
        check(t.contains(QStringLiteral("e<sup>−λ(i−1)</sup>")) && t.contains(QStringLiteral("λ = 0.25")), "the weights and lambda are stated");
        check(t.contains(QStringLiteral("(600 × 24000.00 + 1,200 × 24000.05) / (1,200 + 600)")) && t.contains(QStringLiteral("above")),
              "the microprice is worked, and lies above the mid with the bigger bid");
        check(t.contains(QStringLiteral("S = 0.4 ×")), "S_t is broken into its terms");
        auto* lv = page.levels_table();
        const auto rows = order_flow_detail::level_rows(nullptr, nullptr, 0, 0.25);
        check(rows.empty(), "an empty book has no level rows");
        check(lv->rowCount() == 5 && lv->item(0, 3)->text() == QStringLiteral("1,200") && lv->item(0, 6)->text() == QStringLiteral("600")
                  && lv->item(1, 1)->text() == QString::number(std::exp(-0.25), 'f', 3),
              "the level table: B_i, A_i and w_i = e^-0.25(i-1)");
        const QString last = lv->item(4, 8)->text();
        check(last == QStringLiteral("%1 / %2").arg(order_flow_detail::signed2(s.obi_deep), order_flow_detail::signed2(s.obi_deep_w)),
              "its last row is the engine's OBI_N, plain and weighted");
        const QString c = page.checks_text();
        check(c.contains(QStringLiteral("A — Bid-side pressure")) && c.contains(QStringLiteral("D — Liquidity withdrawal"))
                  && c.contains(QStringLiteral("Long entry conditions")) && c.contains(QStringLiteral("Short entry conditions")),
              "strategies A-D and both sets of entry conditions are listed");
    }
    {   // The checklist itself, on a reading where every long condition holds.
        flow::FlowSignals s;
        s.valid = true; s.obi_short = 0.45; s.persist = 4; s.persist_ns = 900'000'000; s.ofi_norm = 0.6;
        s.buy_exec = 300; s.sell_exec = 75; s.ask_exec_drop = 225; s.prior_high = 2400000.0; s.mid = 2400012.5;
        s.bid = 2400010; s.ask = 2400015;
        const auto c = order_flow_detail::entry_checks(s, 1, order_flow_detail::rules_for(QStringLiteral("NIFTY")), 75);
        bool all = c.size() == 5;
        for (const auto& x : c) all = all && x.ok;
        check(all, "OBI5 > +0.30 held, OFI > +0.10, buys taking the ask, a breakout and room for costs: 5 of 5 long");
        const auto sh = order_flow_detail::entry_checks(s, -1, order_flow_detail::rules_for(QStringLiteral("NIFTY")), 75);
        bool none = true;
        for (std::size_t i = 0; i < 4; ++i) none = none && !sh[i].ok;
        check(none, "and none of the short conditions 1-4");
        s.persist = 2;
        check(!order_flow_detail::entry_checks(s, 1, order_flow_detail::ObiRules{}, 75)[0].ok, "two updates is not held");
        check(order_flow_detail::rules_for(QStringLiteral("BANKNIFTY")).target_pts == 25.0, "BANKNIFTY uses the trader's 25-point target");
        {
            const auto ny = order_flow_detail::rules_for(QStringLiteral("NYKAA"), QStringLiteral(ALTAIR_SOURCE_DIR));
            check(ny.target_pts > 0.0 && ny.target_pts < 5.0, "NYKAA reads its per-share target from config/obi_underlyings.csv");
        }
        check(order_flow_detail::obi_reading(0.5) == QStringLiteral("stronger visible bid-side depth")
                  && order_flow_detail::obi_reading(0.0) == QStringLiteral("balanced visible depth")
                  && order_flow_detail::obi_reading(-0.5) == QStringLiteral("stronger visible ask-side depth"),
              "the brief's table: +0.50 bid-side, 0 balanced, -0.50 ask-side");
    }

    // The 50-level book: asked for in the file the feed and the OBI trader read.
    auto* deep = page.findChild<QCheckBox*>(QStringLiteral("flowDeep"));
    check(deep != nullptr && !deep->isChecked(), "the 50-level book is not asked for until it is ticked");
    deep->click();
    check(order_flow_detail::read_watch(root.path()) == QStringList{QStringLiteral("NSE:NIFTY26OCTFUT")},
          "ticking it writes the symbol to data/live/obi_watch.csv");
    deep->click();
    check(order_flow_detail::read_watch(root.path()).isEmpty(), "and unticking removes it");

    // Any strike: the call, chosen from the list; the future's frames stop.
    contract->setCurrentIndex(1);
    QCoreApplication::processEvents();
    check(page.token() == 101 && page.engine()->signals().books == 0, "choosing the 24000 call opens it, from nothing");
    book(13, 2400000, 2400005, 1000, 1000, t0 + 2'000'000'000);
    book(101, 15000, 15010, 750, 300, t0 + 2'000'000'000);
    pump(bus, 3000, [&] { return page.engine()->signals().books >= 1; });
    check(page.engine()->signals().books == 1 && page.engine()->signals().bid == 15000, "only the call's book reaches its engine");
    {
        auto* search = page.findChild<QLineEdit*>(QStringLiteral("flowSearch"));
        search->setText(QStringLiteral("banknifty26octfut"));
        Q_EMIT search->returnPressed();
        check(page.token() == 14 && under->currentText() == QStringLiteral("BANKNIFTY"), "search opens any symbol, case aside");
    }

    // The events between two snapshots.
    {
        flow::FlowTotals a, b;
        b.buy = 10; b.ask_cancel = 5; b.hidden_sell = 2;
        const auto ev = order_flow_detail::events_between(a, b, 7, 100, 105);
        check(ev.size() == 3 && ev[0].kind == order_flow_detail::BuyExec && ev[1].kind == order_flow_detail::AskCancel
                  && ev[2].kind == order_flow_detail::HiddenSell && ev[2].qty == 2,
              "each total that grew is one event");
        check(order_flow_detail::events_between(b, a, 8, 100, 105).empty(), "a reset in between reports nothing");
    }

    std::printf("%s\n", failures == 0 ? "all order book imbalance checks passed" : "order book imbalance checks did not pass");
    return failures == 0 ? 0 : 1;
}
