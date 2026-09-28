// Broker-service read-side publication. One store instance per provider.
#pragma once

#include <lockfree/snapshot_slots.hpp>
#include <broker/account_snapshot.hpp>
#include <types/broker_log.hpp>
#include <types/broker_positions.hpp>
#include <types/broker_state.hpp>

#include <cstdint>
#include <expected>

namespace altair::broker {

enum class StateStoreError : std::uint8_t {
    InvalidProvider,
    ProviderMismatch,
    SchemaMismatch,
    PublicationFailed,
    NoReaderSlot,
    Contended
};

/// Single-writer immutable publication for one broker. Four simultaneous
/// readers cover app, desktop, OMS and diagnostics; a fifth is refused.
class StateStore {
public:
    explicit StateStore(broker_view::BrokerId provider) noexcept
        : provider_(provider) {}

    [[nodiscard]] broker_view::BrokerId provider() const noexcept {
        return provider_;
    }

    [[nodiscard]] std::expected<std::uint64_t, StateStoreError>
    publish(const broker_view::BrokerEvidence& value) noexcept {
        if (!broker_view::valid(provider_))
            return std::unexpected(StateStoreError::InvalidProvider);
        if (value.schema_version != broker_view::kSchemaVersion)
            return std::unexpected(StateStoreError::SchemaMismatch);
        if (broker_view::valid(value.session)
            && value.session.broker != provider_)
            return std::unexpected(StateStoreError::ProviderMismatch);
        return map_publish(evidence_.publish(value));
    }

    [[nodiscard]] std::expected<std::uint64_t, StateStoreError>
    publish(const broker_view::PositionSnapshot& value) noexcept {
        if (!broker_view::valid(provider_))
            return std::unexpected(StateStoreError::InvalidProvider);
        if (value.schema_version != broker_view::kPositionSchemaVersion)
            return std::unexpected(StateStoreError::SchemaMismatch);
        if (!broker_view::valid(value.account_session)
            || value.account_session.broker != provider_)
            return std::unexpected(StateStoreError::ProviderMismatch);
        return map_publish(positions_.publish(value));
    }

    [[nodiscard]] std::expected<std::uint64_t, StateStoreError>
    publish(const broker_view::BrokerLogEvent& value) noexcept {
        if (!broker_view::valid(provider_))
            return std::unexpected(StateStoreError::InvalidProvider);
        if (value.schema_version != broker_view::kBrokerLogSchemaVersion)
            return std::unexpected(StateStoreError::SchemaMismatch);
        if (value.broker != provider_)
            return std::unexpected(StateStoreError::ProviderMismatch);
        if (!broker_view::valid(value))
            return std::unexpected(StateStoreError::SchemaMismatch);
        return map_publish(log_.publish(value));
    }

    [[nodiscard]] std::expected<std::uint64_t, StateStoreError>
    publish(const broker_view::AccountSnapshot& value) noexcept {
        if (!broker_view::valid(provider_))
            return std::unexpected(StateStoreError::InvalidProvider);
        if (value.schema_version != broker_view::kAccountSnapshotSchemaVersion)
            return std::unexpected(StateStoreError::SchemaMismatch);
        if (!broker_view::valid(value.account_session)
            || value.account_session.broker != provider_)
            return std::unexpected(StateStoreError::ProviderMismatch);
        return map_publish(account_.publish(value));
    }

    [[nodiscard]] std::expected<std::uint64_t, StateStoreError>
    read(broker_view::BrokerEvidence& out) const noexcept {
        return map_read(evidence_.read(out));
    }
    [[nodiscard]] std::expected<std::uint64_t, StateStoreError>
    read(broker_view::PositionSnapshot& out) const noexcept {
        return map_read(positions_.read(out));
    }
    [[nodiscard]] std::expected<std::uint64_t, StateStoreError>
    read(broker_view::BrokerLogEvent& out) const noexcept {
        return map_read(log_.read(out));
    }
    [[nodiscard]] std::expected<std::uint64_t, StateStoreError>
    read(broker_view::AccountSnapshot& out) const noexcept {
        return map_read(account_.read(out));
    }

private:
    [[nodiscard]] static std::expected<std::uint64_t, StateStoreError>
    map_publish(std::expected<std::uint64_t, SlotError> result) noexcept {
        if (result) return *result;
        return std::unexpected(StateStoreError::PublicationFailed);
    }

    [[nodiscard]] static std::expected<std::uint64_t, StateStoreError>
    map_read(std::expected<std::uint64_t, SlotError> result) noexcept {
        if (result) return *result;
        return std::unexpected(result.error() == SlotError::NoHoldFree
            ? StateStoreError::NoReaderSlot : StateStoreError::Contended);
    }

    broker_view::BrokerId provider_;
    SnapshotSlots<broker_view::BrokerEvidence, 4> evidence_;
    SnapshotSlots<broker_view::PositionSnapshot, 4> positions_;
    SnapshotSlots<broker_view::BrokerLogEvent, 4> log_;
    SnapshotSlots<broker_view::AccountSnapshot, 4> account_;
};

} // namespace altair::broker
