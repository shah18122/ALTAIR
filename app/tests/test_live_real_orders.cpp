// The real-order bridge (app/live_real_orders.hpp) on files whose answers are
// known: nothing is asked of the router unless LIVE is armed AND the
// strategy's own switch is on; the arbitrage's two legs become two IOC
// requests at the touch, by "strategy.arbitrage", sized together to the caps
// (the demo's 100 shares a leg, the router's ceiling of 50 lots an order: 50
// real shares a leg); fills read from the router's orders.json make a real
// holding; the strategy's exit becomes a DAY request through the touch; the
// closed round trip is written with gross, expenses and net; a pair whose
// second leg did not fill is flattened, and its leg is not sent out twice when
// the strategy then leaves too; with a halt requested nothing new is sent.
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
    e.add_model(std::make_unique<LiveCrossArbModel>());
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
    quote(2, 99850, 99860, t0 + kSec);   // 14 bp across: the arbitrage enters in paper
    check(!e.book().flat("Cross-exchange arbitrage") && !fs::exists(intents),
          "with its switch off the arbitrage demo-trades and asks for nothing real");
    quote(2, 99995, 100005, t0 + 2 * kSec);
    quote(1, 99995, 100005, t0 + 2 * kSec);   // out
    check(e.book().flat("Cross-exchange arbitrage"), "the demo pair is out");

    // Switch on: the next pair becomes two IOC requests at the touch.
    write_arm(root, now_unix, true);
    bridge.poll(t0);
    quote(1, 100000, 100010, t0 + 3 * kSec);
    quote(2, 99850, 99860, t0 + 3 * kSec);
    auto ls = lines(intents);
    check(e.book().position("Cross-exchange arbitrage", 2) != nullptr && e.book().position("Cross-exchange arbitrage", 2)->qty == 100,
          "the demo still trades its own 100 shares a leg");
    check(ls.size() == 2, "LIVE armed and Auto: Arbitrage on: two requests, one per leg");
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
        // Both fill at FYERS.
        write_orders(root, {{field(a, "id"), "FILLED", 50, 99860}, {field(b, "id"), "FILLED", 50, 100000}});
        bridge.poll(t0);
        check(bridge.holdings().at({"Cross-exchange arbitrage", 2}).qty == 50
                  && bridge.holdings().at({"Cross-exchange arbitrage", 1}).qty == -50,
              "the fills read from orders.json are the real holding: long 50 BSE, short 50 NSE");
        // The prices meet: the strategy exits, and so do the real legs.
        quote(2, 99995, 100005, t0 + 30 * kSec);
        quote(1, 99995, 100005, t0 + 30 * kSec);
        ls = lines(intents);
        check(ls.size() == 4 && field(ls[2], "by") == "strategy.arbitrage.exit" && field(ls[3], "by") == "strategy.arbitrage.exit"
                  && field(ls[2], "validity") == "DAY" && field(ls[2], "lots") == "50" && field(ls[3], "lots") == "50",
              "the strategy's exit asks for both legs out, DAY, as exits");
        const std::string xa = field(ls[2], "token") == "2" ? ls[2] : ls[3];
        const std::string xb = field(ls[2], "token") == "1" ? ls[2] : ls[3];
        check(field(xa, "side") == "SELL" && std::stoll(field(xa, "limit_paise")) < 99995 && field(xb, "side") == "BUY"
                  && std::stoll(field(xb, "limit_paise")) > 100005,
              "each exit is priced through the touch, so it fills now");
        write_orders(root, {{field(a, "id"), "FILLED", 50, 99860}, {field(b, "id"), "FILLED", 50, 100000},
                            {field(xa, "id"), "FILLED", 50, 99995}, {field(xb, "id"), "FILLED", 50, 100005}});
        bridge.poll(t0);
        const auto rec = lines(root / "data/live_orders/strategy_trades.csv");
        check(rec.size() == 3 && rec[0].rfind("date,model,symbol", 0) == 0, "the real round trips are recorded, one per leg");
        double gross = 0.0, net = 0.0;
        for (std::size_t i = 1; i < rec.size(); ++i) {
            std::vector<std::string> c;
            std::string cell;
            bool q = false;
            for (const char ch : rec[i]) {
                if (ch == '"') { q = !q; continue; }
                if (ch == ',' && !q) { c.push_back(cell); cell.clear(); continue; }
                cell += ch;
            }
            c.push_back(cell);
            gross += std::stod(c[10]);
            net += std::stod(c[12]);
            check(c[15] == "REAL", "marked REAL");
        }
        check(std::fabs(gross - ((999.95 - 998.60) * 50 + (1000.00 - 1000.05) * 50)) < 1e-6,
              "gross: the gap less the spreads crossed to get out, from FYERS's prices");
        check(std::fabs((gross - net) - 0.00015 * 50 * (998.60 + 1000.00 + 999.95 + 1000.05)) < 0.02,
              "net: gross less the four fills' expenses");
        check(bridge.holdings().at({"Cross-exchange arbitrage", 2}).qty == 0
                  && bridge.holdings().at({"Cross-exchange arbitrage", 1}).qty == 0,
              "nothing is left held");
    }

    // Legging: the BSE leg fills, the NSE leg does not: the BSE leg is sent back out.
    quote(1, 100000, 100010, t0 + 40 * kSec);
    quote(2, 99850, 99860, t0 + 40 * kSec);
    ls = lines(intents);
    check(ls.size() == 6, "a new pair: two more requests");
    if (ls.size() == 6) {
        const std::string a = field(ls[4], "token") == "2" ? ls[4] : ls[5];
        const std::string b = field(ls[4], "token") == "1" ? ls[4] : ls[5];
        std::vector<std::tuple<std::string, std::string, std::int64_t, std::int64_t>> all;
        for (std::size_t i = 0; i < 4; ++i) all.emplace_back(field(ls[i], "id"), "FILLED", 50, i < 2 ? (i == 0 ? 99860 : 100000) : (i == 2 ? 99995 : 100005));
        all.emplace_back(field(a, "id"), "FILLED", 50, 99860);
        all.emplace_back(field(b, "id"), "CANCELLED", 0, 0);   // IOC: nothing at the NSE bid any more
        write_orders(root, all);
        bridge.poll(t0);
        ls = lines(intents);
        check(ls.size() == 7 && field(ls[6], "token") == "2" && field(ls[6], "side") == "SELL" && field(ls[6], "lots") == "50"
                  && field(ls[6], "by") == "strategy.arbitrage.exit",
              "the leg that filled is flattened at once: sell the 50 bought on BSE");
        // The demo pair then meets and leaves: the BSE leg is already on its way out.
        quote(2, 99995, 100005, t0 + 45 * kSec);
        quote(1, 99995, 100005, t0 + 45 * kSec);
        check(e.book().flat("Cross-exchange arbitrage") && lines(intents).size() == 7,
              "when the strategy leaves too, the flattened leg is not sent out a second time");
        const std::string flat = ls[6];
        all.emplace_back(field(flat, "id"), "FILLED", 50, 99840);
        write_orders(root, all);
        bridge.poll(t0);
        const auto rec = lines(root / "data/live_orders/strategy_trades.csv");
        check(bridge.holdings().at({"Cross-exchange arbitrage", 2}).qty == 0 && rec.size() == 4
                  && rec.back().find("legging") != std::string::npos,
              "the flattened leg is recorded as a legging loss and nothing is left held");
    }

    // A halt: nothing new is asked for.
    { std::ofstream k(root / "data/kill_request.json"); k << "{}"; }
    bridge.poll(t0);
    const std::size_t before = lines(intents).size();
    quote(1, 100000, 100010, t0 + 50 * kSec);
    quote(2, 99850, 99860, t0 + 50 * kSec);
    check(!e.book().flat("Cross-exchange arbitrage") && lines(intents).size() == before,
          "with a halt requested the demo still enters, but no real entry is asked for");

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
