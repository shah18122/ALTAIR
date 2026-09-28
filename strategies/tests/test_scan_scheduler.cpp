// P7-02 acceptance tests for strategies/scan_scheduler.hpp.

#include <strategies/scan_scheduler.hpp>

#include <cstdio>
#include <memory>

namespace {

using namespace altair;
using namespace altair::strategies;
using altair::InstrumentId;
using altair::instruments::CrossVenuePair;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
}

constexpr std::int64_t kSec = 1'000'000'000LL;
constexpr Timestamp kNow{1000 * kSec};

CrossVenuePair pair(std::uint32_t nse, std::uint32_t bse) {
    CrossVenuePair p{};
    p.nse = static_cast<InstrumentId>(nse);
    p.bse = static_cast<InstrumentId>(bse);
    return p;
}

// A generous budget: 10s freshness, 2s skew.
ScanBudget budget() { return ScanBudget{Duration{10 * kSec}, Duration{2 * kSec}}; }

void test_fresh_pair_is_scanned() {
    auto s = std::make_unique<ScanScheduler>();
    check(s->register_pair(pair(0, 1)).has_value(), "pair registers");
    s->on_update(InstrumentId{0}, kNow);
    s->on_update(InstrumentId{1}, kNow);
    std::size_t out[4];
    const auto n = s->drain(kNow, 8, out, 4, budget());
    check(n == 1 && out[0] == 0, "a fresh, both-legs pair is emitted");
    check(s->stats().scans_emitted == 1, "scan counted");
    // Draining again with no new update yields nothing: emitting cleared dirty.
    check(s->drain(kNow, 8, out, 4, budget()) == 0, "no re-emit without a new update");
}

void test_unsubscribed_update_counted_not_scanned() {
    auto s = std::make_unique<ScanScheduler>();
    (void)s->register_pair(pair(0, 1));
    s->on_update(InstrumentId{99}, kNow);          // in no pair
    check(s->stats().updates_unsubscribed == 1, "an unsubscribed update is counted");
    check(s->stats().updates_admitted == 0, "and not admitted");
}

void test_incomplete_pair_suppressed() {
    auto s = std::make_unique<ScanScheduler>();
    (void)s->register_pair(pair(0, 1));
    s->on_update(InstrumentId{0}, kNow);           // only the NSE leg printed
    std::size_t out[4];
    check(s->drain(kNow, 8, out, 4, budget()) == 0, "a one-legged pair is not scanned");
    check(s->stats().suppressed_incomplete == 1, "suppression counted as incomplete");
}

void test_stale_and_skew_suppressed() {
    auto s = std::make_unique<ScanScheduler>();
    (void)s->register_pair(pair(0, 1));
    // Stale: both legs printed 20s ago, budget is 10s.
    const Timestamp old{kNow.ns_since_epoch() - 20 * kSec};
    s->on_update(InstrumentId{0}, old);
    s->on_update(InstrumentId{1}, old);
    std::size_t out[4];
    check(s->drain(kNow, 8, out, 4, budget()) == 0, "a stale pair is not scanned");
    check(s->stats().suppressed_stale == 1, "stale suppression counted");

    // Skew: legs fresh but 5s apart, budget skew is 2s.
    auto s2 = std::make_unique<ScanScheduler>();
    (void)s2->register_pair(pair(0, 1));
    s2->on_update(InstrumentId{0}, kNow);
    s2->on_update(InstrumentId{1}, Timestamp{kNow.ns_since_epoch() - 5 * kSec});
    check(s2->drain(kNow, 8, out, 4, budget()) == 0, "a skewed pair is not scanned");
    check(s2->stats().suppressed_skew == 1, "skew suppression counted");
}

void test_work_is_bounded() {
    auto s = std::make_unique<ScanScheduler>();
    for (std::uint32_t i = 0; i < 3; ++i) {
        (void)s->register_pair(pair(i * 2, i * 2 + 1));
    }
    for (std::uint32_t i = 0; i < 6; ++i) { s->on_update(InstrumentId{i}, kNow); }
    std::size_t out[8];
    const auto first = s->drain(kNow, 2, out, 8, budget());   // max_work = 2
    check(first == 2, "drain hands out at most max_work pairs");
    const auto second = s->drain(kNow, 8, out, 8, budget());
    check(second == 1, "the third pair surfaces on the next drain, still dirty");
    check(s->observable(kNow, budget()) == 3, "all three are observable");
}

void test_duplicate_leg_refused() {
    auto s = std::make_unique<ScanScheduler>();
    check(s->register_pair(pair(0, 1)).has_value(), "first pair ok");
    const auto dup = s->register_pair(pair(1, 2));   // instrument 1 reused
    check(!dup.has_value() && dup.error() == ScheduleError::DuplicateLeg,
          "an instrument cannot belong to two pairs");
}

} // namespace

int main() {
    test_fresh_pair_is_scanned();
    test_unsubscribed_update_counted_not_scanned();
    test_incomplete_pair_suppressed();
    test_stale_and_skew_suppressed();
    test_work_is_bounded();
    test_duplicate_leg_refused();
    std::printf("Scan scheduler: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
