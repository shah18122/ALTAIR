#include <broker/state_store.hpp>

#include <cstdio>

namespace {
using namespace altair;
using namespace altair::broker;
using namespace altair::broker_view;
int failures{};

void check(bool ok, const char* text) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", text); }
}

void evidence_publication() {
    StateStore store{BrokerId::Fyers};
    BrokerEvidence out;
    out.auth = AuthStatus::Rejected;
    const auto empty = store.read(out);
    check(empty && *empty == 0 && out.auth == AuthStatus::Rejected,
          "empty read leaves output untouched");

    BrokerEvidence first;
    first.session = {BrokerId::Fyers, 2, 1};
    first.auth = AuthStatus::SessionSaved;
    check(store.publish(first).value_or(0) == 1, "first evidence published");
    check(store.read(out).value_or(0) == 1 && out.auth == AuthStatus::SessionSaved,
          "evidence round-trips with generation");
    first.auth = AuthStatus::Authenticated;
    check(store.publish(first).value_or(0) == 2, "second evidence published");
    check(out.auth == AuthStatus::SessionSaved,
          "previous reader copy stays immutable after publish");
    check(store.read(out).value_or(0) == 2 && out.auth == AuthStatus::Authenticated,
          "new reader sees replacement snapshot");

    first.session.broker = BrokerId::ZerodhaKite;
    check(!store.publish(first)
              && store.publish(first).error() == StateStoreError::ProviderMismatch,
          "wrong provider evidence refused");
    first.session.broker = BrokerId::Fyers;
    ++first.schema_version;
    check(!store.publish(first)
              && store.publish(first).error() == StateStoreError::SchemaMismatch,
          "wrong evidence schema refused");
}

void positions_and_logs() {
    StateStore store{BrokerId::Fyers};
    PositionSnapshot positions;
    positions.account_session = {BrokerId::Fyers, 2, 1};
    check(store.publish(positions).value_or(0) == 1, "positions published");
    PositionSnapshot read_positions;
    check(store.read(read_positions).value_or(0) == 1
              && read_positions.account_session == positions.account_session,
          "positions round-trip");
    positions.account_session.broker = BrokerId::ZerodhaKite;
    check(!store.publish(positions), "cross-provider positions refused");

    BrokerLogEvent event;
    event.event_id = {1, 9};
    event.occurred_at = Timestamp{100};
    event.broker = BrokerId::Fyers;
    event.kind = BrokerLogKind::Feed;
    event.message = *make_redacted_message("feed heartbeat accepted");
    check(store.publish(event).value_or(0) == 1, "log event published");
    BrokerLogEvent read_event;
    check(store.read(read_event).value_or(0) == 1
              && read_event.message.view() == "feed heartbeat accepted",
          "log event round-trips");
    event.broker = BrokerId::ZerodhaKite;
    check(!store.publish(event), "cross-provider log refused");

    AccountSnapshot account;
    account.account_session = {BrokerId::Fyers, 11, 2};
    check(store.publish(account).value_or(0) == 1,
          "typed account snapshot published");
    AccountSnapshot read_account;
    check(store.read(read_account).value_or(0) == 1
              && read_account.account_session == account.account_session,
          "typed account snapshot round-trips immutably");
    account.account_session.broker = BrokerId::ZerodhaKite;
    check(!store.publish(account), "cross-provider account snapshot refused");

    StateStore invalid{BrokerId::None};
    BrokerEvidence evidence;
    check(!invalid.publish(evidence)
              && invalid.publish(evidence).error() == StateStoreError::InvalidProvider,
          "invalid store provider refuses publication");
}
} // namespace

int main() {
    evidence_publication();
    positions_and_logs();
    std::printf("Broker state store: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
