// Acceptance tests for desktop/obi_page.hpp: the Order Book Imbalance page.
//
// The books and prints come over the real bus into the real client, so what
// is tested is that the page sees EVERY frame, offers only BANKNIFTY and the
// csv stocks, runs an engine for the future and each of the 11 strikes, shows
// the four trade types from the trader's own function, arms the punch buttons
// and fills them on paper (options sold only), works the formulas with the
// live numbers, and asks for the 50-level book in the file the feed reads.
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
    QDir(root.path()).mkpath(QStringLiteral("config"));
    write(root.path() + QStringLiteral("/config/obi_underlyings.csv"), "symbol,target,stop,safety\nNYKAA,0.80,0.40,0.05\n");
    // BANKNIFTY: the future at 52,000 and 13 strikes a side of 100 (the page keeps 11);
    // a later expiry that must not appear; NIFTY, which the page no longer offers; NYKAA's future.
    QByteArray uni = "token,fyers,symbol,underlying,kind,expiry,strike,lot,tick,group,depth\n"
                     "1,NSE:NIFTY50-INDEX,NIFTY 50,NIFTY,index,,0,1,0.05,Indices,0\n"
                     "13,NSE:NIFTY26NOVFUT,NIFTY26NOVFUT,NIFTY,future,2026-11-24,0,75,0.05,Futures,5\n"
                     "14,NSE:BANKNIFTY26NOVFUT,BANKNIFTY26NOVFUT,BANKNIFTY,future,2026-11-24,0,35,0.05,Futures,5\n"
                     "15,NSE:BANKNIFTY26DECFUT,BANKNIFTY26DECFUT,BANKNIFTY,future,2026-12-29,0,35,0.05,Futures,5\n"
                     "20,NSE:NYKAA26NOVFUT,NYKAA26NOVFUT,NYKAA,future,2026-11-24,0,3125,0.01,Futures,5\n";
    for (int k = -6; k <= 6; ++k) {
        const int strike = 52000 + 100 * k;
        uni += QStringLiteral("%1,NSE:BANKNIFTY26NOV%2CE,BANKNIFTY26NOV%2CE,BANKNIFTY,call,2026-11-24,%2,35,0.05,BANKNIFTY options,5\n")
                   .arg(1000 + 2 * (k + 6)).arg(strike).toLatin1();
        uni += QStringLiteral("%1,NSE:BANKNIFTY26NOV%2PE,BANKNIFTY26NOV%2PE,BANKNIFTY,put,2026-11-24,%2,35,0.05,BANKNIFTY options,5\n")
                   .arg(1001 + 2 * (k + 6)).arg(strike).toLatin1();
    }
    uni += "2000,NSE:BANKNIFTY26DEC52000CE,BANKNIFTY26DEC52000CE,BANKNIFTY,call,2026-12-29,52000,35,0.05,BANKNIFTY options,5\n";
    write(root.path() + QStringLiteral("/data/live/universe.csv"), uni);
    const quint32 ce52000 = 1000 + 2 * 6, pe51900 = 1001 + 2 * 5;

    boost::asio::io_context io;
    PriceBus bus(io, 0);
    PriceClient client;
    client.start(QStringLiteral("127.0.0.1"), bus.port());
    pump(bus, 3000, [&] { return client.connected() && bus.clients() == 1; });

    const std::int64_t t0 = 1'793'424'600'000'000'000LL;   // 2026-11-02 11:00 IST
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

    book(14, 5200000, 5200005, 1000, 1000, t0);
    print(14, 5200000, 0, t0);   // the day's volume so far
    pump(bus, 3000, [&] { const LivePrice* p = client.price(14); return p != nullptr && p->levels == 5 && p->has_volume; });

    OrderBookImbalancePage page(&client, root.path());
    page.resize(1600, 950);
    page.show();
    QCoreApplication::processEvents();

    // Two instruments, nothing else.
    {
        const auto buttons = page.findChildren<QPushButton*>();
        QStringList unders;
        for (auto* b : buttons) if (b->objectName().startsWith(QStringLiteral("obiUnder_"))) unders << b->text();
        check(unders == QStringList({QStringLiteral("BANKNIFTY"), QStringLiteral("NYKAA")}), "only BANKNIFTY and the csv stock (NYKAA) are offered");
    }
    check(page.underlying() == QStringLiteral("BANKNIFTY") && page.future_token() == 14 && page.token() == 14,
          "BANKNIFTY opens on its nearest future");
    check(page.engine() != nullptr && page.engine()->signals().valid && page.engine()->config().tick == 5,
          "the future's engine is primed with the book as it stands, tick from the instrument");

    // The 11 strikes of the nearest expiry, each with its own engine.
    {
        const auto& s = page.strike_list();
        bool both = s.size() == 11;
        for (const auto& r : s) both = both && r.ce != 0 && r.pe != 0 && page.engine_of(r.ce) != nullptr && page.engine_of(r.pe) != nullptr;
        check(both && s.front().strike == 51500 && s.back().strike == 52500 && page.strike_list()[5].strike == 52000,
              "11 strikes, ATM +/- 5, call and put, each with its own engine");
        check(page.engine_of(2000) == nullptr && page.engine_of(13) == nullptr, "no later expiry and no NIFTY");
        check(page.watched() == 23 && page.strikes_table()->rowCount() == 11, "23 contracts watched: the future and 22 options");
    }

    // A burst: twenty books in one read; the engine sees every one.
    const std::uint64_t before = page.engine()->signals().books;
    for (int i = 1; i <= 20; ++i) book(14, 5200000, 5200005, 1000 + 10 * i, 1000, t0 + i * 1'000'000);
    pump(bus, 3000, [&] { return page.engine()->signals().books >= before + 20; });
    check(page.engine()->signals().books == before + 20, "every book of a burst reaches the engine, not one per burst");

    // An option's book reaches its own engine and the grid.
    book(ce52000, 30000, 30010, 700, 300, t0 + 25'000'000);
    pump(bus, 3000, [&] { return page.engine_of(ce52000)->signals().valid; });
    page.repaint_now();
    check(page.engine_of(ce52000)->signals().bid == 30000 && page.strikes_table()->item(5, 0)->text() == QStringLiteral("+0.40"),
          "the 52000 call's book reaches its engine; the grid shows its OBI5 (+0.40)");
    check(page.strikes_table()->item(5, 3)->text() == QStringLiteral("bid-heavy"), "and that its book leans to the bid");

    // A buyer lifts 400 off the ask: executed, not cancelled.
    print(14, 5200005, 400, t0 + 30'000'000);
    book(14, 5200000, 5200005, 1200, 1000, t0 + 31'000'000, 600);
    pump(bus, 3000, [&] { return page.engine()->signals().ask_exec_drop >= 400; });
    {
        const auto& s = page.engine()->signals();
        check(s.buy_exec == 400 && s.ask_exec_drop == 400 && s.ask_cancel == 0, "400 bought at the ask: executed off the ask");
        bool buy = false;
        for (const auto& e : page.events()) buy = buy || (e.kind == order_flow_detail::BuyExec && e.qty == 400);
        check(buy, "and the tape lists the buy");
    }

    // Bids heavy and held: type A turns LONG, the punch buttons arm.
    for (int i = 0; i < 4; ++i) book(14, 5200000, 5200005, 3000, 1000, t0 + 100'000'000 + i * 300'000'000LL);
    pump(bus, 3000, [&] { return page.engine()->signals().persist >= 4; });
    page.repaint_now();
    check(page.card_text(0).contains(QStringLiteral("LONG")) && page.card_text(0).contains(QStringLiteral("bids dominate")),
          "card A shows LONG with the trader's reason");
    check(page.card_text(1).contains(QStringLiteral("off")), "card B is off");
    check(page.ready_future()->isEnabled() && page.ready_future()->text().startsWith(QStringLiteral("BUY BANKNIFTY26NOVFUT @ 52000.05")),
          "the future BUY is armed at the ask");
    check(page.ready_option()->isEnabled() && page.ready_option()->text().startsWith(QStringLiteral("SELL BANKNIFTY26NOV51900PE")),
          "the option to SELL is armed: the put one strike below the money");
    check(page.suggest_text().contains(QStringLiteral("A LONG")), "the suggestion names the type");
    check(page.how_text().contains(QStringLiteral("How a trade happens")) && page.how_text().contains(QStringLiteral("SELLS a put"))
              && page.how_text().contains(QStringLiteral("now <b>LONG</b>")),
          "'How a trade happens' walks the steps, with A now LONG");

    // One click: bought on paper at the ask, logged with the reason.
    page.ready_future()->click();
    {
        const auto& pos = page.punch_positions();
        check(pos.count(14) == 1 && pos.at(14).qty == 35 && std::fabs(pos.at(14).avg - 52000.05) < 1e-9, "one click: 1 lot (35) bought at 52000.05");
        QFile f(root.path() + QStringLiteral("/data/live/punch/obi_punch.csv"));
        const bool open = f.open(QIODevice::ReadOnly);
        const QString log = open ? QString::fromUtf8(f.readAll()) : QString();
        check(log.contains(QStringLiteral("BANKNIFTY26NOVFUT,BUY,1,35,52000.05")) && log.contains(QStringLiteral("A LONG: bids dominate")),
              "the fill is logged with the readings behind it");
    }
    check(page.punch(14, -1, 1).startsWith(QStringLiteral("SOLD")) && page.punch_positions().empty() && std::fabs(page.realised() - (-0.05 * 35)) < 1e-9,
          "selling it back at the bid closes it and realises the spread");

    // Options are sold only.
    check(page.punch(pe51900, 1, 1).startsWith(QStringLiteral("refused")) && page.punch_positions().empty(), "a BUY that opens an option is refused");
    book(pe51900, 20000, 20010, 500, 500, t0 + 2'000'000'000);
    pump(bus, 3000, [&] { return page.engine_of(pe51900)->signals().valid; });
    page.ready_option()->click();
    check(page.punch_positions().count(pe51900) == 1 && page.punch_positions().at(pe51900).qty == -35, "the armed put is sold: -35");
    check(page.punch(pe51900, 1, 2).startsWith(QStringLiteral("refused")), "buying more than the short is refused");
    check(page.punch(pe51900, 1, 1).startsWith(QStringLiteral("BOUGHT")) && page.punch_positions().empty(), "buying it back closes the short");

    // Close all.
    (void)page.punch(14, -1, 2);
    page.close_all();
    check(page.punch_positions().empty(), "close all flattens");

    // Open a strike from the grid.
    page.strikes_table()->cellClicked(5, 1);
    check(page.token() == ce52000, "clicking the 52000 call in the grid opens it on the ladder");
    page.strikes_table()->cellClicked(5, 4);
    check(page.token() == 14, "clicking the strike column goes back to the future");

    // The formulas, worked with the future's book: bids 3,000 a level, asks 1,000.
    page.repaint_now();
    {
        const auto& s = page.engine()->signals();
        const QString t = page.imbalance_text();
        check(page.tabs()->tabText(0) == QStringLiteral("How a trade happens") && page.tabs()->tabText(1) == QStringLiteral("Formulas, live"),
              "the tabs: how a trade happens, then the formulas");
        check(t.contains(QStringLiteral("(3,000 − 1,000) / (3,000 + 1,000)")) && std::fabs(s.obi1 - 0.5) < 1e-12, "OBI at the touch is worked");
        check(t.contains(QStringLiteral("(15,000 − 5,000) / (15,000 + 5,000)")), "the five-level sums are shown");
        check(t.contains(QStringLiteral("λ = 0.25")) && t.contains(QStringLiteral("above")), "the weights are stated, the microprice is above the mid");
        auto* lv = page.levels_table();
        check(lv->rowCount() == 5 && lv->item(0, 3)->text() == QStringLiteral("3,000") && lv->item(1, 1)->text() == QString::number(std::exp(-0.25), 'f', 3),
              "the level table: B_i and w_i");
    }
    check(page.head_text().contains(QStringLiteral("BANKNIFTY26NOVFUT")) && page.head_text().contains(QStringLiteral("SIM")),
          "the headline names the instrument and that the stream is simulated");
    check(page.counts_text().contains(QStringLiteral("buy 400")) && page.counts_text().contains(QStringLiteral("engines: 23")),
          "the counters and the engine count");

    // The rules.
    check(order_flow_detail::rules_for(QStringLiteral("BANKNIFTY")).target_pts == 25.0, "BANKNIFTY uses the trader's 25-point target");
    check(order_flow_detail::rules_for(QStringLiteral("NYKAA"), root.path()).target_pts == 0.80, "NYKAA reads its per-share target from the csv");
    {
        order_flow_detail::PunchPosition p;
        check(order_flow_detail::apply_fill(p, 10, 100.0) == 0.0 && order_flow_detail::apply_fill(p, 10, 110.0) == 0.0 && p.avg == 105.0,
              "adding averages the price");
        check(order_flow_detail::apply_fill(p, -30, 120.0) == 300.0 && p.qty == -10 && p.avg == 120.0, "closing realises; the rest reverses at the fill");
    }

    // The 50-level book: asked for in the file the feed and the OBI trader read.
    auto* deep = page.findChild<QCheckBox*>(QStringLiteral("flowDeep"));
    check(deep != nullptr && !deep->isChecked(), "the 50-level book is not asked for until it is ticked");
    deep->click();
    check(order_flow_detail::read_watch(root.path()) == QStringList{QStringLiteral("NSE:BANKNIFTY26NOVFUT")},
          "ticking it writes the symbol to data/live/obi_watch.csv");
    deep->click();
    check(order_flow_detail::read_watch(root.path()).isEmpty(), "and unticking removes it");

    // NYKAA: its future, no strikes in this universe.
    check(page.select_underlying(QStringLiteral("NYKAA")) && page.token() == 20 && page.strike_list().empty() && page.watched() == 1,
          "NYKAA opens its future; the BANKNIFTY contracts stop");

    {
        flow::FlowTotals a, b;
        b.buy = 10; b.ask_cancel = 5; b.hidden_sell = 2;
        const auto ev = order_flow_detail::events_between(a, b, 7, 100, 105);
        check(ev.size() == 3 && ev[0].kind == order_flow_detail::BuyExec, "each total that grew is one event");
    }

    std::printf("%s\n", failures == 0 ? "all order book imbalance checks passed" : "order book imbalance checks did not pass");
    return failures == 0 ? 0 : 1;
}
