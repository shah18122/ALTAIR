// P2-01 acceptance tests for feed/tick.hpp.
// Plain main() (Catch2 blocked on vcpkg -- see LEDGER blocker #7).
//
// No check description here may contain the substring FAIL.

#include <feed/tick.hpp>
#include <core/lockfree/spsc_ring.hpp>

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

} // namespace

using namespace altair;

namespace {

SpecStore g_store;

DepthUpdate two_sided(std::int64_t bid, std::int64_t ask)
{
    DepthUpdate d{};
    d.bid[0] = DepthLevel{Price{bid}, Qty{100}, 3, 0};
    d.ask[0] = DepthLevel{Price{ask}, Qty{150}, 4, 0};
    d.bid_levels = 1;
    d.ask_levels = 1;
    return d;
}

// ── 1 ────────────────────────────────────────────────────────────────────
void layout_is_pinned()
{
    std::printf("\n1 layout_is_pinned\n");
    // The static_asserts in the header already failed the build if any of this
    // were wrong. These restate the important ones so the numbers appear in the
    // test log, where a reviewer reading output rather than source sees them.
    check(sizeof(Tick) == 64, "Tick is 64 bytes -- one cache line");
    check(sizeof(DepthLevel) == 24, "DepthLevel is 24 bytes");
    check(sizeof(DepthUpdate) == 272, "DepthUpdate is 272 bytes");
    check(std::is_trivially_copyable_v<Tick>, "Tick is trivially copyable");
    check(std::is_trivially_copyable_v<DepthUpdate>,
          "DepthUpdate is trivially copyable -- it rides a seqlock");
    check(kTickWireVersion == 1, "wire version 1");
    check(offsetof(Tick, id) == 0, "id is first, at offset 0");
    check(offsetof(DepthUpdate, ask) == 144, "the ask array starts at 144");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// P0-09b, closed. Two feeds hold DIFFERENT tokens for the same contract; the
// normalised tick must carry neither.
void no_broker_token_field()
{
    std::printf("\n2 no_broker_token_field\n");

    ContractSpec s{};
    s.id = InstrumentId::Invalid;
    s.lot_size = LotSize{75};
    s.tick_size = Price{5};
    s.price_scale = 100;
    s.expiry = Timestamp{1790226000000000000LL};
    s.valid_from = Timestamp{1787888700000000000LL};
    s.valid_to = Timestamp::max();
    s.exchange = Exchange::NSE;
    s.segment = Segment::Opt;
    s.opt_type = OptionType::CE;
    s.source = SpecSource::KiteDump;
    // The SAME contract, with two unrelated broker tokens.
    s.token[static_cast<std::size_t>(FeedSource::Kite)] = 11111u;
    s.token[static_cast<std::size_t>(FeedSource::Xts)]  = 99999u;
    std::snprintf(s.symbol, sizeof(s.symbol), "NIFTY26SEP25000CE");
    std::snprintf(s.underlying, sizeof(s.underlying), "NIFTY");

    const auto id = g_store.add(s);
    check(id.has_value(), "the contract is in the store");

    const auto via_kite = g_store.id_of(FeedSource::Kite, 11111u);
    const auto via_xts  = g_store.id_of(FeedSource::Xts, 99999u);
    check(via_kite.has_value() && via_xts.has_value(),
          "both feeds' tokens resolve");
    check(via_kite.has_value() && via_xts.has_value() && *via_kite == *via_xts,
          "and they resolve to the SAME InstrumentId -- two number spaces, "
          "one contract");

    // What a decoder builds. Note there is nowhere to put a token even if you
    // wanted to: Tick has no such field, which is the point.
    Tick t{};
    t.id = *via_kite;
    t.source = FeedSource::Kite;
    Tick u{};
    u.id = *via_xts;
    u.source = FeedSource::Xts;

    check(t.id == u.id,
          "a Kite tick and an XTS tick for this contract carry the same id");
    check(t.source != u.source,
          "while `source` still records which feed produced each -- a "
          "diagnostic, not an identity");
    check(sizeof(Tick) == 64,
          "and the struct has no token field at all: a failover cannot make "
          "an instrument vanish and a stranger take its place");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void empty_side_has_no_mid()
{
    std::printf("\n3 empty_side_has_no_mid\n");

    DepthUpdate d{};
    d.bid[0] = DepthLevel{Price{2500000}, Qty{100}, 3, 0};
    d.bid_levels = 1;
    d.ask_levels = 0;

    // Show what the naive read would have given, so the test documents the
    // hazard rather than merely avoiding it.
    check(d.ask[0].px.raw() == 0,
          "an unpopulated ask level is ZEROED -- reading ask[0] directly "
          "would give a price of 0");
    check(best_ask(d) == nullptr, "so best_ask returns nullptr instead");
    check(best_bid(d) != nullptr, "while the bid side is present");
    check(!mid(d).has_value(), "a one-sided book has NO mid");
    check(!spread(d).has_value(), "and no spread");
    check(!is_crossed(d), "an empty side is not crossed -- it is empty");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void mid_does_not_overflow()
{
    std::printf("\n4 mid_does_not_overflow\n");

    const std::int64_t big = 9'000'000'000'000'000'000LL;   // near int64 max
    const DepthUpdate d = two_sided(big, big + 100);

    // The naive form, evaluated in a wider type to show what it would have
    // done. In int64 this sum is UB, not a large number.
    const long double naive = static_cast<long double>(big)
                            + static_cast<long double>(big + 100);
    check(naive > 9.2e18L,
          "the naive (bid + ask) would exceed int64 -- that is UB, not a "
          "big number");

    const auto m = mid(d);
    check(m.has_value(), "mid is computed anyway");
    check(m.has_value() && m->raw() == big + 50,
          "and is exact, via bid + (ask - bid) / 2");

    const auto sp = spread(d);
    check(sp.has_value() && sp->raw() == 100, "spread is 100 paise");

    // Ordinary values still behave.
    const DepthUpdate n = two_sided(2500000, 2500500);
    check(mid(n)->raw() == 2500250, "an ordinary book mids correctly");
    check(spread(n)->raw() == 500, "and spreads correctly");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void crossed_and_locked()
{
    std::printf("\n5 crossed_and_locked\n");

    check(!is_crossed(two_sided(2500000, 2500500)), "a normal book is not crossed");
    check(is_crossed(two_sided(2500500, 2500000)), "bid above ask is crossed");
    check(is_crossed(two_sided(2500000, 2500000)),
          "and a LOCKED book counts too -- a zero spread is not a free trade, "
          "it is a book that cannot be traded");

    const auto sp = spread(two_sided(2500500, 2500000));
    check(sp.has_value() && sp->raw() == -500,
          "a crossed spread is negative and is NOT clamped to zero -- the "
          "sign is the evidence something is wrong");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void levels_bound_the_reads()
{
    std::printf("\n6 levels_bound_the_reads\n");

    DepthUpdate d{};
    for (std::size_t i = 0; i < kDepthLevels; ++i) {
        d.bid[i] = DepthLevel{Price{2500000 - static_cast<std::int64_t>(i) * 5},
                              Qty{10}, 1, 0};
    }
    d.bid_levels = 2;
    d.ask_levels = 0;

    check(best_bid(d) != nullptr && best_bid(d)->px.raw() == 2500000,
          "best_bid is level 0");
    check(d.bid[4].px.raw() == 2499980,
          "levels beyond bid_levels still hold data in the array");
    check(d.bid_levels == 2,
          "but bid_levels says only 2 are real -- a consumer must not read "
          "past it");

    // Stale data with a zero count must still report empty.
    d.bid_levels = 0;
    check(best_bid(d) == nullptr,
          "with bid_levels 0 the side is empty even though the array is not");
    check(!mid(d).has_value(), "and there is no mid");
}

// ── 7 ────────────────────────────────────────────────────────────────────
void flags_round_trip()
{
    std::printf("\n7 flags_round_trip\n");

    std::uint16_t f = 0;
    check(!has_flag(f, TickFlag::Snapshot), "a zero bitset holds nothing");
    check(!has_flag(f, TickFlag::Stale), "nothing at all");

    f = set_flag(f, TickFlag::Snapshot);
    check(has_flag(f, TickFlag::Snapshot), "Snapshot sets");
    check(!has_flag(f, TickFlag::Stale), "without setting Stale");

    f = set_flag(f, TickFlag::Stale);
    f = set_flag(f, TickFlag::ClosingAuct);
    check(has_flag(f, TickFlag::Snapshot) && has_flag(f, TickFlag::Stale)
              && has_flag(f, TickFlag::ClosingAuct),
          "flags compose");
    check(!has_flag(f, TickFlag::Synthetic),
          "and Synthetic stays clear -- a live tick must never claim it");
    check(!has_flag(f, TickFlag::None), "None tests false for everything");

    f = set_flag(f, TickFlag::Snapshot);
    check(has_flag(f, TickFlag::Snapshot), "setting twice is idempotent");
}

// ── 8 ────────────────────────────────────────────────────────────────────
// D7 against the actual transport, not just is_trivially_copyable.
SpscRing<Tick, 64> g_tick_ring;
SpscRing<DepthUpdate, 64> g_depth_ring;

void rides_a_ring()
{
    std::printf("\n8 rides_a_ring\n");

    Tick sent{};
    sent.id = static_cast<InstrumentId>(7);
    sent.seq = 4242;
    sent.exchange_ts = Timestamp{1787888700000000000LL};
    sent.recv_ts = Timestamp{1787888700000000123LL};
    sent.last = Price{2500050};
    sent.last_qty = Qty{75};
    sent.volume = Qty{123456};
    sent.oi = 987654;
    sent.source = FeedSource::Kite;
    sent.flags = set_flag(0, TickFlag::Snapshot);

    check(g_tick_ring.try_push(sent), "a Tick pushes onto the ring");
    Tick got{};
    check(g_tick_ring.try_pop(got), "and pops back");
    check(std::memcmp(&sent, &got, sizeof(Tick)) == 0,
          "byte-identical through the ring -- padding included");

    DepthUpdate d = two_sided(2500000, 2500500);
    d.id = static_cast<InstrumentId>(7);
    d.seq = 4243;
    d.source = FeedSource::Xts;
    check(g_depth_ring.try_push(d), "a DepthUpdate pushes");
    DepthUpdate dg{};
    check(g_depth_ring.try_pop(dg), "and pops back");
    check(std::memcmp(&d, &dg, sizeof(DepthUpdate)) == 0,
          "byte-identical too -- which is what the tick store will rely on");
    check(mid(dg).has_value() && mid(dg)->raw() == 2500250,
          "and it is still a usable book on the far side");
}

} // namespace

int main()
{
    std::printf("altair feed tick tests\n");
    layout_is_pinned();
    no_broker_token_field();
    empty_side_has_no_mid();
    mid_does_not_overflow();
    crossed_and_locked();
    levels_bound_the_reads();
    flags_round_trip();
    rides_a_ring();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
