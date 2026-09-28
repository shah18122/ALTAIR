// P4-08 acceptance tests for oms/reconcile.hpp.
//
// The asymmetry is the card: an orphan trips the kill switch and a ghost does
// not, because only one of them means something is trading with nobody
// watching it. Test 2 asserts that difference directly.
//
// Test 5 is the one that would otherwise bite in production: an unsorted input
// silently produces a reconciliation full of phantom orphans -- the most
// alarming possible output and the least true -- so it is refused.
//
// No check description here may contain the substring FAIL.

#include <oms/reconcile.hpp>

#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <limits>

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

constexpr std::int64_t kMs = 1'000'000;
const Timestamp kNow{500'000 * kMs};

PositionView pv(std::uint64_t key, std::int64_t qty, std::int64_t px,
                ReconProduct product = ReconProduct::Nrml)
{
    PositionView v{};
    v.key = key;
    v.qty = Qty{qty};
    v.avg_price = Price{px};
    v.present = true;
    v.product = product;
    return v;
}

// ── 1 ────────────────────────────────────────────────────────────────────
void agreeing_books_reconcile_clean()
{
    std::printf("\n1 agreeing_books_reconcile_clean\n");
    const PositionView b[3] = {pv(10, 150, 2'408'000), pv(20, -300, 290'000),
                               pv(30, 75, 4'500'000)};
    const PositionView l[3] = {pv(10, 150, 2'408'000), pv(20, -300, 290'000),
                               pv(30, 75, 4'500'000)};
    const auto r = reconcile(b, 3, l, 3);
    check(r.has_value(), "identical books reconcile");
    if (!r) { return; }
    check(r->matched == 3 && r->line_count == 0,
          "all three lines match and nothing is reported");
    check(r->clean() && !r->has_unmanaged(),
          "the result is clean and nothing is unmanaged");

    // Empty on both sides is a normal state, not an error.
    const auto empty = reconcile(nullptr, 0, nullptr, 0);
    check(empty && empty->clean() && empty->matched == 0,
          "two flat books reconcile clean -- an empty account is a normal"
          " state, not an exception");

    // A price difference INSIDE tolerance is a match, because brokers round
    // average prices their own way.
    const PositionView b2[1] = {pv(10, 150, 2'408'003)};
    const PositionView l2[1] = {pv(10, 150, 2'408'000)};
    const auto near = reconcile(b2, 1, l2, 1);
    check(near && near->matched == 1 && near->price_divergences == 0,
          "a 3-paise average-price difference is within tolerance and matches"
          " -- blocking on broker rounding would block every session");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// THE asymmetry.
void an_orphan_trips_the_switch_and_a_ghost_does_not()
{
    std::printf("\n2 an_orphan_trips_the_switch_and_a_ghost_does_not\n");

    // ORPHAN: the broker holds 150 that Altair knows nothing about.
    {
        const PositionView b[2] = {pv(10, 150, 2'408'000), pv(20, 75, 290'000)};
        const PositionView l[1] = {pv(20, 75, 290'000)};
        const auto r = reconcile(b, 2, l, 1);
        check(r.has_value(), "the orphan case reconciles");
        if (!r) { return; }
        std::printf("    broker holds key 10 (%lld units); Altair does not"
                    " -> %s\n",
                    static_cast<long long>(r->lines[0].broker_qty.raw()),
                    describe(r->lines[0].what));
        check(r->orphans == 1 && r->ghosts == 0,
              "it is reported as an orphan");
        check(r->has_unmanaged(),
              "and flagged as unmanaged -- it has no stop, no square-off"
              " clock, and no size in any limit check");

        KillSwitch k;
        check(enforce_reconciliation(*r, k, kNow) == 1,
              "enforcement returns the orphan count");
        check(k.tripped() && k.reason() == KillReason::ReconciliationMismatch,
              "AN ORPHAN TRIPS THE KILL SWITCH -- something is on the book"
              " with nobody watching it");
    }

    // GHOST: Altair believes in something the broker does not have.
    {
        const PositionView b[1] = {pv(20, 75, 290'000)};
        const PositionView l[2] = {pv(10, 150, 2'408'000), pv(20, 75, 290'000)};
        const auto r = reconcile(b, 1, l, 2);
        check(r.has_value(), "the ghost case reconciles");
        if (!r) { return; }
        check(r->ghosts == 1 && r->orphans == 0, "it is reported as a ghost");
        check(!r->clean(), "the result is not clean");
        check(!r->has_unmanaged(),
              "but nothing is UNMANAGED -- a ghost is a position Altair will"
              " try to manage and cannot, which is loud and self-announcing");

        KillSwitch k;
        check(enforce_reconciliation(*r, k, kNow) == 0,
              "no orphans to report");
        check(!k.tripped(),
              "and a ghost does NOT trip the switch -- tripping on everything"
              " makes the switch routine, and a switch that trips routinely"
              " gets reset routinely");
    }
}

// ── 3 ────────────────────────────────────────────────────────────────────
void quantity_is_exact_and_price_is_not()
{
    std::printf("\n3 quantity_is_exact_and_price_is_not\n");
    // One unit apart. No tolerance: two systems counting the same shares must
    // agree to the share.
    const PositionView b[1] = {pv(10, 150, 2'408'000)};
    const PositionView l[1] = {pv(10, 149, 2'408'000)};
    const auto r = reconcile(b, 1, l, 1);
    check(r.has_value(), "the mismatch reconciles");
    if (!r) { return; }
    check(r->qty_mismatches == 1 && r->matched == 0,
          "a ONE UNIT difference is a mismatch -- a tolerance here would be a"
          " tolerance on how wrong the position is allowed to be");
    check(r->lines[0].broker_qty.raw() == 150
          && r->lines[0].local_qty.raw() == 149,
          "and both quantities are reported, so nobody has to guess which"
          " side to believe");
    check(!r->clean() && !r->has_unmanaged(),
          "it blocks, but nothing is unmanaged");

    // A LARGE price gap on an agreeing quantity is a real finding.
    const PositionView b2[1] = {pv(10, 150, 2'420'000)};
    const PositionView l2[1] = {pv(10, 150, 2'408'000)};
    const auto d = reconcile(b2, 1, l2, 1);
    check(d && d->price_divergences == 1,
          "a Rs 120 average-price gap on an agreeing quantity IS reported --"
          " it means the two systems disagree about which fills happened");
    check(d && d->lines[0].price_gap == 12'000,
          "with the signed gap carried, broker minus local");
    check(d && d->clean(),
          "though it does not block: quantity is what a limit checks against");

    // A price gap on a line whose QUANTITY already disagrees is not reported
    // twice -- it is a consequence, not a second finding.
    const PositionView b3[1] = {pv(10, 150, 2'420'000)};
    const PositionView l3[1] = {pv(10, 100, 2'408'000)};
    const auto both = reconcile(b3, 1, l3, 1);
    check(both && both->qty_mismatches == 1 && both->price_divergences == 0,
          "a price gap on an already-mismatched quantity is not counted"
          " separately -- it is a consequence, and reporting both doubles the"
          " noise on the same underlying problem");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void a_flat_line_is_not_an_orphan()
{
    std::printf("\n4 a_flat_line_is_not_an_orphan\n");
    // The broker reports a line at zero -- squared off earlier today, still
    // listed. Altair carries no line at all. Nothing is on the book.
    const PositionView b[2] = {pv(10, 0, 2'408'000), pv(20, 75, 290'000)};
    const PositionView l[1] = {pv(20, 75, 290'000)};
    const auto r = reconcile(b, 2, l, 1);
    check(r.has_value(), "it reconciles");
    if (!r) { return; }
    check(r->orphans == 0,
          "a broker line at ZERO quantity is not an orphan -- there is nothing"
          " on the book to manage, and reporting it would trip the kill switch"
          " every day after the first square-off");
    check(r->clean(), "and the reconciliation is clean");

    // Same in the other direction.
    const PositionView b2[1] = {pv(20, 75, 290'000)};
    const PositionView l2[2] = {pv(10, 0, 0), pv(20, 75, 290'000)};
    const auto g = reconcile(b2, 1, l2, 2);
    check(g && g->ghosts == 0 && g->clean(),
          "and a local line at zero is not a ghost");

    // But a NON-zero broker line absolutely is.
    const PositionView b3[1] = {pv(10, -75, 2'408'000)};
    const auto o = reconcile(b3, 1, nullptr, 0);
    check(o && o->orphans == 1,
          "while a SHORT orphan is still an orphan -- the sign does not make"
          " it someone else's problem");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void unsorted_or_duplicated_input_is_refused()
{
    std::printf("\n5 unsorted_or_duplicated_input_is_refused\n");
    const PositionView bad[3] = {pv(30, 75, 100), pv(10, 75, 100),
                                 pv(20, 75, 100)};
    const PositionView ok[1] = {pv(10, 75, 100)};
    const auto r = reconcile(bad, 3, ok, 1);
    check(!r && r.error() == ReconError::Unsorted,
          "an unsorted input is REFUSED -- the merge would otherwise produce"
          " a reconciliation full of phantom orphans, which is the most"
          " alarming possible output and the least true");

    const PositionView dup[2] = {pv(10, 75, 100), pv(10, 75, 100)};
    const auto d = reconcile(dup, 2, ok, 1);
    check(!d && d.error() == ReconError::DuplicateKey,
          "and a duplicate key is refused -- two lines for one instrument"
          " would silently double a position");

    const auto d2 = reconcile(ok, 1, dup, 2);
    check(!d2 && d2.error() == ReconError::DuplicateKey,
          "on either side");

    // The sorted equivalent works, so the refusals above are about order and
    // not about the data.
    const PositionView sorted[3] = {pv(10, 75, 100), pv(20, 75, 100),
                                    pv(30, 75, 100)};
    check(reconcile(sorted, 3, ok, 1).has_value(),
          "the same three lines in order reconcile fine");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void many_discrepancies_are_reported_and_truncation_is_declared()
{
    std::printf("\n6 many_discrepancies_are_reported_and_truncation_is_declared\n");
    static PositionView big[kMaxReconLines + 40];
    for (std::size_t i = 0; i < kMaxReconLines + 40; ++i) {
        big[i] = pv(static_cast<std::uint64_t>(i + 1), 75, 100);
    }
    const auto r = reconcile(big, kMaxReconLines + 40, nullptr, 0);
    check(r.has_value(), "a large discrepancy set reconciles");
    if (!r) { return; }
    std::printf("    %d orphans found, %d lines recorded, truncated=%s\n",
                r->orphans, r->line_count, r->truncated ? "yes" : "no");
    check(r->orphans == kMaxReconLines + 40,
          "every orphan is COUNTED, even past the line buffer -- the count is"
          " what decides whether the switch trips");
    check(r->line_count == kMaxReconLines && r->truncated,
          "the detail is capped and truncation is DECLARED, rather than the"
          " remainder being silently dropped");

    KillSwitch k;
    check(enforce_reconciliation(*r, k, kNow) == kMaxReconLines + 40
          && k.tripped(),
          "and the switch trips on the full count, not the truncated one");
}

// ── 7 ────────────────────────────────────────────────────────────────────
void reconcile_qty_mismatch_trips_kill()
{
    std::printf("\n7 reconcile_qty_mismatch_trips_kill\n");
    const PositionView broker[1] = {pv(10, 150, 2'408'000)};
    const PositionView local[1] = {pv(10, 149, 2'408'000)};
    const auto mismatch = reconcile(broker, 1, local, 1);
    check(mismatch && mismatch->qty_mismatches == 1,
          "a one-unit difference is a quantity mismatch");
    if (!mismatch) { return; }
    KillSwitch ordinary;
    check(enforce_reconciliation(*mismatch, ordinary, kNow) == 0
              && ordinary.tripped(),
          "any exact quantity mismatch trips the kill switch, even without an orphan");

    const PositionView short_broker[1] = {pv(11, -75, 2'408'000)};
    const PositionView long_local[1] = {pv(11, 75, 2'408'000)};
    const auto sign_flip = reconcile(short_broker, 1, long_local, 1);
    check(sign_flip && sign_flip->qty_mismatches == 1
              && sign_flip->lines[0].broker_qty.raw() == -75
              && sign_flip->lines[0].local_qty.raw() == 75,
          "opposite nonzero positions are explicitly represented as a sign-flip mismatch");
    if (sign_flip) {
        KillSwitch flipped;
        check(flipped.tripped() == false,
              "a fresh switch begins clear before sign-flip enforcement");
        (void)enforce_reconciliation(*sign_flip, flipped, kNow);
        check(flipped.tripped(), "a sign flip trips the kill switch");
    }
}

void mis_and_nrml_same_token_do_not_collide()
{
    std::printf("\n8 mis_and_nrml_same_token_do_not_collide\n");
    constexpr std::uint64_t token = 260105;
    const PositionView broker[2] = {
        pv(token, 75, 2'408'000, ReconProduct::Mis),
        pv(token, 150, 2'408'000, ReconProduct::Nrml)};
    const PositionView local[1] = {
        pv(token, 75, 2'408'000, ReconProduct::Mis)};
    const auto r = reconcile(broker, 2, local, 1);
    check(r && r->matched == 1 && r->orphans == 1,
          "the shared token matches MIS only; the distinct NRML line remains an orphan");
    check(r && r->line_count == 1 && r->lines[0].key == token
              && r->lines[0].product == ReconProduct::Nrml,
          "the composite identity is preserved in the reported discrepancy");
    const PositionView unknown[1] = {pv(token, 75, 2'408'000,
                                       ReconProduct::Unknown)};
    const auto unknown_product = reconcile(unknown, 1, local, 1);
    check(!unknown_product && unknown_product.error() == ReconError::InvalidProduct,
          "an absent product is refused rather than silently collapsing identities");
}

void extreme_price_gaps_do_not_overflow()
{
    std::printf("\n9 extreme_price_gaps_do_not_overflow\n");
    constexpr auto hi = std::numeric_limits<std::int64_t>::max();
    constexpr auto lo = std::numeric_limits<std::int64_t>::min();
    const PositionView positive_broker[1] = {pv(1, 1, hi)};
    const PositionView positive_local[1] = {pv(1, 1, lo)};
    const auto positive = reconcile(positive_broker, 1, positive_local, 1);
    check(positive && positive->price_divergences == 1
              && positive->lines[0].price_gap == hi,
          "a positive mathematical gap wider than int64 is detected and saturated");

    const PositionView negative_broker[1] = {pv(1, 1, lo)};
    const PositionView negative_local[1] = {pv(1, 1, hi)};
    const auto negative = reconcile(negative_broker, 1, negative_local, 1);
    check(negative && negative->price_divergences == 1
              && negative->lines[0].price_gap == lo,
          "a negative mathematical gap wider than int64 is detected and saturated");

    const auto bad_tolerance = reconcile(positive_broker, 1, positive_local, 1, -1);
    check(!bad_tolerance && bad_tolerance.error() == ReconError::InvalidTolerance,
          "negative price tolerance is rejected instead of changing comparison semantics");
}

void malformed_views_and_counts_are_refused()
{
    std::printf("\n10 malformed_views_and_counts_are_refused\n");
    const auto null_input = reconcile(nullptr, 1, nullptr, 0);
    check(!null_input && null_input.error() == ReconError::NullInput,
          "a nonzero row count cannot be paired with a null input pointer");

    PositionView absent = pv(1, 0, 0);
    absent.present = false;
    const auto absent_row = reconcile(&absent, 1, nullptr, 0);
    check(!absent_row && absent_row.error() == ReconError::InvalidPresence,
          "a counted row marked absent is rejected instead of silently compared");

    constexpr std::size_t kIntMax =
        static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (kIntMax < std::numeric_limits<std::size_t>::max()) {
        const PositionView one[1] = {pv(1, 0, 0)};
        const auto too_many = reconcile(one, kIntMax + 1u, nullptr, 0);
        check(!too_many && too_many.error() == ReconError::TooManyRows,
              "a count beyond the signed result-counter capacity is refused before reading");
    }
}

} // namespace

int main()
{
    std::printf("altair oms reconcile tests\n");
    agreeing_books_reconcile_clean();
    an_orphan_trips_the_switch_and_a_ghost_does_not();
    quantity_is_exact_and_price_is_not();
    a_flat_line_is_not_an_orphan();
    unsorted_or_duplicated_input_is_refused();
    many_discrepancies_are_reported_and_truncation_is_declared();
    reconcile_qty_mismatch_trips_kill();
    mis_and_nrml_same_token_do_not_collide();
    extreme_price_gaps_do_not_overflow();
    malformed_views_and_counts_are_refused();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
