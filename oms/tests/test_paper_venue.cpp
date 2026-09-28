// P7-07 acceptance tests for oms/paper_venue.hpp.
// Plain main(), following the other oms/tests.

#include <oms/paper_venue.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace altair;
using namespace altair::oms;

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
}

constexpr std::int64_t kSec = 1'000'000'000LL;
constexpr Timestamp kNow{100 * kSec};

// A verified equity-intraday schedule, built the way risk/tests/test_cost.cpp
// builds one. verified = true is what makes compute_cost price a fill at all.
ChargeSchedule schedule() {
    ChargeSchedule s{};
    s.valid_from = Timestamp{0};
    s.valid_to   = Timestamp::max();
    s.verified   = true;
    s.equity_intraday.present      = true;
    s.equity_intraday.stt          = rate_from(0.00025L);
    s.equity_intraday.stt_side     = ChargeSide::Sell;
    s.equity_intraday.exch_txn_nse = rate_from(0.0000297L);
    s.equity_intraday.stamp        = rate_from(0.00003L);
    s.equity_intraday.ipft         = rate_from(0.000001L);
    return s;
}

BrokerageRule brokerage() {
    BrokerageRule b{};
    b.flat_per_order = Notional{2000};   // Rs 20 in paise
    b.pct = rate_from(0.0003L);
    b.take_lower = true;
    return b;
}

PaperConfig config(Duration latency = Duration{0},
                   Duration max_age = Duration{10 * kSec},
                   bool verified = true) {
    PaperConfig c{};
    c.max_quote_age = max_age;
    c.latency = latency;
    c.schedule = schedule();
    c.schedule.verified = verified;
    c.brokerage = brokerage();
    c.segment = Segment::Cash;
    c.exchange = Exchange::NSE;
    c.delivery = false;                  // intraday
    return c;
}

// A complete() intent. lots and side/type/limit vary per test.
OrderIntent intent(IntentSide side, IntentType type, std::int64_t lots,
                   std::int64_t limit_paise) {
    OrderIntent i{};
    i.version = kIntentSchemaVersion;
    i.id = "paper-1";
    i.at = "2026-09-25T10:00:00+05:30";
    i.by = "test";
    i.token = 256265;                    // NIFTY-ish; any non-zero token
    i.symbol = "TESTSYM";
    i.exchange = "NSE";
    i.side = side;
    i.lots = lots;
    i.order_type = type;
    i.limit_paise = limit_paise;
    i.product = "MIS";
    i.validity = "DAY";
    i.at_ns = kNow.ns_since_epoch();
    return i;
}

PaperQuote quote(Price bid, Qty bid_qty, Price ask, Qty ask_qty, Timestamp ts) {
    return PaperQuote{bid, ask, bid_qty, ask_qty, ts};
}

constexpr LotSize kLot{50};

// The cost of one buy fill, so the zero-cash test can fund it exactly.
Notional buy_fill_cost(Qty qty, Price px) {
    Trade t{};
    t.segment = Segment::Cash;
    t.exchange = Exchange::NSE;
    t.side = Side::Buy;
    t.qty = qty;
    t.price = px;
    t.delivery = false;
    t.trade_ts = kNow;
    const auto c = compute_cost(t, schedule(), brokerage());
    return c.has_value() ? c->total : Notional{-1};
}

// ── 1 ──────────────────────────────────────────────────────────────────────
void test_limit_buy_fills_at_or_through_limit() {
    PaperVenue v{Notional{100'000'000}, config()};
    const auto id = v.submit(intent(IntentSide::Buy, IntentType::Limit, 1, 10'000),
                             kLot, kNow);
    check(id.has_value(), "limit buy submits");

    std::vector<PaperFill> out;
    const auto n = v.on_quote(quote(Price{9'980}, Qty{50}, Price{9'990}, Qty{50},
                                    kNow), kNow, out);
    check(n.has_value() && *n == 1, "one fill when ask is through the limit");
    check(out.size() == 1 && out[0].fill_qty == Qty{50}, "50 units filled");
    check(out.size() == 1 && out[0].fill_price == Price{9'990},
          "filled at the ask, price improvement to us");
    check(out.size() == 1 && out[0].state == OrderState::Filled, "order Filled");
    check(v.working_count() == 0, "nothing left working");

    // A second venue where the ask sits above the limit: no fill.
    PaperVenue v2{Notional{100'000'000}, config()};
    (void)v2.submit(intent(IntentSide::Buy, IntentType::Limit, 1, 10'000), kLot, kNow);
    std::vector<PaperFill> out2;
    const auto n2 = v2.on_quote(quote(Price{10'000}, Qty{50}, Price{10'010},
                                      Qty{50}, kNow), kNow, out2);
    check(n2.has_value() && *n2 == 0, "no fill when the ask is above the limit");
    check(v2.working_count() == 1, "the unfilled order stays working");
}

// ── 2 ──────────────────────────────────────────────────────────────────────
void test_market_buy_respects_depth_and_partials() {
    PaperVenue v{Notional{1'000'000'000}, config()};
    (void)v.submit(intent(IntentSide::Buy, IntentType::Market, 3, 0), kLot, kNow);

    std::vector<PaperFill> out;
    const auto n1 = v.on_quote(quote(Price{9'990}, Qty{60}, Price{10'000},
                                     Qty{60}, kNow), kNow, out);
    check(n1.has_value() && *n1 == 1, "first quote fills once");
    check(out.size() == 1 && out[0].fill_qty == Qty{60}, "60 of 150 filled");
    check(out.size() == 1 && out[0].state == OrderState::PartiallyFilled,
          "order PartiallyFilled after 60");
    check(v.position() == Qty{60}, "position is 60");

    const Timestamp later{kNow.ns_since_epoch() + kSec};
    const auto n2 = v.on_quote(quote(Price{9'990}, Qty{120}, Price{10'000},
                                     Qty{120}, later), later, out);
    check(n2.has_value() && *n2 == 1, "second quote fills the rest");
    check(out.size() == 2 && out[1].fill_qty == Qty{90}, "remaining 90 filled");
    check(out.size() == 2 && out[1].state == OrderState::Filled, "now Filled");
    check(v.position() == Qty{150}, "position is 150");
}

// ── 3 ──────────────────────────────────────────────────────────────────────
void test_stale_quote_never_fills() {
    PaperVenue v{Notional{100'000'000}, config(Duration{0}, Duration{1 * kSec})};
    (void)v.submit(intent(IntentSide::Buy, IntentType::Market, 1, 0), kLot, kNow);

    // Quote observed 2s before now, max age 1s -> stale -> no fill.
    const Timestamp stale_ts{kNow.ns_since_epoch() - 2 * kSec};
    std::vector<PaperFill> out;
    const auto n = v.on_quote(quote(Price{9'990}, Qty{50}, Price{10'000}, Qty{50},
                                    stale_ts), kNow, out);
    check(n.has_value() && *n == 0, "a stale quote produces no fill");
    check(v.working_count() == 1, "the order is still working");

    // A fresh quote at the same prices fills it.
    const auto n2 = v.on_quote(quote(Price{9'990}, Qty{50}, Price{10'000}, Qty{50},
                                     kNow), kNow, out);
    check(n2.has_value() && *n2 == 1, "a fresh quote fills");
}

// ── 4 ──────────────────────────────────────────────────────────────────────
void test_latency_gates_first_fill() {
    PaperVenue v{Notional{100'000'000}, config(Duration{5 * kSec})};
    (void)v.submit(intent(IntentSide::Buy, IntentType::Market, 1, 0), kLot, kNow);

    const Timestamp t4{kNow.ns_since_epoch() + 4 * kSec};
    std::vector<PaperFill> out;
    const auto early = v.on_quote(quote(Price{9'990}, Qty{50}, Price{10'000},
                                        Qty{50}, t4), t4, out);
    check(early.has_value() && *early == 0, "no fill before submit+latency");

    const Timestamp t5{kNow.ns_since_epoch() + 5 * kSec};
    const auto ontime = v.on_quote(quote(Price{9'990}, Qty{50}, Price{10'000},
                                         Qty{50}, t5), t5, out);
    check(ontime.has_value() && *ontime == 1, "fills at submit+latency");
}

// ── 5 ──────────────────────────────────────────────────────────────────────
void test_zero_cash_refuses_buy() {
    PaperVenue v{Notional{0}, config()};
    (void)v.submit(intent(IntentSide::Buy, IntentType::Market, 1, 0), kLot, kNow);
    std::vector<PaperFill> out;
    const auto n = v.on_quote(quote(Price{9'990}, Qty{50}, Price{10'000}, Qty{50},
                                    kNow), kNow, out);
    check(n.has_value() && *n == 0, "zero cash cannot fund a buy");
    check(v.cash() == Notional{0}, "cash stays zero");
    check(v.working_count() == 1, "the order stays working");

    // Open with exactly notional + cost and the same buy fills.
    const Notional cost = buy_fill_cost(Qty{50}, Price{10'000});
    check(cost.raw() > 0, "the fill has a positive cost");
    const Notional need{10'000 * 50 + cost.raw()};
    PaperVenue funded{need, config()};
    (void)funded.submit(intent(IntentSide::Buy, IntentType::Market, 1, 0), kLot, kNow);
    std::vector<PaperFill> out2;
    const auto n2 = funded.on_quote(quote(Price{9'990}, Qty{50}, Price{10'000},
                                          Qty{50}, kNow), kNow, out2);
    check(n2.has_value() && *n2 == 1, "exact funding fills");
    check(funded.cash() == Notional{0}, "cash lands exactly at zero");
}

// ── 6 ──────────────────────────────────────────────────────────────────────
void test_every_fill_is_cost_priced() {
    PaperVenue v{Notional{100'000'000}, config()};
    (void)v.submit(intent(IntentSide::Buy, IntentType::Market, 1, 0), kLot, kNow);
    std::vector<PaperFill> out;
    (void)v.on_quote(quote(Price{9'990}, Qty{50}, Price{10'000}, Qty{50}, kNow),
                     kNow, out);
    check(out.size() == 1, "one fill produced");
    const CostBreakdown& c = out[0].cost;
    check(c.total.raw() > 0, "the fill carries a positive cost");
    const std::int64_t parts = c.brokerage.raw() + c.stt.raw()
        + c.exchange_txn.raw() + c.sebi.raw() + c.stamp.raw()
        + c.ipft.raw() + c.gst.raw() + c.dp.raw();
    check(parts == c.total.raw(), "itemised parts sum to total");
    check(c.schedule_verified, "the cost records a verified schedule");

    // An unverified schedule is refused at submit.
    PaperVenue u{Notional{100'000'000}, config(Duration{0}, Duration{10 * kSec}, false)};
    const auto id = u.submit(intent(IntentSide::Buy, IntentType::Market, 1, 0),
                             kLot, kNow);
    check(!id.has_value() && id.error() == PaperError::ScheduleUnverified,
          "an unverified schedule is refused at submit");
}

// ── 7 ──────────────────────────────────────────────────────────────────────
void test_conservation_holds_exactly() {
    PaperVenue v{Notional{100'000'000}, config()};

    // Buy 1 lot, then sell 1 lot: a closed round trip.
    (void)v.submit(intent(IntentSide::Buy, IntentType::Market, 1, 0), kLot, kNow);
    std::vector<PaperFill> out;
    const auto b = v.on_quote(quote(Price{9'990}, Qty{50}, Price{10'000}, Qty{50},
                                    kNow), kNow, out);
    check(b.has_value() && *b == 1, "buy leg fills");
    check(v.conservation_ok(), "conservation holds after the buy");

    const Timestamp t2{kNow.ns_since_epoch() + kSec};
    (void)v.submit(intent(IntentSide::Sell, IntentType::Market, 1, 0), kLot, t2);
    const auto s = v.on_quote(quote(Price{10'100}, Qty{50}, Price{10'110}, Qty{50},
                                    t2), t2, out);
    check(s.has_value() && *s == 1, "sell leg fills against the bid");
    check(v.conservation_ok(), "conservation holds after the sell");
    check(v.position() == Qty{0}, "flat after the round trip");

    // cash_delta at a flat position is realised P&L net of all costs; here the
    // buy was 10000 and the sell 10100, so it is positive minus two-sided cost.
    const auto delta = v.ledger().cash_delta();
    check(delta.has_value(), "cash_delta is computable");
}

// ── 8 ──────────────────────────────────────────────────────────────────────
void test_deterministic_replay() {
    const auto run = [](std::vector<PaperFill>& out) {
        PaperVenue v{Notional{1'000'000'000}, config()};
        (void)v.submit(intent(IntentSide::Buy, IntentType::Market, 3, 0), kLot, kNow);
        (void)v.on_quote(quote(Price{9'990}, Qty{60}, Price{10'000}, Qty{60}, kNow),
                         kNow, out);
        const Timestamp t2{kNow.ns_since_epoch() + kSec};
        (void)v.on_quote(quote(Price{9'990}, Qty{120}, Price{10'000}, Qty{120}, t2),
                         t2, out);
    };
    std::vector<PaperFill> a, b;
    run(a);
    run(b);
    check(a.size() == b.size() && a.size() == 2, "same number of fills");
    bool same = a.size() == b.size();
    for (std::size_t i = 0; same && i < a.size(); ++i) {
        same = a[i].id == b[i].id && a[i].fill_qty == b[i].fill_qty
            && a[i].fill_price == b[i].fill_price
            && a[i].cost.total == b[i].cost.total
            && a[i].state == b[i].state && a[i].ts == b[i].ts;
    }
    check(same, "two runs produce byte-identical fills");
}

} // namespace

int main() {
    test_limit_buy_fills_at_or_through_limit();
    test_market_buy_respects_depth_and_partials();
    test_stale_quote_never_fills();
    test_latency_gates_first_fill();
    test_zero_cash_refuses_buy();
    test_every_fill_is_cost_priced();
    test_conservation_holds_exactly();
    test_deterministic_replay();
    std::printf("Paper venue: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
