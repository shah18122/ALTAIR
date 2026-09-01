// P4-05 acceptance tests for oms/kite_adapter.hpp.
//
// Every mapping here is testable without a network, a credential or a live
// session, which is the reason the translation is separate from the transport.
//
// The two that carry the file:
//   * test 2 -- an unknown status is REFUSED, not defaulted
//   * test 3 -- Kite has no partial-fill status, so it must be derived from
//     the quantities or it is invisible
//
// No check description here may contain the substring FAIL.

#include <oms/kite_adapter.hpp>

#include <cstdio>
#include <cstdint>
#include <cstddef>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

bool contains(std::string_view hay, std::string_view needle)
{
    return hay.find(needle) != std::string_view::npos;
}

} // namespace

using namespace altair;

namespace {

OrderIntentKite nifty_order()
{
    OrderIntentKite o{};
    o.exchange = kExchangeNfo;
    o.tradingsymbol = "NIFTY25SEP24000CE";
    o.side = Side::Buy;
    o.quantity = Qty{150};
    o.limit_price = Price{45'125};        // Rs 451.25
    o.product = kProductNrml;
    o.order_type = kOrderTypeLimit;
    o.validity = kValidityDay;
    o.tag = "altair-000123";
    return o;
}

// ── 1 ────────────────────────────────────────────────────────────────────
void a_place_order_body_carries_every_field()
{
    std::printf("\n1 a_place_order_body_carries_every_field\n");
    const auto b = build_place_order(nifty_order());
    check(b.has_value(), "a well-formed intent builds");
    if (!b) { return; }
    std::printf("    %s\n", b->c_str());

    check(contains(*b, "exchange=NFO"), "exchange");
    check(contains(*b, "tradingsymbol=NIFTY25SEP24000CE"), "trading symbol");
    check(contains(*b, "transaction_type=BUY"), "transaction type");
    check(contains(*b, "quantity=150"), "quantity");
    check(contains(*b, "product=NRML"), "product");
    check(contains(*b, "order_type=LIMIT"), "order type");
    check(contains(*b, "validity=DAY"), "validity");
    check(contains(*b, "price=451.25"),
          "and the price as DECIMAL RUPEES -- 45,125 paise becomes 451.25,"
          " converted at this boundary and nowhere else");
    check(contains(*b, "tag=altair-000123"),
          "with Altair's own id as the tag, so a postback matches without a"
          " lookup table that could go stale across a restart");

    OrderIntentKite sell = nifty_order();
    sell.side = Side::Sell;
    const auto s = build_place_order(sell);
    check(s && contains(*s, "transaction_type=SELL")
          && contains(*s, "quantity=150"),
          "a sell flips only the transaction type -- quantity stays POSITIVE,"
          " because a signed quantity could silently flip a sell into a buy");

    // Symbols carry characters that break an unencoded form body.
    OrderIntentKite odd = nifty_order();
    odd.tradingsymbol = "M&M25SEPFUT";
    const auto e = build_place_order(odd);
    check(e && contains(*e, "M%26M25SEPFUT"),
          "an ampersand in a trading symbol is percent-encoded -- unencoded"
          " it would truncate the request at that character");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// THE phantom-default trap, in its most expensive form.
void an_unknown_status_is_refused_not_defaulted()
{
    std::printf("\n2 an_unknown_status_is_refused_not_defaulted\n");
    struct Row { const char* s; OrderState want; };
    const Row known[] = {
        {"COMPLETE", OrderState::Filled},
        {"CANCELLED", OrderState::Cancelled},
        {"REJECTED", OrderState::Rejected},
        {"OPEN", OrderState::Open},
        {"TRIGGER PENDING", OrderState::Open},
        {"PUT ORDER REQ RECEIVED", OrderState::PendingNew},
        {"VALIDATION PENDING", OrderState::PendingNew},
        {"OPEN PENDING", OrderState::PendingNew},
        {"MODIFY VALIDATION PENDING", OrderState::PendingReplace},
        {"MODIFY PENDING", OrderState::PendingReplace},
        {"CANCEL PENDING", OrderState::PendingCancel},
    };
    int mapped = 0;
    for (const Row& r : known) {
        const auto m = map_status(r.s);
        if (m && *m == r.want) { ++mapped; }
    }
    check(mapped == 11, "all eleven documented statuses map explicitly");

    // TRIGGER PENDING is the one worth calling out: it is a stop order LIVE
    // at the exchange, waiting for its trigger.
    check(map_status("TRIGGER PENDING").value() == OrderState::Open,
          "TRIGGER PENDING is Open -- the stop is resting at the exchange and"
          " will fill without further action, so treating it as pending would"
          " leave Altair thinking it can still walk away from it");

    // And the point of the card.
    for (const char* unknown : {"SOME NEW STATUS", "", "open", "COMPLETED",
                                "AMO REQ RECEIVED"}) {
        const auto m = map_status(unknown);
        check(!m && m.error() == AdapterError::UnknownStatus,
              "an unrecognised status is REFUSED");
    }
    std::printf("    -> there is no default arm. A status this build has never"
                " seen blocks the\n       symbol rather than becoming an order"
                " Altair believes is resting.\n");
    check(!map_status("open"),
          "and the match is case sensitive, because Kite's are upper case and"
          " a lenient compare would accept a string from somewhere else");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// Kite has no partial-fill status.
void a_partial_fill_is_derived_from_the_quantities()
{
    std::printf("\n3 a_partial_fill_is_derived_from_the_quantities\n");
    KiteOrderUpdate u{};
    u.status = "OPEN";
    u.filled_quantity = Qty{75};
    u.pending_quantity = Qty{75};
    u.average_price = "451.25";
    const auto m = map_update(u);
    check(m.has_value(), "the update maps");
    if (!m) { return; }
    std::printf("    status OPEN with filled_quantity 75 -> %s\n",
                m->state == OrderState::PartiallyFilled ? "PartiallyFilled"
                                                        : "Open");
    check(m->state == OrderState::PartiallyFilled && m->derived_partial,
          "status OPEN with a filled quantity is PARTIALLY FILLED -- Kite has"
          " no distinct status for it, so reading the string alone makes a"
          " half-done order look untouched");
    check(m->cum_qty.raw() == 75, "the cumulative quantity comes through");
    check(m->avg_price.raw() == 45'125,
          "and the average price converts back to paise exactly");

    // OPEN with nothing done is plain Open.
    u.filled_quantity = Qty{0};
    const auto o = map_update(u);
    check(o && o->state == OrderState::Open && !o->derived_partial,
          "OPEN with nothing filled stays Open, and says it was not derived");

    // A COMPLETE is Filled whatever else is true.
    u.status = "COMPLETE";
    u.filled_quantity = Qty{150};
    const auto c = map_update(u);
    check(c && c->state == OrderState::Filled,
          "COMPLETE is Filled");

    // The mapped update feeds P4-04 without translation.
    Order ord = *open_order(Qty{150}, Timestamp{0});
    (void)apply(ord, OrderEvent::Ack, Timestamp{10});
    const auto part = map_update(KiteOrderUpdate{"OPEN", Qty{75}, Qty{75},
                                                 "451.25", ""});
    check(part.has_value(), "the partial maps");
    if (part) {
        const auto r = apply_fill(ord, FillReport{part->cum_qty,
                                                  part->avg_price,
                                                  Timestamp{20}});
        check(r && *r == Applied::Changed
              && ord.state == OrderState::PartiallyFilled,
              "and drives the P4-04 state machine directly, with the"
              " CUMULATIVE quantity it already speaks in");
    }
}

// ── 4 ────────────────────────────────────────────────────────────────────
void prices_round_trip_exactly_and_refuse_what_they_cannot_represent()
{
    std::printf("\n4 prices_round_trip_exactly_and_refuse_what_they_cannot"
                "_represent\n");
    int ok_count = 0;
    for (std::int64_t paise : {0LL, 5LL, 100LL, 45'125LL, 2'408'000LL,
                               99LL, 1LL, 123'456'789LL}) {
        const auto s = detail::paise_to_rupee_string(Price{paise});
        if (!s) { continue; }
        const auto back = detail::rupee_string_to_paise(*s);
        if (back && back->raw() == paise) { ++ok_count; }
    }
    check(ok_count == 8, "every price round-trips paise -> rupees -> paise");
    check(detail::paise_to_rupee_string(Price{45'125}).value() == "451.25",
          "45,125 paise is \"451.25\"");
    check(detail::paise_to_rupee_string(Price{5}).value() == "0.05",
          "5 paise is \"0.05\", with the leading zero and both decimals");
    check(detail::paise_to_rupee_string(Price{100}).value() == "1.00",
          "and a whole rupee still carries two decimals");

    check(detail::rupee_string_to_paise("451.2").value().raw() == 45'120,
          "one decimal is read as tenths, not hundredths");
    check(detail::rupee_string_to_paise("451").value().raw() == 45'100,
          "and no decimal point is whole rupees");
    check(detail::rupee_string_to_paise("451.250").value().raw() == 45'125,
          "a trailing zero third decimal is harmless");

    // The one that matters.
    const auto third = detail::rupee_string_to_paise("451.253");
    check(!third && third.error() == AdapterError::UnrepresentablePrice,
          "a NON-ZERO third decimal is REFUSED, not rounded -- rounding it"
          " would put a number in the ledger the broker never quoted");
    check(!detail::rupee_string_to_paise("abc"),
          "and a non-numeric price is refused");
    check(!detail::rupee_string_to_paise(""), "as is an empty one");
    check(!detail::paise_to_rupee_string(Price{-1}),
          "a negative price is refused");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void malformed_intents_are_caught_before_the_round_trip()
{
    std::printf("\n5 malformed_intents_are_caught_before_the_round_trip\n");
    OrderIntentKite o = nifty_order();
    o.quantity = Qty{0};
    check(!build_place_order(o), "a zero quantity is refused");
    o = nifty_order(); o.tradingsymbol = "";
    check(!build_place_order(o), "an empty trading symbol is refused");
    o = nifty_order(); o.exchange = "";
    check(!build_place_order(o), "an empty exchange is refused");

    o = nifty_order(); o.limit_price = Price{0};
    const auto lim = build_place_order(o);
    check(!lim && lim.error() == AdapterError::MalformedIntent,
          "a LIMIT order with no price is refused -- it would become a market"
          " order by accident, and the two differ by unbounded slippage");

    o = nifty_order(); o.order_type = kOrderTypeSlm; o.trigger_price = Price{0};
    check(!build_place_order(o),
          "an SL-M with no trigger is refused here rather than by Kite, which"
          " saves a round trip on an order that cannot work");
    o.trigger_price = Price{44'000};
    o.limit_price = Price{0};
    const auto slm = build_place_order(o);
    check(slm && contains(*slm, "trigger_price=440.00"),
          "and with a trigger it builds, carrying the trigger in rupees");

    // A MARKET order legitimately has no price.
    o = nifty_order();
    o.order_type = kOrderTypeMarket;
    o.limit_price = Price{0};
    const auto mkt = build_place_order(o);
    check(mkt && !contains(*mkt, "price="),
          "a MARKET order builds with no price field at all, rather than a"
          " price of zero that Kite would read as a limit");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void the_order_id_is_parsed_from_a_real_shaped_response()
{
    std::printf("\n6 the_order_id_is_parsed_from_a_real_shaped_response\n");
    const auto id = parse_order_id(
        R"({"status":"success","data":{"order_id":"250901000123456"}})");
    check(id && *id == "250901000123456",
          "the order id is pulled out of a success response");

    const auto num = parse_order_id(R"({"data":{"order_id":123456}})");
    check(num && *num == "123456",
          "and an UNQUOTED order id parses too -- Kite has returned both, and"
          " a parser that assumed quotes would fail on a live response");

    check(!parse_order_id(R"({"status":"error","message":"no"})"),
          "a response with no order_id is refused");
    check(!parse_order_id(""), "as is an empty body");
    check(!parse_order_id(R"({"order_id":""})"),
          "and an empty order id, which would otherwise become an order"
          " nobody can cancel");
}

} // namespace

int main()
{
    std::printf("altair oms kite adapter tests\n");
    a_place_order_body_carries_every_field();
    an_unknown_status_is_refused_not_defaulted();
    a_partial_fill_is_derived_from_the_quantities();
    prices_round_trip_exactly_and_refuse_what_they_cannot_represent();
    malformed_intents_are_caught_before_the_round_trip();
    the_order_id_is_parsed_from_a_real_shaped_response();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
