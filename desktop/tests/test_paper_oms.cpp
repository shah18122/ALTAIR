// The Terminal's paper order book (desktop/paper_oms.hpp) and the windows
// and market-watch actions around it: + / − orders fill at the live bid and
// ask, limits rest until crossed, positions net and mark, the book survives
// a restart, and the scrip search adds and removes scrips.
//
// No check description here may contain the substring "F" "AIL" joined.

#include "../paper_oms.hpp"
#include "../paper_windows.hpp"

#include <QApplication>
#include <QCompleter>
#include <QComboBox>
#include <QFile>
#include <QKeyEvent>
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

PaperQuote quote(qint64 bid, qint64 ask, std::int64_t ns = 1'790'000'000'000'000'000LL, qint64 size = 1500) {
    PaperQuote q;
    q.bid = bid; q.ask = ask; q.ltp = (bid + ask) / 2; q.ns = ns; q.quote_ns = ns;
    q.bid_qty = size; q.ask_qty = size;
    return q;
}

/// No latency: an order meets the quote it was placed against (sections 1-4).
PaperOms instant() {
    PaperOms oms;
    oms.set_policy(PaperExecPolicy{0, 10'000'000'000LL});
    return oms;
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
        PaperOms oms = instant();
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
        oms.on_quote(12468226, quote(2'499'800, 2'500'100), 4);
        check(oms.order(*lmt)->status == PaperStatus::Open, "and keeps resting while the ask is above it");
        oms.on_quote(12468226, quote(2'499'800, 2'499'900), 5);
        check(oms.order(*lmt)->status == PaperStatus::Filled && oms.trades().back().price_paise == 2'499'900,
              "it fills when the ask crosses, at that ask (better than the limit)");
        check(!std::isnan(oms.trades().back().expenses), "expenses are charged on the fill");
    }

    std::printf("\n[2] refusals, modify, cancel\n");
    {
        PaperOms oms = instant();
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
        PaperQuote last_only;
        last_only.ltp = 2'510'050;
        last_only.ns = 1'790'000'000'000'000'000LL;
        const auto none = oms.place(order(PaperSide::Buy, PaperType::Market, 75), last_only, 5);
        check(!none && oms.orders().back().status == PaperStatus::Rejected
                  && oms.orders().back().note.contains(QStringLiteral("not filled at the last trade")),
              "a market order with only a last trade is rejected, never filled at it");
        (void)oms.place(order(PaperSide::Buy, PaperType::Limit, 75, 2'000'000), quote(2'510'000, 2'510'100), 6);
        (void)oms.place(order(PaperSide::Buy, PaperType::Limit, 75, 2'000'000), quote(2'510'000, 2'510'100), 7);
        check(oms.cancel_all(8) == 2 && oms.open_orders() == 0, "cancel all clears every pending order");
    }

    std::printf("\n[3] positions\n");
    {
        PaperOms oms = instant();
        (void)oms.place(order(PaperSide::Buy, PaperType::Market, 150), quote(2'500'000, 2'500'100), 1);
        (void)oms.place(order(PaperSide::Sell, PaperType::Market, 75), quote(2'510'000, 2'510'100), 2);
        auto pos = oms.positions();
        check(pos.size() == 1 && pos[0].net == 75 && std::fabs(pos[0].realised - 75.0 * 99.0) < 1e-6,
              "a partial close realises against the average: 75 x 99.00");
        oms.on_quote(12468226, quote(2'520'000, 2'520'100), 3);
        pos = oms.positions();
        check(pos[0].mark_paise == 2'520'000 && std::fabs(pos[0].unrealised - 75.0 * 199.0) < 1e-6,
              "the open long is marked at the bid");
        (void)oms.place(order(PaperSide::Sell, PaperType::Market, 150), quote(2'520'000, 2'520'100), 3);
        pos = oms.positions();
        check(pos[0].net == -75 && std::fabs(pos[0].avg_open - 25'200.0) < 1e-6,
              "selling through flat opens a short at that price");
        check(std::isnan(oms.trades()[0].expenses) && pos[0].expenses_unpriced,
              "with no expense function the fills say unpriced, not zero");
        check(std::isnan(pos[0].net_pnl()), "and the net is unavailable, not gross presented as net");
    }

    std::printf("\n[4] the book survives a restart\n");
    {
        QTemporaryDir dir;
        const QString orders = dir.filePath(QStringLiteral("manual_orders.csv"));
        const QString trades = dir.filePath(QStringLiteral("manual_trades.csv"));
        const std::int64_t today = 1'790'000'000'000'000'000LL;
        {
            PaperOms oms = instant();
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
        PaperOms again = instant();
        again.restore(o, t);
        const auto id = again.place(order(PaperSide::Sell, PaperType::Market, 75), quote(2'500'000, 2'500'100, today), today);
        check(id && *id == 3 && again.positions()[0].net == 0, "numbering continues and the position nets out");
        std::vector<PaperOrder> o2;
        std::vector<PaperTrade> t2;
        paper_store::load(orders, trades, today + 86'400'000'000'000LL, o2, t2);
        check(o2.size() == 1 && o2[0].status == PaperStatus::Cancelled, "the next day, yesterday's resting order shows expired");
    }

    std::printf("\n[4b] a book written before partial fills still reads\n");
    {
        QTemporaryDir dir;
        const QString orders = dir.filePath(QStringLiteral("manual_orders.csv"));
        QFile f(orders);
        (void)f.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream out(&f);
        out << "id,ns,token,symbol,exchange,lot,tick,product,side,type,qty,limit,status,fill,done_ns,sim,note\n"
            << "7,1790000000000000000,12468226,NIFTY26OCTFUT,NFO,75,10,NRML,B,MKT,75,0,EXECUTED,2500100,1790000000000000000,0,an old note\n";
        f.close();
        std::vector<PaperOrder> o;
        std::vector<PaperTrade> t;
        paper_store::load(orders, dir.filePath(QStringLiteral("none.csv")), 1'790'000'000'000'000'000LL, o, t);
        check(o.size() == 1 && o[0].filled == 75 && o[0].note == QStringLiteral("an old note"),
              "an old EXECUTED row is read as filled whole, its note intact");
    }

    std::printf("\n[7] executable fills: latency, freshness, size, the book\n");
    {
        const std::int64_t t0 = 1'790'000'000'000'000'000LL;
        PaperOms oms;   // the default policy: 250 ms latency, 10 s quote age
        const auto id = oms.place(order(PaperSide::Buy, PaperType::Market, 150), quote(2'510'000, 2'510'100, t0, 75), t0);
        check(id && oms.order(*id)->status == PaperStatus::Open && oms.trades().empty(),
              "an order does not fill before the latency has passed on the feed's clock");
        oms.on_quote(12468226, quote(2'510'000, 2'510'100, t0 + 300'000'000, 75), t0 + 300'000'000);
        check(oms.trades().size() == 1 && oms.trades()[0].qty == 75 && oms.order(*id)->filled == 75
                  && oms.order(*id)->status == PaperStatus::Open && oms.trades()[0].basis.contains(QStringLiteral("part 75/150")),
              "after it, only the 75 shown at the ask fill; the rest keeps working");
        oms.on_quote(12468226, quote(2'510'100, 2'510'300, t0 + 400'000'000, 75), t0 + 400'000'000);
        check(oms.order(*id)->status == PaperStatus::Filled && oms.order(*id)->fill_paise == 2'510'200,
              "the rest fills on the next quote; the order shows the average");

        // Stale: a quote 30 s behind the feed's clock is not a price.
        PaperQuote other = quote(100, 200, t0 + 30'000'000'000LL);
        other.ltp = 150;
        oms.on_quote(1, other, t0 + 30'000'000'000LL);
        const auto stale = oms.place(order(PaperSide::Sell, PaperType::Market, 75), quote(2'510'000, 2'510'100, t0, 75), t0 + 30'000'000'000LL);
        check(!stale && oms.orders().back().note.contains(QStringLiteral("30.0 s old")),
              "a 30-second-old quote is refused, and the reason gives its age");
        // A stopped feed: no update for 20 s of wall time keeps nothing executable.
        PaperOms quiet;
        quiet.set_policy(PaperExecPolicy{0, 10'000'000'000LL});
        quiet.on_quote(12468226, quote(2'510'000, 2'510'100, t0), t0);
        check(!quiet.place(order(PaperSide::Buy, PaperType::Market, 75), quote(2'510'000, 2'510'100, t0), t0 + 20'000'000'000LL),
              "a feed that stopped 20 s ago does not leave its last quote dealable");

        // Five levels, walked when fresh; a limit takes only what is inside it.
        PaperOms deep;
        deep.set_policy(PaperExecPolicy{0, 10'000'000'000LL});
        PaperQuote b = quote(2'510'000, 2'510'100, t0, 75);
        b.levels = 3; b.book_ns = t0;
        b.asks[0] = {2'510'100, 75}; b.asks[1] = {2'510'200, 75}; b.asks[2] = {2'510'500, 150};
        const auto lim = deep.place(order(PaperSide::Buy, PaperType::Limit, 225, 2'510'200), b, t0);
        check(lim && deep.order(*lim)->filled == 150 && deep.order(*lim)->status == PaperStatus::Open
                  && deep.order(*lim)->fill_paise == 2'510'150,
              "a limit buy walks the asks inside its price (75 + 75), not the 25,105.00 level beyond");

        // Unmarked: no fresh quote, no unrealised figure, and no net.
        PaperOms mark;
        mark.set_policy(PaperExecPolicy{0, 10'000'000'000LL});
        mark.set_expenses([](const PaperTrade&) -> std::optional<double> { return 1.0; });
        (void)mark.place(order(PaperSide::Buy, PaperType::Market, 75), quote(2'510'000, 2'510'100, t0), t0);
        PaperQuote later = quote(100, 200, t0 + 60'000'000'000LL);
        mark.on_quote(1, later, t0 + 60'000'000'000LL);   // the feed moves on; the future is not quoted again
        const auto pos = mark.positions();
        check(pos.size() == 1 && pos[0].mark_paise == 0 && std::isnan(pos[0].net_pnl()),
              "a position whose quote went stale is unmarked, and its net is unavailable");
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

        // The GETS Add Scrip window: Exchange -> Instrument -> Symbol -> Expiry -> Option -> Strike.
        const auto row = [](quint32 tok, const char* sym, const char* ex, const char* seg, const char* name, const char* type,
                            const char* expiry, double strike, qint64 lot) {
            MasterScrip m;
            m.token = tok; m.symbol = QString::fromLatin1(sym); m.exchange = QString::fromLatin1(ex); m.segment = QString::fromLatin1(seg);
            m.name = QString::fromLatin1(name); m.type = QString::fromLatin1(type); m.expiry = QString::fromLatin1(expiry);
            m.strike = strike; m.lot = lot; m.display = m.symbol;
            return m;
        };
        const std::vector<MasterScrip> master{
            row(779521, "SBIN", "NSE", "NSE", "STATE BANK OF INDIA", "EQ", "", 0, 1),
            row(128028676, "SBIN", "BSE", "BSE", "STATE BANK OF INDIA", "EQ", "", 0, 1),
            row(9001, "NIFTY26OCTFUT", "NFO", "NFO-FUT", "NIFTY", "FUT", "2026-10-27", 0, 75),
            row(9002, "NIFTY26NOVFUT", "NFO", "NFO-FUT", "NIFTY", "FUT", "2026-11-24", 0, 75),
            row(9003, "SBIN26OCTFUT", "NFO", "NFO-FUT", "SBIN", "FUT", "2026-10-27", 0, 750),
            row(9010, "NIFTY26OCT25000CE", "NFO", "NFO-OPT", "NIFTY", "CE", "2026-10-27", 25000, 75),
            row(9011, "NIFTY26OCT25000PE", "NFO", "NFO-OPT", "NIFTY", "PE", "2026-10-27", 25000, 75),
            row(9012, "NIFTY26OCT25100CE", "NFO", "NFO-OPT", "NIFTY", "CE", "2026-10-27", 25100, 75),
        };
        // The loader: Exchange (NSE/BSE) -> Segment (E/FO) -> Symbol -> Expiry -> Type (FUT unless an option) -> Strike.
        std::vector<MasterScrip> full = master;
        full.push_back(row(9020, "SENSEX26O0880000CE", "BFO", "BFO-OPT", "SENSEX", "CE", "2026-10-08", 80000, 20));
        full.push_back(row(9021, "SENSEX26O0880000PE", "BFO", "BFO-OPT", "SENSEX", "PE", "2026-10-08", 80000, 20));
        AddScripBar add;
        add.set_master(&full);
        add.set_today(QStringLiteral("2026-10-01"));
        add.choose(QStringLiteral("NSE"), QStringLiteral("FO"), QStringLiteral("NIFTY"));
        check(add.type() == QStringLiteral("FUT") && add.token() == 9001 && add.expiries().size() == 2,
              "NSE FO NIFTY with nothing else chosen is the near future: FUT is the default");
        add.choose(QStringLiteral("NSE"), QStringLiteral("FO"), QStringLiteral("NIFTY"), QStringLiteral("2026-10-27"),
                   QStringLiteral("PE"), QStringLiteral("25000"));
        check(add.token() == 9011 && add.strikes() == QStringList({QStringLiteral("25000")}),
              "NIFTY 27-Oct PE 25000 names one contract; only the strikes that exist for PE are offered");
        add.choose(QStringLiteral("NSE"), QStringLiteral("FO"), QStringLiteral("NIFTY"), QStringLiteral("2026-11-24"), QStringLiteral("FUT"));
        check(add.token() == 9002, "FUT NIFTY November is the November future");
        add.set_today(QStringLiteral("2026-10-28"));
        check(add.expiries() == QStringList({QStringLiteral("2026-11-24")}), "an expiry that has passed is not offered");
        add.set_today(QStringLiteral("2026-10-01"));
        add.choose(QStringLiteral("NSE"), QStringLiteral("FO"), QStringLiteral("SBIN"), QStringLiteral("2026-10-27"), QStringLiteral("FUT"));
        check(add.token() == 9003, "a stock's future is under the same NSE FO segment");
        add.choose(QStringLiteral("BSE"), QStringLiteral("E"), QStringLiteral("SBIN"));
        check(add.token() == 128028676 && add.resolved_text().contains(QStringLiteral("BSE")), "BSE E SBIN is the BSE listing");
        add.choose(QStringLiteral("BSE"), QStringLiteral("FO"), QStringLiteral("SENSEX"));
        check(add.type() == QStringLiteral("CE") && !add.type_enabled(QStringLiteral("FUT")) && add.token() == 9020,
              "BSE FO lists SENSEX options; with no SENSEX future FUT is greyed out and CE is chosen");
        add.choose(QStringLiteral("NSE"), QStringLiteral("FO"), QStringLiteral("NIFTY"), QStringLiteral("2026-10-27"),
                   QStringLiteral("CE"), QStringLiteral("99999"));
        check(add.token() == 0, "choices naming no contract cannot be added");
        // Search: an equity by its company, any symbol by any part of it.
        auto* symbol_box = add.findChild<QComboBox*>(QStringLiteral("addSymbol"));
        add.choose(QStringLiteral("NSE"), QStringLiteral("E"), QStringLiteral("SBIN"));
        QCompleter* comp = symbol_box->completer();
        comp->setCompletionPrefix(QStringLiteral("bank of"));
        check(comp->completionCount() == 1 && comp->currentCompletion() == QStringLiteral("SBIN"),
              "typing the company ('bank of') offers SBIN, and choosing it types the symbol alone");
        add.choose(QStringLiteral("NSE"), QStringLiteral("FO"), QStringLiteral("NIFTY"));
        comp = symbol_box->completer();
        comp->setCompletionPrefix(QStringLiteral("ifty"));
        check(comp->completionCount() == 1 && comp->currentCompletion() == QStringLiteral("NIFTY"),
              "a symbol matches on any part of it, not only its start");
        quint32 added = 0;
        add.on_add = [&added](quint32 t) { added = t; };
        add.choose(QStringLiteral("NSE"), QStringLiteral("E"), QStringLiteral("SBIN"));
        add.findChild<QPushButton*>(QStringLiteral("addScripButton"))->click();
        check(added == 779521, "Add hands the contract to the watch");
        watch.set_master_for_test(master);
        check(watch.add_scrip(128028676), "the BSE listing joins the watch");
        const LiveRow* bse = watch.row_of_token(128028676);
        check(bse != nullptr && bse->fyers == QStringLiteral("BSE:SBIN-A") && paper_instrument_of(*bse).exchange == QStringLiteral("BSE"),
              "streamed as BSE:SBIN-A and ordered as a BSE equity");
        check(watch.add_scrip(9010) && watch.row_of_token(9010) != nullptr && watch.row_of_token(9010)->kind == QStringLiteral("call")
                  && watch.row_of_token(9010)->lot == 75,
              "an option joins as a call, with its lot");
        const LiveRow* r = watch.row_of_token(779521);
        const PaperInstrument pi = r != nullptr ? paper_instrument_of(*r) : PaperInstrument{};
        check(pi.exchange == QStringLiteral("NSE") && pi.tradable, "an added equity trades on NSE");

        // Watchlists: what is added joins the active list; Save as makes a new
        // one; loading a list shows only its scrips.
        check(watch.list_name() == QStringLiteral("Default") && watch.filter()->members().contains(9010),
              "an added scrip joins the active watchlist (Default)");
        check(watch.save_list_as(QStringLiteral("Options desk")) && watch.list_names().contains(QStringLiteral("Options desk")),
              "Save as keeps the list under a name of its own");
        watch.remove_scrip(9010);
        check(!watch.filter()->members().contains(9010), "Delete takes it off the list on screen");
        check(watch.load_list(QStringLiteral("Default")) && watch.filter()->members().contains(9010),
              "loading Default brings back its own scrips");
        check(watch.load_list(QStringLiteral("Options desk")) && !watch.filter()->members().contains(9010),
              "and Options desk its own: lists are saved and loaded by name");
        check(!watch.delete_list(QStringLiteral("Default")) && watch.delete_list(QStringLiteral("Options desk"))
                  && watch.list_name() == QStringLiteral("Default"),
              "Default cannot be deleted; deleting the open list goes back to Default");

        // Enter on a scrip opens the chain of its underlying, from the master.
        std::vector<MasterScrip> chain_master = full;
        chain_master.push_back(row(9030, "SBIN26OCT800CE", "NFO", "NFO-OPT", "SBIN", "CE", "2026-10-27", 800, 750));
        chain_master.push_back(row(9031, "SBIN26OCT800PE", "NFO", "NFO-OPT", "SBIN", "PE", "2026-10-27", 800, 750));
        chain_master.push_back(row(9032, "SBIN26OCT820CE", "NFO", "NFO-OPT", "SBIN", "CE", "2026-10-27", 820, 750));
        watch.chain()->set_today(QStringLiteral("2026-10-01"));
        watch.set_master_for_test(chain_master);
        (void)watch.add_scrip(779521);
        watch.select_token(779521);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(watch.view(), &enter);
        check(watch.view_mode() == LiveMarketWatch::View::Chain && watch.chain()->underlying() == QStringLiteral("SBIN")
                  && watch.chain()->grid()->rowCount() == 2,
              "Enter on SBIN (a stock, not an index) opens the SBIN option chain: two strikes");
        bool asked = false;
        for (const auto& [t, sym] : watch.chain()->requested()) asked = asked || t == 9030;
        QFile req(root.filePath(QStringLiteral("data/live/chain_request.csv")));
        const QString req_text = req.open(QIODevice::ReadOnly | QIODevice::Text) ? QTextStream(&req).readAll() : QString();
        check(asked && req_text.contains(QStringLiteral("9030,SBIN26OCT800CE")) && req_text.contains(QStringLiteral("9003,future")),
              "the chain asks the feed for its strikes and the stock's future (chain_request.csv)");
        quint32 greek = 0;
        watch.on_greek = [&greek](quint32 t) { greek = t; };
        watch.chain()->grid()->setCurrentCell(0, LiveChainView::CLtp);
        QApplication::sendEvent(watch.chain()->grid(), &enter);
        check(greek == 9030, "Enter on a CE cell opens Greek Watch for that call");
        QKeyEvent back(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
        QApplication::sendEvent(watch.chain()->grid(), &back);
        check(watch.view_mode() == LiveMarketWatch::View::Watch, "Backspace in the chain goes back to the watch");
    }

    std::printf("\n%s\n", failures == 0 ? "all paper book checks passed" : "paper book checks did not pass");
    return failures == 0 ? 0 : 1;
}
