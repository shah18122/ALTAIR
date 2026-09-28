#include <types/broker_state.hpp>
#include <cstdio>
#include <type_traits>

namespace {
using namespace altair;
using namespace altair::broker_view;
int failures{};
constexpr Timestamp now{100};
constexpr EvidenceWindow window{Timestamp{90}, Timestamp{110}};

void check(bool ok, const char* message) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}

BrokerEvidence verified(BrokerId broker) {
    BrokerEvidence e;
    e.session = {broker, 1, 2};
    e.auth = AuthStatus::Authenticated;
    e.authentication = window;
    e.feed = FeedStatus::Live;
    e.origin = DataOrigin::BrokerLive;
    e.market_session = e.session;
    e.market_data = window;
    e.account_session = e.session;
    e.account_snapshot = window;
    return e;
}

void defaults_and_units() {
    static_assert(kSchemaVersion == 2);
    static_assert(std::is_same_v<decltype(AccountFunds{}.cash),
                                 std::optional<Notional>>);
    static_assert(std::is_same_v<decltype(AccountFunds{}.available_trading_balance),
                                 std::optional<Notional>>);
    static_assert(std::is_same_v<decltype(AccountFunds{}.collateral),
                                 std::optional<Notional>>);
    static_assert(std::is_same_v<decltype(AccountFunds{}.utilised_margin),
                                 std::optional<Notional>>);
    check(select_data({}, {}, {}, now) == BrokerId::None, "defaults have no live feed");
    check(order_readiness({}, {}, {}, now) == OrderReadiness::Disabled,
          "defaults deny orders");
    AccountFunds f;
    check(!f.cash.has_value() && !f.available_trading_balance.has_value()
              && !f.collateral.has_value() && !f.utilised_margin.has_value(),
          "all missing funds are unknown");
    f.cash = Notional{0};
    f.available_trading_balance = Notional{0};
    f.collateral = Notional{0};
    f.utilised_margin = Notional{0};
    check(f.cash == Notional{0}, "known zero cash");
    check(f.available_trading_balance == Notional{0},
          "known zero available trading balance");
    check(f.collateral == Notional{0}, "known zero collateral");
    check(f.utilised_margin == Notional{0}, "known zero utilised margin");

    f.cash = Notional{12345};
    check(f.cash == Notional{12345}, "cash stays exact paise");
    check(f.available_trading_balance == Notional{0} && f.collateral == Notional{0}
              && f.utilised_margin == Notional{0},
          "assigning cash does not alter another funds field");
    f.available_trading_balance = Notional{-23};
    check(f.available_trading_balance == Notional{-23},
          "available trading balance stays exact signed paise");
    check(f.cash == Notional{12345} && f.collateral == Notional{0}
              && f.utilised_margin == Notional{0},
          "assigning available balance does not alter another funds field");
    f.collateral = Notional{-7};
    check(f.collateral == Notional{-7}, "collateral stays provider-semantic paise");
    check(f.cash == Notional{12345} && f.available_trading_balance == Notional{-23}
              && f.utilised_margin == Notional{0},
          "assigning collateral does not alter another funds field");
    f.utilised_margin = Notional{8900};
    check(f.utilised_margin == Notional{8900}, "utilised margin stays exact paise");
    check(f.cash == Notional{12345} && f.available_trading_balance == Notional{-23}
              && f.collateral == Notional{-7},
          "assigning utilised margin does not alter another funds field");
}

void freshness_boundaries() {
    check(fresh(window, Timestamp{90}), "observation inclusive");
    check(!fresh(window, Timestamp{110}), "expiry exclusive");
    check(!fresh(window, Timestamp{89}), "future observation refused");
    check(!fresh({}, now), "missing evidence refused");
    check(!fresh({Timestamp{110}, Timestamp{90}}, now), "inverted window refused");
    check(!fresh({Timestamp::min(), Timestamp::max()}, now), "negative observation refused");
    check(fresh({Timestamp{1}, Timestamp::max()}, now), "extreme expiry no overflow");
    check(!fresh(window, Timestamp::min()) && !fresh(window, Timestamp::max()),
          "extreme current times no overflow");
}

void authentication_evidence() {
    auto e = verified(BrokerId::Fyers);
    check(authenticated(e, now), "verified fresh login accepted");
    e.schema_version = 1;
    check(!authenticated(e, now), "previous evidence schema is refused");
    e.schema_version = kSchemaVersion;
    check(authenticated(e, now), "current evidence schema is accepted");
    for (auto state : {AuthStatus::NotConfigured, AuthStatus::CredentialsPresent,
         AuthStatus::LinkRequired, AuthStatus::SessionSaved, AuthStatus::Unverified,
         AuthStatus::Expired, AuthStatus::Rejected, AuthStatus::Malformed,
         AuthStatus::NoTransport, static_cast<AuthStatus>(255)}) {
        e.auth = state;
        check(!authenticated(e, now), "non-authenticated state refused");
    }
    e = verified(BrokerId::Fyers);
    e.session.generation = 0;
    check(!authenticated(e, now), "unbound generation refused");
    e = verified(BrokerId::Fyers);
    e.authentication.expires_at = now;
    check(!authenticated(e, now), "expired verification refused");
}

void feed_routing() {
    Routes r; r.revision = 1;
    auto a = verified(BrokerId::Fyers);
    auto b = verified(BrokerId::ZerodhaKite);
    check(select_data(r, a, b, now) == BrokerId::Fyers, "preferred fresh provider");
    a.origin = DataOrigin::Replay;
    check(select_data(r, a, b, now) == BrokerId::ZerodhaKite, "replay cannot win live route");
    b.market_data.expires_at = now;
    check(select_data(r, a, b, now) == BrokerId::None, "stale fallback refused");
    a = verified(BrokerId::Fyers); b = verified(BrokerId::ZerodhaKite);
    check(select_data(r, b, a, now) == BrokerId::None, "wrong provider slots refused");
    a.schema_version = kSchemaVersion + 1; b.feed = FeedStatus::Disconnected;
    check(select_data(r, a, b, now) == BrokerId::None, "unknown schema and disconnected feed");
    a = verified(BrokerId::Fyers); a.feed = static_cast<FeedStatus>(255);
    check(!live_data(a, now), "unknown feed state refused");
    a = verified(BrokerId::Fyers); ++a.session.generation;
    check(!live_data(a, now), "previous login feed cannot be reused after reauthentication");
}

void order_gates() {
    Routes r; r.revision = 7; r.mode = TradingMode::LiveArmed;
    const auto e = verified(BrokerId::ZerodhaKite);
    const OrderGate g{e.session, 7, window, true, true, false};
    check(order_readiness(r, e, g, now) == OrderReadiness::Ready, "all prerequisites ready");
    auto bad = g; bad.kill_switch_active = true;
    check(order_readiness(r, e, bad, now) == OrderReadiness::Disabled, "kill switch wins");
    bad = g; bad.risk_passed = false;
    check(order_readiness(r, e, bad, now) == OrderReadiness::Disabled, "risk failed");
    bad = g; bad.transport_permitted = false;
    check(order_readiness(r, e, bad, now) == OrderReadiness::Disabled, "transport disabled");
    bad = g; bad.risk_check.expires_at = now;
    check(order_readiness(r, e, bad, now) == OrderReadiness::Disabled, "risk evidence expired");
    auto stale = e; stale.account_snapshot.expires_at = now;
    check(order_readiness(r, stale, g, now) == OrderReadiness::Disabled, "account expired");
    stale = e; stale.auth = AuthStatus::Rejected;
    check(order_readiness(r, stale, g, now) == OrderReadiness::Disabled, "broker rejected");
    r.mode = TradingMode::Paper;
    check(order_readiness(r, e, g, now) == OrderReadiness::PaperOnly, "paper never live");
    r.mode = static_cast<TradingMode>(255);
    check(order_readiness(r, e, g, now) == OrderReadiness::Disabled, "unknown mode refused");
    r.mode = TradingMode::LiveDisabled;
    check(order_readiness(r, e, g, now) == OrderReadiness::Disabled, "live disabled");
}

void session_and_route_changes() {
    Routes r; r.revision = 7; r.mode = TradingMode::LiveArmed;
    auto e = verified(BrokerId::ZerodhaKite);
    OrderGate g{e.session, 7, window, true, true, false};
    ++r.revision;
    check(order_readiness(r, e, g, now) == OrderReadiness::Disabled, "route change invalidates gate");
    g.route_revision = r.revision;
    ++e.session.generation;
    check(order_readiness(r, e, g, now) == OrderReadiness::Disabled, "late previous login refused");
    g.session = e.session;
    check(order_readiness(r, e, g, now) == OrderReadiness::Disabled, "old account snapshot refused");
    e.account_session = e.session;
    check(order_readiness(r, e, g, now) == OrderReadiness::Ready, "new matching evidence accepted");
    ++e.account_session.account_slot;
    check(order_readiness(r, e, g, now) == OrderReadiness::Disabled, "other account refused");
    e = verified(BrokerId::Fyers); g.session = e.session;
    check(order_readiness(r, e, g, now) == OrderReadiness::Disabled, "no order provider fallback");
}
} // namespace

int main() {
    defaults_and_units(); freshness_boundaries(); authentication_evidence();
    feed_routing(); order_gates(); session_and_route_changes();
    std::printf("Broker evidence: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
