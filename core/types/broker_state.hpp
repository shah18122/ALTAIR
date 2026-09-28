// Read-side broker evidence, v2. No transport, credentials or order capability.
#pragma once

#include <time/timestamp.hpp>
#include <types/units.hpp>

#include <cstdint>
#include <optional>

namespace altair::broker_view {

inline constexpr std::uint32_t kSchemaVersion = 2;
enum class BrokerId : std::uint8_t { None, Fyers, ZerodhaKite };
enum class AuthStatus : std::uint8_t {
    NotConfigured, CredentialsPresent, LinkRequired, SessionSaved, Unverified,
    Authenticated, Expired, Rejected, Malformed, NoTransport
};
enum class FeedStatus : std::uint8_t {
    Disabled, Connecting, Live, Stale, Disconnected, Rejected, Error
};
enum class DataOrigin : std::uint8_t { Unknown, BrokerLive, Replay, LocalSnapshot };
enum class TradingMode : std::uint8_t { Paper, LiveDisabled, LiveArmed };
enum class OrderReadiness : std::uint8_t { Disabled, PaperOnly, Ready };

// Opaque local slot, not the provider's user ID. Increment generation on login,
// credential changes and logout. Refuse generation exhaustion; never wrap/reuse.
struct SessionKey {
    BrokerId broker{BrokerId::None};
    std::uint64_t account_slot{};
    std::uint64_t generation{};
    friend constexpr bool operator==(SessionKey, SessionKey) noexcept = default;
};

// UTC nanoseconds. Producer chooses a bounded expiry policy. Zero is unknown.
struct EvidenceWindow {
    Timestamp observed_at{};
    Timestamp expires_at{};
};

// Optional fields preserve unknown vs known zero. Units: integer paise.
// Fields keep provider semantics; they are never added, netted or inferred.
struct AccountFunds {
    /// Provider-reported cash balance, integer paise.
    std::optional<Notional> cash;
    /// Provider-reported balance available for trading, integer paise.
    std::optional<Notional> available_trading_balance;
    /// Provider-reported collateral value, integer paise.
    std::optional<Notional> collateral;
    /// Provider-reported utilised margin, integer paise.
    std::optional<Notional> utilised_margin;
};

// Build on a service-owned thread and publish using an existing safe snapshot
// mechanism. Plain concurrent assignment/read of this struct is NOT safe.
struct BrokerEvidence {
    std::uint32_t schema_version{kSchemaVersion};
    SessionKey session{};
    AuthStatus auth{AuthStatus::NotConfigured};
    EvidenceWindow authentication{};
    FeedStatus feed{FeedStatus::Disabled};
    DataOrigin origin{DataOrigin::Unknown};
    SessionKey market_session{};
    EvidenceWindow market_data{};
    // Prevent a delayed account reply from a previous login being accepted.
    SessionKey account_session{};
    EvidenceWindow account_snapshot{};
    AccountFunds funds{};
};

struct Routes {
    BrokerId data_primary{BrokerId::Fyers};
    BrokerId data_fallback{BrokerId::ZerodhaKite};
    BrokerId order_primary{BrokerId::ZerodhaKite};
    TradingMode mode{TradingMode::Paper};
    // Increment for every route/mode change. Zero is not yet configured.
    std::uint64_t revision{};
};

struct OrderGate {
    SessionKey session{};
    std::uint64_t route_revision{};
    EvidenceWindow risk_check{};
    bool risk_passed{};
    bool transport_permitted{};
    bool kill_switch_active{true};
};

/// Dimensionless provider validation; unknown enum values refuse.
[[nodiscard]] constexpr bool valid(BrokerId broker) noexcept {
    return broker == BrokerId::Fyers || broker == BrokerId::ZerodhaKite;
}

/// Dimensionless identity validation; zero slot/generation is unbound.
[[nodiscard]] constexpr bool valid(SessionKey key) noexcept {
    return valid(key.broker) && key.account_slot != 0 && key.generation != 0;
}

/// UTC ns, no wall-clock reads. Half-open interval; safe for all int64 inputs.
[[nodiscard]] constexpr bool fresh(EvidenceWindow window, Timestamp now) noexcept {
    return window.observed_at > Timestamp::epoch()
        && window.observed_at <= now && now < window.expires_at;
}

/// A successful service verification for this session, still within its TTL.
[[nodiscard]] constexpr bool authenticated(const BrokerEvidence& evidence,
                                           Timestamp now) noexcept {
    return evidence.schema_version == kSchemaVersion && valid(evidence.session)
        && evidence.auth == AuthStatus::Authenticated
        && fresh(evidence.authentication, now);
}

/// Live broker provenance plus fresh authentication/feed evidence, UTC ns.
[[nodiscard]] constexpr bool live_data(const BrokerEvidence& evidence,
                                      Timestamp now) noexcept {
    return authenticated(evidence, now) && evidence.feed == FeedStatus::Live
        && evidence.origin == DataOrigin::BrokerLive
        && evidence.market_session == evidence.session
        && fresh(evidence.market_data, now);
}

/// Return the observed eligible provider, or None. Never changes order routes.
[[nodiscard]] constexpr BrokerId select_data(const Routes& routes,
    const BrokerEvidence& primary, const BrokerEvidence& fallback, Timestamp now) noexcept {
    if (routes.revision == 0 || !valid(routes.data_primary)) return BrokerId::None;
    if (primary.session.broker == routes.data_primary && live_data(primary, now))
        return routes.data_primary;
    if (valid(routes.data_fallback) && routes.data_fallback != routes.data_primary
        && fallback.session.broker == routes.data_fallback && live_data(fallback, now))
        return routes.data_fallback;
    return BrokerId::None;
}

/// Read-side readiness, NOT authority to submit. OMS must recheck at dispatch.
/// All windows use UTC ns. Paper never yields Ready; no order fallback exists.
[[nodiscard]] constexpr OrderReadiness order_readiness(const Routes& routes,
    const BrokerEvidence& evidence, const OrderGate& gate, Timestamp now) noexcept {
    if (gate.kill_switch_active || routes.revision == 0)
        return OrderReadiness::Disabled;
    if (routes.mode == TradingMode::Paper) return OrderReadiness::PaperOnly;
    if (routes.mode != TradingMode::LiveArmed || !valid(routes.order_primary)
        || evidence.session.broker != routes.order_primary
        || !authenticated(evidence, now) || evidence.account_session != evidence.session
        || !fresh(evidence.account_snapshot, now) || gate.session != evidence.session
        || gate.route_revision != routes.revision || !gate.transport_permitted
        || !gate.risk_passed || !fresh(gate.risk_check, now))
        return OrderReadiness::Disabled;
    return OrderReadiness::Ready;
}

} // namespace altair::broker_view
