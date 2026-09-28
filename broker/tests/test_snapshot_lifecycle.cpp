#include <broker/snapshot_lifecycle.hpp>

#include <cstdio>

int main() {
    using namespace altair;
    using namespace altair::broker;
    using namespace altair::broker_view;
    SnapshotLifecycle life{{100, 800, 5}};
    const SessionKey one{BrokerId::Fyers, 1, 1};
    const SessionKey two{BrokerId::Fyers, 1, 2};
    int failures = 0;
    const auto check = [&failures](bool ok, const char* text) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };
    life.link(one);
    const auto old_epoch = life.epoch();
    check(life.state() == SnapshotConnection::Polling && life.backoff_ms() == 100,
          "link starts a new polling epoch with bounded initial backoff");
    check(!life.accepted(two, old_epoch, Timestamp{1}),
          "a reply from another account session is refused");
    check(life.accepted(one, old_epoch, Timestamp{1'000'000}),
          "the current epoch accepts a fresh snapshot");
    check(life.fresh(Timestamp{2'000'000}), "accepted snapshot is fresh");
    life.transport_failure();
    check(life.state() == SnapshotConnection::Stale && life.backoff_ms() == 200,
          "a transport failure marks the last snapshot stale and backs off");
    life.link(two);
    check(life.epoch() != old_epoch && !life.accepts(one, old_epoch),
          "reconnect invalidates delayed replies from the old epoch");
    life.revoke();
    check(life.state() == SnapshotConnection::Revoked && !life.fresh(Timestamp{2}),
          "revocation disables freshness without erasing diagnostic timestamps");
    return failures == 0 ? 0 : 1;
}
