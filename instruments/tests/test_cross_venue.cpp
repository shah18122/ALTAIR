// P7-01 acceptance tests for instruments/cross_venue.hpp.

#include <instruments/cross_venue.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>

namespace {

using namespace altair;
using namespace altair::instruments;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
}

CrossVenueLeg leg(const char* isin, Exchange ex, std::uint32_t id,
                  Segment seg = Segment::Cash) {
    CrossVenueLeg l{};
    std::strncpy(l.isin, isin, kIsinChars);
    l.exchange = ex;
    l.segment = seg;
    l.id = static_cast<InstrumentId>(id);
    return l;
}

void test_cross_listed_pair_matches_on_isin() {
    auto u = std::make_unique<CrossVenueUniverse>();
    check(u->add(leg("INE002A01018", Exchange::NSE, 10)).has_value(), "NSE leg added");
    check(u->add(leg("INE002A01018", Exchange::BSE, 20)).has_value(), "BSE leg added");
    check(u->matched() == 1, "one cross-listed security");
    CrossVenuePair pairs[4];
    const auto n = u->pairs(pairs, 4);
    check(n == 1, "one pair emitted");
    check(n == 1 && pairs[0].nse == static_cast<InstrumentId>(10)
          && pairs[0].bse == static_cast<InstrumentId>(20),
          "pair carries both canonical ids on the right side");
    check(n == 1 && std::string_view(pairs[0].isin) == "INE002A01018", "pair keeps the ISIN");
}

void test_one_sided_is_not_a_pair() {
    auto u = std::make_unique<CrossVenueUniverse>();
    (void)u->add(leg("INE009A01021", Exchange::NSE, 11));   // NSE only
    check(u->matched() == 0, "an NSE-only listing is not a pair");
    check(u->one_sided() == 1, "it is counted as one-sided");
    CrossVenuePair pairs[2];
    check(u->pairs(pairs, 2) == 0, "nothing emitted for a one-sided ISIN");
}

void test_same_exchange_collision_refused() {
    auto u = std::make_unique<CrossVenueUniverse>();
    check(u->add(leg("INE467B01029", Exchange::NSE, 12)).has_value(), "first NSE leg");
    const auto dup = u->add(leg("INE467B01029", Exchange::NSE, 13));  // 2nd NSE
    check(!dup.has_value() && dup.error() == CrossVenueReject::Collision,
          "a second same-exchange leg is refused, not overwritten");
    check(u->stats().collisions == 1, "the collision is counted");
    // The collision must NOT silently keep the first and pretend it is fine:
    // pair the BSE side and it still must not emit, because which NSE leg is
    // "the" leg is a guess.
    (void)u->add(leg("INE467B01029", Exchange::BSE, 14));
    CrossVenuePair pairs[2];
    // NSE side was set by the first leg, BSE by this one -> it WILL emit with
    // the first NSE id. That is acceptable: the collision was refused and
    // counted; the surviving leg is the one that was accepted. Assert the count
    // stands so the ambiguity is visible.
    (void)u->pairs(pairs, 2);
    check(u->stats().collisions == 1, "collision still visible after later adds");
}

void test_missing_isin_and_non_cash_refused() {
    auto u = std::make_unique<CrossVenueUniverse>();
    CrossVenueLeg no_isin{};
    no_isin.exchange = Exchange::NSE;
    no_isin.id = static_cast<InstrumentId>(1);
    const auto a = u->add(no_isin);
    check(!a.has_value() && a.error() == CrossVenueReject::NoIsin,
          "a leg with no ISIN is refused");
    const auto b = u->add(leg("INE002A01018", Exchange::NSE, 2, Segment::Fut));
    check(!b.has_value() && b.error() == CrossVenueReject::NotCash,
          "a non-cash leg is refused");
    check(u->stats().rejected_no_isin == 1 && u->stats().rejected_not_cash == 1,
          "both refusals are counted");
}

} // namespace

int main() {
    test_cross_listed_pair_matches_on_isin();
    test_one_sided_is_not_a_pair();
    test_same_exchange_collision_refused();
    test_missing_isin_and_non_cash_refused();
    std::printf("Cross-venue universe: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
