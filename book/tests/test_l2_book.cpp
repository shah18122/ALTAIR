// P2-08 acceptance tests for book/l2_book.hpp.
//
// No check description here may contain the substring FAIL.

#include <book/l2_book.hpp>

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <chrono>

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

constexpr Timestamp kTs{1787888700000000000LL};

// 2.13 MB: file scope, never the stack.
L2Book g_book;

/// A depth snapshot. `n_bid`/`n_ask` levels, best bid at `bid_px` descending,
/// best ask at `ask_px` ascending.
DepthUpdate mk(std::uint32_t id, std::uint32_t seq,
               std::int64_t bid_px, std::int64_t ask_px,
               std::size_t n_bid = 1, std::size_t n_ask = 1)
{
    DepthUpdate d{};
    d.id = static_cast<InstrumentId>(id);
    d.seq = seq;
    d.exchange_ts = kTs;
    d.recv_ts = kTs;
    d.source = FeedSource::Kite;
    for (std::size_t k = 0; k < n_bid; ++k) {
        d.bid[k] = DepthLevel{Price{bid_px - static_cast<std::int64_t>(k) * 5},
                              Qty{100 + static_cast<std::int64_t>(k)},
                              static_cast<std::uint32_t>(3 + k), 0};
    }
    for (std::size_t k = 0; k < n_ask; ++k) {
        d.ask[k] = DepthLevel{Price{ask_px + static_cast<std::int64_t>(k) * 5},
                              Qty{200 + static_cast<std::int64_t>(k)},
                              static_cast<std::uint32_t>(4 + k), 0};
    }
    d.bid_levels = static_cast<std::uint8_t>(n_bid);
    d.ask_levels = static_cast<std::uint8_t>(n_ask);
    return d;
}

// ── 1 ────────────────────────────────────────────────────────────────────
// The phantom-default-state bug, designed out rather than found again.
void never_updated_is_not_empty()
{
    std::printf("\n1 never_updated_is_not_empty\n");
    g_book.clear();

    const auto none = g_book.at(static_cast<InstrumentId>(7));
    check(!none.has_value() && none.error() == BookError::NotFound,
          "an instrument never updated is NotFound");
    check(!g_book.has(static_cast<InstrumentId>(7)), "and has() says so");

    // An update carrying NO levels. That is a real observation of an empty
    // book, and it is a different claim from "no observation".
    DepthUpdate empty{};
    empty.id = static_cast<InstrumentId>(7);
    empty.seq = 1;
    empty.exchange_ts = kTs;
    empty.source = FeedSource::Kite;
    check(g_book.apply(empty).has_value(), "an empty-sided update applies");

    const auto seen = g_book.at(static_cast<InstrumentId>(7));
    check(seen.has_value(),
          "and now at() RETURNS a state -- observing an empty book is not the "
          "same as never having looked");
    check(seen.has_value() && (*seen)->bid_levels == 0
              && (*seen)->ask_levels == 0,
          "whose levels are genuinely zero");
    check(seen.has_value() && !is_tradable(**seen),
          "it is not tradable, but for want of liquidity rather than data");

    const auto other = g_book.at(static_cast<InstrumentId>(8));
    check(!other.has_value() && other.error() == BookError::NotFound,
          "while a different instrument is still NotFound -- the two states "
          "stay distinguishable");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// D2, and the case the skeleton got wrong. NSE's pre-open auction collects
// orders WITHOUT matching, so the book legitimately crosses.
void crossed_is_stored_and_marked()
{
    std::printf("\n2 crossed_is_stored_and_marked\n");
    g_book.clear();

    // bid 2500.10 above ask 2500.00 -- exactly what pre-open looks like.
    const auto r = g_book.apply(mk(1, 1, 2'500'010, 2'500'000, 3, 3));
    check(r.has_value(),
          "a crossed book APPLIES -- it is not an error, it is the pre-open");

    const auto b = g_book.at(static_cast<InstrumentId>(1));
    check(b.has_value() && (*b)->crossed, "and is marked crossed");
    check(b.has_value() && !is_tradable(**b),
          "so is_tradable is false and nothing can act on it by accident");
    check(b.has_value() && (*b)->bid_levels == 3 && (*b)->ask_levels == 3,
          "while the LEVELS are still readable -- the measurement was real and "
          "was not thrown away to prevent a downstream bug");
    check(b.has_value() && (*b)->bid[0].px.raw() == 2'500'010,
          "with the actual crossed prices intact");
    check(g_book.stats().crossed == 1, "and counted");

    // Continuous session opens; the book uncrosses.
    check(g_book.apply(mk(1, 2, 2'500'000, 2'500'050)).has_value(),
          "a normal book follows");
    const auto n = g_book.at(static_cast<InstrumentId>(1));
    check(n.has_value() && !(*n)->crossed, "and the flag CLEARS");
    check(n.has_value() && is_tradable(**n), "the book is tradable again");
    check(n.has_value() && mid(**n).has_value() && mid(**n)->raw() == 2'500'025,
          "and mids correctly");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void locked_counts_as_crossed()
{
    std::printf("\n3 locked_counts_as_crossed\n");
    g_book.clear();

    check(g_book.apply(mk(1, 1, 2'500'000, 2'500'000)).has_value(),
          "a LOCKED book (bid == ask) applies");
    const auto b = g_book.at(static_cast<InstrumentId>(1));
    check(b.has_value() && (*b)->crossed,
          "and counts as crossed -- a zero spread is not a free trade, it is "
          "a book that cannot be traded");
    check(b.has_value() && spread(**b).has_value() && spread(**b)->raw() == 0,
          "its spread is exactly zero");
    check(b.has_value() && !is_tradable(**b), "and it is not tradable");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void stale_sequence_rejected_state_survives()
{
    std::printf("\n4 stale_sequence_rejected_state_survives\n");
    g_book.clear();

    check(g_book.apply(mk(1, 10, 2'500'000, 2'500'050, 5, 5)).has_value(),
          "seq 10 applies");
    BookState before = **g_book.at(static_cast<InstrumentId>(1));

    const auto older = g_book.apply(mk(1, 9, 9'999'999, 9'999'999));
    check(!older.has_value() && older.error() == BookError::StaleSequence,
          "seq 9 is rejected as StaleSequence");

    const BookState& after = **g_book.at(static_cast<InstrumentId>(1));
    // The reject counter is expected to differ; compare everything else.
    BookState cmp = after;
    cmp.consecutive_rejects = before.consecutive_rejects;
    check(std::memcmp(&before, &cmp, sizeof(BookState)) == 0,
          "and the stored book is otherwise BYTE-IDENTICAL -- the rejection "
          "cannot have partially written");
    check(after.bid[0].px.raw() == 2'500'000,
          "the good prices are untouched by the bad update");

    const auto same = g_book.apply(mk(1, 10, 1, 2));
    check(!same.has_value() && same.error() == BookError::StaleSequence,
          "an EQUAL seq is rejected too -- strictly greater, or nothing");

    check(g_book.apply(mk(1, 11, 2'500'005, 2'500'055)).has_value(),
          "seq 11 applies");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void consecutive_rejects_tracks_now_not_ever()
{
    std::printf("\n5 consecutive_rejects_tracks_now_not_ever\n");
    g_book.clear();

    (void)g_book.apply(mk(1, 10, 2'500'000, 2'500'050));
    for (int i = 0; i < 3; ++i) {
        (void)g_book.apply(mk(1, 5, 1, 2));
    }
    const auto b = g_book.at(static_cast<InstrumentId>(1));
    check(b.has_value() && (*b)->consecutive_rejects == 3,
          "three rejections in a row raise the per-instrument counter to 3");
    check(g_book.stats().stale_sequence == 3, "and the global count agrees");

    // A second instrument is unaffected: the counter names the broken symbol,
    // which is the whole point of it being per instrument.
    (void)g_book.apply(mk(2, 1, 2'400'000, 2'400'050));
    const auto other = g_book.at(static_cast<InstrumentId>(2));
    check(other.has_value() && (*other)->consecutive_rejects == 0,
          "a healthy instrument's counter stays at zero");

    check(g_book.apply(mk(1, 11, 2'500'005, 2'500'055)).has_value(),
          "one good update arrives");
    const auto c = g_book.at(static_cast<InstrumentId>(1));
    check(c.has_value() && (*c)->consecutive_rejects == 0,
          "and the counter RESETS -- it means 'right now', not 'ever'");
}

// ── 6 ────────────────────────────────────────────────────────────────────
// The whole consequence of snapshot semantics.
void snapshot_replaces_rather_than_merges()
{
    std::printf("\n6 snapshot_replaces_rather_than_merges\n");
    g_book.clear();

    check(g_book.apply(mk(1, 1, 2'500'000, 2'500'050, 5, 5)).has_value(),
          "a five-level book applies");
    const auto full = g_book.at(static_cast<InstrumentId>(1));
    check(full.has_value() && (*full)->bid_levels == 5, "five bid levels");
    check(full.has_value() && (*full)->bid[4].px.raw() == 2'499'980,
          "level 4 holds a real price");

    // Liquidity thins to two levels.
    check(g_book.apply(mk(1, 2, 2'500'000, 2'500'050, 2, 2)).has_value(),
          "a two-level book follows");
    const auto thin = g_book.at(static_cast<InstrumentId>(1));
    check(thin.has_value() && (*thin)->bid_levels == 2, "now two bid levels");
    check(thin.has_value() && (*thin)->bid[4].px.raw() == 0,
          "and level 4 is ZEROED -- the old price does not show through "
          "beneath the shorter book and invent liquidity that is gone");
    check(thin.has_value() && (*thin)->bid[2].px.raw() == 0
              && (*thin)->ask[2].px.raw() == 0,
          "levels 2 and 3 likewise, on both sides");
    check(thin.has_value() && (*thin)->bid[1].px.raw() == 2'499'995,
          "while the two real levels are correct");
}

// ── 7 ────────────────────────────────────────────────────────────────────
void is_tradable_needs_both_sides()
{
    std::printf("\n7 is_tradable_needs_both_sides\n");
    g_book.clear();

    // Bids only.
    DepthUpdate one = mk(1, 1, 2'500'000, 0, 3, 0);
    check(g_book.apply(one).has_value(), "a one-sided book applies");
    const auto b = g_book.at(static_cast<InstrumentId>(1));
    check(b.has_value() && !(*b)->crossed,
          "it is NOT crossed -- there is nothing to cross with");
    check(b.has_value() && !is_tradable(**b),
          "but it is not tradable either: there is nothing to trade against");
    check(b.has_value() && best_ask(**b) == nullptr, "best_ask is nullptr");
    check(b.has_value() && !mid(**b).has_value(), "and there is no mid");

    check(g_book.apply(mk(1, 2, 2'500'000, 2'500'050)).has_value(),
          "a two-sided uncrossed book follows");
    const auto t = g_book.at(static_cast<InstrumentId>(1));
    check(t.has_value() && is_tradable(**t), "and THAT is tradable");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void bad_instrument_and_capacity()
{
    std::printf("\n8 bad_instrument_and_capacity\n");
    g_book.clear();

    DepthUpdate bad = mk(1, 1, 2'500'000, 2'500'050);
    bad.id = InstrumentId::Invalid;
    const auto r = g_book.apply(bad);
    check(!r.has_value() && r.error() == BookError::BadInstrument,
          "InstrumentId::Invalid is BadInstrument, checked before any indexing");

    const auto q = g_book.at(InstrumentId::Invalid);
    check(!q.has_value() && q.error() == BookError::BadInstrument,
          "and at() refuses it the same way");
    check(!g_book.has(InstrumentId::Invalid), "has() says no");
    check(g_book.stats().bad_instrument == 1, "counted");

    // size() counts distinct instruments, not updates.
    for (std::uint32_t id = 0; id < 5; ++id) {
        for (std::uint32_t s = 1; s <= 4; ++s) {
            (void)g_book.apply(mk(id, s, 2'500'000, 2'500'050));
        }
    }
    check(g_book.size() == 5,
          "five instruments after twenty updates -- size() counts distinct "
          "instruments");
    check(g_book.stats().applied == 20, "while applied counts the updates");

    g_book.clear();
    check(g_book.size() == 0 && !g_book.has(static_cast<InstrumentId>(0)),
          "clear() empties it, and NotFound returns");
}

// ── gate 6: the latency budget ───────────────────────────────────────────
void benchmark()
{
    std::printf("\napply latency -- ALTAIR_HOT\n");
    std::printf("  sizeof(BookState) = %zu, sizeof(L2Book) = %.2f MB\n",
                sizeof(BookState),
                static_cast<double>(sizeof(L2Book)) / (1024.0 * 1024.0));
    g_book.clear();

    // 512 instruments, full five-level books -- the worst realistic case.
    static DepthUpdate batch[512];
    for (std::size_t i = 0; i < 512; ++i) {
        batch[i] = mk(static_cast<std::uint32_t>(i), 1,
                      2'500'000, 2'500'050, 5, 5);
    }

    // Batch-timed. A single apply is far below clock resolution and
    // bracketing each call would measure the harness (the P0-03 lesson).
    constexpr int kIters = 4'000;
    const auto t0 = std::chrono::steady_clock::now();
    std::size_t sink = 0;
    std::uint32_t seq = 1;
    for (int it = 0; it < kIters; ++it) {
        ++seq;
        for (std::size_t i = 0; i < 512; ++i) {
            batch[i].seq = seq;
            sink += g_book.apply(batch[i]).has_value() ? 1u : 0u;
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double per_ns = secs * 1e9 / (static_cast<double>(kIters) * 512.0);

    std::printf("  %d x 512 applies in %.3f s -> %.1f ns each\n",
                kIters, secs, per_ns);
    check(sink == static_cast<std::size_t>(kIters) * 512,
          "every apply succeeded -- the loop was not optimised away");
    check(per_ns < 200.0,
          "an apply costs well under 200 ns, so the book is not the "
          "bottleneck beside a 70 ns decode and an 11 ns normalise");
}

} // namespace

int main()
{
    std::printf("altair book l2_book tests\n");
    never_updated_is_not_empty();
    crossed_is_stored_and_marked();
    locked_counts_as_crossed();
    stale_sequence_rejected_state_survives();
    consecutive_rejects_tracks_now_not_ever();
    snapshot_replaces_rather_than_merges();
    is_tradable_needs_both_sides();
    bad_instrument_and_capacity();
    benchmark();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
