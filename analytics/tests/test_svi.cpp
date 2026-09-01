// P3-05a acceptance tests for analytics/svi.hpp.
//
// The check that carries this file is not that g(k) is computed correctly. It
// is that g(k) < 0 MEANS SOMETHING IN MONEY: real butterfly spreads, priced
// through black76 off the slice's own vols, come out negative exactly where g
// does. An arbitrage condition that is only ever compared against itself is a
// tautology; this one is compared against a price.
//
// Note the butterflies are spaced in STRIKE, not in log-moneyness. A first
// draft of this test spaced them in k, and the CLEAN slice then priced a
// negative "butterfly" at k = -0.10 -- because K*e^-dk, K, K*e^+dk are not
// equally spaced in K, so that structure is not a butterfly and has no reason
// to be convex. Convexity is in K. Getting that wrong would have condemned a
// perfectly good surface.
//
// No check description here may contain the substring FAIL.

#include <analytics/svi.hpp>
#include <analytics/greeks.hpp>
#include <analytics/iv.hpp>

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

// A realistic NIFTY 30-day slice: 15.06% at the money, with the equity skew
// the index actually has -- 29.7 vol at k = -0.20, 12.5 at k = +0.10.
constexpr SviParams kGood{0.0002, 0.019, -0.7, 0.01, 0.08};

// Same shape, too much curvature. Satisfies every domain condition AND Lee's
// slope bound, and still implies a negative density near the money.
constexpr SviParams kBad{0.0004, 0.25, -0.99, 0.01, 0.04};

const Years kT{30.0 / 365.0};
constexpr double kF = 2'408'000.0;      // forward, paise
constexpr double kRate = 0.065;

/// Black-76 call price at an absolute strike, using the slice's own vol.
double call_at_strike(const SviParams& p, double strike_paise)
{
    const double k = std::log(strike_paise / kF);
    const auto vol = svi_implied_vol(p, k, kT);
    if (!vol) { return 0.0; }
    const Greeks g = detail::black76_unchecked(
        OptionRight::Call, kF, strike_paise, kT.raw(), vol->raw(), kRate);
    return g.price;
}

/// A real butterfly: equal spacing in STRIKE. UNIT: paise.
double butterfly(const SviParams& p, double strike_paise, double dk_paise)
{
    return call_at_strike(p, strike_paise - dk_paise)
         - 2.0 * call_at_strike(p, strike_paise)
         + call_at_strike(p, strike_paise + dk_paise);
}

// ── 1 ────────────────────────────────────────────────────────────────────
void matches_independent_reference_values()
{
    std::printf("\n1 matches_independent_reference_values\n");
    check(svi_check_params(kGood).has_value(),
          "the reference slice passes every domain condition");

    struct Ref { double k, w, wp, wpp, g; };
    const Ref refs[] = {
        {-0.20, 0.007262718960306404, -0.03105526696365039,
                0.010715106889897453, 0.29974296441219966},
        {-0.05, 0.0028980000000000004, -0.024699999999999996,
                0.12159999999999999, 0.6273778651046649},
        { 0.00, 0.0018648289721767243, -0.01565665995719496,
                0.23204036501611922, 1.0831424646955332},
        { 0.05, 0.0013674116628998403, -0.004802941685500798,
                0.16994116628998404, 1.2640840232747135},
        { 0.20, 0.0015899503443367782, 0.004211072127623241,
                0.013878744472667336, 0.5445872746147908},
    };
    int ok_w = 0, ok_d = 0, ok_g = 0;
    for (const Ref& r : refs) {
        const SviLocal L = svi_local(kGood, r.k);
        if (rel_near(L.w, r.w, 1e-14)) { ++ok_w; }
        if (rel_near(L.w_prime, r.wp, 1e-13)
            && rel_near(L.w_double_prime, r.wpp, 1e-13)) { ++ok_d; }
        if (rel_near(svi_g(kGood, r.k), r.g, 1e-12)) { ++ok_g; }
        if (rel_near(svi_total_variance(kGood, r.k), L.w, 1e-15) == false) {
            check(false, "svi_total_variance agrees with svi_local");
        }
    }
    check(ok_w == 5, "total variance matches the Python reference at five k");
    check(ok_d == 5, "and so do both derivatives");
    check(ok_g == 5, "and so does Gatheral's g");

    check(rel_near(svi_min_total_variance(kGood), 0.0012854971211385133, 1e-14),
          "the minimum total variance matches a + b*sigma*sqrt(1-rho^2)");
    check(rel_near(svi_argmin_k(kGood), 0.08841568470556854, 1e-13),
          "and it sits at m - rho*sigma/sqrt(1-rho^2)");
    // The minimum must actually BE the minimum -- the closed form and the
    // function have to agree, or one of them is wrong.
    const double at_argmin = svi_total_variance(kGood, svi_argmin_k(kGood));
    bool nothing_lower = true;
    for (int i = -500; i <= 500; ++i) {
        if (svi_total_variance(kGood, i * 0.002) < at_argmin - 1e-18) {
            nothing_lower = false;
        }
    }
    check(rel_near(at_argmin, svi_min_total_variance(kGood), 1e-14)
          && nothing_lower,
          "and no sampled k anywhere gives a smaller total variance");

    // The smile the caller actually sees.
    std::printf("    the slice as a smile:\n");
    for (double k : {-0.20, -0.10, 0.0, 0.05, 0.10}) {
        std::printf("      k=%+.2f  vol = %6.2f%%\n",
                    k, svi_implied_vol(kGood, k, kT)->raw() * 100.0);
    }
    check(rel_near(svi_implied_vol(kGood, 0.0, kT)->raw(), 0.15062786095147476,
                   1e-13),
          "the at-the-money vol is 15.06%");
    check(svi_implied_vol(kGood, -0.20, kT)->raw()
          > svi_implied_vol(kGood, 0.10, kT)->raw(),
          "and puts are bid over calls, which is the equity skew");
    check(svi_slopes_within_lee(kGood), "the slice respects Lee's slope bound");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void domain_violations_are_refused()
{
    std::printf("\n2 domain_violations_are_refused\n");
    auto err = [](SviParams p) {
        const auto r = svi_check_params(p);
        return r ? SviError::NotFinite : r.error();
    };
    SviParams p = kGood;
    p.b = -0.01;
    check(err(p) == SviError::NegativeB, "a negative b is refused");
    p = kGood; p.rho = 1.0;
    check(err(p) == SviError::RhoOutOfRange, "rho = 1 is refused, not clamped");
    p = kGood; p.rho = -1.0;
    check(err(p) == SviError::RhoOutOfRange, "and so is rho = -1");
    p = kGood; p.rho = -1.5;
    check(err(p) == SviError::RhoOutOfRange, "and rho outside [-1,1]");
    p = kGood; p.sigma = 0.0;
    check(err(p) == SviError::NonPositiveSigma,
          "sigma = 0 is refused -- the smile would have a kink and w'' a"
          " delta at k = m");
    p = kGood; p.a = -0.5;
    check(err(p) == SviError::NegativeMinVariance,
          "a slice whose minimum total variance is negative is refused --"
          " there is no square root there, so no implied vol at the money");
    p = kGood; p.a = std::nan("");
    check(err(p) == SviError::NotFinite, "a NaN parameter is refused");

    // A refused slice must not be scannable either.
    p = kGood; p.sigma = 0.0;
    check(!svi_butterfly_scan(p, -0.2, 0.2),
          "and a butterfly scan on it refuses rather than returning clean");
    check(!svi_butterfly_scan(kGood, 0.2, -0.2), "an inverted range is refused");
    check(!svi_butterfly_scan(kGood, -0.2, 0.2, 1),
          "a single-point scan is refused -- one sample is not a scan");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// An independent check on g that does not go through g's own definition:
// the implied density must integrate to one.
void the_implied_density_integrates_to_one()
{
    std::printf("\n3 the_implied_density_integrates_to_one\n");
    constexpr int kN = 200'000;
    constexpr double lo = -4.0, hi = 4.0;
    const double h = (hi - lo) / static_cast<double>(kN);
    double integral = 0.0;
    double negatives = 0.0;
    for (int i = 0; i < kN; ++i) {
        const double k = lo + h * (static_cast<double>(i) + 0.5);
        const double d = svi_density(kGood, k);
        integral += d;
        if (d < 0.0) { negatives += 1.0; }
    }
    integral *= h;
    std::printf("    integral over [-4,4] = %.12f, negative points %.0f\n",
                integral, negatives);
    check(rel_near(integral, 1.0, 1e-6),
          "the density of a clean slice integrates to 1");
    check(negatives == 0.0, "and is non-negative everywhere");

    // The violating slice must fail this, or the check is not discriminating.
    double neg_bad = 0.0;
    for (int i = 0; i < kN; ++i) {
        const double k = lo + h * (static_cast<double>(i) + 0.5);
        if (svi_density(kBad, k) < 0.0) { neg_bad += 1.0; }
    }
    std::printf("    the arbitrageable slice has %.0f negative-density points\n",
                neg_bad);
    check(neg_bad > 0.0,
          "the arbitrageable slice implies negative probability somewhere");
}

// ── 4 ────────────────────────────────────────────────────────────────────
// g(k) tied to money. Real butterflies, equal spacing in STRIKE, priced
// through black76 off each slice's own vols.
void g_predicts_the_sign_of_a_real_butterfly()
{
    std::printf("\n4 g_predicts_the_sign_of_a_real_butterfly\n");
    constexpr double dK = 5'000.0;      // Rs 50, one NIFTY strike step

    std::printf("    strike       k        clean slice    arbitrageable\n");
    int neg_clean = 0, neg_bad = 0;
    for (int strike_rs : {22000, 22500, 23000, 23500, 24000, 24500, 25000}) {
        const double K = static_cast<double>(strike_rs) * 100.0;
        const double k = std::log(K / kF);
        const double bg = butterfly(kGood, K, dK);
        const double bb = butterfly(kBad, K, dK);
        if (bg < 0.0) { ++neg_clean; }
        if (bb < 0.0) { ++neg_bad; }
        std::printf("    %6d   %+.4f   %+13.6f   %+13.6f%s\n",
                    strike_rs, k, bg, bb, bb < 0.0 ? "   <-- NEGATIVE" : "");
    }
    check(neg_clean == 0,
          "a butterfly-free slice prices every butterfly positive");
    check(neg_bad > 0,
          "and the violating slice prices some NEGATIVE -- free money that"
          " is not there, against the engine's own fit");

    // The real claim: the sign of g predicts the sign of the butterfly, at
    // every strike, on both slices.
    int agree = 0, total = 0;
    for (int strike_rs = 21000; strike_rs <= 26000; strike_rs += 250) {
        const double K = static_cast<double>(strike_rs) * 100.0;
        const double k = std::log(K / kF);
        for (const SviParams& p : {kGood, kBad}) {
            const bool b_neg = butterfly(p, K, dK) < 0.0;
            const bool g_neg = svi_g(p, k) < 0.0;
            ++total;
            if (b_neg == g_neg) { ++agree; }
        }
    }
    std::printf("    sign of g agrees with sign of the butterfly on %d/%d"
                " strike-slice pairs\n", agree, total);
    check(agree == total,
          "g < 0 and a negative butterfly price are the same event");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void the_scan_catches_what_lee_does_not()
{
    std::printf("\n5 the_scan_catches_what_lee_does_not\n");
    const auto good = svi_butterfly_scan(kGood, -0.5, 0.5);
    const auto bad = svi_butterfly_scan(kBad, -0.5, 0.5);
    check(good && bad, "both slices scan");
    if (!good || !bad) { return; }
    std::printf("    clean slice        min g = %+.6f at k = %+.3f  -> %s\n",
                good->worst, good->worst_k, good->clean ? "clean" : "DIRTY");
    std::printf("    arbitrageable      min g = %+.6f at k = %+.3f  -> %s\n",
                bad->worst, bad->worst_k, bad->clean ? "clean" : "DIRTY");
    check(good->clean, "the reference slice is butterfly free");
    check(!bad->clean, "and the violating one is caught");
    check(bad->worst_k < 0.0,
          "with the worst point on the put side, where the curvature is");

    // The point of having Lee as a separate, cheaper check: it is NECESSARY,
    // not sufficient. This slice passes it and is still arbitrageable, so a
    // fit cannot be validated by the slope bound alone.
    check(svi_slopes_within_lee(kBad),
          "the violating slice PASSES Lee's slope bound");
    check(!bad->clean,
          "while failing the butterfly scan -- Lee is necessary, not"
          " sufficient, and a fit validated only by slopes is not validated");

    // And the range is the check. A window positioned away from the violation
    // reports clean, which is why callers pass the range they will quote over.
    const auto narrow = svi_butterfly_scan(kBad, 0.10, 0.50);
    check(narrow && narrow->clean,
          "scanning only the call wing of the SAME violating slice reports"
          " clean -- a scan is evidence about its range and nothing else");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void calendar_arbitrage_is_caught()
{
    std::printf("\n6 calendar_arbitrage_is_caught\n");
    // A 60-day slice that dominates the 30-day one everywhere.
    constexpr SviParams kLong{0.0005, 0.038, -0.7, 0.01, 0.08};
    // One that crosses it slightly on the call side.
    constexpr SviParams kCrossing{0.0001, 0.020, -0.7, 0.01, 0.08};

    const auto ok = svi_calendar_scan(kGood, kLong, -0.5, 0.5);
    const auto bad = svi_calendar_scan(kGood, kCrossing, -0.5, 0.5);
    check(ok && bad, "both pairs scan");
    if (!ok || !bad) { return; }
    std::printf("    dominating 60d  min (w60 - w30) = %+.9f at k = %+.3f\n",
                ok->worst, ok->worst_k);
    std::printf("    crossing   60d  min (w60 - w30) = %+.9f at k = %+.3f\n",
                bad->worst, bad->worst_k);
    check(ok->clean, "a properly dominating longer slice passes");
    check(!bad->clean, "a crossing one is caught");
    check(bad->worst < 0.0 && bad->worst > -1e-3,
          "and the violation is TINY -- 4e-5 in total variance, which is why"
          " it has to be scanned for rather than noticed");

    // Total variance, not vol. The longer slice can have LOWER implied vol at
    // the same k without any arbitrage, and confusing the two condemns
    // perfectly good surfaces in a stressed market.
    const Years t_long{60.0 / 365.0};
    const double vol30 = svi_implied_vol(kGood, 0.0, kT)->raw();
    const double vol60 = svi_implied_vol(kLong, 0.0, t_long)->raw();
    std::printf("    at k=0: 30d vol %.4f, 60d vol %.4f; total variance"
                " %.6f -> %.6f\n", vol30, vol60,
                svi_total_variance(kGood, 0.0),
                svi_total_variance(kLong, 0.0));
    check(svi_total_variance(kLong, 0.0) > svi_total_variance(kGood, 0.0),
          "total variance rises with maturity, which is the condition");
}

// ── 7 ────────────────────────────────────────────────────────────────────
// Ties the surface to the rest of Phase 3: a vol off the slice, priced with
// black76, must invert back to the same vol through the P3-04 solver.
void a_slice_vol_round_trips_through_the_iv_solver()
{
    std::printf("\n7 a_slice_vol_round_trips_through_the_iv_solver\n");
    int recovered = 0, attempted = 0;
    double worst = 0.0;
    for (int strike_rs = 21000; strike_rs <= 27000; strike_rs += 500) {
        const double K = static_cast<double>(strike_rs) * 100.0;
        const double k = std::log(K / kF);
        const auto vol = svi_implied_vol(kGood, k, kT);
        if (!vol) { continue; }
        const Greeks g = detail::black76_unchecked(
            OptionRight::Call, kF, K, kT.raw(), vol->raw(), kRate);
        const auto back = detail::solve_iv(OptionRight::Call, g.price, kF, K,
                                           kT.raw(), kRate);
        ++attempted;
        if (!back) { continue; }
        const double e = std::fabs(back->vol.raw() - vol->raw());
        if (e > worst) { worst = e; }
        if (e < 1e-9) { ++recovered; }
    }
    std::printf("    %d strikes, %d recovered to 1e-9, worst error %.3e\n",
                attempted, recovered, worst);
    check(attempted == 13, "every strike on the ladder produced a vol");
    check(recovered == attempted,
          "and every one inverts back to the vol the slice quoted");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void the_scan_reports_where_not_just_whether()
{
    std::printf("\n8 the_scan_reports_where_not_just_whether\n");
    const auto s = svi_butterfly_scan(kBad, -0.5, 0.5, 1001);
    check(s.has_value(), "a fine scan runs");
    if (!s) { return; }
    check(s->points == 1001, "it reports how many points it sampled");
    // The reported worst_k must actually be where the worst value is.
    const double at_reported = svi_g(kBad, s->worst_k);
    check(rel_near(at_reported, s->worst, 1e-12),
          "the reported worst value is g evaluated at the reported k");
    bool nothing_worse = true;
    const double step = 1.0 / 1000.0;
    for (int i = 0; i <= 1000; ++i) {
        if (svi_g(kBad, -0.5 + step * i) < s->worst - 1e-15) {
            nothing_worse = false;
        }
    }
    check(nothing_worse, "and no sampled point is worse than it");

    // Resolution matters: a coarse scan can step over a narrow violation.
    // Reported rather than asserted as a defect, because it is inherent to
    // sampling and the answer is to scan finely, not to pretend otherwise.
    const auto coarse = svi_butterfly_scan(kBad, -0.5, 0.5, 5);
    const auto fine = svi_butterfly_scan(kBad, -0.5, 0.5, 4001);
    std::printf("    5 points: min g %+.6f    4001 points: min g %+.6f\n",
                coarse->worst, fine->worst);
    check(fine->worst <= coarse->worst,
          "a finer scan never finds a less severe violation");
    check(std::fabs(fine->worst - s->worst) < 0.02,
          "and 1001 points already resolves this violation to within 0.02");
}

} // namespace

int main()
{
    std::printf("altair analytics svi tests\n");
    matches_independent_reference_values();
    domain_violations_are_refused();
    the_implied_density_integrates_to_one();
    g_predicts_the_sign_of_a_real_butterfly();
    the_scan_catches_what_lee_does_not();
    calendar_arbitrage_is_caught();
    a_slice_vol_round_trips_through_the_iv_solver();
    the_scan_reports_where_not_just_whether();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
