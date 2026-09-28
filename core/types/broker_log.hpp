// Read-side broker audit event, v1. Bounded, non-secret and allocation-free.
#pragma once

#include <types/broker_state.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace altair::broker_view {

inline constexpr std::uint32_t kBrokerLogSchemaVersion = 1;
inline constexpr std::size_t kMaxBrokerLogMessageBytes = 192;

enum class BrokerLogKind : std::uint8_t {
    Auth, AccountSnapshot, Feed, Route, OrderGate, Error
};

enum class BrokerLogError : std::uint8_t {
    EmptyMessage, TooLong, SensitiveMarker, ControlCharacter
};

/// Opaque 128-bit local identifier. It carries no provider credential.
struct BrokerEventId {
    std::uint64_t high{};
    std::uint64_t low{};
    friend constexpr bool operator==(BrokerEventId, BrokerEventId) noexcept = default;
};

/// A bounded diagnostic already proven not to contain common secret markers.
struct RedactedMessage {
    std::uint16_t size{};
    std::array<char, kMaxBrokerLogMessageBytes> bytes{};

    /// UTF-8/ASCII bytes. The producer owns human-readable encoding policy.
    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return {bytes.data(), size};
    }
};

/// Non-secret state projection used on both sides of a transition.
struct BrokerStateSummary {
    AuthStatus auth{AuthStatus::NotConfigured};
    FeedStatus feed{FeedStatus::Disabled};
    OrderReadiness order{OrderReadiness::Disabled};
    friend constexpr bool operator==(BrokerStateSummary,
                                     BrokerStateSummary) noexcept = default;
};

struct BrokerLogEvent {
    std::uint32_t schema_version{kBrokerLogSchemaVersion};
    BrokerEventId event_id{};
    /// UTC nanoseconds since Unix epoch.
    Timestamp occurred_at{};
    BrokerId broker{BrokerId::None};
    BrokerLogKind kind{BrokerLogKind::Error};
    BrokerStateSummary old_state{};
    BrokerStateSummary new_state{};
    std::optional<BrokerEventId> request_id{};
    /// HTTP status or WebSocket close code; zero means not applicable.
    std::uint16_t transport_status{};
    /// End-to-end operation latency in nanoseconds.
    std::uint64_t latency_ns{};
    RedactedMessage message{};
};

namespace detail {

[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] constexpr bool contains_ci(std::string_view text,
                                         std::string_view marker) noexcept {
    if (marker.size() > text.size()) return false;
    for (std::size_t i = 0; i + marker.size() <= text.size(); ++i) {
        bool same = true;
        for (std::size_t j = 0; j < marker.size(); ++j) {
            if (ascii_lower(text[i + j]) != ascii_lower(marker[j])) {
                same = false;
                break;
            }
        }
        if (same) return true;
    }
    return false;
}

} // namespace detail

/// Copy one already-redacted diagnostic into bounded storage. Refuses rather
/// than truncating and refuses markers that commonly introduce credentials.
[[nodiscard]] constexpr std::expected<RedactedMessage, BrokerLogError>
make_redacted_message(std::string_view text) noexcept {
    if (text.empty()) return std::unexpected(BrokerLogError::EmptyMessage);
    if (text.size() > kMaxBrokerLogMessageBytes)
        return std::unexpected(BrokerLogError::TooLong);
    constexpr std::array<std::string_view, 7> sensitive{
        "token", "secret", "auth_code", "request_token", "api_key",
        "apikey", "authorization"};
    for (const auto marker : sensitive) {
        if (detail::contains_ci(text, marker))
            return std::unexpected(BrokerLogError::SensitiveMarker);
    }
    RedactedMessage result;
    result.size = static_cast<std::uint16_t>(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c == 0x7f)
            return std::unexpected(BrokerLogError::ControlCharacter);
        result.bytes[i] = text[i];
    }
    return result;
}

[[nodiscard]] constexpr bool valid(BrokerEventId id) noexcept {
    return id.high != 0 || id.low != 0;
}

[[nodiscard]] constexpr bool valid(BrokerLogKind kind) noexcept {
    switch (kind) {
    case BrokerLogKind::Auth:
    case BrokerLogKind::AccountSnapshot:
    case BrokerLogKind::Feed:
    case BrokerLogKind::Route:
    case BrokerLogKind::OrderGate:
    case BrokerLogKind::Error:
        return true;
    }
    return false;
}

/// Structural validation only; this does not read a clock or authenticate.
[[nodiscard]] constexpr bool valid(const BrokerLogEvent& event) noexcept {
    return event.schema_version == kBrokerLogSchemaVersion
        && valid(event.event_id) && event.occurred_at > Timestamp::epoch()
        && valid(event.broker) && valid(event.kind)
        && (!event.request_id.has_value() || valid(*event.request_id))
        && event.message.size > 0
        && event.message.size <= kMaxBrokerLogMessageBytes;
}

} // namespace altair::broker_view
