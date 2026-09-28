#include <broker/account_snapshot_parser.hpp>

#include <cstdio>

int main() {
    using namespace altair;
    using namespace altair::broker_view;
    const auto ok = [](int status, std::string_view body) {
        return ProviderResponse{status, body};
    };
    const SessionKey key{BrokerId::Fyers, 9, 1};
    const EvidenceWindow window{Timestamp{10}, Timestamp{100}};
    const auto result = parse_account_snapshot(
        BrokerId::Fyers, key, window,
        ok(200, R"({"s":"ok","fy_id":"FY-9"})"),
        ok(200, R"({"s":"ok","cash":"123.45"})"),
        ok(200, R"({"s":"ok","data":[]})"),
        ok(200, R"({"s":"ok","data":[]})"),
        ok(200, R"({"s":"ok","data":[]})"), "fy_id");
    int failures = 0;
    const auto check = [&failures](bool v, const char* text) {
        std::printf("%s %s\n", v ? "PASS" : "FAIL", text);
        if (!v) ++failures;
    };
    check(result.has_value(), "FYERS response set parses into a typed snapshot");
    if (result) {
        check(std::string_view(result->account_id.data()) == "FY-9",
              "provider account identity is preserved");
        check(result->typed_funds.available_trading_balance.has_value()
              && result->typed_funds.available_trading_balance->raw() == 12345,
              "decimal provider money is converted to exact paise");
        check(usable(*result, Timestamp{50}), "parsed snapshot is usable while fresh");
        check(complete(*result) && result->typed_positions_present,
              "five raw sections and an empty typed position set are complete");
    }
    const auto nonempty_positions = parse_account_snapshot(
        BrokerId::Fyers, key, window,
        ok(200, R"({"s":"ok","fy_id":"FY-9"})"),
        ok(200, R"({"s":"ok","cash":"0"})"),
        ok(200, R"({"s":"ok","data":[{"symbol":"NSE:SBIN-EQ"}]})"),
        ok(200, R"({"s":"ok","data":[]})"),
        ok(200, R"({"s":"ok","data":[]})"), "fy_id");
    check(nonempty_positions && complete(*nonempty_positions)
              && !nonempty_positions->typed_positions_present
              && usable(*nonempty_positions, Timestamp{50}),
          "non-empty raw positions stay complete without inventing typed rows");
    const auto numeric = parse_account_snapshot(
        BrokerId::Fyers, key, window,
        ok(200, R"({"s":"ok","fy_id":"FY-9"})"),
        ok(200, R"({"s":"ok","available":42.5})"),
        ok(0, ""), ok(0, ""), ok(0, ""), "fy_id");
    check(numeric.has_value() && numeric->typed_funds.available_trading_balance
              .has_value()
              && numeric->typed_funds.available_trading_balance->raw() == 4250,
          "numeric JSON balances are accepted without floating point rounding");
    const auto failed = parse_account_snapshot(
        BrokerId::ZerodhaKite, key, window,
        ok(200, R"({"status":"success","user_id":"K"})"),
        ok(500, "{}"), ok(200, R"({"status":"success"})"),
        ok(200, R"({"status":"success"})"), ok(200, R"({"status":"success"})"),
        "user_id");
    check(!failed.has_value(), "a broker/session mismatch is refused");
    return failures == 0 ? 0 : 1;
}
