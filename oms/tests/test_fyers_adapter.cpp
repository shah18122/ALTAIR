#include <oms/fyers_adapter.hpp>

#include <cstdio>

namespace { int failures = 0; void check(bool ok, const char* s) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", s); if (!ok) ++failures;
}}

using namespace altair;
using namespace altair::oms;

int main() {
    FyersOrderIntent order{"NSE:SBIN-EQ", Qty{10}, Side::Buy,
        FyersOrderType::Limit, "INTRADAY", Price{50'025}, Price{0},
        "DAY", "altair-42", false};
    const auto body = build_fyers_place_order(order);
    check(body.has_value() && body->find("\"limitPrice\":500.25") != std::string::npos
              && body->find("\"side\":1") != std::string::npos
              && body->find("\"offlineOrder\":false") != std::string::npos,
          "limit order maps exact paise and documented FYERS fields");
    auto stop = order; stop.type = FyersOrderType::StopMarket;
    stop.limit_price = Price{0}; stop.stop_price = Price{49'900};
    check(build_fyers_place_order(stop).has_value(), "SL-M requires and carries stopPrice");
    stop.stop_price = Price{0};
    check(!build_fyers_place_order(stop).has_value(), "SL-M without trigger is refused");
    auto deprecated = order; deprecated.product_type = "BO";
    check(!build_fyers_place_order(deprecated).has_value(), "deprecated bundled product is not guessed");
    check(map_fyers_status(2).value_or(OrderState::Unset) == OrderState::Filled
              && !map_fyers_status(3).has_value(), "known status maps and reserved status refuses");
    check(parse_fyers_order_id(R"({"s":"ok","id":"26092700012345"})")
              .value_or("") == "26092700012345", "response order id is extracted");
    check(fyers_dispatch_disposition(201, false)
              == FyersDispatchDisposition::ReconcileRequired
              && fyers_dispatch_disposition(408, false)
              == FyersDispatchDisposition::ReconcileRequired,
          "no-ack and timeout require reconciliation, never blind retry");
    return failures == 0 ? 0 : 1;
}
