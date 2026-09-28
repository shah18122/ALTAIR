// OMS broker dispatch boundary. This authorises no network operation by itself.
#pragma once

#include <types/broker_state.hpp>

#include <cstdint>
#include <expected>

namespace altair::oms {

enum class BrokerDispatchError : std::uint8_t {
    PaperMode,
    NotReady
};

class BrokerDispatchPermit {
public:
    BrokerDispatchPermit() = delete;

    [[nodiscard]] constexpr broker_view::BrokerId broker() const noexcept {
        return broker_;
    }
    [[nodiscard]] constexpr broker_view::SessionKey session() const noexcept {
        return session_;
    }
    [[nodiscard]] constexpr std::uint64_t route_revision() const noexcept {
        return route_revision_;
    }
    /// UTC nanoseconds at which the complete gate was evaluated.
    [[nodiscard]] constexpr Timestamp evaluated_at() const noexcept {
        return evaluated_at_;
    }

private:
    constexpr BrokerDispatchPermit(broker_view::BrokerId broker,
                                   broker_view::SessionKey session,
                                   std::uint64_t route_revision,
                                   Timestamp evaluated_at) noexcept
        : broker_(broker), session_(session), route_revision_(route_revision),
          evaluated_at_(evaluated_at) {}

    broker_view::BrokerId broker_;
    broker_view::SessionKey session_;
    std::uint64_t route_revision_;
    Timestamp evaluated_at_;

    friend constexpr std::expected<BrokerDispatchPermit, BrokerDispatchError>
    authorize_broker_dispatch(const broker_view::Routes&,
                              const broker_view::BrokerEvidence&,
                              const broker_view::OrderGate&, Timestamp) noexcept;
};

/// Recheck the shared service evidence at the OMS boundary. A permit is bound
/// to this exact session generation and route revision; no order fallback.
[[nodiscard]] constexpr std::expected<BrokerDispatchPermit, BrokerDispatchError>
authorize_broker_dispatch(const broker_view::Routes& routes,
                          const broker_view::BrokerEvidence& evidence,
                          const broker_view::OrderGate& gate,
                          Timestamp now) noexcept {
    const auto readiness = broker_view::order_readiness(routes, evidence, gate, now);
    if (readiness == broker_view::OrderReadiness::PaperOnly)
        return std::unexpected(BrokerDispatchError::PaperMode);
    if (readiness != broker_view::OrderReadiness::Ready)
        return std::unexpected(BrokerDispatchError::NotReady);
    return BrokerDispatchPermit{routes.order_primary, evidence.session,
                                routes.revision, now};
}

/// Mandatory immediate pre-transport recheck. A previously issued permit is
/// invalid after any route revision, account slot or login-generation change.
[[nodiscard]] constexpr bool
permit_matches(const BrokerDispatchPermit& permit,
               const broker_view::Routes& routes,
               const broker_view::BrokerEvidence& evidence,
               const broker_view::OrderGate& gate,
               Timestamp now) noexcept {
    const auto current = authorize_broker_dispatch(routes, evidence, gate, now);
    return current.has_value() && current->broker() == permit.broker()
        && current->session() == permit.session()
        && current->route_revision() == permit.route_revision();
}

} // namespace altair::oms
