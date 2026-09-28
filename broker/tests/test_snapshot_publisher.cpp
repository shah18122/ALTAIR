#include <broker/snapshot_publisher.hpp>

#include <cstdio>
#include <cstring>

int main() {
    using namespace altair;
    using namespace altair::broker;
    using namespace altair::broker_view;
    const SessionKey session{BrokerId::Fyers, 3, 1};
    const EvidenceWindow window{Timestamp{1'000}, Timestamp{10'000}};
    SnapshotPublisher publisher{BrokerId::Fyers, {10, 80, 5}};
    int failures = 0;
    const auto check = [&failures](bool ok, const char* text) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };
    check(publisher.link(session).has_value(), "link publishes a saved-session evidence state");
    const auto old_epoch = publisher.epoch();
    AccountSnapshot snapshot{};
    snapshot.account_session = session;
    snapshot.observed = window;
    std::memcpy(snapshot.account_id.data(), "FY3", 4);
    snapshot.profile.status = SnapshotSectionStatus::Present;
    snapshot.positions.status = SnapshotSectionStatus::Present;
    snapshot.typed_positions.account_session = session;
    snapshot.typed_positions.observed = window;
    snapshot.typed_funds.cash = Notional{12345};
    check(publisher.accept(snapshot, old_epoch, Timestamp{2'000}).has_value(),
          "a fresh typed response publishes account and authenticated evidence");
    BrokerEvidence evidence{};
    AccountSnapshot copy{};
    check(publisher.read(evidence).has_value()
              && evidence.auth == AuthStatus::Authenticated
              && evidence.account_session == session
              && evidence.funds.cash.has_value(),
          "readers see authenticated evidence with provider funds");
    check(publisher.read(copy).has_value() && copy.account_session == session,
          "the account snapshot is immutable and readable");
    check(publisher.link(SessionKey{BrokerId::Fyers, 3, 2}).has_value(),
          "reconnect publishes a new saved-session epoch");
    check(!publisher.accept(snapshot, old_epoch, Timestamp{2'000})
              && publisher.state() == SnapshotConnection::Polling,
          "an old reply cannot republish after reconnect");
    publisher.revoke();
    check(publisher.state() == SnapshotConnection::Revoked,
          "revocation changes the published lifecycle state");
    return failures == 0 ? 0 : 1;
}
