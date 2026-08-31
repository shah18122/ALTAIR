// P2-09a acceptance tests for book/microstructure.hpp.
//
// No check description here may contain the substring FAIL.

#include <book/microstructure.hpp>

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cmath>

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

bool near(double a, double b, double tol = 1e-12)
{
    return std::fabs(a - b) <= tol;
}

} // namespace

using namespace altair;

namespace {

constexpr Timestamp kTs{1787888700000000000LL};

/// A book built directly, so a test can state exactly the shape it needs.
BookState book(std::int64_t bid_px, std::int64_t bid_qty,
               std::int64_t ask_px, std::int64_t ask_qty)
{
    BookState b{};
    b.id = static_cast<InstrumentId>(1);
    b.seq = 1;
    b.exchange_ts = kTs;
    b.recv_ts = kTs;
    b.bid[0] = DepthLevel{Price{bid_px}, Qty{bid_qty}, 1, 0};
    b.ask[0] = DepthLevel{Price{ask_px}, Qty{ask_qty}, 1, 0};
    b.bid_levels = 1;
    b.ask_levels = 1;
    b.crossed = bid_px >= ask_px;
    return b;
}

/// Multi-level, with per-level quantities supplied.
BookState deep(const std::int64_t* bq, std::uint8_t nb,
               const std::int64_t* aq, std::uint8_t na)
{
    BookState b{};
    b.id = static_cast<InstrumentId>(1);
    b.seq = 1;
    for (std::uint8_t k = 0; k < nb; ++k) {
        b.bid[k] = DepthLevel{Price{2'500'000 - std::int64_t{k} * 5},
                              Qty{bq[k]}, 1, 0};
    }
    for (std::uint8_t k = 0; k < na; ++k) {
        b.ask[k] = DepthLevel{Price{2'500'050 + std::int64_t{k} * 5},
                              Qty{aq[k]}, 1, 0};
    }
    b.bid_levels = nb;
    b.ask_levels = na;
    b.crossed = false;
    return b;
}

// ── 1 ────────────────────────────────────────────────────────────────────
// The handoff from P2-08. Crossed books are STORED there; they must not be
// computed on here.
void untradable_books_yield_nothing()
{
    std::printf("\n1 untradable_books_yield_nothing\n");

    BookState crossed = book(2'500'100, 500, 2'500'000, 500);
    check(crossed.crossed, "the book is crossed -- as a pre-open book is");
    check(crossed.bid[0].px.raw() == 2'500'100 && crossed.bid[0].qty.raw() == 500,
          "and its levels are READABLE -- the measurement was kept");
    check(!obi(crossed).has_value(), "but obi declines to use it");
    check(!weighted_obi(crossed).has_value(), "and weighted_obi");
    check(!microprice(crossed).has_value(),
          "and microprice -- arithmetic on a state that cannot be traded is "
          "the free-money signal Phase 5 exists to disprove");

    BookState locked = book(2'500'000, 500, 2'500'000, 500);
    check(!microprice(locked).has_value(), "a LOCKED book yields nothing too");

    BookState one_sided = book(2'500'000, 500, 2'500'050, 500);
    one_sided.ask_levels = 0;
    check(!is_tradable(one_sided), "a one-sided book is not tradable");
    check(!obi(one_sided).has_value() && !microprice(one_sided).has_value(),
          "and yields nothing, for want of a side rather than a crossing");

    BookState good = book(2'500'000, 500, 2'500'050, 500);
    check(microprice(good).has_value(), "a normal book does produce a value");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// D1. The trap: the swapped formula still lands between bid and ask.
void microprice_weights_are_crossed()
{
    std::printf("\n2 microprice_weights_are_crossed\n");

    // Heavy on the BID: 900 vs 100. The next trade is more likely to lift the
    // offer, so fair value should sit nearer the ASK.
    const std::int64_t bp = 2'500'000, bq = 900;
    const std::int64_t ap = 2'500'100, aq = 100;
    BookState b = book(bp, bq, ap, aq);

    const auto mp = microprice(b);
    check(mp.has_value(), "microprice computes");
    const std::int64_t mid_px = bp + (ap - bp) / 2;

    check(mp.has_value() && mp->raw() > mid_px,
          "with heavy size on the BID it sits ABOVE the mid, nearer the ask");

    // The correct value, computed here independently.
    const std::int64_t correct = (bp * aq + ap * bq) / (bq + aq);
    check(mp.has_value() && mp->raw() == correct,
          "matching (bid_px * ask_qty + ask_px * bid_qty) / total exactly");

    // What the swapped weighting would have given. It is still between bid and
    // ask -- which is precisely why the mistake is invisible.
    const std::int64_t swapped = (bp * bq + ap * aq) / (bq + aq);
    check(swapped >= bp && swapped <= ap,
          "the SWAPPED formula also lands between bid and ask -- it looks "
          "entirely plausible");
    check(swapped < mid_px,
          "but it leans the WRONG WAY, below the mid, and a model built on it "
          "would learn the inverse of the truth");
    check(mp->raw() != swapped, "the two differ, so the test can tell them apart");

    // And the mirror case: heavy on the ask pushes fair value down.
    BookState c = book(bp, 100, ap, 900);
    check(microprice(c)->raw() < mid_px,
          "heavy size on the ASK puts it below the mid");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void microprice_is_within_the_touch()
{
    std::printf("\n3 microprice_is_within_the_touch\n");

    const std::int64_t bp = 2'500'000, ap = 2'500'100;
    int checked = 0;
    int outside = 0;
    for (std::int64_t bq = 1; bq <= 1000; bq += 37) {
        for (std::int64_t aq = 1; aq <= 1000; aq += 41) {
            const auto mp = microprice(book(bp, bq, ap, aq));
            if (!mp) {
                continue;
            }
            ++checked;
            if (mp->raw() < bp || mp->raw() > ap) {
                ++outside;
            }
        }
    }
    check(checked > 500, "a wide grid of size combinations was evaluated");
    check(outside == 0,
          "and every microprice lies within [bid, ask] -- it is a convex "
          "combination, so anything outside would be a bug by construction");

    const auto eq = microprice(book(bp, 500, ap, 500));
    check(eq.has_value() && eq->raw() == bp + (ap - bp) / 2,
          "equal sizes put it exactly at the mid");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void microprice_is_exact_and_integer()
{
    std::printf("\n4 microprice_is_exact_and_integer\n");

    // (100 * 2 + 200 * 1) / 3 = 400/3 = 133.33 -> truncates to 133.
    const auto t = microprice(book(100, 1, 200, 2));
    check(t.has_value() && t->raw() == 133,
          "a fractional true value truncates deterministically, with no double "
          "anywhere in the path");

    // Overflow: a price near the top of int64 times a real quantity.
    const std::int64_t huge = std::numeric_limits<std::int64_t>::max() / 2;
    const auto o = microprice(book(huge, 1000, huge + 100, 1000));
    check(!o.has_value(),
          "a product that would overflow int64 returns EMPTY rather than a "
          "wrapped number that would look like a plausible price");

    // Just inside the range still works.
    const auto ok = microprice(book(1'000'000'000LL, 10, 1'000'000'100LL, 10));
    check(ok.has_value(), "a large but representable case still computes");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void obi_range_and_sign()
{
    std::printf("\n5 obi_range_and_sign\n");

    const auto all_bid = obi(book(2'500'000, 1000, 2'500'050, 0));
    check(all_bid.has_value() && near(*all_bid, 1.0),
          "all size on the bid gives +1");
    const auto all_ask = obi(book(2'500'000, 0, 2'500'050, 1000));
    check(all_ask.has_value() && near(*all_ask, -1.0),
          "all size on the ask gives -1");
    const auto balanced = obi(book(2'500'000, 500, 2'500'050, 500));
    check(balanced.has_value() && near(*balanced, 0.0),
          "equal size gives 0");

    const auto lean = obi(book(2'500'000, 750, 2'500'050, 250));
    check(lean.has_value() && near(*lean, 0.5),
          "750 against 250 gives +0.5");

    bool in_range = true;
    for (std::int64_t bq = 0; bq <= 1000; bq += 13) {
        for (std::int64_t aq = 0; aq <= 1000; aq += 17) {
            const auto v = obi(book(2'500'000, bq, 2'500'050, aq));
            if (v && (*v < -1.0 || *v > 1.0)) {
                in_range = false;
            }
        }
    }
    check(in_range, "and every result across a grid is within [-1, +1]");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void depth_does_not_imagine_levels()
{
    std::printf("\n6 depth_does_not_imagine_levels\n");

    const std::int64_t bq[2] = {100, 200};
    const std::int64_t aq[2] = {300, 400};
    BookState b = deep(bq, 2, aq, 2);

    check(b.bid[4].px.raw() == 0 && b.bid[4].qty.raw() == 0,
          "levels beyond the populated two are zeroed");

    const auto d = bid_depth(b, 5);
    check(d.has_value() && d->levels_used == 2,
          "asking for 5 levels reports levels_used == 2");
    check(d.has_value() && d->qty.raw() == 300,
          "and sums only the two that exist -- the zeroed slots are not "
          "counted as zero-size liquidity, and nothing is scaled up to pretend");

    const auto one = bid_depth(b, 1);
    check(one.has_value() && one->qty.raw() == 100 && one->levels_used == 1,
          "asking for 1 gives the touch alone");

    check(!bid_depth(b, 0).has_value(),
          "asking for 0 levels is empty, not a division by zero");
}

// ── 7 ────────────────────────────────────────────────────────────────────
// The reason both functions exist.
void weighted_obi_favours_the_touch()
{
    std::printf("\n7 weighted_obi_favours_the_touch\n");

    // Same TOTAL size on each side in both books; only the depth at which the
    // bid's extra size sits differs.
    const std::int64_t near_bq[3] = {300, 100, 100};   // heavy at level 0
    const std::int64_t far_bq[3]  = {100, 100, 300};   // heavy at level 2
    const std::int64_t aq[3]      = {100, 100, 100};

    BookState near_book = deep(near_bq, 3, aq, 3);
    BookState far_book  = deep(far_bq, 3, aq, 3);

    const auto u_near = obi(near_book, 3);
    const auto u_far  = obi(far_book, 3);
    check(u_near.has_value() && u_far.has_value(), "both give an unweighted OBI");
    check(near(*u_near, *u_far),
          "which is IDENTICAL for the two books -- total size is the same, and "
          "unweighted OBI cannot tell them apart");

    const auto w_near = weighted_obi(near_book, 3);
    const auto w_far  = weighted_obi(far_book, 3);
    check(w_near.has_value() && w_far.has_value(), "both give a weighted OBI");
    check(*w_near > *w_far,
          "but the weighted one is LARGER when the extra size sits at the "
          "touch -- which is the whole reason both functions exist");
    check(*w_near > 0.0 && *w_far > 0.0 && *w_near <= 1.0 && *w_far <= 1.0,
          "and both stay in range");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void zero_size_is_empty_not_balanced()
{
    std::printf("\n8 zero_size_is_empty_not_balanced\n");

    BookState b = book(2'500'000, 0, 2'500'050, 0);
    check(is_tradable(b),
          "a book with levels present but no size is still 'tradable' by the "
          "book's own test -- the levels exist");
    check(!obi(b).has_value(),
          "but obi is EMPTY rather than 0.0: perfectly balanced is a strong "
          "claim, and the truth here is that there is no information");
    check(!microprice(b).has_value(), "and microprice likewise");
    check(!weighted_obi(b).has_value(), "and weighted_obi");
}

} // namespace

int main()
{
    std::printf("altair book microstructure tests\n");
    untradable_books_yield_nothing();
    microprice_weights_are_crossed();
    microprice_is_within_the_touch();
    microprice_is_exact_and_integer();
    obi_range_and_sign();
    depth_does_not_imagine_levels();
    weighted_obi_favours_the_touch();
    zero_size_is_empty_not_balanced();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
