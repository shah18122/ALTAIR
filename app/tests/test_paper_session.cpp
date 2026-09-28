#include "../paper_session.hpp"

#include <cstdio>
#include <cstring>

namespace { int failures = 0; void check(bool ok, const char* s) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", s); if (!ok) ++failures;
}}

using namespace altair;

static ChargeSchedule schedule() {
    ChargeSchedule s{}; s.valid_from = Timestamp::epoch(); s.valid_to = Timestamp::max();
    s.verified = true; s.equity_intraday.present = true;
    s.equity_intraday.stt = rate_from(0.00025L);
    s.equity_intraday.stt_side = ChargeSide::Sell;
    return s;
}
static app::PaperSessionConfig config(std::int64_t cash = 100'000'000) {
    app::PaperSessionConfig c{}; c.opening_cash = Notional{cash};
    c.venue.max_quote_age = Duration{1'000'000'000}; c.venue.schedule = schedule();
    c.venue.segment = Segment::Cash; c.venue.exchange = Exchange::NSE;
    c.session = {broker_view::BrokerId::Fyers, 99, 1};
    c.instrument = broker_view::InstrumentKey{77};
    c.product = broker_view::PositionProduct::Intraday; c.lot_size = LotSize{50};
    std::memcpy(c.account_id.data(), "PAPER-ALT", 10); return c;
}
static oms::OrderIntent intent(const char* id) {
    oms::OrderIntent i{}; i.version = oms::kIntentSchemaVersion; i.id = id;
    i.at = "2026-09-27T10:00:00+05:30"; i.by = "paper-test"; i.token = 77;
    i.symbol = "CIPLA"; i.exchange = "NSE"; i.side = oms::IntentSide::Buy;
    i.lots = 2; i.order_type = oms::IntentType::Market; i.product = "MIS";
    i.validity = "DAY"; i.at_ns = 100'000'000'000; return i;
}

int main() {
    constexpr Timestamp now{100'000'000'000};
    app::PaperSession session{config()};
    check(session.submit(intent("paper-1"), now).has_value(), "paper intent is accepted without a live route");
    oms::PaperQuote q{Price{9'990}, Price{10'000}, Qty{100}, Qty{100}, now};
    check(session.on_quote(q, now).value_or(-1) == 1, "normalised quote deterministically fills paper order");
    check(session.position() == Qty{100} && session.conservation_ok(), "paper position and conservation are exact");
    const auto snap = session.snapshot({now, Timestamp{110'000'000'000}}, Price{10'050});
    check(snap.has_value() && snap->typed_positions.count == 1
              && snap->typed_funds.cash.has_value(), "paper funds and position publish through shared typed snapshot");

    const auto recovered = app::PaperSession::recover(config(), session.journal());
    check(recovered.has_value() && recovered->cash() == session.cash()
              && recovered->position() == session.position()
              && recovered->average_price() == session.average_price(),
          "restart replays the journal to byte-equivalent balances");

    app::PaperSession leg_a{config()}, leg_b{config()};
    check(leg_a.submit(intent("leg-a"), now).has_value()
              && leg_b.submit(intent("leg-b"), now).has_value(), "two paper legs submit independently");
    check(leg_a.on_quote(q, now).value_or(-1) == 1, "first leg fills");
    oms::PaperQuote stale = q; stale.ts = Timestamp{90'000'000'000};
    check(leg_b.on_quote(stale, now).value_or(-1) == 0
              && leg_a.position() == Qty{100} && leg_b.position().is_zero(),
          "leg failure remains visible as exposure and is never rolled back or hidden");
    check(leg_a.conservation_ok() && leg_b.conservation_ok(), "both ledgers conserve through leg failure");
    return failures == 0 ? 0 : 1;
}
