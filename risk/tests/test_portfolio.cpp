// P4-03 acceptance tests for risk/portfolio.hpp.
//
// The load-bearing test is 1: a two-underlying book is repriced EXACTLY by
// black76 after a 1% move, and the cash greeks are checked against what
// actually happened. The raw sum of deltas is computed alongside and does not
// even have the right units, let alone the right answer.
//
// Test 3 is the other one that matters: a calendar spread with total vega of
// zero that is not remotely flat.
//
// No check description here may contain the substring FAIL.

#include <risk/portfolio.hpp>

#include <cstdio>
#include <cstdint>
#include <cstddef>
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

bool rel_near(double a, double b, double tol)
{
    const double d = std::fabs(a - b);
    const double m = std::fabs(b) > 1.0 ? std::fabs(b) : 1.0;
    return d / m <= tol;
}

} // namespace

using namespace altair;

namespace {

constexpr double kRate = 0.065;
constexpr double kT = 30.0 / 365.0;

// Two underlyings with very different prices and lot sizes -- which is the
// whole point, because it is what makes raw deltas non-additive.
//   NIFTY    Rs 24,080, lot 75
//   RELIANCE Rs  2,900, lot 250
constexpr double kNiftyS = 2'408'000.0;
constexpr std::int64_t kNiftyLot = 75;
constexpr double kRelS = 290'000.0;
constexpr std::int64_t kRelLot = 250;

/// Build a line from a live option, with greeks priced at `S`.
PositionLine make_line(std::int64_t lots, double S, std::int64_t lot,
                       double K, double vol, std::uint16_t uid,
                       std::uint8_t bucket, std::uint16_t sector,
                       OptionRight right = OptionRight::Call, double t = kT)
{
    PositionLine p{};
    p.lots = Lots{lots};
    p.lot_size = LotSize{lot};
    p.underlying = Price{static_cast<std::int64_t>(S)};
    p.greeks = detail::black76_unchecked(right, S, K, t, vol, kRate);
    p.higher = detail::black76_higher_unchecked(right, S, K, t, vol, kRate);
    p.underlying_id = uid;
    p.expiry_bucket = bucket;
    p.sector_id = sector;
    return p;
}

/// Reprice a book exactly, by re-running black76 at new inputs.
double book_value(double nifty_S, double rel_S, double dvol, double dt,
                  bool short_rel_put)
{
    const double n = detail::black76_unchecked(
        OptionRight::Call, nifty_S, 2'400'000.0, kT - dt, 0.15 + dvol,
        kRate).price * 2.0 * kNiftyLot;
    const double r = detail::black76_unchecked(
        OptionRight::Put, rel_S, 300'000.0, kT - dt, 0.28 + dvol,
        kRate).price * (short_rel_put ? -4.0 : 4.0) * kRelLot;
    return n + r;
}

// ── 1 ────────────────────────────────────────────────────────────────────
// Cash greeks predict a real reprice. Raw delta sums do not.
void cash_greeks_predict_a_reprice_and_raw_sums_do_not()
{
    std::printf("\n1 cash_greeks_predict_a_reprice_and_raw_sums_do_not\n");
    // Long 2 NIFTY 24000 calls, short 4 RELIANCE 3000 puts.
    PositionLine book[2] = {
        make_line(2, kNiftyS, kNiftyLot, 2'400'000.0, 0.15, 1, 0, 1),
        make_line(-4, kRelS, kRelLot, 300'000.0, 0.28, 2, 0, 2,
                  OptionRight::Put),
    };
    const auto g = aggregate(book, 2);
    check(g.has_value(), "the book aggregates");
    if (!g) { return; }

    const double raw_delta_sum = book[0].greeks.delta + book[1].greeks.delta;
    std::printf("    raw delta sum across two underlyings: %+.6f"
                "  (dimensionless, predicts nothing)\n", raw_delta_sum);
    std::printf("    cash delta  %+15.2f paise per 1%% move\n",
                static_cast<double>(g->cash_delta));
    std::printf("    cash gamma  %+15.2f    cash vega %+13.2f/vol pt\n",
                static_cast<double>(g->cash_gamma),
                static_cast<double>(g->cash_vega));
    std::printf("    cash theta  %+15.2f per day\n",
                static_cast<double>(g->cash_theta));

    // Move BOTH underlyings up 1% and reprice exactly.
    const double before = book_value(kNiftyS, kRelS, 0.0, 0.0, true);
    const double after = book_value(kNiftyS * 1.01, kRelS * 1.01, 0.0, 0.0,
                                    true);
    const double actual = after - before;
    const double predicted = predicted_pnl(*g, 1.0, 0.0, 0.0);
    std::printf("    actual reprice %+.2f paise, greeks predicted %+.2f"
                "  (%.2f%% error)\n",
                actual, predicted, 100.0 * (predicted - actual) / actual);
    check(rel_near(predicted, actual, 0.02),
          "delta and gamma together predict the 1% reprice to 2%");
    check((predicted > 0.0) == (actual > 0.0),
          "and get the direction right");

    // The raw sum has the wrong SIGN here, which is the cleanest possible
    // demonstration that it is not an exposure.
    std::printf("    raw sum sign %+d, actual P&L sign %+d\n",
                raw_delta_sum > 0 ? 1 : -1, actual > 0 ? 1 : -1);
    check((raw_delta_sum > 0.0) != (actual > 0.0),
          "while the raw delta sum has the OPPOSITE sign to the actual P&L --"
          " it is not a small error, it is not a quantity at all");

    // Second order matters: delta alone is measurably worse than delta+gamma.
    const double delta_only = static_cast<double>(g->cash_delta) * 1.0;
    check(std::fabs(predicted - actual) < std::fabs(delta_only - actual),
          "and the gamma term genuinely improves the prediction");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// The two conversion factors that are most often wrong.
void the_percent_and_the_square_are_both_present()
{
    std::printf("\n2 the_percent_and_the_square_are_both_present\n");
    PositionLine one[1] = {
        make_line(1, kNiftyS, kNiftyLot, 2'400'000.0, 0.15, 1, 0, 1)};
    const auto e = line_exposure(one[0]);
    check(e.has_value(), "one line converts");
    if (!e) { return; }

    const double qty = static_cast<double>(kNiftyLot);
    check(rel_near(e->cash_delta,
                   one[0].greeks.delta * qty * kNiftyS * 0.01, 1e-12),
          "cash delta carries ONE factor of 0.01 -- it is per 1% move");
    check(rel_near(e->cash_gamma,
                   one[0].greeks.gamma * qty * kNiftyS * kNiftyS * 1e-4,
                   1e-12),
          "cash gamma carries the 0.01 SQUARED, because it is a second"
          " derivative -- using one factor overstates it a hundredfold");

    // Show what the common error would produce, so the magnitude is on record.
    const double wrong = one[0].greeks.gamma * qty * kNiftyS * kNiftyS * 0.01;
    std::printf("    cash gamma %.2f; with a single 0.01 it would be %.2f"
                " -- %.0fx too big\n",
                e->cash_gamma, wrong, wrong / e->cash_gamma);
    check(rel_near(wrong / e->cash_gamma, 100.0, 1e-9),
          "the error is exactly a factor of one hundred");

    check(rel_near(e->cash_vega, one[0].greeks.vega * qty * 0.01, 1e-12),
          "cash vega is per VOL POINT, not per unit of sigma");
    check(rel_near(e->cash_theta, one[0].greeks.theta * qty / 365.0, 1e-12),
          "cash theta is per CALENDAR DAY, not per year");

    // The Taylor half on gamma.
    PortfolioGreeks g{};
    g.cash_delta = 1000;
    g.cash_gamma = 2000;
    check(rel_near(predicted_pnl(g, 1.0, 0.0, 0.0), 1000.0 + 1000.0, 1e-12),
          "predicted_pnl applies the Taylor HALF to gamma -- dropping it"
          " doubles the convexity term");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// A calendar spread: total vega zero, and nowhere near flat.
void total_vega_of_zero_is_not_a_flat_book()
{
    std::printf("\n3 total_vega_of_zero_is_not_a_flat_book\n");
    // Long back-month vega, short front-month vega, sized so they cancel.
    const double t_front = 7.0 / 365.0;
    const double t_back = 90.0 / 365.0;
    PositionLine front = make_line(-1, kNiftyS, kNiftyLot, 2'400'000.0, 0.15,
                                   1, 0, 1, OptionRight::Call, t_front);
    PositionLine back = make_line(1, kNiftyS, kNiftyLot, 2'400'000.0, 0.15,
                                  1, 1, 1, OptionRight::Call, t_back);
    // Scale the front leg so the vegas cancel. PER-UNIT vega is positive for
    // BOTH legs -- greeks are per unit and the sign comes from lots -- so the
    // ratio is of two positive numbers and the minus goes on the lots. My
    // first version divided by -vf as well, which cancelled the sign twice and
    // quietly built a book that was long both legs.
    const double vf = front.greeks.vega;      // > 0
    const double vb = back.greeks.vega;       // > 0
    const double ratio = vb / vf;             // back-month vega is the larger
    front.lots = Lots{-static_cast<std::int64_t>(std::llround(ratio))};

    PositionLine book[2] = {front, back};
    const auto g = aggregate(book, 2);
    check(g.has_value(), "the calendar spread aggregates");
    if (!g) { return; }

    std::printf("    total cash vega %+.2f paise/vol pt\n",
                static_cast<double>(g->cash_vega));
    std::printf("    bucket 0 (7d)  %+15.2f\n",
                static_cast<double>(g->vega_by_expiry[0]));
    std::printf("    bucket 1 (90d) %+15.2f\n",
                static_cast<double>(g->vega_by_expiry[1]));

    const double total = std::fabs(static_cast<double>(g->cash_vega));
    const double front_v = std::fabs(
        static_cast<double>(g->vega_by_expiry[0]));
    const double back_v = std::fabs(static_cast<double>(g->vega_by_expiry[1]));
    // Not exactly zero, and it CANNOT be: the hedge ratio is a real number
    // and lots are integers, so a calendar spread is never exactly vega flat.
    // The residual here is 11% of a leg, which is what whole lots buy.
    std::printf("    residual is %.1f%% of the front leg -- whole lots cannot"
                " cancel exactly\n", 100.0 * total / front_v);
    check(total < 0.15 * front_v,
          "total vega is a small fraction of either leg -- by the total alone"
          " the book looks nearly flat");
    check(front_v > 1e4 && back_v > 1e4,
          "while each bucket carries a large, opposite exposure");
    check((g->vega_by_expiry[0] > 0) != (g->vega_by_expiry[1] > 0),
          "and the two buckets have opposite signs, which is exactly what a"
          " total hides");
    std::printf("    -> a one-vol move in the FRONT month alone moves this"
                " book by %.0f paise\n", front_v);
    check(front_v > 5.0 * total,
          "a front-month-only vol move is several times the exposure the total"
          " reports -- summing vega across expiries is not conservative, it is"
          " wrong");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void the_sign_comes_from_the_position()
{
    std::printf("\n4 the_sign_comes_from_the_position\n");
    PositionLine lng = make_line(1, kNiftyS, kNiftyLot, 2'400'000.0, 0.15,
                                 1, 0, 1);
    PositionLine sht = lng;
    sht.lots = Lots{-1};

    const auto a = line_exposure(lng);
    const auto b = line_exposure(sht);
    check(a && b, "both convert");
    if (!a || !b) { return; }
    std::printf("    long  call: delta %+12.2f  theta %+10.2f  vega %+10.2f\n",
                a->cash_delta, a->cash_theta, a->cash_vega);
    std::printf("    short call: delta %+12.2f  theta %+10.2f  vega %+10.2f\n",
                b->cash_delta, b->cash_theta, b->cash_vega);
    check(a->cash_delta > 0.0 && b->cash_delta < 0.0,
          "a short call has NEGATIVE cash delta");
    check(a->cash_theta < 0.0 && b->cash_theta > 0.0,
          "and POSITIVE theta -- a short option earns decay, and getting this"
          " backwards makes every short look like a bleeding position");
    check(a->cash_vega > 0.0 && b->cash_vega < 0.0,
          "and negative vega");
    check(rel_near(b->cash_delta, -a->cash_delta, 1e-12)
          && rel_near(b->cash_theta, -a->cash_theta, 1e-12),
          "every greek flips together, because the sign enters in exactly one"
          " place -- the signed lots");

    // A flat book is flat in money, not merely in lots.
    PositionLine flat[2] = {lng, sht};
    const auto g = aggregate(flat, 2);
    check(g && g->cash_delta == 0 && g->cash_vega == 0 && g->cash_theta == 0,
          "a long and an equal short net to exactly zero in every greek");
    check(g && g->gross_notional > 0 && g->net_notional == 0,
          "while gross notional stays positive -- the position is flat, not"
          " absent, and a limit on gross must still see it");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void concentration_uses_magnitude_and_excludes_unclassified()
{
    std::printf("\n5 concentration_uses_magnitude_and_excludes_unclassified\n");
    PositionLine book[3] = {
        make_line(4, kNiftyS, kNiftyLot, 2'400'000.0, 0.15, 1, 0, 3),
        // Same sector, OPPOSITE direction. Delta cancels; the operational
        // exposure does not.
        make_line(-4, kNiftyS, kNiftyLot, 2'400'000.0, 0.15, 1, 0, 3,
                  OptionRight::Put),
        // Unclassified: sector 0 is a gap in reference data, not a sector.
        make_line(1, kRelS, kRelLot, 300'000.0, 0.28, 2, 0, 0),
    };
    const auto g = aggregate(book, 3);
    check(g.has_value(), "the book aggregates");
    if (!g) { return; }
    std::printf("    sector 3 absolute delta %.2f, sector 0 (unclassified)"
                " %.2f\n",
                static_cast<double>(g->abs_delta_by_sector[3]),
                static_cast<double>(g->abs_delta_by_sector[0]));
    check(g->abs_delta_by_sector[3] > 0,
          "a long and a short in one sector do not cancel as a concentration"
          " -- magnitude is what an operational limit cares about");
    check(g->unclassified == 1,
          "the unclassified line is COUNTED, so a reference-data gap is"
          " visible rather than silently absorbed");

    const SectorPeak peak = largest_sector(*g);
    std::printf("    largest classified sector: %d at %.2f\n",
                peak.sector, static_cast<double>(peak.exposure));
    check(peak.sector == 3,
          "the peak is a real sector, and sector 0 is excluded -- reporting a"
          " data gap as the largest concentration would send someone to fix"
          " the wrong thing");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void degenerate_input_is_refused()
{
    std::printf("\n6 degenerate_input_is_refused\n");
    PositionLine p = make_line(1, kNiftyS, kNiftyLot, 2'400'000.0, 0.15,
                               1, 0, 1);
    PositionLine bad = p;
    bad.lot_size = LotSize{0};
    check(!line_exposure(bad)
          && line_exposure(bad).error() == PortfolioError::BadPosition,
          "a zero lot size is refused -- the spec store was read before it"
          " was loaded (rule 1)");
    bad = p; bad.underlying = Price{0};
    check(!line_exposure(bad),
          "a zero underlying price is refused, not treated as free");
    bad = p; bad.expiry_bucket = kMaxExpiryBuckets;
    check(!aggregate(&bad, 1)
          && aggregate(&bad, 1).error() == PortfolioError::TooManyBuckets,
          "an out-of-range expiry bucket is refused rather than wrapping into"
          " another bucket's number");
    bad = p; bad.sector_id = kMaxPortfolioSectors;
    check(!aggregate(&bad, 1), "and so is an out-of-range sector");

    check(!margin_utilisation(Notional{100}, Notional{0}),
          "a zero margin-available is refused, not divided by");
    const auto u = margin_utilisation(Notional{7'500'000},
                                      Notional{10'000'000});
    check(u && rel_near(*u, 0.75, 1e-12),
          "and a normal utilisation is the plain ratio");

    // An empty book is a valid book.
    const auto empty = aggregate(nullptr, 0);
    check(empty && empty->positions == 0 && empty->cash_delta == 0,
          "an empty book aggregates to zero rather than erroring -- a flat"
          " account is a normal state, not an exception");
}

} // namespace

int main()
{
    std::printf("altair risk portfolio tests\n");
    cash_greeks_predict_a_reprice_and_raw_sums_do_not();
    the_percent_and_the_square_are_both_present();
    total_vega_of_zero_is_not_a_flat_book();
    the_sign_comes_from_the_position();
    concentration_uses_magnitude_and_excludes_unclassified();
    degenerate_input_is_refused();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
