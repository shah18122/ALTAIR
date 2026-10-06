// GETS-style Terminal: FYERS account/quote decoding, the book, every tab's
// rows, and the typed snapshot the Terminal's position table accepts.
//
// Fixtures are shaped like FYERS API v3 responses (funds fund_limit,
// positions netPositions, orders orderBook, tradebook tradeBook, quotes d[])
// as written by altair_fyers_account / altair_fyers_quotes.

#include "../gets_workspace.hpp"
#include "../funds_summary.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

namespace {

using namespace altair;
using namespace altair::ui;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

constexpr qint64 kFetched = 1'790'000'000;   // 2026-09-21 ~19:03 IST
const Timestamp kNow{(kFetched + 5) * 1'000'000'000LL};

QByteArray account_json() {
    return QByteArrayLiteral(R"({
  "schema_version": 1, "broker": "FYERS", "fetched_at_unix": 1790000000,
  "service_epoch": 1, "auth_verified_at_unix": 1790000000,
  "auth_expires_at_unix": 1790000030, "account_expires_at_unix": 1790000030,
  "feed_status": "disabled", "account_id": "XA00001", "snapshot_complete": true,
  "profile_status": 200, "profile_state": "present", "profile": {"s":"ok","data":{"fy_id":"XA00001"}},
  "funds_status": 200, "funds_state": "present",
  "funds": {"s":"ok","code":200,"fund_limit":[
    {"id":1,"title":"Total Balance","equityAmount":500000.0,"commodityAmount":0},
    {"id":2,"title":"Utilized Amount","equityAmount":420000.5,"commodityAmount":0},
    {"id":3,"title":"Clear Balance","equityAmount":480000.0,"commodityAmount":0},
    {"id":5,"title":"Collaterals","equityAmount":20000.0,"commodityAmount":0},
    {"id":10,"title":"Available Balance","equityAmount":79999.5,"commodityAmount":0}]},
  "positions_status": 200, "positions_state": "present",
  "positions": {"s":"ok","code":200,"netPositions":[
    {"symbol":"NSE:NIFTY2692925000CE","productType":"MARGIN","netQty":75,"side":1,"netAvg":150.0,"ltp":175.0,
     "buyQty":75,"buyAvg":150.0,"sellQty":0,"sellAvg":0,"realized_profit":0,"unrealized_profit":1875.0},
    {"symbol":"NSE:NIFTY26OCT25500PE","productType":"MARGIN","netQty":-75,"side":-1,"netAvg":600.0,"ltp":620.0,
     "buyQty":0,"buyAvg":0,"sellQty":75,"sellAvg":600.0,"realized_profit":0,"unrealized_profit":-1500.0},
    {"symbol":"NSE:NIFTY26OCTFUT","productType":"MARGIN","netQty":-75,"side":-1,"netAvg":25100.0,"ltp":25150.0,
     "buyQty":0,"buyAvg":0,"sellQty":75,"sellAvg":25100.0,"realized_profit":0,"unrealized_profit":-3750.0},
    {"symbol":"NSE:SBIN-EQ","productType":"CNC","netQty":10,"side":1,"netAvg":800.0,"ltp":812.35,
     "buyQty":10,"buyAvg":800.0,"sellQty":0,"sellAvg":0,"realized_profit":0,"unrealized_profit":123.5},
    {"symbol":"NSE:INFY-EQ","productType":"INTRADAY","netQty":0,"side":0,"netAvg":0,"ltp":1500.0,
     "buyQty":10,"buyAvg":1490.0,"sellQty":10,"sellAvg":1500.0,"realized_profit":100.0,"unrealized_profit":0}]},
  "holdings_status": 200, "holdings_state": "present", "holdings": {"s":"ok","holdings":[]},
  "orders_status": 200, "orders_state": "present",
  "orders": {"s":"ok","code":200,"orderBook":[
    {"id":"26092100001","symbol":"NSE:NIFTY2692925000CE","status":2,"side":1,"type":2,"qty":75,"filledQty":75,
     "limitPrice":0,"tradedPrice":150.0,"productType":"MARGIN","orderDateTime":"21-Sep-2026 09:20:01","message":""},
    {"id":"26092100002","symbol":"NSE:RELIANCE-EQ","status":5,"side":-1,"type":1,"qty":5,"filledQty":0,
     "limitPrice":2950.5,"tradedPrice":0,"productType":"INTRADAY","orderDateTime":"21-Sep-2026 10:05:00",
     "message":"RMS:Margin Exceeds"},
    {"id":"26092100003","symbol":"NSE:SBIN-EQ","status":6,"side":1,"type":1,"qty":1,"filledQty":0,
     "limitPrice":790.0,"tradedPrice":0,"productType":"CNC","orderDateTime":"21-Sep-2026 11:00:00","message":""}]},
  "tradebook_status": 200,
  "tradebook": {"s":"ok","code":200,"tradeBook":[
    {"symbol":"NSE:NIFTY2692925000CE","side":1,"tradedQty":50,"tradePrice":150.0,"tradeValue":7500.0,
     "productType":"MARGIN","orderNumber":"26092100001","tradeNumber":"T1","orderDateTime":"21-Sep-2026 09:20:01"},
    {"symbol":"NSE:NIFTY2692925000CE","side":1,"tradedQty":25,"tradePrice":150.0,"tradeValue":3750.0,
     "productType":"MARGIN","orderNumber":"26092100001","tradeNumber":"T2","orderDateTime":"21-Sep-2026 09:20:01"},
    {"symbol":"NSE:INFY-EQ","side":1,"tradedQty":10,"tradePrice":1490.0,"tradeValue":14900.0,
     "productType":"INTRADAY","orderNumber":"26092100004","tradeNumber":"T3","orderDateTime":"21-Sep-2026 09:30:00"},
    {"symbol":"NSE:INFY-EQ","side":-1,"tradedQty":10,"tradePrice":1500.0,"tradeValue":15000.0,
     "productType":"INTRADAY","orderNumber":"26092100005","tradeNumber":"T4","orderDateTime":"21-Sep-2026 14:30:00"}]}
})");
}

QByteArray quotes_json() {
    return QByteArrayLiteral(R"({
  "schema_version": 1, "broker": "FYERS", "fetched_at_unix": 1790000002, "requested": 9,
  "responses": [
    {"status": 200, "body": {"s":"ok","code":200,"d":[
      {"n":"NSE:NIFTY50-INDEX","s":"ok","v":{"lp":25000.0,"ch":120.5,"chp":0.48,"open_price":24900.0,
        "high_price":25050.0,"low_price":24880.0,"prev_close_price":24879.5,"tt":1789990000}},
      {"n":"NSE:NIFTYBANK-INDEX","s":"ok","v":{"lp":55000.0,"ch":-165.0,"chp":-0.3,"open_price":55200.0,
        "high_price":55300.0,"low_price":54900.0,"prev_close_price":55165.0,"tt":1789990000}},
      {"n":"NSE:NIFTY2692925000CE","s":"ok","v":{"lp":180.0,"ch":30.0,"chp":20.0}},
      {"n":"NSE:NIFTY26OCTFUT","s":"ok","v":{"lp":25150.0,"ch":100.0,"chp":0.4}},
      {"n":"NSE:SBIN-EQ","s":"ok","v":{"lp":812.35,"ch":9.6,"chp":1.2,"open_price":805.0,"high_price":815.0,
        "low_price":801.0,"prev_close_price":802.75,"volume":1234567}},
      {"n":"NSE:RELIANCE-EQ","s":"ok","v":{"lp":2900.0,"ch":-62.2,"chp":-2.1,"volume":999}},
      {"n":"NSE:TCS-EQ","s":"ok","v":{"lp":4000.0,"ch":20.0,"chp":0.5,"volume":10}},
      {"n":"NSE:BADSYM-EQ","s":"error","v":{"code":-300,"errmsg":"Invalid symbol"}}]}},
    {"status": 429, "body": null}]
})");
}

QStringList universe() {
    return {QStringLiteral("NSE:NIFTY50-INDEX"), QStringLiteral("NSE:NIFTYBANK-INDEX"),
            QStringLiteral("NSE:SBIN-EQ"), QStringLiteral("NSE:RELIANCE-EQ"),
            QStringLiteral("NSE:TCS-EQ"), QStringLiteral("NSE:BADSYM-EQ")};
}

void test_numbers() {
    check(gets_price_paise(QJsonValue{812.35}) == std::optional<std::int64_t>{81'235},
          "812.35 is exactly 81235 paise");
    check(!gets_price_paise(QJsonValue{100.005}).has_value(), "a sub-paisa price is refused, not rounded");
    check(gets_amount_paise(QJsonValue{100.005}) == std::optional<std::int64_t>{10'001}
              || gets_amount_paise(QJsonValue{100.005}) == std::optional<std::int64_t>{10'000},
          "a broker amount is rounded to the paisa");
    check(!gets_price_paise(QJsonValue{QStringLiteral("abc")}).has_value(), "non-numeric text is refused");
}

void test_account() {
    const GetsAccount a = parse_gets_account(account_json());
    check(a.loaded && a.account_id == QStringLiteral("XA00001"), "account snapshot parses");
    check(a.funds_ok && a.funds.size() == 5 && gets_fund(a, 10) == std::optional<std::int64_t>{7'999'950},
          "fund_limit rows by id, exact paise");
    check(a.positions_ok && a.positions.size() == 5, "five position rows");
    check(a.positions[1].net_qty == -75 && a.positions[2].net_qty == -75, "shorts are negative");
    check(a.orders_ok && a.orders.size() == 3 && a.orders[1].status == 5
              && a.orders[1].message == QStringLiteral("RMS:Margin Exceeds"),
          "order book with a rejection reason");
    check(a.trades_ok && a.trades.size() == 4 && a.trades[0].at.isValid(), "trade book with IST times");
    check(parse_gets_account(QByteArrayLiteral("{\"schema_version\":2}")).error.contains(QStringLiteral("schema")),
          "a wrong schema is refused");
}

void test_quotes() {
    const GetsQuotes q = parse_gets_quotes(quotes_json());
    check(q.loaded && q.by_symbol.size() == 8, "eight quote rows");
    check(q.failed_responses == 1, "a failed HTTP response is counted");
    check(!q.by_symbol.value(QStringLiteral("NSE:BADSYM-EQ")).ok
              && q.by_symbol.value(QStringLiteral("NSE:BADSYM-EQ")).error == QStringLiteral("Invalid symbol"),
          "a per-symbol error keeps FYERS' message");
    check(gets_quote_ltp(q, QStringLiteral("NSE:NIFTY50-INDEX")) == std::optional<Price>{Price{2'500'000}},
          "index LTP in paise");
}

MasterIndex master_fixture(QTemporaryDir& dir) {
    const QString path = dir.filePath(QStringLiteral("instruments.csv"));
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write("instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,strike,tick_size,lot_size,"
            "instrument_type,segment,exchange\n"
            "12345,678,NIFTY26OCT25500PE,NIFTY,0,2026-10-27,25500,0.05,75,PE,NFO-OPT,NFO\n"
            "12346,679,NIFTY26OCTFUT,NIFTY,0,2026-10-27,0,0.05,75,FUT,NFO-FUT,NFO\n"
            "12347,680,NIFTY2692925000CE,NIFTY,0,2026-09-29,25000,0.05,75,CE,NFO-OPT,NFO\n"
            "779521,3045,SBIN,STATE BANK OF INDIA,0,,0,0.05,1,EQ,NSE,NSE\n");
    f.close();
    MasterIndex m;
    m.load(path);
    return m;
}

void test_instruments(const MasterIndex& master) {
    const auto weekly = resolve_gets_instrument(QStringLiteral("NSE:NIFTY2692925000CE"), nullptr, 2026);
    check(weekly.expiry && weekly.expiry_source == QStringLiteral("ticker")
              && weekly.spot_symbol == QStringLiteral("NSE:NIFTY50-INDEX"),
          "weekly: expiry from the ticker, priced off NIFTY 50");
    const auto monthly = resolve_gets_instrument(QStringLiteral("NSE:NIFTY26OCT25500PE"), nullptr, 2026);
    check(!monthly.expiry && !monthly.lot, "monthly without a master: expiry and lot unknown");
    check(master.loaded(), "Kite master fixture loads");
    const auto resolved = resolve_gets_instrument(QStringLiteral("NSE:NIFTY26OCT25500PE"), &master, 2026);
    const QDateTime at = resolved.expiry
        ? QDateTime::fromMSecsSinceEpoch(resolved.expiry->ns_since_epoch() / 1'000'000, QTimeZone(19800))
        : QDateTime{};
    check(resolved.expiry_source == QStringLiteral("master") && resolved.lot == std::optional<std::int64_t>{75}
              && at.date() == QDate(2026, 10, 27) && at.time() == QTime(15, 30),
          "monthly with the master: 27-Oct 15:30 IST, lot 75");
    const auto sbin = resolve_gets_instrument(QStringLiteral("NSE:SBIN-EQ"), &master, 2026);
    check(sbin.lot == std::optional<std::int64_t>{1} && sbin.segment == Segment::Cash,
          "equity resolves through the NSE cash line");
}

void test_book(const MasterIndex& master) {
    const GetsAccount a = parse_gets_account(account_json());
    const GetsQuotes q = parse_gets_quotes(quotes_json());
    BookParams p{};
    p.now = kNow;
    const QStringList watch{QStringLiteral("NSE:NIFTY2692925000CE"), QStringLiteral("NSE:NIFTY2692925500CE")};
    const GetsBook b = build_gets_book(a, q, watch, &master, {}, p, 2026);
    check(b.legs.size() == 5, "four open positions plus one new watch row (held rows are not repeated)");
    check(b.watch_only[4] && b.legs[4].units.raw() == 0, "the watch row carries no units");
    const BookLegView& call = b.views[0];
    check(call.greeks_ok && call.implied && call.implied->raw() > 0.05 && call.implied->raw() < 0.5,
          "the weekly call gets an IV from its quote LTP (180) and NIFTY spot");
    check(b.legs[0].mark == std::optional<Price>{Price{18'000}}, "quote LTP wins over the position's LTP");
    check(b.legs[1].expiry_known && b.views[1].greeks_ok && b.views[1].delta > 0.0,
          "the monthly put resolves its expiry through the master; short put is +delta");
    check(b.views[1].mtm && b.views[1].mtm->raw() == -75 * 2'000, "short put MTM: (620-600) x -75");
    check(b.views[2].greeks_ok && b.views[2].delta == -75.0, "the short future is delta -75");
    check(b.views[3].mtm && b.views[3].mtm->raw() == 12'350, "SBIN MTM exact: (812.35-800) x 10");

    const QStringList need = gets_quote_symbols(a, watch, 2026);
    check(need.contains(QStringLiteral("NSE:NIFTY50-INDEX")) && need.contains(QStringLiteral("NSE:NIFTY26OCTFUT"))
              && !need.contains(QStringLiteral("NSE:INFY-EQ")),
          "the quote request covers open legs and their underlyings, not closed ones");
}

void test_typed_snapshot(const MasterIndex& master) {
    const GetsAccount a = parse_gets_account(account_json());
    const GetsTypedAccount t = gets_typed_account(a, &master, 2026);
    check(t.snapshot.has_value(), "a typed snapshot is built");
    const auto& s = *t.snapshot;
    check(s.typed_positions_present && s.typed_positions.count == 4, "four open positions typed (closed one left out)");
    check(s.typed_funds.available_trading_balance == std::optional<Notional>{Notional{7'999'950}}
              && s.typed_funds.utilised_margin == std::optional<Notional>{Notional{42'000'050}},
          "FYERS fund_limit ids map to available and utilised");
    check(broker_view::usable(s, kNow), "the snapshot is usable inside its 30 s window");
    check(!broker_view::usable(s, Timestamp{(kFetched + 31) * 1'000'000'000LL}),
          "and refused after it");

    PositionTableModel positions([&t](broker_view::InstrumentKey k) -> std::optional<InstrumentDisplay> {
        const auto it = t.instruments.constFind(static_cast<std::uint32_t>(k));
        if (it == t.instruments.constEnd()) return std::nullopt;
        return *it;
    });
    const auto applied = positions.apply({s.account_session, PositionSource::Broker, QStringLiteral("FYERS")},
                                         s.typed_positions, kNow);
    check(applied.has_value() && positions.rowCount() == 4, "the Terminal's position table accepts it");
    bool named = false;
    for (int r = 0; r < positions.rowCount(); ++r)
        named = named || positions.index(r, PositionTableModel::Instrument).data().toString()
                             == QStringLiteral("NIFTY26OCT25500PE");
    check(named, "rows show FYERS instrument names, not unresolved keys");

    FundsSummaryModel funds;
    FundsAccountView view;
    view.session = s.account_session;
    view.label = QStringLiteral("FYERS");
    view.connected = true;
    view.funds = s.typed_funds;
    check(funds.apply(view) && funds.account_count() == 1, "the funds table accepts the typed funds");
}

void test_workspace(const MasterIndex&) {
    GetsWorkspace w(new QWidget);
    w.set_paths({}, {}, {}, {});
    w.set_clock(kNow, 2026);
    int typed_calls = 0;
    w.on_account = [&typed_calls](const GetsTypedAccount& t) { if (t.snapshot) ++typed_calls; };
    w.set_watch({QStringLiteral("NSE:NIFTY2692925500CE")});
    w.load(parse_gets_account(account_json()), parse_gets_quotes(quotes_json()), universe());

    const auto visible_tabs = [&w] {
        QStringList out;
        for (int i = 0; i < w.tabs()->count(); ++i) if (w.tabs()->isTabVisible(i)) out << w.tabs()->tabText(i);
        return out;
    };
    check(visible_tabs() == QStringList({QStringLiteral("Positions && Funds"), QStringLiteral("Simulation"),
                                         QStringLiteral("Expense && Margin"), QStringLiteral("Trade History"),
                                         QStringLiteral("RMS"), QStringLiteral("Index Info")}),
          "Alt+F6: Greek Watch, Greek Summary and Top Movers have moved to the Terminal");
    w.set_demo(true);
    check(visible_tabs().size() == 6 && w.tabs()->isTabVisible(w.tabs()->indexOf(w.demo_trades()))
              && w.tabs()->isTabVisible(w.tabs()->indexOf(w.demo_rms()))
              && w.tabs()->isTabVisible(w.tabs()->indexOf(w.demo_expense())),
          "Demo swaps Trade History, RMS and Expense & Margin to the demo books");
    {
        QTemporaryDir paper;
        const auto write = [&paper](const char* name, const char* text) {
            QFile f(paper.filePath(QString::fromLatin1(name)));
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) f.write(text);
        };
        write("fills.csv", "time,model,symbol,token,side,qty,price,expenses,at_quote,reason,source,costs\n"
                           "2026-10-01 09:20:00,\"Strangle 80% NIFTY\",NIFTY26O0623200CE,10430722,sell,65,81.75,33.81,1,x,SIM,OK\n"
                           "2026-10-01 15:20:00,\"Strangle 80% NIFTY\",NIFTY26O0623200CE,10430722,buy,65,61.75,30.00,1,y,SIM,OK\n");
        write("trades.csv", "date,model,symbol,token,side,qty,entry_time,entry,exit_time,exit,gross,expenses,net,why_in,why_out,source,costs\n"
                            "2026-10-01,Strangle 80% NIFTY,NIFTY26O0623200CE,10430722,sell,65,09:20,81.75,15:20,61.75,1300.00,63.81,1236.19,a,b,SIM,OK\n");
        write("manual_trades.csv", "id,order_id,ns,token,symbol,exchange,lot,tick,product,side,qty,price,basis,expenses,sim\n"
                                   "1,1,1790739600000000000,779521,SBIN,NSE,1,5,MIS,B,10,80000,ask,2.10,1\n");
        write("open_positions.csv", "model,token,symbol,side,qty,entry,entry_ns,entry_expenses,carry,why_in\n"
                                    "OHL NIFTY,12468226,NIFTY26OCTFUT,1,65,24000.00,0,10,0,open = low\n");
        auto* trades = w.demo_trades();
        trades->set_today(QStringLiteral("2026-10-01"));
        trades->set_dir(paper.path());
        check(trades->table()->rowCount() == 2 && trades->table()->item(0, 1)->text().contains(QStringLiteral("Strangle")),
              "demo Trade History lists the models' fills (the manual one is another day)");
        auto* expense = w.demo_expense();
        expense->set_today(QStringLiteral("2026-10-01"));
        expense->set_dir(paper.path());
        check(expense->table()->rowCount() == 2 && expense->table()->item(0, 3)->text() == QStringLiteral("63.81")
                  && expense->table()->item(0, 5)->text() == QStringLiteral("1300.00")
                  && expense->table()->item(0, 6)->text() == QStringLiteral("1236.19"),
              "demo Expense & Margin: expenses, and the P&L without and with them");
        w.set_demo_ltp([](quint32 tok) { return tok == 12468226u ? 24100.0 : tok == 779521u ? 810.0 : 0.0; });
        auto* rms = w.demo_rms();
        rms->set_today(QStringLiteral("2026-10-01"));
        rms->set_dir(paper.path());
        check(rms->table()->rowCount() == 2 && rms->state_text().contains(QStringLiteral("OK"))
                  && rms->table()->item(0, 6)->text() == QStringLiteral("6500.00"),
              "demo RMS marks the models' and the paper book's positions to the stream");
    }
    w.set_demo(false);
    check(typed_calls >= 1, "the Terminal is handed a typed snapshot on load");
    check(w.watch_model()->rowCount() == 5, "Greek watch: four positions and one watch row");
    check(w.watch_model()->text(0, GwStatus) == QStringLiteral("OK"), "the weekly call is fully valued");
    check(w.watch_model()->text(1, GwStatus) == QStringLiteral("expiry unknown"),
          "without a master the monthly put says why it has no Greeks");
    check(w.watch_model()->text(1, GwIv) == QStringLiteral("—"), "and shows — rather than a number");
    check(w.summary_model()->rowCount() >= 3, "portfolio summary has groups and totals");
    check(w.simulation_model()->rowCount() == 11, "simulation: ±5% in 1% steps");

    // User IV edit through the model.
    const QModelIndex iv = w.watch_model()->index(0, GwUserIv);
    check(w.watch_model()->flags(iv).testFlag(Qt::ItemIsEditable), "User IV is editable");
    check(w.watch_model()->setData(iv, QStringLiteral("20"), Qt::EditRole), "User IV accepts 20%");
    check(w.watch_model()->text(0, GwUserIv) == QStringLiteral("20.00"), "and the row shows it");
    check(w.book().views[0].used && std::fabs(w.book().views[0].used->raw() - 0.20) < 1e-12,
          "and the Greeks now use it");
    check(!w.watch_model()->setData(iv, QStringLiteral("-5"), Qt::EditRole), "a negative IV is refused");

    check(w.expense_model()->rowCount() == 2, "expense: one row per traded instrument");
    check(w.expense_model()->text(0, 5) == QStringLiteral("75"), "fills of one order are summed (50 + 25)");
    check(w.funds_model()->rowCount() == 5, "fund limits listed");
    check(w.trades_model()->rowCount() == 4, "trade history rows");
    check(w.rejections_model()->rowCount() == 1
              && w.rejections_model()->text(0, 7) == QStringLiteral("RMS:Margin Exceeds"),
          "RMS rejections show the broker's reason");
    bool util_warn = false;
    for (int r = 0; r < w.rms_model()->rowCount(); ++r)
        if (w.rms_model()->text(r, 0) == QStringLiteral("Margin utilisation"))
            util_warn = w.rms_model()->text(r, 3) == QStringLiteral("WARN");
    check(util_warn, "84% utilisation is WARN between 80% and 95%");

    check(w.movers_model()->rowCount() == 2 && w.movers_model()->text(0, 1) == QStringLiteral("NSE:SBIN-EQ"),
          "top scrip gainers: SBIN (+1.2%) then TCS (+0.5%)");
    w.movers_side()->setCurrentIndex(1);
    check(w.movers_model()->rowCount() == 1 && w.movers_model()->text(0, 1) == QStringLiteral("NSE:RELIANCE-EQ"),
          "top losers: RELIANCE");
    w.movers_kind()->setCurrentIndex(1);
    check(w.movers_model()->rowCount() == 1 && w.movers_model()->text(0, 1) == QStringLiteral("NSE:NIFTYBANK-INDEX"),
          "index losers: NIFTY BANK");
    check(w.index_model()->rowCount() == 2 && w.index_model()->text(0, 10) == QStringLiteral("OK"),
          "index information rows");
    check(w.status_text().contains(QStringLiteral("XA00001")), "the status line names the account and its age");
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    std::printf("GETS-style Terminal\n");
    QTemporaryDir dir;
    const MasterIndex master = master_fixture(dir);
    test_numbers();
    test_account();
    test_quotes();
    test_instruments(master);
    test_book(master);
    test_typed_snapshot(master);
    test_workspace(master);
    std::printf("GETS: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
