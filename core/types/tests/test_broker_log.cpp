#include <types/broker_log.hpp>

#include <cstdio>
#include <string_view>

namespace {
using namespace altair;
using namespace altair::broker_view;
int failures{};

void check(bool ok, const char* message) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}

void messages_refuse_unsafe_input() {
    check(!make_redacted_message("")
              && make_redacted_message("").error() == BrokerLogError::EmptyMessage,
          "empty message refused");
    constexpr std::string_view exact{"broker session verified"};
    const auto message = make_redacted_message(exact);
    check(message && message->view() == exact, "message round-trips exactly");

    char full[kMaxBrokerLogMessageBytes]{};
    for (char& c : full) c = 'x';
    const auto maximum = make_redacted_message(
        std::string_view{full, kMaxBrokerLogMessageBytes});
    check(maximum && maximum->view().size() == kMaxBrokerLogMessageBytes,
          "maximum message accepted exactly");
    char over[kMaxBrokerLogMessageBytes + 1]{};
    for (char& c : over) c = 'x';
    const auto too_long = make_redacted_message(
        std::string_view{over, kMaxBrokerLogMessageBytes + 1});
    check(!too_long && too_long.error() == BrokerLogError::TooLong,
          "oversized message refused, not truncated");

    for (const auto unsafe : {"TOKEN refreshed", "client Secret present",
                              "AUTH_CODE received", "Api_Key=hidden",
                              "Authorization header"}) {
        const auto result = make_redacted_message(unsafe);
        check(!result && result.error() == BrokerLogError::SensitiveMarker,
              "sensitive marker refused case-insensitively");
    }
    const char control[]{'o', 'k', '\n'};
    const auto bad_control = make_redacted_message(std::string_view{control, 3});
    check(!bad_control && bad_control.error() == BrokerLogError::ControlCharacter,
          "control character refused");
}

void event_structure() {
    BrokerLogEvent event;
    check(!valid(event), "default event is invalid");
    event.event_id = {4, 9};
    event.occurred_at = Timestamp{100};
    event.broker = BrokerId::Fyers;
    event.kind = BrokerLogKind::Auth;
    event.message = *make_redacted_message("broker session verified");
    check(valid(event), "complete event accepted without request id");

    event.request_id = BrokerEventId{};
    check(!valid(event), "present zero request id refused");
    event.request_id = BrokerEventId{7, 3};
    check(valid(event), "present nonzero request id accepted");
    event.kind = static_cast<BrokerLogKind>(255);
    check(!valid(event), "unknown event kind refused");
    event.kind = BrokerLogKind::Feed;
    event.broker = BrokerId::None;
    check(!valid(event), "invalid broker refused");
    event.broker = BrokerId::ZerodhaKite;
    event.occurred_at = Timestamp::epoch();
    check(!valid(event), "non-positive occurrence time refused");
    event.occurred_at = Timestamp{100};
    event.schema_version = kBrokerLogSchemaVersion + 1;
    check(!valid(event), "unknown schema refused");
}
} // namespace

int main() {
    messages_refuse_unsafe_input();
    event_structure();
    std::printf("Broker log: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
