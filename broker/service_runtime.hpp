// broker/service_runtime.hpp -- deterministic broker-service control plane.
// No network, clock, JSON, credential, allocation or order capability.
#pragma once

#include "snapshot_publisher.hpp"

#include <types/broker_log.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <string_view>

namespace altair::broker {

enum class PollDecision : std::uint8_t { Wait, Start, AlreadyRunning, Revoked };

struct BrokerServicePolicy {
    SnapshotPollPolicy snapshot{};
    std::uint32_t healthy_poll_ms{2'000};
};

/// One writer owns this object. Readers consume immutable publications from
/// SnapshotPublisher. Poll timing and the audit ring are bounded and supplied
/// with an explicit clock, so tests/replay are deterministic.
template <std::size_t LogCapacity = 128>
class BrokerServiceRuntime {
    static_assert(LogCapacity > 0);
public:
    explicit BrokerServiceRuntime(broker_view::BrokerId provider,
                                  BrokerServicePolicy policy = {}) noexcept
        : provider_(provider), policy_(policy), publisher_(provider, policy.snapshot) {}

    [[nodiscard]] bool link(broker_view::SessionKey session, Timestamp now) noexcept {
        const auto result = publisher_.link(session);
        if (!result) return false;
        in_flight_ = false;
        next_poll_ = now;
        log(now, broker_view::BrokerLogKind::Auth, "session saved; verification pending");
        return true;
    }

    [[nodiscard]] PollDecision poll(Timestamp now) noexcept {
        publisher_.observe_time(now);
        if (publisher_.state() == SnapshotConnection::Revoked)
            return PollDecision::Revoked;
        if (in_flight_) return PollDecision::AlreadyRunning;
        if (now < next_poll_) return PollDecision::Wait;
        in_flight_ = true;
        poll_epoch_ = publisher_.epoch();
        return PollDecision::Start;
    }

    [[nodiscard]] std::uint64_t poll_epoch() const noexcept { return poll_epoch_; }
    [[nodiscard]] broker_view::BrokerId provider() const noexcept { return provider_; }
    [[nodiscard]] bool in_flight() const noexcept { return in_flight_; }
    [[nodiscard]] Timestamp next_poll() const noexcept { return next_poll_; }

    [[nodiscard]] bool accept(const broker_view::AccountSnapshot& snapshot,
                              std::uint64_t reply_epoch, Timestamp now) noexcept {
        if (!in_flight_ || reply_epoch != poll_epoch_) return false;
        in_flight_ = false;
        const auto result = publisher_.accept(snapshot, reply_epoch, now);
        if (!result) {
            schedule(now, policy_.snapshot.initial_backoff_ms);
            log(now, broker_view::BrokerLogKind::Error, "account reply refused");
            return false;
        }
        schedule(now, policy_.healthy_poll_ms);
        log(now, broker_view::BrokerLogKind::AccountSnapshot, "account snapshot published");
        return true;
    }

    /// Transport/helper death preserves the last observation and its original
    /// expiry. It never rewrites old data with a new timestamp.
    void helper_failed(Timestamp now) noexcept {
        in_flight_ = false;
        publisher_.transport_failure();
        schedule(now, policy_.snapshot.max_backoff_ms < publisher_backoff()
                          ? policy_.snapshot.max_backoff_ms : publisher_backoff());
        log(now, broker_view::BrokerLogKind::Error, "account helper unavailable");
    }

    void credentials_rejected(Timestamp now) noexcept {
        in_flight_ = false;
        publisher_.authentication_rejected();
        schedule(now, policy_.snapshot.max_backoff_ms);
        log(now, broker_view::BrokerLogKind::Auth, "broker rejected session");
    }

    void revoke(Timestamp now) noexcept {
        in_flight_ = false;
        publisher_.revoke();
        next_poll_ = Timestamp::max();
        log(now, broker_view::BrokerLogKind::Auth, "session revoked");
    }

    [[nodiscard]] bool feed_connecting(Timestamp now) noexcept {
        const auto result = publisher_.feed_connecting(session());
        if (result) log(now, broker_view::BrokerLogKind::Feed, "market feed connecting");
        return result.has_value();
    }

    [[nodiscard]] bool feed_live(broker_view::EvidenceWindow window,
                                 Timestamp now) noexcept {
        const auto result = publisher_.feed_live(session(), window);
        if (result) log(now, broker_view::BrokerLogKind::Feed, "market feed live");
        return result.has_value();
    }

    void feed_failed(Timestamp now) noexcept {
        publisher_.feed_failure();
        log(now, broker_view::BrokerLogKind::Feed, "market feed disconnected");
    }

    [[nodiscard]] std::expected<std::uint64_t, StateStoreError>
    read(broker_view::BrokerEvidence& value) const noexcept { return publisher_.read(value); }
    [[nodiscard]] std::expected<std::uint64_t, StateStoreError>
    read(broker_view::AccountSnapshot& value) const noexcept { return publisher_.read(value); }

    [[nodiscard]] std::size_t log_size() const noexcept { return log_size_; }
    [[nodiscard]] std::uint64_t omitted_logs() const noexcept { return omitted_logs_; }
    [[nodiscard]] const broker_view::BrokerLogEvent* log_at(std::size_t index) const noexcept {
        if (index >= log_size_) return nullptr;
        const std::size_t oldest = log_size_ == LogCapacity ? log_next_ : 0;
        return &logs_[(oldest + index) % LogCapacity];
    }

private:
    [[nodiscard]] broker_view::SessionKey session() const noexcept {
        broker_view::BrokerEvidence evidence{};
        return publisher_.read(evidence) ? evidence.session : broker_view::SessionKey{};
    }

    [[nodiscard]] std::uint32_t publisher_backoff() const noexcept {
        // SnapshotPublisher intentionally hides transport internals; the
        // service still applies a bounded exponential series locally.
        return current_backoff_;
    }

    void schedule(Timestamp now, std::uint32_t delay_ms) noexcept {
        const auto max_ns = std::numeric_limits<std::int64_t>::max();
        const auto base = now.ns_since_epoch();
        const auto delta = static_cast<std::int64_t>(delay_ms) * 1'000'000LL;
        next_poll_ = Timestamp{base > max_ns - delta ? max_ns : base + delta};
        if (delay_ms == policy_.healthy_poll_ms) {
            current_backoff_ = policy_.snapshot.initial_backoff_ms;
        } else {
            current_backoff_ = current_backoff_ > policy_.snapshot.max_backoff_ms / 2
                ? policy_.snapshot.max_backoff_ms
                : current_backoff_ * 2u;
        }
    }

    void log(Timestamp now, broker_view::BrokerLogKind kind,
             std::string_view message) noexcept {
        const auto redacted = broker_view::make_redacted_message(message);
        if (!redacted || now <= Timestamp::epoch()) { ++omitted_logs_; return; }
        broker_view::BrokerLogEvent event{};
        event.event_id = {static_cast<std::uint64_t>(provider_), ++event_sequence_};
        event.occurred_at = now;
        event.broker = provider_;
        event.kind = kind;
        event.message = *redacted;
        logs_[log_next_] = event;
        log_next_ = (log_next_ + 1) % LogCapacity;
        if (log_size_ < LogCapacity) ++log_size_;
        else ++omitted_logs_;
    }

    broker_view::BrokerId provider_{};
    BrokerServicePolicy policy_{};
    SnapshotPublisher publisher_;
    Timestamp next_poll_{};
    std::uint64_t poll_epoch_{};
    std::uint32_t current_backoff_{policy_.snapshot.initial_backoff_ms};
    bool in_flight_{};
    std::array<broker_view::BrokerLogEvent, LogCapacity> logs_{};
    std::size_t log_next_{};
    std::size_t log_size_{};
    std::uint64_t omitted_logs_{};
    std::uint64_t event_sequence_{};
};

} // namespace altair::broker
