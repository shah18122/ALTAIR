// The order router against a fake FYERS: the arm file and its ceilings, the
// market hours, every limit a request must pass, the dispatch gate, the order
// book folded back into the rows, an uncertain send found by its tag or marked
// not placed (never re-sent), a duplicate request never sent twice, the kill
// switch cancelling what is open, and a dry run that sends nothing.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <oms/live_router.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "BAD ", what);
    if (!ok) { ++failures; }
}

using namespace altair;
using namespace altair::oms;

constexpr std::int64_t kSec = 1'000'000'000LL;
// Monday 2026-10-05, 10:00 IST = 04:30 UTC.
constexpr std::int64_t kMonday10 = (20'731LL * 86'400 + 4 * 3600 + 30 * 60) * kSec;

std::string arm_text(bool armed, std::int64_t now_unix, int lots = 1) {
    return std::string("{\"armed\":") + (armed ? "true" : "false") + ",\"by\":\"smit\",\"armed_unix\":" + std::to_string(now_unix - 60)
         + ",\"expires_unix\":" + std::to_string(now_unix + 3 * 3600) + ",\"max_lots\":" + std::to_string(lots)
         + ",\"max_order_value\":2500000,\"max_orders_per_day\":3,\"max_open_orders\":2,\"max_daily_loss\":5000,"
           "\"price_band_pct\":3}";
}

RouterInstrument fut() {
    return RouterInstrument{111u, "NIFTY26OCTFUT", "NSE:NIFTY26OCTFUT", "NFO", 75, 10};
}
RouterInstrument stock() {
    return RouterInstrument{222u, "M&M", "NSE:M&M-EQ", "NSE", 1, 5};
}

OrderIntent intent(const std::string& id, std::uint32_t token, const char* sym, const char* ex, std::int64_t limit,
                   const char* product = "NRML", std::int64_t lots = 1) {
    OrderIntent in;
    in.version = 1;
    in.id = id;
    in.by = "smit";
    in.token = token;
    in.symbol = sym;
    in.exchange = ex;
    in.side = IntentSide::Buy;
    in.lots = lots;
    in.order_type = limit > 0 ? IntentType::Limit : IntentType::Market;
    in.limit_paise = limit;
    in.product = product;
    in.validity = "DAY";
    return in;
}

/// The line desktop/order_ticket.hpp writes.
std::string intent_line(const std::string& id, std::int64_t at_ns, std::uint32_t token, const char* sym, const char* ex,
                        std::int64_t limit, const char* product = "NRML") {
    const std::int64_t s = at_ns / kSec;
    const std::int64_t day = s / 86'400, sod = s % 86'400;
    // civil from days (Howard Hinnant)
    const std::int64_t z = day + 719'468, era = z / 146'097, doe = z - era * 146'097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36'524 - doe / 146'096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    char at[32];
    std::snprintf(at, sizeof at, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ", static_cast<long long>(y),
                  static_cast<long long>(m), static_cast<long long>(d), static_cast<long long>(sod / 3600),
                  static_cast<long long>(sod / 60 % 60), static_cast<long long>(sod % 60));
    return std::string("{\"v\":1,\"id\":\"") + id + "\",\"at\":\"" + at + "\",\"by\":\"smit\",\"token\":" + std::to_string(token)
         + ",\"symbol\":\"" + sym + "\",\"exchange\":\"" + ex + "\",\"side\":\"BUY\",\"lots\":1,\"order_type\":\""
         + (limit > 0 ? "LIMIT" : "MARKET") + "\",\"limit_paise\":" + std::to_string(limit) + ",\"product\":\"" + product
         + "\",\"validity\":\"DAY\"}\n";
}

void write(const std::string& path, const std::string& text, bool append = false) {
    std::ofstream f(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    f << text;
}

/// A fake FYERS: a quote, positions, and an order book it keeps itself.
struct FakeFyers {
    double lp = 25'000.0;
    std::string pnl = "{\"s\":\"ok\",\"overall\":{\"pl_total\":-1200.5},\"netPositions\":[]}";
    bool drop_next_post = false;     ///< the order is placed, the reply is lost
    int posts = 0, deletes = 0, book_reads = 0;
    int next_id = 1;
    struct Placed { std::string id, tag; int status; int filled; };
    std::vector<Placed> book;
    std::string last_body;

    RouterTransport transport() {
        RouterTransport t;
        t.get = [this](const std::string& target) {
            RouterCall c{true, 200, {}};
            if (target.rfind(kRouterQuotesPath, 0) == 0) {
                const std::string sym = target.substr(std::string(kRouterQuotesPath).size());
                const std::string name = sym == "NSE%3ANIFTY26OCTFUT" ? "NSE:NIFTY26OCTFUT" : "NSE:M&M-EQ";
                c.body = "{\"s\":\"ok\",\"d\":[{\"n\":\"" + name + "\",\"s\":\"ok\",\"v\":{\"lp\":" + std::to_string(lp) + "}}]}";
            } else if (target == kRouterPositionsPath) {
                c.body = pnl;
            } else if (target == kRouterOrderBookPath) {
                ++book_reads;
                c.body = "{\"s\":\"ok\",\"orderBook\":[";
                for (std::size_t i = 0; i < book.size(); ++i)
                    c.body += (i ? "," : "") + std::string("{\"id\":\"") + book[i].id + "\",\"orderTag\":\"" + book[i].tag
                            + "\",\"status\":" + std::to_string(book[i].status) + ",\"filledQty\":" + std::to_string(book[i].filled)
                            + ",\"tradedPrice\":25001.5,\"message\":\"\"}";
                c.body += "]}";
            } else {
                c.status = 404;
            }
            return c;
        };
        t.send = [this](const std::string& method, const std::string& target, const std::string& body) {
            RouterCall c{true, 200, {}};
            if (target != kRouterOrderPath) { c.status = 404; return c; }
            if (method == "POST") {
                ++posts;
                last_body = body;
                const std::size_t at = body.find("\"orderTag\":\"");
                const std::string tag = at == std::string::npos ? "" : body.substr(at + 12, 18);
                const std::string id = "2610050000" + std::to_string(next_id++);
                book.push_back(Placed{id, tag, 6, 0});
                if (drop_next_post) { drop_next_post = false; return RouterCall{false, 0, {}}; }
                c.body = "{\"s\":\"ok\",\"code\":1101,\"message\":\"Order submitted successfully\",\"id\":\"" + id + "\"}";
            } else if (method == "DELETE") {
                ++deletes;
                for (auto& p : book)
                    if (body.find(p.id) != std::string::npos && p.status == 6) p.status = 1;
                c.body = "{\"s\":\"ok\",\"code\":1103,\"message\":\"Successfully cancelled order\"}";
            }
            return c;
        };
        return t;
    }
};

} // namespace

int main() {
    std::printf("order router\n");
    const std::int64_t now_unix = kMonday10 / kSec;

    // ---- JSON ----------------------------------------------------------------------
    {
        const auto j = parse_router_json(R"({"a":[1,2.5,-3e2],"b":{"c":"x\"yA"},"t":true,"n":null,"id":26100500001})");
        check(j && j->get("a")->items.size() == 3 && j->get("a")->items[2].number == -300.0, "arrays and exponents");
        check(j && j->get("b")->str("c") == "x\"yA", "escapes, \\u included");
        check(j && j->truth("t") && j->str("id") == "26100500001", "true, and an integral id sent as a number reads as digits");
        check(!parse_router_json("{\"a\":1,}") && !parse_router_json("{\"a\" 1}") && !parse_router_json("[1] x"),
              "a trailing comma, a missing colon and trailing text are refused");
        check(!parse_router_json(std::string(40, '[') + std::string(40, ']')), "nesting past the bound is refused");
    }

    // ---- the arm file ----------------------------------------------------------------------
    {
        const auto a = read_router_arm(arm_text(true, now_unix), now_unix);
        check(a.verdict == RouterArmVerdict::Armed && a.arm.max_lots == 1 && a.arm.max_order_value_paise == 250'000'000
                  && a.arm.max_daily_loss_paise == 500'000,
              "an armed file with every limit inside the ceilings arms, in paise");
        check(read_router_arm(arm_text(false, now_unix), now_unix).verdict == RouterArmVerdict::Disarmed, "armed false is off");
        check(read_router_arm(arm_text(true, now_unix), now_unix + 4 * 3600).verdict == RouterArmVerdict::Expired,
              "an arm past its expiry is off");
        check(read_router_arm(arm_text(true, now_unix, 500), now_unix).verdict == RouterArmVerdict::OutOfBounds,
              "500 lots is beyond the ceiling: refused, not clamped");
        check(read_router_arm("{\"armed\":true,\"armed_unix\":1,\"expires_unix\":2}", now_unix).verdict
                  == RouterArmVerdict::OutOfBounds,
              "an armed file without its limits is refused");
        check(read_router_arm("not json", now_unix).verdict == RouterArmVerdict::Unreadable, "garbage is unreadable, and off");
        std::string long_arm = arm_text(true, now_unix);
        long_arm.replace(long_arm.find(std::to_string(now_unix + 3 * 3600)), 10, std::to_string(now_unix + 9 * 3600));
        check(read_router_arm(long_arm, now_unix).verdict == RouterArmVerdict::Expired, "an arm longer than one session is refused");
    }

    // ---- the clock -------------------------------------------------------------------------
    {
        check(router_ist(kMonday10).weekday == 0 && router_ist(kMonday10).second_of_day == 10 * 3600, "Monday 10:00 IST");
        check(router_market_open(kMonday10), "open at 10:00 on a Monday");
        check(!router_market_open(kMonday10 - 46 * 60 * kSec) && router_market_open(kMonday10 - 45 * 60 * kSec),
              "closed at 09:14, open at 09:15");
        check(!router_market_open(kMonday10 + (5 * 3600 + 30 * 60) * kSec), "closed at 15:30");
        check(!router_market_open(kMonday10 + 5 * 86'400 * kSec), "closed on Saturday");
    }

    // ---- the limits ------------------------------------------------------------------------
    {
        const RouterArm arm = read_router_arm(arm_text(true, now_unix), now_unix).arm;
        const RouterInstrument f = fut(), s = stock();
        const RouterBook book{0, 0, -120'050};
        const auto ok = plan_router_order(intent("a1", 111, "NIFTY26OCTFUT", "NFO", 2'500'010), &f, arm, book, 2'500'000, kMonday10);
        check(ok && ok->qty == 75 && ok->product == "MARGIN" && ok->body.find("\"symbol\":\"NSE:NIFTY26OCTFUT\"") != std::string::npos
                  && ok->body.find("\"qty\":75") != std::string::npos && ok->body.find("\"limitPrice\":25000.10") != std::string::npos
                  && ok->body.find("\"orderTag\":\"AL") != std::string::npos,
              "one lot of the future: 75, MARGIN, the exact limit, a tag");
        const auto mis = plan_router_order(intent("a2", 222, "M&M", "NSE", 0, "MIS"), &s, arm, book, 300'000, kMonday10);
        check(mis && mis->product == "INTRADAY" && mis->body.find("\"type\":2") != std::string::npos,
              "a market MIS on equity is INTRADAY, type 2");
        const auto refused = [&](OrderIntent in, const RouterInstrument* inst, RouterBook b, std::optional<std::int64_t> ltp,
                                 std::int64_t now, const char* needle) {
            const auto r = plan_router_order(in, inst, arm, b, ltp, now);
            if (r) return false;
            if (r.error().find(needle) == std::string::npos) std::printf("      (said: %s)\n", r.error().c_str());
            return r.error().find(needle) != std::string::npos;
        };
        check(refused(intent("b", 111, "NIFTY26OCTFUT", "NFO", 2'500'000, "NRML", 2), &f, book, 2'500'000, kMonday10, "per order"),
              "two lots over a one-lot limit");
        check(refused(intent("b", 111, "NIFTY26OCTFUT", "NFO", 2'500'000, "CNC"), &f, book, 2'500'000, kMonday10, "F&O product"),
              "CNC on a future");
        check(refused(intent("b", 222, "M&M", "NSE", 300'000, "NRML"), &s, book, 300'000, kMonday10, "equity product"),
              "NRML on equity");
        check(refused(intent("b", 111, "NIFTY26OCTFUT", "NFO", 2'500'005), &f, book, 2'500'000, kMonday10, "tick"),
              "a limit off the tick");
        check(refused(intent("b", 111, "NIFTY26OCTFUT", "NFO", 2'600'000), &f, book, 2'500'000, kMonday10, "band"),
              "a limit 4% from the last price");
        check(refused(intent("b", 111, "NIFTY26OCTFUT", "NFO", 3'400'000 * 10), &f, book, 3'400'000 * 10, kMonday10, "worth"),
              "one lot worth Rs 2.55 crore over a Rs 25 lakh limit");
        check(refused(intent("b", 111, "NIFTY26OCTFUT", "NFO", 2'500'000), &f, RouterBook{3, 0, 0}, 2'500'000, kMonday10, "orders today"),
              "the fourth order of a three-a-day limit");
        check(refused(intent("b", 111, "NIFTY26OCTFUT", "NFO", 2'500'000), &f, RouterBook{0, 2, 0}, 2'500'000, kMonday10, "open"),
              "a third open order over two");
        check(refused(intent("b", 111, "NIFTY26OCTFUT", "NFO", 2'500'000), &f, RouterBook{0, 0, std::nullopt}, 2'500'000, kMonday10,
                      "P&L could not be read"),
              "an unread P&L is not a zero P&L");
        check(refused(intent("b", 111, "NIFTY26OCTFUT", "NFO", 2'500'000), &f, RouterBook{0, 0, -500'000}, 2'500'000, kMonday10,
                      "loss limit"),
              "a day down Rs 5,000 stops at a Rs 5,000 loss limit");
        check(refused(intent("b", 111, "NIFTY26OCTFUT", "NFO", 2'500'000), &f, book, std::nullopt, kMonday10, "no last price"),
              "no last price from FYERS");
        check(refused(intent("b", 111, "NIFTY26OCTFUT", "NFO", 2'500'000), &f, book, 2'500'000, kMonday10 + 6 * 3600 * kSec, "closed"),
              "after 15:30");
        check(refused(intent("b", 999, "X", "NFO", 2'500'000), nullptr, book, 2'500'000, kMonday10, "universe"), "an unknown token");
        check(refused(intent("b", 111, "BANKNIFTY26OCTFUT", "NFO", 2'500'000), &f, book, 2'500'000, kMonday10, "the request says"),
              "a token whose symbol is not the one requested");
    }

    // ---- the gate --------------------------------------------------------------------------
    {
        RouterEvidence e{true, false, true, 77, 5, kMonday10, kMonday10, true, kMonday10};
        const RouterGate g = router_gate(e);
        const auto permit = authorize_broker_dispatch(g.routes, g.evidence, g.gate, Timestamp{kMonday10});
        check(permit.has_value(), "armed, not killed, a fresh session, positions and risk check: a permit");
        e.killed = true;
        const RouterGate k = router_gate(e);
        check(!authorize_broker_dispatch(k.routes, k.evidence, k.gate, Timestamp{kMonday10}).has_value(), "the kill switch shuts it");
        e.killed = false;
        e.arm_revision = 78;
        const RouterGate r = router_gate(e);
        check(permit && !permit_matches(*permit, r.routes, r.evidence, r.gate, Timestamp{kMonday10}),
              "a changed arm file voids an issued permit");
        e.arm_revision = 77;
        e.auth_ok_ns = kMonday10 - 61 * kSec;
        const RouterGate s = router_gate(e);
        check(!authorize_broker_dispatch(s.routes, s.evidence, s.gate, Timestamp{kMonday10}).has_value(),
              "a session last accepted 61 s ago is not current");
    }

    // ---- replies --------------------------------------------------------------------------
    {
        check(router_day_pnl_paise(*parse_router_json(R"({"s":"ok","overall":{"pl_total":-1200.5}})")) == -120'050,
              "overall.pl_total, in paise");
        check(router_day_pnl_paise(*parse_router_json(
                  R"({"s":"ok","netPositions":[{"realized_profit":100,"unrealized_profit":-50.25},{"pl":-10}]})"))
                  == 3'975,
              "else the positions summed");
        check(!router_day_pnl_paise(*parse_router_json(R"({"s":"error","message":"token expired"})")), "an error is not a P&L");
        check(router_ltp_paise(*parse_router_json(R"({"s":"ok","d":[{"n":"NSE:SBIN-EQ","v":{"lp":812.35}}]})"), "NSE:SBIN-EQ")
                  == 81'235,
              "the last price of the symbol asked for");
        std::vector<RouterOrder> rows(3);
        rows[0].fyers_id = "1"; rows[0].status = RouterOrderStatus::Pending;
        rows[1].tag = "ALT"; rows[1].status = RouterOrderStatus::Uncertain;
        rows[2].tag = "GONE"; rows[2].status = RouterOrderStatus::Uncertain;
        const auto book = *parse_router_json(
            R"({"s":"ok","orderBook":[{"id":"1","status":2,"filledQty":75,"tradedPrice":250.05},{"id":"9","orderTag":"ALT","status":6}]})");
        apply_router_book(rows, book, 1);
        check(rows[0].status == RouterOrderStatus::Filled && rows[0].filled == 75 && rows[0].avg_paise == 25'005,
              "status 2 is filled, with the quantity and the average");
        check(rows[1].status == RouterOrderStatus::Open && rows[1].fyers_id == "9", "an uncertain send found by its tag");
        apply_router_book(rows, book, 2);
        apply_router_book(rows, book, 3);
        check(rows[2].status == RouterOrderStatus::NotPlaced, "an uncertain send missing from three reads was not placed");
        const auto stale = *parse_router_json(R"({"s":"ok","orderBook":[{"id":"1","status":2,"filledQty":10}]})");
        apply_router_book(rows, stale, 4);
        check(rows[0].filled == 75, "a stale read never undoes a fill");
    }

    // ---- the router end to end -------------------------------------------------------------
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("altair_router_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(root);
    fs::create_directories(root / "data/live_orders");
    const RouterPaths paths = RouterPaths::under(root.string());
    {
        FakeFyers fx;
        OrderRouter router(paths, {fut(), stock()}, fx.transport(), 42, false, kMonday10);
        check(router.startup_error().empty(), "starts with no saved cursor");
        write(paths.intents, intent_line("r1", kMonday10, 111, "NIFTY26OCTFUT", "NFO", 2'500'010));
        router.tick(kMonday10 + kSec);
        check(fx.posts == 0 && router.rows().size() == 1 && router.rows()[0].status == RouterOrderStatus::Refused,
              "not armed: refused, nothing sent");

        write(paths.arm, arm_text(true, now_unix));
        write(paths.intents, intent_line("r2", kMonday10 + kSec, 111, "NIFTY26OCTFUT", "NFO", 2'500'010), true);
        router.tick(kMonday10 + 2 * kSec);
        check(fx.posts == 1 && router.rows().back().status == RouterOrderStatus::Pending
                  && router.rows().back().fyers_id == "26100500001",
              "armed: sent once, pending with FYERS's id");
        router.tick(kMonday10 + 3 * kSec);
        check(router.rows().back().status == RouterOrderStatus::Open, "the order book says open");

        // The same request again (a crash between the write and the cursor save): never sent twice.
        write(paths.intents, intent_line("r2", kMonday10 + kSec, 111, "NIFTY26OCTFUT", "NFO", 2'500'010), true);
        router.tick(kMonday10 + 4 * kSec);
        check(fx.posts == 1, "a duplicate request id is not sent again");

        // A send whose reply is lost: found in the book by its tag, not re-sent.
        fx.drop_next_post = true;
        write(paths.intents, intent_line("r3", kMonday10 + 4 * kSec, 222, "M&M", "NSE", 0, "MIS"), true);
        router.tick(kMonday10 + 5 * kSec);
        check(fx.posts == 2 && router.rows().back().status == RouterOrderStatus::Uncertain, "no reply: uncertain");
        router.tick(kMonday10 + 6 * kSec);
        check(fx.posts == 2 && router.rows().back().status == RouterOrderStatus::Open && !router.rows().back().fyers_id.empty(),
              "found by its tag in the next order-book read");

        // The third order of the day would make three open: the open limit (2) refuses it.
        write(paths.intents, intent_line("r4", kMonday10 + 6 * kSec, 111, "NIFTY26OCTFUT", "NFO", 2'500'010), true);
        router.tick(kMonday10 + 7 * kSec);
        check(fx.posts == 2 && router.rows().back().status == RouterOrderStatus::Refused, "two open: the third is refused");

        // A cancel from the Terminal.
        write(paths.cancels, "{\"id\":\"26100500001\",\"at_unix\":" + std::to_string(now_unix + 8) + "}\n", true);
        router.tick(kMonday10 + 8 * kSec);
        check(fx.deletes == 1, "a cancel request becomes one DELETE");
        router.tick(kMonday10 + 10 * kSec);
        check(router.rows()[1].status == RouterOrderStatus::Cancelled, "and the book says cancelled");

        // The kill switch cancels what is still open and refuses new requests.
        write(paths.kill, "{\"requested_by\":\"smit\"}");
        write(paths.intents, intent_line("r5", kMonday10 + 10 * kSec, 222, "M&M", "NSE", 0, "MIS"), true);
        router.tick(kMonday10 + 11 * kSec);
        check(fx.deletes == 2 && fx.posts == 2 && router.rows().back().status == RouterOrderStatus::Refused,
              "kill: the open order is cancelled, the new request refused");
        check(router.status().killed && !router.status().why.empty(), "and the status says why");
        fs::remove(paths.kill);

        const std::string saved = [&] { std::ifstream f(paths.orders); std::ostringstream o; o << f.rdbuf(); return o.str(); }();
        check(saved.find("\"status\":\"CANCELLED\"") != std::string::npos && saved.find("\"beat_ns\"") != std::string::npos,
              "orders.json carries the rows and the heartbeat");
    }
    {
        // A restart carries today's rows and the cursor: nothing old is sent again.
        FakeFyers fx;
        OrderRouter again(paths, {fut(), stock()}, fx.transport(), 42, false, kMonday10 + 12 * kSec);
        again.tick(kMonday10 + 12 * kSec);
        check(again.rows().size() == 5 && fx.posts == 0, "a restart keeps today's five rows and re-sends nothing");
        check(again.status().orders_today == 2, "two of them count as sent today");
    }
    {
        // A dry run checks everything and sends nothing.
        fs::remove_all(root / "data/live_orders");
        fs::create_directories(root / "data/live_orders");
        fs::remove(paths.intents);
        FakeFyers fx;
        OrderRouter dry(paths, {fut(), stock()}, fx.transport(), 42, true, kMonday10 + 20 * kSec);
        write(paths.intents, intent_line("d1", kMonday10 + 20 * kSec, 111, "NIFTY26OCTFUT", "NFO", 2'500'010));
        dry.tick(kMonday10 + 21 * kSec);
        check(fx.posts == 0 && dry.rows().size() == 1 && dry.rows()[0].status == RouterOrderStatus::DryRun
                  && dry.rows()[0].message.find("\"qty\":75") != std::string::npos,
              "a dry run shows the exact body and sends nothing");
        // No FYERS session: refused with the reason.
        OrderRouter none(RouterPaths::under((root / "none").string()), {fut()}, fx.transport(), 0, false, kMonday10);
        fs::create_directories(root / "none/data");
        write((root / "none/data/live_trading.json").string(), arm_text(true, now_unix));
        write((root / "none/data/order_intents.jsonl").string(),
              intent_line("n1", kMonday10, 111, "NIFTY26OCTFUT", "NFO", 2'500'010));
        none.tick(kMonday10 + kSec);
        check(fx.posts == 0 && none.rows().size() == 1 && none.rows()[0].message.find("no FYERS session") != std::string::npos,
              "no FYERS session: refused, and it says so");
    }
    fs::remove_all(root);

    std::printf("%s\n", failures == 0 ? "all order router checks passed" : "order router checks did not pass");
    return failures == 0 ? 0 : 1;
}
