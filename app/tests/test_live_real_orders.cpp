// The real-order bridge (app/live_real_orders.hpp) on files whose answers are
// known: nothing is asked of the router unless LIVE is armed AND the
// strategy's own switch is on; the arbitrage's two legs become two IOC
// requests at the touch, by "strategy.arbitrage", sized together to the caps
// (the demo's 100 shares a leg, the router's ceiling of 50 lots an order: 50
// real shares a leg); once both fill at FYERS they are HELD -- FYERS keeps an
// intraday long on one exchange and a short on the other apart -- until the
// demo closes the pair, when both exits are sent and each leg is written as
// its own round trip; a pair whose second leg did not fill (or filled less)
// is flattened, once; with a halt requested nothing new is sent.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <app/live_real_orders.hpp>
#include <live/arbitrage.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "BAD ", what);
    if (!ok) ++failures;
}

using namespace altair;
using namespace altair::live;
namespace fs = std::filesystem;

constexpr std::int64_t kSec = 1'000'000'000LL;

LiveInstrument eq(std::uint32_t tok, const char* sym, const char* venue) {
    LiveInstrument i;
    i.token = tok; i.symbol = sym; i.underlying = sym; i.kind = LiveKind::Equity; i.lot = 1; i.tick = 0.05;
    i.fyers = std::string(venue) + ":" + sym + (std::string(venue) == "NSE" ? "-EQ" : "-A");
    return i;
}

std::vector<std::string> lines(const fs::path& p) {
    std::vector<std::string> out;
    std::ifstream f(p);
    std::string l;
    while (std::getline(f, l)) if (!l.empty()) out.push_back(l);
    return out;
}
std::string field(const std::string& line, const std::string& key) {
    const auto at = line.find("\"" + key + "\":");
    if (at == std::string::npos) return {};
    std::size_t i = at + key.size() + 3;
    if (line[i] == '"') { const auto e = line.find('"', i + 1); return line.substr(i + 1, e - i - 1); }
    const auto e = line.find_first_of(",}", i);
    return line.substr(i, e - i);
}

void write_arm(const fs::path& root, std::int64_t now_unix, bool arb_on) {
    std::ofstream f(root / "data/live_trading.json", std::ios::trunc);
    f << "{\"armed\":true,\"by\":\"smit\",\"armed_unix\":" << now_unix - 60 << ",\"expires_unix\":" << now_unix + 3 * 3600
      << ",\"max_lots\":50,\"max_order_value\":2500000,\"max_orders_per_day\":20,\"max_open_orders\":5,"
         "\"max_daily_loss\":5000,\"price_band_pct\":3,\"strategies\":{\"arbitrage\":{\"on\":"
      << (arb_on ? "true" : "false")
      << ",\"max_lots\":50,\"max_order_value\":250000,\"max_orders_per_day\":10,\"max_daily_loss\":2000}}}";
}

/// The router's orders.json for these requests, each with its status and fill.
void write_orders(const fs::path& root, const std::vector<std::tuple<std::string, std::string, std::int64_t, std::int64_t>>& rows) {
    fs::create_directories(root / "data/live_orders");
    std::ofstream f(root / "data/live_orders/orders.json", std::ios::trunc);
    f << "{\"router\":{},\"orders\":[";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& [id, status, filled, avg] = rows[i];
        f << (i ? "," : "") << "{\"intent\":\"" << id << "\",\"status\":\"" << status << "\",\"filled\":" << filled
          << ",\"avg_paise\":" << avg << "}";
    }
    f << "]}";
}

} // namespace

int main() {
    std::printf("real orders for switched-on strategies\n");
    const fs::path root = fs::temp_directory_path() / ("altair_real_orders_" + std::to_string(std::rand()));
    fs::remove_all(root);
    fs::create_directories(root / "data");
    const std::int64_t day = parse_day("2026-10-05");
    const std::int64_t t0 = (day * 86400 + 10 * 3600 - 19800) * kSec;   // 10:00 IST
    const std::int64_t now_unix = t0 / kSec;

    const std::vector<LiveInstrument> u{eq(1, "RELIANCE", "NSE"), eq(2, "RELIANCE", "BSE")};
    LiveExecPolicy instant;
    instant.latency_ns = 0;
    const LiveCostFn cost = [](const LiveInstrument&, bool, double qty, double px, std::int64_t) { return 0.00015 * qty * px; };
    LiveEngine e(u, cost, instant);
    LiveCrossArbRule visible;
    visible.fixed_qty = 0;   // the demo trades the lower visible size (100), so the caps are what size the real legs
    e.add_model(std::make_unique<LiveCrossArbModel>(visible));
    live_real::RealOrderBridge bridge(root, cost);
    bridge.attach(e);
    bridge.set_wall_clock([t0] { return t0; });
    const auto quote = [&](std::uint32_t tok, std::int64_t bid, std::int64_t ask, std::int64_t at) {
        QuotePayload q;
        q.token = tok; q.flags = kQuoteHasTop; q.bid = bid; q.ask = ask; q.bid_qty = 100; q.ask_qty = 100;
        e.on_quote(q, at);
    };
    PricePayload p;
    p.token = 1; p.last_paise = 100000; p.exchange_ts_ns = t0;
    e.on_trade(p, t0);
    p.token = 2;
    e.on_trade(p, t0);
    const fs::path intents = root / "data/order_intents.jsonl";

    // Switch off: the demo trades, nothing real is asked for.
    write_arm(root, now_unix, false);
    bridge.poll(t0);
    quote(1, 100000, 100010, t0 + kSec);
    quote(2, 99850, 99860, t0 + kSec);   // 14 bp across: the arbitrage enters in paper, two legs, held
    check(e.book().trades().empty() && !e.book().flat("Cross-exchange arbitrage") && !fs::exists(intents),
          "with its switch off the arbitrage demo-trades (a pair held) and asks for nothing real");
    quote(2, 99995, 100005, t0 + 2 * kSec);   // the prices meet: the demo closes both legs
    check(e.book().trades().size() == 2 && e.book().flat("Cross-exchange arbitrage") && !fs::exists(intents),
          "the demo closes both legs when the prices meet; still nothing real");

    // Switch on: the next pair becomes two IOC requests at the touch -- and nothing more.
    write_arm(root, now_unix, true);
    bridge.poll(t0);
    quote(1, 100000, 100010, t0 + 10 * kSec);
    quote(2, 99850, 99860, t0 + 10 * kSec);
    auto ls = lines(intents);
    const LivePosition* demo_leg = e.book().position("Cross-exchange arbitrage", 2);
    check(demo_leg != nullptr && demo_leg->qty == 100, "the demo trades its own 100 shares a leg (the lower visible size)");
    check(ls.size() == 2, "LIVE armed and Auto: Arbitrage on: two requests, one per leg");
    {
        const auto why = lines(root / "data" / "live_orders" / "strategy_orders.jsonl");
        check(why.size() == 2 && why[0].find("\"key\":\"arbitrage\"") != std::string::npos
                  && why[0].find("\"kind\":\"entry\"") != std::string::npos && why[0].find("\"why\":\"") != std::string::npos
                  && why[0].find(field(ls[0], "id")) != std::string::npos,
              "every real request is logged with its strategy, its intent id and the strategy's reasoning");
        check(altair::live_real::strategy_key("OBI A futures BANKNIFTY") == "obi-a-futures-banknifty"
                  && altair::live_real::strategy_key("Option arbitrage").empty(),
              "every model has a key to switch by; option arbitrage (it buys options) has none");
    }
    std::vector<std::tuple<std::string, std::string, std::int64_t, std::int64_t>> all;
    const auto csv = [](const std::string& line) {
        std::vector<std::string> c;
        std::string cell;
        bool q = false;
        for (const char ch : line) {
            if (ch == '"') { q = !q; continue; }
            if (ch == ',' && !q) { c.push_back(cell); cell.clear(); continue; }
            cell += ch;
        }
        c.push_back(cell);
        return c;
    };
    if (ls.size() == 2) {
        const std::string a = field(ls[0], "token") == "2" ? ls[0] : ls[1];   // the BSE buy
        const std::string b = field(ls[0], "token") == "1" ? ls[0] : ls[1];   // the NSE sell
        check(field(a, "by") == "strategy.arbitrage" && field(a, "side") == "BUY" && field(a, "exchange") == "BSE"
                  && field(a, "limit_paise") == "99860" && field(a, "validity") == "IOC" && field(a, "product") == "MIS"
                  && field(a, "lots") == "50",
              "BSE: buy 50 (the cap) at the ask 998.60, IOC, intraday, by the strategy");
        check(field(b, "side") == "SELL" && field(b, "exchange") == "NSE" && field(b, "limit_paise") == "100000"
                  && field(b, "lots") == "50",
              "NSE: sell the same 50 at the bid 1000.00");
        // Both fill at FYERS: held, nothing sent until the demo closes the pair.
        all.emplace_back(field(a, "id"), "FILLED", 50, 99860);
        all.emplace_back(field(b, "id"), "FILLED", 50, 100000);
        write_orders(root, all);
        bridge.poll(t0);
        bridge.poll(t0);
        check(lines(intents).size() == 2 && bridge.holdings().at({"Cross-exchange arbitrage", 2}).qty == 50
                  && bridge.holdings().at({"Cross-exchange arbitrage", 1}).qty == -50,
              "both legs filled: held (long 50 on BSE, short 50 on NSE), no exit yet");
        // The prices meet: the demo closes both legs, and both real exits go.
        quote(2, 99995, 100005, t0 + 11 * kSec);
        ls = lines(intents);
        check(ls.size() == 4, "the prices met: two exit requests");
        if (ls.size() == 4) {
            const std::string xa = field(ls[2], "token") == "2" ? ls[2] : ls[3];
            const std::string xb = field(ls[2], "token") == "1" ? ls[2] : ls[3];
            check(field(xa, "side") == "SELL" && field(xa, "lots") == "50" && field(xa, "by") == "strategy.arbitrage.exit"
                      && field(xa, "validity") == "DAY" && field(xb, "side") == "BUY" && field(xb, "lots") == "50",
                  "sell the 50 held on BSE, buy back the 50 short on NSE, DAY limits, by the strategy's exit");
            all.emplace_back(field(xa, "id"), "FILLED", 50, 99995);
            all.emplace_back(field(xb, "id"), "FILLED", 50, 100000);
            write_orders(root, all);
            bridge.poll(t0);
            const auto rec = lines(root / "data/live_orders/strategy_trades.csv");
            check(rec.size() == 3 && rec[0].rfind("date,model,symbol", 0) == 0, "one row a leg");
            double gross = 0.0;
            for (std::size_t i = 1; i < rec.size(); ++i) {
                const auto c = csv(rec[i]);
                gross += std::stod(c[10]);
                check(c[15] == "REAL", "each leg is written as REAL");
            }
            check(std::fabs(gross - (999.95 - 998.60) * 50) < 1e-6, "gross over both legs: 1.35 x 50 = 67.50");
            check(bridge.holdings().at({"Cross-exchange arbitrage", 2}).qty == 0
                      && bridge.holdings().at({"Cross-exchange arbitrage", 1}).qty == 0,
                  "nothing is left held");
        }
    }

    // Legging: the BSE leg fills, the NSE leg does not: the BSE leg is sent back out.
    quote(1, 100000, 100010, t0 + 20 * kSec);
    quote(2, 99850, 99860, t0 + 20 * kSec);
    ls = lines(intents);
    check(ls.size() == 6, "a new pair: two more requests");
    if (ls.size() == 6) {
        const std::string a = field(ls[4], "token") == "2" ? ls[4] : ls[5];
        const std::string b = field(ls[4], "token") == "1" ? ls[4] : ls[5];
        all.emplace_back(field(a, "id"), "FILLED", 50, 99860);
        all.emplace_back(field(b, "id"), "CANCELLED", 0, 0);   // IOC: nothing at the NSE bid any more
        write_orders(root, all);
        bridge.poll(t0);
        ls = lines(intents);
        check(ls.size() == 7 && field(ls[6], "token") == "2" && field(ls[6], "side") == "SELL" && field(ls[6], "lots") == "50"
                  && field(ls[6], "by") == "strategy.arbitrage.exit",
              "the leg that filled is flattened at once: sell the 50 bought on BSE");
        bridge.poll(t0);
        check(lines(intents).size() == 7, "and it is not sent out a second time");
        all.emplace_back(field(ls[6], "id"), "FILLED", 50, 99840);
        write_orders(root, all);
        bridge.poll(t0);
        const auto rec = lines(root / "data/live_orders/strategy_trades.csv");
        check(bridge.holdings().at({"Cross-exchange arbitrage", 2}).qty == 0 && rec.size() == 4
                  && rec.back().find("legging") != std::string::npos,
              "the flattened leg is recorded as a legging loss and nothing is left held");
        // The demo still holds its pair; when it closes, nothing real is left to send.
        quote(2, 99995, 100005, t0 + 21 * kSec);
        check(e.book().flat("Cross-exchange arbitrage") && lines(intents).size() == 7,
              "the demo's own exit sends nothing for legs no longer held");
    }

    // A halt: nothing new is asked for.
    { std::ofstream k(root / "data/kill_request.json"); k << "{}"; }
    bridge.poll(t0);
    const std::size_t before = lines(intents).size();
    const std::size_t demo_before = e.book().trades().size();
    quote(1, 100000, 100010, t0 + 50 * kSec);
    quote(2, 99850, 99860, t0 + 50 * kSec);
    check(e.book().trades().size() == demo_before && !e.book().flat("Cross-exchange arbitrage") && lines(intents).size() == before,
          "with a halt requested the demo still trades, but no real entry is asked for");

    // The SIM: the same switch on, the same gap, but simulated prices: nothing real.
    {
        const fs::path sroot = root / "sim";
        fs::create_directories(sroot / "data");
        write_arm(sroot, now_unix, true);
        LiveEngine es(u, cost, instant);
        es.add_model(std::make_unique<LiveCrossArbModel>());
        live_real::RealOrderBridge sb(sroot, cost);
        sb.attach(es);
        sb.set_wall_clock([t0] { return t0; });
        sb.poll(t0);
        PricePayload sp;
        sp.flags = kPriceSimulated; sp.last_paise = 100000; sp.exchange_ts_ns = t0;
        sp.token = 1; es.on_trade(sp, t0);
        sp.token = 2; es.on_trade(sp, t0);
        for (const auto& [tok, bid, ask] : {std::tuple{1u, 100000LL, 100010LL}, std::tuple{2u, 99850LL, 99860LL}}) {
            QuotePayload q;
            q.token = tok; q.flags = kQuoteHasTop | kQuoteSimulated; q.bid = bid; q.ask = ask; q.bid_qty = 100; q.ask_qty = 100;
            es.on_quote(q, t0 + kSec);
        }
        check(!es.book().flat("Cross-exchange arbitrage") && !fs::exists(sroot / "data/order_intents.jsonl"),
              "on the SIM the arbitrage demo-trades, and no real order is ever asked for");
    }

    fs::remove_all(root);
    std::printf("%s\n", failures == 0 ? "all real order checks passed" : "real order checks did not pass");
    return failures == 0 ? 0 : 1;
}
