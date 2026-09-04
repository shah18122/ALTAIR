// desktop/tests/test_depth_ladder.cpp -- P11Q-03.
//
// The ladder's logic is `ladder_view()`, which is pure, so it is tested here
// without a window. What is tested is the four things that would each render a
// plausible, wrong ladder:
//
//   1. zeroed slots drawn as liquidity      (feed/tick.hpp D6)
//   2. a crossed book refused instead of marked  (P2-08 carried debt)
//   3. never-updated shown as empty          (BookError::NotFound D1)
//   4. imbalance of +/-1 from an absent side
//
// Every one of them produces a ladder that looks right.

#include <desktop/depth_ladder.hpp>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-4s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) { ++failures; }
}

/// A book with `nb` bid and `na` ask levels populated, the rest left ZEROED
/// exactly as a decoder leaves them.
altair::BookState make_book(std::size_t nb, std::size_t na,
                            std::int64_t bid_top = 10'000,
                            std::int64_t ask_top = 10'050) {
    altair::BookState b{};
    b.seq = 1;
    for (std::size_t i = 0; i < nb; ++i) {
        b.bid[i].px = altair::Price{bid_top - static_cast<std::int64_t>(i) * 5};
        b.bid[i].qty = altair::Qty{100 + static_cast<std::int64_t>(i) * 10};
        b.bid[i].orders = static_cast<std::uint32_t>(i + 1);
    }
    for (std::size_t i = 0; i < na; ++i) {
        b.ask[i].px = altair::Price{ask_top + static_cast<std::int64_t>(i) * 5};
        b.ask[i].qty = altair::Qty{200 + static_cast<std::int64_t>(i) * 10};
        b.ask[i].orders = static_cast<std::uint32_t>(i + 2);
    }
    b.bid_levels = static_cast<std::uint8_t>(nb);
    b.ask_levels = static_cast<std::uint8_t>(na);
    b.crossed = nb > 0 && na > 0 && !(b.bid[0].px.raw() < b.ask[0].px.raw());
    return b;
}

} // namespace

int main() {
    std::printf("P11Q-03 depth ladder\n");

    // ---- 1. zeroed slots are not liquidity -------------------------------
    {
        const altair::BookState b = make_book(2, 3);
        const auto v = altair::ui::ladder_view(&b);
        std::printf("    2 bid + 3 ask populated of %zu slots each -> %zu "
                    "rows\n", altair::kDepthLevels, v.rows.size());
        check(v.rows.size() == 5,
              "only the POPULATED levels become rows — the loop is bounded by "
              "bid_levels/ask_levels, not by kDepthLevels");
        bool any_zero_price = false;
        for (const auto& r : v.rows) {
            if (r.px.raw() == 0) { any_zero_price = true; }
        }
        check(!any_zero_price,
              "no row carries price 0 — a zeroed slot drawn as a level looks "
              "exactly like liquidity and implies infinite edge (tick.hpp D6)");
    }

    // ---- 2. a crossed book RENDERS, marked --------------------------------
    {
        // Bid 101.00 against ask 100.50: crossed, and legitimate in pre-open.
        const altair::BookState b = make_book(2, 2, 10'100, 10'050);
        const auto v = altair::ui::ladder_view(&b);
        check(v.crossed, "the crossed flag survives into the view");
        check(!v.rows.empty(),
              "and the book STILL DRAWS — refusing one blanks the ladder "
              "through every pre-open auction (P2-08)");
        check(!v.tradable,
              "but it is not tradable, which is a different statement from "
              "not drawable");
    }

    // ---- 3. never updated is not empty ------------------------------------
    {
        const auto none = altair::ui::ladder_view(nullptr);
        check(none.never_updated,
              "a book the feed never delivered is flagged, not shown as empty");
        check(none.rows.empty() && none.bid_total == 0,
              "and carries no rows");

        altair::BookState empty{};
        empty.seq = 7;                    // updated, genuinely nothing resting
        const auto e = altair::ui::ladder_view(&empty);
        check(!e.never_updated,
              "an UPDATED book with no resting orders is NOT never_updated — "
              "one of those is our fault and the other is the market's");
        check(e.rows.empty(), "and it also has no rows, which is why the two "
                              "are indistinguishable without the flag");
    }

    // ---- 4. imbalance with one side empty is UNSET ------------------------
    {
        const altair::BookState both = make_book(3, 3);
        const auto v = altair::ui::ladder_view(&both);
        check(v.imbalance.has_value(),
              "imbalance is computed when both sides have liquidity");

        const altair::BookState bid_only = make_book(3, 0);
        const auto b = altair::ui::ladder_view(&bid_only);
        std::printf("    bid-only book: bid_total %lld, ask_total %lld\n",
                    static_cast<long long>(b.bid_total),
                    static_cast<long long>(b.ask_total));
        check(!b.imbalance.has_value(),
              "with the ask side empty it is UNSET — (bid-ask)/(bid+ask) would "
              "be +1.000, which reads as maximum buying pressure when it is "
              "the absence of a measurement");
    }

    // ---- cumulative counts from the TOUCH ---------------------------------
    {
        const altair::BookState b = make_book(3, 3);
        const auto v = altair::ui::ladder_view(&b);
        // Bids are rows 3..5 (asks come first), best first.
        const altair::ui::LadderRow* first_bid = nullptr;
        const altair::ui::LadderRow* last_bid = nullptr;
        for (const auto& r : v.rows) {
            if (!r.is_ask) {
                if (first_bid == nullptr) { first_bid = &r; }
                last_bid = &r;
            }
        }
        check(first_bid != nullptr && last_bid != nullptr, "bids present");
        if (first_bid != nullptr && last_bid != nullptr) {
            std::printf("    bid cumulative: touch %lld -> deepest %lld "
                        "(total %lld)\n",
                        static_cast<long long>(first_bid->cumulative),
                        static_cast<long long>(last_bid->cumulative),
                        static_cast<long long>(v.bid_total));
            check(first_bid->cumulative == first_bid->qty.raw(),
                  "the touch row's cumulative is its own size");
            check(last_bid->cumulative == v.bid_total,
                  "and the deepest row's equals the side total — the number "
                  "answers \"what would I sweep through\", counted from the "
                  "touch regardless of which way the screen reads");
        }
    }

    // ---- a level count beyond the array is clamped, not walked off --------
    {
        altair::BookState b = make_book(2, 2);
        b.bid_levels = 200;               // a decoder bug, not a real book
        const auto v = altair::ui::ladder_view(&b);
        std::size_t bids = 0;
        for (const auto& r : v.rows) { if (!r.is_ask) { ++bids; } }
        check(bids <= altair::kDepthLevels,
              "a level count past the array is clamped rather than read out "
              "of bounds");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
