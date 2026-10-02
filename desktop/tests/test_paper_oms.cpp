// The Terminal's paper order book (desktop/paper_oms.hpp) and the windows
// and market-watch actions around it: + / − orders fill at the live bid and
// ask, limits rest until crossed, positions net and mark, the book survives
// a restart, and the scrip search adds and removes scrips.
//
// No check description here may contain the substring "F" "AIL" joined.

#include "../paper_oms.hpp"
#include "../paper_windows.hpp"

#include <QApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

using namespace altair::ui;

PaperInstrument fut() {
    PaperInstrument i;
    i.token = 12468226;
    i.symbol = QStringLiteral("NIFTY26OCTFUT");
    i.exchange = QStringLiteral("NFO");
    i.lot = 75;
    i.tick_paise = 10;
    return i;
}

PaperQuote quote(qint64 bid, qint64 ask, std::int64_t ns = 1'790'000'000'000'000'000LL) {
    PaperQuote q;
    q.bid = bid; q.ask = ask; q.ltp = (bid + ask) / 2; q.ns = ns;
    return q;
}

PaperOrder order(PaperSide side, PaperType type, qint64 qty, qint64 limit = 0) {
    PaperOrder o;
    o.inst = fut();
    o.side = side;
    o.type = type;
    o.qty = qty;
    o.limit_paise = limit;
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    std::printf("paper order book\n");

    std::printf("\n[1] market and limit fills\n");
    {
        PaperOms oms;
        oms.set_expenses([](const PaperTrade& t) -> std::optional<double> { return 0.0001 * static_cast<double>(t.qty * t.price_paise) / 100.0; });
        const auto buy = oms.place(order(PaperSide::Buy, PaperType::Market, 75), quote(2'510'000, 2'510'100), 1);
        check(buy && oms.trades().size() == 1 && oms.trades()[0].price_paise == 2'510'100
                  && oms.trades()[0].basis == QStringLiteral("at ask"),
              "a market buy fills at once, at the ask");
        const auto sell = oms.place(order(PaperSide::Sell, PaperType::Market, 75), quote(2'510'000, 2'510'100), 2);
        check(sell && oms.trades().size() == 2 && oms.trades()[1].price_paise == 2'510'000,
              "a market sell fills at the bid");
        const auto lmt = oms.place(order(PaperSide::Buy, PaperType::Limit, 150, 2'500'000), quote(2'510'000, 2'510'100), 3);
        check(lmt && oms.order(*lmt)->status == PaperStatus::Open && oms.trades().size() == 2,
              "a limit buy below the ask rests");
        oms.on_quote(12468226, quote(2'499'800, 2'500'100));
        check(oms.order(*lmt)->status == PaperStatus::Open, "and keeps resting while the ask is above it");
        oms.on_quote(12468226, quote(2'499'800, 2'499'900));
        check(oms.order(*lmt)->status == PaperStatus::Filled && oms.trades().back().price_paise == 2'499'900,
              "it fills when the ask crosses, at that ask (better than the limit)");
        check(!std::isnan(oms.trades().back().expenses), "expenses are charged on the fill");
    }

    std::printf("\n[2] refusals, modify, cancel\n");
    {
        PaperOms oms;
        auto idx = order(PaperSide::Buy, PaperType::Market, 1);
        idx.inst.tradable = false;
        idx.inst.symbol = QStringLiteral("NIFTY 50");
        check(!oms.place(idx, quote(0, 0), 1) && oms.orders().back().status == PaperStatus::Rejected,
              "an index is refused, and the refusal is in the book");
        check(!oms.place(order(PaperSide::Buy, PaperType::Market, 70), quote(1, 2), 1),
              "a quantity that is not whole lots is refused");
        check(!oms.place(order(PaperSide::Buy, PaperType::Limit, 75, 2'500'005), quote(1, 2), 1),
              "an off-tick limit is refused");
        const auto id = oms.place(order(PaperSide::Sell, PaperType::Limit, 75, 2'600'000), quote(2'510'000, 2'510'100), 2);
        check(id && oms.modify(*id, 150, 2'550'000, quote(2'510'000, 2'510'100)) && oms.order(*id)->qty == 150
                  && oms.order(*id)->limit_paise == 2'550'000,
              "a resting order modifies");
        check(oms.cancel(*id, 3) && oms.order(*id)->status == PaperStatus::Cancelled && !oms.cancel(*id, 4),
              "and cancels, once");
        const auto none = oms.place(order(PaperSide::Buy, PaperType::Market, 75), PaperQuote{}, 5);
        check(none && oms.order(*none)->status == PaperStatus::Open, "a market order with no price yet waits for one");
        oms.on_quote(12468226, quote(2'510'000, 2'510'100));
        check(oms.order(*none)->status == PaperStatus::Filled, "and fills on the first quote");
        (void)oms.place(order(PaperSide::Buy, PaperType::Limit, 75, 2'000'000), quote(2'510'000, 2'510'100), 6);
        (void)oms.place(order(PaperSide::Buy, PaperType::Limit, 75, 2'000'000), quote(2'510'000, 2'510'100), 7);
        check(oms.cancel_all(8) == 2 && oms.open_orders() == 0, "cancel all clears every pending order");
    }

    std::printf("\n[3] positions\n");
    {
        PaperOms oms;
        (void)oms.place(order(PaperSide::Buy, PaperType::Market, 150), quote(2'500'000, 2'500'100), 1);
        (void)oms.place(order(PaperSide::Sell, PaperType::Market, 75), quote(2'510'000, 2'510'100), 2);
        auto pos = oms.positions();
        check(pos.size() == 1 && pos[0].net == 75 && std::fabs(pos[0].realised - 75.0 * 99.0) < 1e-6,
              "a partial close realises against the average: 75 x 99.00");
        oms.on_quote(12468226, quote(2'520'000, 2'520'100));
        pos = oms.positions();
        check(pos[0].mark_paise == 2'520'000 && std::fabs(pos[0].unrealised - 75.0 * 199.0) < 1e-6,
              "the open long is marked at the bid");
        (void)oms.place(order(PaperSide::Sell, PaperType::Market, 150), quote(2'520'000, 2'520'100), 3);
        pos = oms.positions();
        check(pos[0].net == -75 && std::fabs(pos[0].avg_open - 25'200.0) < 1e-6,
              "selling through flat opens a short at that price");
        check(std::isnan(oms.trades()[0].expenses) && pos[0].expenses_unpriced,
              "with no expense function the fills say unpriced, not zero");
    }

    std::printf("\n[4] the book survives a restart\n");
    {
        QTemporaryDir dir;
        const QString orders = dir.filePath(QStringLiteral("manual_orders.csv"));
        const QString trades = dir.filePath(QStringLiteral("manual_trades.csv"));
        const std::int64_t today = 1'790'000'000'000'000'000LL;
        {
            PaperOms oms;
            oms.on_order = [&](const PaperOrder& o) { paper_store::append_order(orders, o); };
            oms.on_trade = [&](const PaperTrade& t) { paper_store::append_trade(trades, t); };
            (void)oms.place(order(PaperSide::Buy, PaperType::Market, 75), quote(2'500'000, 2'500'100, today), today);
            (void)oms.place(order(PaperSide::Buy, PaperType::Limit, 75, 2'400'000), quote(2'500'000, 2'500'100, today), today);
        }
        std::vector<PaperOrder> o;
        std::vector<PaperTrade> t;
        paper_store::load(orders, trades, today, o, t);
        check(t.size() == 1 && t[0].price_paise == 2'500'100 && t[0].inst.lot == 75, "the trade is read back");
        check(o.size() == 2 && o[1].status == PaperStatus::Open, "today's resting order is read back resting");
        PaperOms again;
        again.restore(o, t);
        const auto id = again.place(order(PaperSide::Sell, PaperType::Market, 75), quote(2'500'000, 2'500'100, today), today);
        check(id && *id == 3 && again.positions()[0].net == 0, "numbering continues and the position nets out");
        std::vector<PaperOrder> o2;
        std::vector<PaperTrade> t2;
        paper_store::load(orders, trades, today + 86'400'000'000'000LL, o2, t2);
        check(o2.size() == 1 && o2[0].status == PaperStatus::Cancelled, "the next day, yesterday's resting order shows expired");
    }

    std::printf("\n[5] the order window\n");
    {
        PaperInstrument i = fut();
        PaperOrderWindow w(PaperSide::Sell, i, nullptr);
        w.set_for_test(3, true, 25'101.20);
        const PaperOrder o = w.order();
        check(o.side == PaperSide::Sell && o.qty == 225 && o.type == PaperType::Limit && o.limit_paise == 2'510'120,
              "lots become units, and the limit is in paise");
    }

    std::printf("\n[6] adding and removing scrips\n");
    {
        QTemporaryDir root;
        LiveMarketWatch watch(nullptr, root.path());
        MasterScrip sbin;
        sbin.token = 779521; sbin.symbol = QStringLiteral("SBIN"); sbin.exchange = QStringLiteral("NSE");
        sbin.segment = QStringLiteral("NSE"); sbin.display = QStringLiteral("SBIN  ·  NSE EQ  ·  STATE BANK OF INDIA");
        watch.set_master_for_test({sbin});
        check(watch.add_scrip(779521) && watch.model()->row_of(779521) >= 0, "a scrip from the master joins the watch at once");
        QFile f(root.filePath(QStringLiteral("data/live/watchlist.csv")));
        const bool opened = f.open(QIODevice::ReadOnly | QIODevice::Text);
        const QString text = opened ? QTextStream(&f).readAll() : QString();
        check(text.contains(QStringLiteral("779521,SBIN")), "and is written to data/live/watchlist.csv for the feed");
        check(!watch.add_scrip(1), "a token the master does not know is not added");
        watch.remove_scrip(779521);
        f.close();
        QFile g(root.filePath(QStringLiteral("data/live/watchlist.csv")));
        const QString after = g.open(QIODevice::ReadOnly | QIODevice::Text) ? QTextStream(&g).readAll() : QString();
        check(!after.contains(QStringLiteral("779521")), "removing takes it out of the feed's list");
        const LiveRow* r = watch.row_of_token(779521);
        const PaperInstrument pi = r != nullptr ? paper_instrument_of(*r) : PaperInstrument{};
        check(pi.exchange == QStringLiteral("NSE") && pi.tradable, "an added equity trades on NSE");
    }

    std::printf("\n%s\n", failures == 0 ? "all paper book checks passed" : "paper book checks did not pass");
    return failures == 0 ? 0 : 1;
}
