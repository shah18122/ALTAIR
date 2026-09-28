// broker/snapshot_publisher.hpp -- the service-owned bridge from typed account
// replies to immutable broker evidence.  It has no transport and cannot place
// orders; callers provide already-authenticated read-only responses.
#pragma once

#include "snapshot_lifecycle.hpp"
#include "state_store.hpp"

#include <cstdint>
#include <expected>

namespace altair::broker {

enum class SnapshotPublishError : std::uint8_t {
    InvalidProvider,
    InvalidSession,
    StaleReply,
    InvalidSnapshot,
    PublicationFailed
};

/// Single-writer service façade. It makes the account-session/epoch check and
/// evidence publication one operation, so a delayed response cannot become a
/// connected broker pill after a logout or reconnect.
class SnapshotPublisher {
public:
    explicit SnapshotPublisher(broker_view::BrokerId provider,
                               SnapshotPollPolicy policy = {}) noexcept
        : provider_(provider), store_(provider), lifecycle_(policy) {}

    [[nodiscard]] broker_view::BrokerId provider() const noexcept { return provider_; }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return lifecycle_.epoch(); }
    [[nodiscard]] SnapshotConnection state() const noexcept { return lifecycle_.state(); }

    [[nodiscard]] std::expected<std::uint64_t, SnapshotPublishError>
    link(broker_view::SessionKey session) noexcept {
        if (!broker_view::valid(provider_) || !broker_view::valid(session)
            || session.broker != provider_)
            return std::unexpected(SnapshotPublishError::InvalidSession);
        lifecycle_.link(session);
        evidence_ = {};
        evidence_.session = session;
        evidence_.auth = broker_view::AuthStatus::SessionSaved;
        evidence_.feed = broker_view::FeedStatus::Disabled;
        return publish_evidence();
    }

    [[nodiscard]] std::expected<std::uint64_t, SnapshotPublishError>
    accept(const broker_view::AccountSnapshot& snapshot,
           std::uint64_t reply_epoch, Timestamp now) noexcept {
        if (!lifecycle_.accepts(snapshot.account_session, reply_epoch))
            return std::unexpected(SnapshotPublishError::StaleReply);
        if (!broker_view::usable(snapshot, now))
            return std::unexpected(SnapshotPublishError::InvalidSnapshot);

        const auto account_generation = store_.publish(snapshot);
        if (!account_generation)
            return std::unexpected(SnapshotPublishError::PublicationFailed);

        evidence_.session = snapshot.account_session;
        evidence_.auth = broker_view::AuthStatus::Authenticated;
        evidence_.authentication = snapshot.observed;
        evidence_.account_session = snapshot.account_session;
        evidence_.account_snapshot = snapshot.observed;
        evidence_.funds = snapshot.typed_funds;
        const auto evidence_generation = publish_evidence();
        if (!evidence_generation)
            return std::unexpected(evidence_generation.error());
        if (!lifecycle_.accepted(snapshot.account_session, reply_epoch,
                                 snapshot.observed.observed_at))
            return std::unexpected(SnapshotPublishError::StaleReply);
        return *evidence_generation;
    }

    void transport_failure() noexcept {
        lifecycle_.transport_failure();
        // A failed account poll says nothing about credential validity and
        // nothing about an independently owned market-data socket.
        (void)publish_evidence();
    }

    void observe_time(Timestamp now) noexcept {
        const auto before = lifecycle_.state();
        lifecycle_.observe_time(now);
        if (lifecycle_.state() != before) (void)publish_evidence();
    }

    void authentication_rejected() noexcept {
        lifecycle_.transport_failure();
        evidence_.auth = broker_view::AuthStatus::Expired;
        (void)publish_evidence();
    }

    [[nodiscard]] std::expected<std::uint64_t, SnapshotPublishError>
    feed_connecting(broker_view::SessionKey session) noexcept {
        if (!lifecycle_.accepts(session, lifecycle_.epoch()))
            return std::unexpected(SnapshotPublishError::InvalidSession);
        evidence_.feed = broker_view::FeedStatus::Connecting;
        evidence_.origin = broker_view::DataOrigin::Unknown;
        return publish_evidence();
    }

    [[nodiscard]] std::expected<std::uint64_t, SnapshotPublishError>
    feed_live(broker_view::SessionKey session,
              broker_view::EvidenceWindow window) noexcept {
        if (!lifecycle_.accepts(session, lifecycle_.epoch())
            || window.observed_at <= Timestamp::epoch()
            || window.expires_at <= window.observed_at)
            return std::unexpected(SnapshotPublishError::InvalidSession);
        evidence_.feed = broker_view::FeedStatus::Live;
        evidence_.origin = broker_view::DataOrigin::BrokerLive;
        evidence_.market_session = session;
        evidence_.market_data = window;
        return publish_evidence();
    }

    void feed_failure() noexcept {
        evidence_.feed = broker_view::FeedStatus::Disconnected;
        evidence_.origin = broker_view::DataOrigin::Unknown;
        (void)publish_evidence();
    }

    void revoke() noexcept {
        lifecycle_.revoke();
        evidence_.auth = broker_view::AuthStatus::Rejected;
        evidence_.feed = broker_view::FeedStatus::Rejected;
        (void)publish_evidence();
    }

    [[nodiscard]] std::expected<std::uint64_t, StateStoreError>
    read(broker_view::BrokerEvidence& out) const noexcept { return store_.read(out); }
    [[nodiscard]] std::expected<std::uint64_t, StateStoreError>
    read(broker_view::AccountSnapshot& out) const noexcept { return store_.read(out); }

private:
    [[nodiscard]] std::expected<std::uint64_t, SnapshotPublishError>
    publish_evidence() noexcept {
        const auto result = store_.publish(evidence_);
        if (!result) return std::unexpected(SnapshotPublishError::PublicationFailed);
        return *result;
    }

    broker_view::BrokerId provider_;
    StateStore store_;
    SnapshotLifecycle lifecycle_;
    broker_view::BrokerEvidence evidence_{};
};

} // namespace altair::broker
