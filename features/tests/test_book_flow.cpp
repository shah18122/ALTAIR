// P5-03 acceptance tests for features/book_flow.hpp.
//
// Test 2 is the card: a book with NO LIQUIDITY is not a book with zero
// imbalance. A zeroed BookState reads as balanced, tight and unpressured --
// three confident, tradable-looking numbers -- when what actually happened is
// that no depth arrived.
//
// Test 3 is the other one: a crossed book is DATA, not an error, but its level
// features are not comparable, so they are absent while the crossed flag
// itself is present.
//
// No check description here may contain the substring FAIL.

#include <features/book_flow.hpp>

#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <optional>

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

bool rel_near(double a, double b, double tol)
{
    const double d = std::fabs(a - b);
    const double m = std::fabs(b) > 1.0 ? std::fabs(b) : 1.0;
    return d / m <= tol;
}

} // namespace

using namespace altair;

namespace {

/// A normal two-sided NIFTY book: bid 24,079.50 x 900, ask 24,080.00 x 300.
/// Deliberately lopsided, so imbalance has a sign to get wrong.
BookState normal_book()
{
    BookState b{};
    b.bid[0] = DepthLevel{Price{2'407'950}, Qty{900}, 3};
    b.bid[1] = DepthLevel{Price{2'407'900}, Qty{600}, 2};
    b.ask[0] = DepthLevel{Price{2'408'000}, Qty{300}, 2};
    b.ask[1] = DepthLevel{Price{2'408'050}, Qty{450}, 2};
    b.bid_levels = 2;
    b.ask_levels = 2;
    return b;
}

BookSlots all_slots()
{
    BookSlots s{};
    s.imbalance = 0; s.weighted_imbalance = 1; s.microprice_offset = 2;
    s.spread = 3; s.spread_bps = 4; s.bid_depth = 5; s.ask_depth = 6;
    s.crossed = 7;
    s.depth_levels = 2;      // both visible levels
    return s;
}

// ── 1 ────────────────────────────────────────────────────────────────────
void a_normal_book_fills_the_family()
{
    std::printf("\n1 a_normal_book_fills_the_family\n");
    FeatureVector v{1, 8, Timestamp{0}};
    const auto n = build_book(normal_book(), all_slots(), v);
    check(n.has_value(), "a two-sided book builds");
    if (!n) { return; }
    std::printf("    imbalance %+.4f  spread %.0f p (%.3f bps)"
                "  microprice offset %+.2f p\n",
                v.value(0).value(), v.value(3).value(), v.value(4).value(),
                v.value(2).value());
    check(v.complete(), "every registered slot is filled");
    check(v.value(0).value() > 0.0,
          "imbalance is POSITIVE with 900 bid against 300 ask -- bid-heavy is"
          " a positive number, and the sign is the whole content of it");
    check(rel_near(v.value(3).value(), 50.0, 1e-12),
          "the spread is 50 paise");
    check(v.value(4).value() > 0.2 && v.value(4).value() < 0.21,
          "which is about 0.21 bps of a 24,080 mid -- the form that makes two"
          " instruments at different prices comparable");
    check(v.value(2).value() > 0.0,
          "and the microprice sits ABOVE the mid, because the heavy bid pulls"
          " the size-weighted fair value up");
    check(v.value(7).value() == 0.0, "the crossed flag is a present zero");
    check(rel_near(v.value(5).value(), 1500.0, 1e-12)
          && rel_near(v.value(6).value(), 750.0, 1e-12),
          "and both depths are summed across the visible levels");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// THE card.
void an_empty_book_is_not_a_balanced_book()
{
    std::printf("\n2 an_empty_book_is_not_a_balanced_book\n");
    const BookState empty{};        // exactly what a zeroed struct looks like
    FeatureVector v{1, 8, Timestamp{0}};
    const auto n = build_book(empty, all_slots(), v);
    check(!n && n.error() == BookFeatureError::NoLiquidity,
          "a book with no depth reports NoLiquidity");
    check(v.present_count() == 0,
          "and fills NOTHING -- not one slot");
    check(!v.complete(),
          "so the vector reports itself incomplete, which is the truth");

    for (FeatureIndex i = 0; i < 8; ++i) {
        if (v.has(i)) { check(false, "a slot was written on an empty book"); }
    }
    std::printf("    a zeroed BookState would otherwise read as: imbalance"
                " 0.000 (balanced),\n    spread 0 (infinitely tight), depth 0"
                " -- three tradable-looking numbers\n    describing a book"
                " that never arrived.\n");
    check(v.value(0).error() == VectorError::Absent,
          "reading the imbalance gives Absent, not 0.0 -- and 0.0 is a REAL"
          " imbalance value meaning perfectly balanced, which is why the two"
          " must not collapse together");

    // One-sided is also no book. Half a book prices nothing.
    BookState bid_only{};
    bid_only.bid[0] = DepthLevel{Price{2'407'950}, Qty{900}, 3};
    bid_only.bid_levels = 1;
    const auto one = build_book(bid_only, all_slots(), v);
    check(!one && one.error() == BookFeatureError::NoLiquidity,
          "a book with bids and no asks is also NoLiquidity -- there is no"
          " mid, no spread, and no imbalance to speak of");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void a_crossed_book_is_data_and_its_levels_are_absent()
{
    std::printf("\n3 a_crossed_book_is_data_and_its_levels_are_absent\n");
    // The `crossed` flag is set by L2Book when it APPLIES an update, not
    // recomputed by is_tradable -- so a hand-built state has to set it. That
    // is the right design (a book is crossed as a fact about the update that
    // produced it), and it caught this test constructing an inconsistent
    // struct: bid above ask with crossed still false.
    BookState x = normal_book();
    x.bid[0] = DepthLevel{Price{2'408'100}, Qty{900}, 3};   // bid ABOVE ask
    x.crossed = true;
    check(!is_tradable(x), "the book is crossed and therefore not tradable");

    FeatureVector v{1, 8, Timestamp{0}};
    const auto n = build_book(x, all_slots(), v);
    check(!n && n.error() == BookFeatureError::Crossed,
          "the builder reports Crossed rather than erroring out or pretending");
    check(v.has(7) && v.value(7).value() == 1.0,
          "the CROSSED FLAG is present and set -- it is the one thing that is"
          " still meaningful, and a model seeing the level features absent"
          " needs to know why");
    check(!v.has(0) && !v.has(2) && !v.has(3),
          "while imbalance, microprice and spread are ABSENT: computed on a"
          " crossed book they are not comparable to normal ones, and a model"
          " trained on normal books would read them as an extreme");
    check(v.present_count() == 1,
          "exactly one slot filled -- the flag, and nothing else");
    std::printf("    -> a crossed book happens at the open, in a circuit move,"
                " and on a lagging\n       feed. P2-08 stores and marks it"
                " rather than discarding it; this carries\n       that"
                " distinction into the vector.\n");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void the_flow_family_reports_its_own_sample_counts()
{
    std::printf("\n4 the_flow_family_reports_its_own_sample_counts\n");
    // Stand-in estimators with the same shape as P2-09b's, so this test is
    // about the BUILDER's contract rather than about re-testing VPIN.
    struct FakeVpin {
        struct Reading { double value; std::size_t buckets; };
        std::optional<Reading> r;
        std::optional<Reading> value() const { return r; }
    };
    struct FakeKyle {
        struct Reading { double lambda; std::size_t samples; double r2; };
        std::optional<Reading> r;
        std::optional<Reading> value() const { return r; }
    };

    FlowSlots s{};
    s.vpin = 0; s.vpin_buckets = 1; s.kyle_lambda = 2; s.kyle_r2 = 3;
    s.kyle_samples = 4;

    // Neither estimator ready.
    FeatureVector cold{1, 5, Timestamp{0}};
    check(build_flow(FakeVpin{}, FakeKyle{}, s, cold) == 0,
          "with neither estimator ready, nothing is written");
    check(cold.present_count() == 0,
          "and no slot gets a zero -- VPIN needs a completed volume bucket and"
          " lambda needs a regression, and neither returns a zero to say so");

    // Both ready.
    FakeVpin fv{FakeVpin::Reading{0.42, 51}};
    FakeKyle fk{FakeKyle::Reading{0.0031, 240, 0.66}};
    FeatureVector warm{1, 5, Timestamp{0}};
    const int w = build_flow(fv, fk, s, warm);
    std::printf("    vpin %.2f over %.0f buckets; lambda %.4f, r2 %.2f over"
                " %.0f trades\n",
                warm.value(0).value(), warm.value(1).value(),
                warm.value(2).value(), warm.value(3).value(),
                warm.value(4).value());
    check(w == 5 && warm.complete(), "with both ready all five slots fill");
    check(warm.value(4).value() == 240.0 && warm.value(1).value() == 51.0,
          "the SAMPLE COUNTS travel with the estimates -- P2-09b refuses to"
          " report a lambda without its sample size, and a feature set that"
          " dropped it would undo that");

    // One ready, one not: partial, and the absence is on the right slots.
    FeatureVector half{1, 5, Timestamp{0}};
    check(build_flow(fv, FakeKyle{}, s, half) == 2,
          "with only VPIN ready, exactly its two slots fill");
    check(half.has(0) && half.has(1) && !half.has(2) && !half.has(4),
          "and the Kyle slots stay absent rather than taking a stale value");
}

} // namespace

int main()
{
    std::printf("altair features book/flow tests\n");
    a_normal_book_fills_the_family();
    an_empty_book_is_not_a_balanced_book();
    a_crossed_book_is_data_and_its_levels_are_absent();
    the_flow_family_reports_its_own_sample_counts();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
