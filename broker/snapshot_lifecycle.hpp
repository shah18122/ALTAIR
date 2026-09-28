// broker/snapshot_lifecycle.hpp -- deterministic polling/reconnect policy for
// account snapshots. Transport and credentials stay outside this state machine.
#pragma once

#include <types/broker_state.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace altair::broker {

enum class SnapshotConnection : std::uint8_t {
    Disconnected, Polling, Healthy, Stale, Revoked
};

struct SnapshotPollPolicy {
    std::uint32_t initial_backoff_ms{250};
    std::uint32_t max_backoff_ms{30'000};
    std::uint32_t freshness_ms{5'000};
};

/// Single-writer state for one provider. A reconnect always increments the
/// epoch, so a delayed reply from the old session cannot publish into the new
/// account. The last accepted snapshot timestamp is retained for diagnostics,
/// but `fresh()` controls whether consumers may use it.
class SnapshotLifecycle {
public:
    explicit SnapshotLifecycle(SnapshotPollPolicy policy = {}) noexcept
        : policy_(policy), backoff_ms_(policy.initial_backoff_ms) {}

    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
    [[nodiscard]] SnapshotConnection state() const noexcept { return state_; }
    [[nodiscard]] std::uint32_t backoff_ms() const noexcept { return backoff_ms_; }
    [[nodiscard]] Timestamp last_observed() const noexcept { return last_observed_; }
    [[nodiscard]] const broker_view::SessionKey& session() const noexcept { return session_; }

    void link(broker_view::SessionKey session) noexcept {
        if (epoch_ == std::numeric_limits<std::uint64_t>::max()) {
            state_ = SnapshotConnection::Revoked;
            return;
        }
        session_ = session;
        ++epoch_;
        state_ = SnapshotConnection::Polling;
        backoff_ms_ = policy_.initial_backoff_ms;
        last_observed_ = Timestamp::epoch();
    }

    [[nodiscard]] bool accepts(broker_view::SessionKey session,
                               std::uint64_t reply_epoch) const noexcept {
        return reply_epoch == epoch_ && session == session_;
    }

    bool accepted(broker_view::SessionKey session, std::uint64_t reply_epoch,
                  Timestamp observed) noexcept {
        if (!accepts(session, reply_epoch) || observed <= Timestamp::epoch())
            return false;
        last_observed_ = observed;
        state_ = SnapshotConnection::Healthy;
        backoff_ms_ = policy_.initial_backoff_ms;
        return true;
    }

    void transport_failure() noexcept {
        if (state_ != SnapshotConnection::Revoked) {
            state_ = last_observed_ == Timestamp::epoch()
                ? SnapshotConnection::Disconnected : SnapshotConnection::Stale;
        }
        backoff_ms_ = std::min(policy_.max_backoff_ms,
                               backoff_ms_ > policy_.max_backoff_ms / 2
                                   ? policy_.max_backoff_ms
                                   : backoff_ms_ * 2u);
    }

    /// Advance freshness without inventing a transport failure.  A poll that
    /// simply has not completed before its TTL is stale, not revoked.
    void observe_time(Timestamp now) noexcept {
        if (state_ == SnapshotConnection::Healthy && !fresh(now))
            state_ = SnapshotConnection::Stale;
    }

    void revoke() noexcept {
        if (epoch_ != std::numeric_limits<std::uint64_t>::max()) ++epoch_;
        state_ = SnapshotConnection::Revoked;
    }

    [[nodiscard]] bool fresh(Timestamp now) const noexcept {
        if (state_ != SnapshotConnection::Healthy
            || last_observed_ <= Timestamp::epoch() || now < last_observed_)
            return false;
        const auto age = now.ns_since_epoch() - last_observed_.ns_since_epoch();
        return age >= 0
            && age <= static_cast<std::int64_t>(policy_.freshness_ms) * 1'000'000;
    }

private:
    SnapshotPollPolicy policy_{};
    broker_view::SessionKey session_{};
    std::uint64_t epoch_{};
    std::uint32_t backoff_ms_{};
    Timestamp last_observed_{};
    SnapshotConnection state_{SnapshotConnection::Disconnected};
};

} // namespace altair::broker
