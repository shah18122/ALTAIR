#include <oms/broker_dispatch_gate.hpp>

#include <cstdio>
#include <type_traits>

namespace {
using namespace altair;
using namespace altair::broker_view;
using namespace altair::oms;
int failures{};
constexpr Timestamp now{100};
constexpr EvidenceWindow fresh_window{Timestamp{90}, Timestamp{110}};

void check(bool ok, const char* message) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}

BrokerEvidence evidence() {
    BrokerEvidence result;
    result.session = {BrokerId::ZerodhaKite, 17, 4};
    result.auth = AuthStatus::Authenticated;
    result.authentication = fresh_window;
    result.account_session = result.session;
    result.account_snapshot = fresh_window;
    return result;
}

Routes routes() {
    Routes result;
    result.order_primary = BrokerId::ZerodhaKite;
    result.mode = TradingMode::LiveArmed;
    result.revision = 8;
    return result;
}

OrderGate gate(SessionKey session) {
    return {session, 8, fresh_window, true, true, false};
}

void permit_is_bound_to_current_evidence() {
    static_assert(!std::is_default_constructible_v<BrokerDispatchPermit>);
    auto e = evidence();
    auto r = routes();
    auto g = gate(e.session);
    const auto permit = authorize_broker_dispatch(r, e, g, now);
    check(permit.has_value(), "ready evidence creates permit");
    check(permit->broker() == BrokerId::ZerodhaKite
              && permit->session() == e.session
              && permit->route_revision() == 8
              && permit->evaluated_at() == now,
          "permit binds broker, session, route and evaluation time");
    check(permit_matches(*permit, r, e, g, now), "fresh permit rechecks true");

    ++r.revision;
    check(!permit_matches(*permit, r, e, g, now), "route change invalidates permit");
    r.revision = 8;
    ++e.session.generation;
    check(!permit_matches(*permit, r, e, g, now), "login change invalidates permit");
}

void every_safety_dimension_refuses() {
    auto e = evidence();
    auto r = routes();
    auto g = gate(e.session);

    r.mode = TradingMode::Paper;
    auto result = authorize_broker_dispatch(r, e, g, now);
    check(!result && result.error() == BrokerDispatchError::PaperMode,
          "paper mode never creates live permit");
    r.mode = TradingMode::LiveDisabled;
    check(!authorize_broker_dispatch(r, e, g, now), "live-disabled refuses");
    r.mode = TradingMode::LiveArmed;

    g.kill_switch_active = true;
    check(!authorize_broker_dispatch(r, e, g, now), "kill switch refuses");
    g.kill_switch_active = false;
    g.risk_passed = false;
    check(!authorize_broker_dispatch(r, e, g, now), "risk failure refuses");
    g.risk_passed = true;
    g.transport_permitted = false;
    check(!authorize_broker_dispatch(r, e, g, now), "transport refusal wins");
    g.transport_permitted = true;
    g.risk_check.expires_at = now;
    check(!authorize_broker_dispatch(r, e, g, now), "stale risk evidence refuses");
    g.risk_check = fresh_window;

    e.account_snapshot.expires_at = now;
    check(!authorize_broker_dispatch(r, e, g, now), "stale account refuses");
    e = evidence();
    e.auth = AuthStatus::Rejected;
    check(!authorize_broker_dispatch(r, e, g, now), "rejected login refuses");
    e = evidence();
    e.session.broker = BrokerId::Fyers;
    e.account_session = e.session;
    g.session = e.session;
    check(!authorize_broker_dispatch(r, e, g, now), "wrong order broker refuses");
    e = evidence();
    g = gate(e.session);
    ++g.route_revision;
    check(!authorize_broker_dispatch(r, e, g, now), "old route gate refuses");
}
} // namespace

int main() {
    permit_is_bound_to_current_evidence();
    every_safety_dimension_refuses();
    std::printf("Broker dispatch gate: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
