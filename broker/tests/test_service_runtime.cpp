#include <broker/service_runtime.hpp>

#include <cstdio>
#include <cstring>

int main() {
    using namespace altair;
    using namespace altair::broker;
    using namespace altair::broker_view;
    int failures = 0;
    const auto check = [&failures](bool ok, const char* text) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };
    const SessionKey first{BrokerId::Fyers, 1, 1};
    BrokerServiceRuntime<4> service{BrokerId::Fyers, {{10, 80, 5}, 20}};
    check(service.link(first, Timestamp{1'000'000}), "link starts a service epoch");
    check(service.poll(Timestamp{1'000'000}) == PollDecision::Start,
          "first account poll starts immediately");
    check(service.poll(Timestamp{1'000'001}) == PollDecision::AlreadyRunning,
          "only one helper poll may be in flight");

    AccountSnapshot snapshot{};
    snapshot.account_session = first;
    snapshot.observed = {Timestamp{1'000'000}, Timestamp{11'000'000}};
    std::memcpy(snapshot.account_id.data(), "FY1", 4);
    snapshot.profile.status = SnapshotSectionStatus::Present;
    snapshot.positions.status = SnapshotSectionStatus::Present;
    snapshot.typed_positions.account_session = first;
    snapshot.typed_positions.observed = snapshot.observed;
    const auto epoch = service.poll_epoch();
    check(service.accept(snapshot, epoch, Timestamp{2'000'000}),
          "validated reply publishes and schedules healthy polling");
    BrokerEvidence evidence{};
    check(service.read(evidence).has_value()
              && evidence.auth == AuthStatus::Authenticated,
          "service publication is authenticated only after typed response");
    const auto original_observed = evidence.account_snapshot.observed_at;
    check(service.poll(Timestamp{21'999'999}) == PollDecision::Wait
              && service.poll(Timestamp{22'000'000}) == PollDecision::Start,
          "bounded healthy polling deadline is deterministic");
    service.helper_failed(Timestamp{23'000'000});
    check(service.read(evidence).has_value()
              && evidence.account_snapshot.observed_at == original_observed
              && evidence.auth == AuthStatus::Authenticated,
          "helper death preserves original evidence time and auth claim");
    check(service.poll(Timestamp{32'999'999}) == PollDecision::Wait
              && service.poll(Timestamp{33'000'000}) == PollDecision::Start,
          "failure applies bounded retry backoff");

    check(service.feed_connecting(Timestamp{44'000'000})
              && service.feed_live({Timestamp{44'000'000}, Timestamp{50'000'000}},
                                   Timestamp{44'000'000}),
          "feed state is published separately from account polling");
    check(service.read(evidence).has_value()
              && evidence.feed == FeedStatus::Live
              && evidence.origin == DataOrigin::BrokerLive,
          "live feed carries verified broker provenance");
    service.feed_failed(Timestamp{45'000'000});
    check(service.read(evidence).has_value()
              && evidence.feed == FeedStatus::Disconnected
              && evidence.origin == DataOrigin::Unknown,
          "disconnect invalidates live provenance immediately");

    check(service.link({BrokerId::Fyers, 1, 2}, Timestamp{46'000'000}),
          "reconnect creates a new epoch");
    check(service.poll(Timestamp{46'000'000}) == PollDecision::Start,
          "new session polls immediately");
    check(!service.accept(snapshot, epoch, Timestamp{47'000'000}),
          "reply from old epoch can never republish after reconnect");
    service.credentials_rejected(Timestamp{48'000'000});
    check(service.read(evidence).has_value() && evidence.auth == AuthStatus::Expired,
          "explicit provider rejection invalidates authentication");
    service.revoke(Timestamp{49'000'000});
    check(service.poll(Timestamp{99'000'000}) == PollDecision::Revoked,
          "revoked sessions cannot schedule helpers");
    check(service.log_size() == 4 && service.omitted_logs() > 0,
          "audit retention is bounded and reports overwritten events");
    for (std::size_t i = 0; i < service.log_size(); ++i)
        check(service.log_at(i) != nullptr && valid(*service.log_at(i)),
              "retained service log event is bounded, redacted and valid");
    return failures == 0 ? 0 : 1;
}
