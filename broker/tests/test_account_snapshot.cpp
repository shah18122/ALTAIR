#include <types/broker_state.hpp>
#include <broker/account_snapshot.hpp>

#include <cstdio>
#include <cstring>

int main() {
    using namespace altair;
    using namespace altair::broker_view;
    int failures = 0;
    const auto check = [&failures](bool ok, const char* text) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };
    const Timestamp now{2'000'000'000};
    AccountSnapshot s{};
    s.account_session = SessionKey{BrokerId::Fyers, 7, 3};
    s.observed = EvidenceWindow{Timestamp{1'000'000'000}, Timestamp{3'000'000'000}};
    std::memcpy(s.account_id.data(), "FY123", 6);
    s.profile.status = SnapshotSectionStatus::Present;
    s.funds.status = SnapshotSectionStatus::Present;
    s.positions.status = SnapshotSectionStatus::Present;
    s.holdings.status = SnapshotSectionStatus::Present;
    s.orders.status = SnapshotSectionStatus::Present;
    s.typed_positions_present = true;
    s.typed_positions.account_session = s.account_session;
    s.typed_positions.observed = s.observed;
    s.typed_funds.cash = Notional{0};
    check(usable(s, now), "profile and freshness make a typed snapshot usable");
    check(complete(s), "all five parsed account sections make a complete snapshot");
    check(s.typed_funds.cash.has_value() && s.typed_funds.cash->raw() == 0,
          "known zero funds remain distinct from an absent value");
    s.funds.status = SnapshotSectionStatus::Absent;
    s.typed_funds.cash.reset();
    check(usable(s, now), "an absent optional section does not become a fake zero");
    check(!complete(s), "an absent optional section is visibly partial");
    s.profile.status = SnapshotSectionStatus::HttpError;
    check(!usable(s, now), "profile HTTP failure refuses the whole identity");
    s.profile.status = SnapshotSectionStatus::Present;
    s.observed.expires_at = Timestamp{2'000'000'000};
    check(!usable(s, now), "expiry at now is stale under the half-open window");
    return failures == 0 ? 0 : 1;
}
