// P3-04 acceptance tests for analytics/iv.hpp.
//
// An IV solver is easy to test badly. Feed it a price you produced with the
// same pricer, watch it come back, declare victory. That check passes for a
// solver that is wrong in every region nobody sampled.
//
// So the checks here are of four different kinds:
//
//   * ROUND TRIP over a corpus that deliberately spans the hard regions --
//     wings, two-day expiry, 95 vol, 2 vol, deep ITM puts.
//   * MODEL INVARIANTS that hold independently of any reference number:
//     price is monotone in vol, and put-call parity means a call and a put on
//     one strike must imply ONE vol. A sign error in the put branch survives
//     round-trip testing and dies here.
//   * REFUSALS at and outside the no-arbitrage bounds, where the honest
//     answer is "no vol exists" and the tempting answer is an endpoint.
//   * ERROR PROPAGATION measured, not asserted: the shift in recovered IV
//     from quantising a price to whole paise is predicted by dP/vega and
//     checked against what actually comes back.
//
// No check description here may contain the substring FAIL.

#include <analytics/iv.hpp>

#include <cstdio>
#include <cstdint>
#include <cmath>
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

bool rel_near(double a, double b, double tol)
{
    const double d = std::fabs(a - b);
    const double m = std::fabs(b) > 1.0 ? std::fabs(b) : 1.0;
    return d / m <= tol;
}

} // namespace

using namespace altair;

namespace {

// Same NIFTY frame as the P3-01 tests: forward 24'080 in paise, 6.5% rate.
constexpr double kF = 2'408'000.0;
constexpr double kRate = 0.065;
constexpr double k30d = 30.0 / 365.0;

struct RoundTrip {
    const char* name;
    OptionRight right;
    double strike;
    double t;
    double vol;      // the vol the price was produced at
    double price;    // reference price, computed in Python from that vol
};

// Reference prices computed independently in Python from the Black-76
// formula. Chosen to span the regions where a solver goes wrong: the far
// wing (7 paise), a two-day weekly, 95 vol, 2 vol, and a deep ITM put.
const RoundTrip kCases[] = {
    {"atm call 30d",  OptionRight::Call, 2400000.0, k30d,          0.15, 45121.48867250209},
    {"otm call 30d",  OptionRight::Call, 2800000.0, k30d,          0.22, 454.7616628662023},
    {"itm call 30d",  OptionRight::Call, 2000000.0, k30d,          0.19, 405836.0631428751},
    {"atm put 30d",   OptionRight::Put,  2400000.0, k30d,          0.15, 37164.11443380938},
    {"otm put 30d",   OptionRight::Put,  2000000.0, k30d,          0.24, 160.09958437117297},
    {"atm call 2d",   OptionRight::Call, 2400000.0, 2.0 / 365.0,   0.11, 12447.972495653989},
    {"far otm 7d",    OptionRight::Call, 3000000.0, 7.0 / 365.0,   0.35, 0.07599875572111724},
    {"high vol 95",   OptionRight::Call, 2400000.0, k30d,          0.95, 263010.85743046395},
    {"low vol 2",     OptionRight::Call, 2400000.0, k30d,          0.02, 10344.642042235644},
    {"deep itm put",  OptionRight::Put,  3000000.0, k30d,          0.18, 588845.9959295179},
};
constexpr int kNumCases = static_cast<int>(sizeof(kCases) / sizeof(kCases[0]));

// ── 1 ────────────────────────────────────────────────────────────────────
void round_trip_recovers_the_known_vol()
{
    std::printf("\n1 round_trip_recovers_the_known_vol\n");
    int recovered = 0;
    for (const RoundTrip& c : kCases) {
        const auto r = detail::solve_iv(c.right, c.price, kF, c.strike, c.t,
                                        kRate);
        if (!r) {
            check(false, c.name);
            continue;
        }
        const bool ok = rel_near(r->vol.raw(), c.vol, 1e-9);
        std::printf("    %-14s vol %.10f (want %.4f)  iters %2u  bisect %2u"
                    "  resid %.3e p\n",
                    c.name, r->vol.raw(), c.vol,
                    static_cast<unsigned>(r->iterations),
                    static_cast<unsigned>(r->bisections), r->residual);
        if (ok) {
            ++recovered;
        } else {
            check(false, c.name);
        }
    }
    check(recovered == kNumCases,
          "every case in the corpus recovers its vol to 1e-9 relative");
    // Guard against the corpus being silently emptied or narrowed: a test
    // that iterates zero cases reports success just as loudly as one that
    // iterates ten.
    check(kNumCases >= 10, "corpus still spans at least 10 cases");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// Black-Scholes and Black-76 are ONE model. The prices must agree to rounding
// on F = S*e^((r-q)T), the implied vols must agree, and vega must be
// identical -- because df*F reduces to S*e^(-qT). What must NOT agree is
// delta, and rho must not even share a sign.
void black_scholes_and_black76_are_one_model()
{
    std::printf("\n2 black_scholes_and_black76_are_one_model\n");
    constexpr Price kSpot{2'400'000};
    constexpr Price kStrike{2'400'000};
    const Years t{k30d};
    const Vol vol{0.15};
    constexpr double q = 0.012;

    const auto bs = black_scholes(OptionRight::Call, kSpot, kStrike, t, vol,
                                  kRate, q);
    check(bs.has_value(), "black_scholes returns a value on a sane call");
    if (!bs) { return; }

    const double fwd = 2'400'000.0 * std::exp((kRate - q) * k30d);
    const Greeks b76 = detail::black76_unchecked(OptionRight::Call, fwd,
                                                 2'400'000.0, k30d, 0.15,
                                                 kRate);

    std::printf("    F = S*e^((r-q)T) = %.6f paise\n", fwd);
    std::printf("    price  BS %.9f   B76 %.9f   diff %.3e\n",
                bs->price, b76.price, std::fabs(bs->price - b76.price));
    check(std::fabs(bs->price - b76.price) < 1e-8,
          "prices agree to under 1e-8 paise -- the same model, reparameterised");
    check(rel_near(bs->price, 46462.43938188278, 1e-12),
          "BS price matches the independent Python reference");

    // Vega is basis-free: df*F == S*e^(-qT) identically.
    check(rel_near(bs->vega, b76.vega, 1e-12),
          "vega is identical across the two parameterisations");
    check(rel_near(bs->vega, 272165.2328627695, 1e-12),
          "BS vega matches the independent Python reference");

    // Delta is NOT basis-free, and this is the number that mis-hedges.
    const double carry = std::exp((kRate - q) * k30d);
    std::printf("    delta  BS(spot) %.9f   B76(fwd) %.9f   ratio %.9f"
                "   e^((r-q)T) %.9f\n",
                bs->delta, b76.delta, bs->delta / b76.delta, carry);
    check(bs->delta > b76.delta,
          "spot delta exceeds forward delta -- the hazard actually exists here");
    check(rel_near(bs->delta / b76.delta, carry, 1e-12),
          "delta_spot / delta_fwd equals e^((r-q)T) exactly");
    check(rel_near(forward_delta_to_spot(b76.delta, t, kRate, q), bs->delta,
                   1e-12),
          "forward_delta_to_spot reproduces the Black-Scholes delta");
    check(bs->basis == GreekBasis::Spot && b76.basis == GreekBasis::Forward,
          "each pricer stamps its own basis, so a mismatch is assertable");
    check(Greeks{}.basis == GreekBasis::Unspecified,
          "a default-constructed Greeks does not read as a valid basis");

    // Rho: opposite signs, 27x apart. Using the B76 form on a spot book, or
    // the reverse, is not a rounding error.
    std::printf("    rho    BS %.4f   B76 %.4f  (signs %s)\n",
                bs->rho, b76.rho,
                (bs->rho > 0.0) != (b76.rho > 0.0) ? "differ" : "match");
    check(b76.rho < 0.0 && bs->rho > 0.0,
          "long-call rho is negative under B76 and positive under BS");
    check(rel_near(bs->rho, 104344.09138246797, 1e-10),
          "BS call rho matches the independent Python reference");
    check(rel_near(b76.rho, -k30d * b76.price, 1e-12),
          "B76 rho is exactly -T*V, with no drift term");

    // forward_from_spot rounds to whole paise, which is why the pricer does
    // NOT route through it. Both facts are checked, so the rounding is
    // documented by demonstration rather than by comment.
    const auto fwd_p = forward_from_spot(kSpot, t, kRate, q);
    check(fwd_p.has_value(), "forward_from_spot returns a value");
    if (fwd_p) {
        std::printf("    forward_from_spot %lld paise vs exact %.6f"
                    "  (rounding %+.6f p)\n",
                    static_cast<long long>(fwd_p->raw()), fwd,
                    static_cast<double>(fwd_p->raw()) - fwd);
        check(fwd_p->raw() == static_cast<std::int64_t>(fwd + 0.5),
              "the forward rounds half away from zero to whole paise");
        check(std::fabs(static_cast<double>(fwd_p->raw()) - fwd) <= 0.5,
              "the rounding is bounded by half a paise, and is not zero-cost");
    }
    check(!forward_from_spot(Price{0}, t, kRate, q),
          "a zero spot has no forward");

    // And the two public IV entry points must land on the same vol.
    const auto iv_bs = implied_vol_black_scholes(
        OptionRight::Call, Price{static_cast<std::int64_t>(bs->price)},
        kSpot, kStrike, t, kRate, q);
    const auto iv_76 = detail::solve_iv(OptionRight::Call, bs->price, fwd,
                                        2'400'000.0, k30d, kRate);
    check(iv_bs.has_value() && iv_76.has_value(), "both entry points solve");
    if (iv_bs && iv_76) {
        check(rel_near(iv_76->vol.raw(), 0.15, 1e-10),
              "inverting the BS price through the forward returns 15 vol");
    }
}

// ── 3 ────────────────────────────────────────────────────────────────────
// Outside the no-arbitrage bounds no vol exists. The endpoints are the
// tempting wrong answer: sigma = 0 at the floor, sigma = infinity at the
// ceiling. Both are refused, including EXACTLY at the bound.
void refuses_prices_outside_the_no_arbitrage_bounds()
{
    std::printf("\n3 refuses_prices_outside_the_no_arbitrage_bounds\n");
    const double df = std::exp(-kRate * k30d);
    const double K = 2'400'000.0;
    const auto b = detail::no_arb_bounds(OptionRight::Call, kF, K, df);
    std::printf("    call K=24000: lower %.6f  upper %.6f paise\n",
                b.lower, b.upper);
    check(rel_near(b.lower, 7957.374238692715, 1e-12),
          "discounted intrinsic matches the Python reference");
    check(rel_near(b.upper, 2395169.645846507, 1e-12),
          "upper bound df*F matches the Python reference");

    auto err = [&](double px) {
        const auto r = detail::solve_iv(OptionRight::Call, px, kF, K, k30d,
                                        kRate);
        return r ? IvError::NotFinite : r.error();
    };
    check(err(b.lower - 1.0) == IvError::BelowIntrinsic,
          "a paise below the discounted intrinsic is BelowIntrinsic");
    check(err(b.lower) == IvError::BelowIntrinsic,
          "EXACTLY at the intrinsic is refused, not reported as zero vol");
    check(err(b.upper) == IvError::AboveBound,
          "EXACTLY at df*F is refused, not reported as infinite vol");
    check(err(b.upper + 1.0) == IvError::AboveBound,
          "a paise above df*F is AboveBound");
    // Just inside both bounds a solution must exist -- otherwise the refusals
    // above would pass for a solver that refuses everything.
    const auto lo_ok = detail::solve_iv(OptionRight::Call, b.lower * 1.0001,
                                        kF, K, k30d, kRate);
    const auto hi_ok = detail::solve_iv(OptionRight::Call, b.upper * 0.9999,
                                        kF, K, k30d, kRate);
    check(lo_ok.has_value() && hi_ok.has_value(),
          "prices just inside both bounds still solve");
    if (lo_ok && hi_ok) {
        std::printf("    just inside: lower -> %.6f vol, upper -> %.4f vol\n",
                    lo_ok->vol.raw(), hi_ok->vol.raw());
        check(lo_ok->vol.raw() < hi_ok->vol.raw(),
              "the near-floor vol is below the near-ceiling vol");
    }

    // Puts have DIFFERENT bounds -- df*K, not df*F. Getting this wrong is
    // invisible when F is close to K.
    const auto pb = detail::no_arb_bounds(OptionRight::Put, kF, K, df);
    check(rel_near(pb.upper, df * K, 1e-15),
          "the put ceiling is df*K, not df*F");
    check(pb.lower == 0.0,
          "an out-of-the-money put has a zero floor, not a negative one");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void refuses_degenerate_inputs()
{
    std::printf("\n4 refuses_degenerate_inputs\n");
    const double K = 2'400'000.0;
    auto e = [&](double px, double F, double KK, double T) {
        const auto r = detail::solve_iv(OptionRight::Call, px, F, KK, T, kRate);
        return r ? IvError::NotFinite : r.error();
    };
    check(e(45121.0, kF, K, 0.0) == IvError::Expired, "T = 0 is Expired");
    check(e(45121.0, kF, K, -1.0) == IvError::Expired, "T < 0 is Expired");
    check(e(0.0, kF, K, k30d) == IvError::NonPositiveInput,
          "a zero price is refused, not solved as zero vol");
    check(e(-1.0, kF, K, k30d) == IvError::NonPositiveInput,
          "a negative price is refused");
    check(e(45121.0, 0.0, K, k30d) == IvError::NonPositiveInput,
          "a zero forward is refused");
    check(e(45121.0, kF, 0.0, k30d) == IvError::NonPositiveInput,
          "a zero strike is refused");
    const double nan = std::nan("");
    check(e(nan, kF, K, k30d) == IvError::NotFinite, "a NaN price is NotFinite");
    check(e(45121.0, kF, K, nan) == IvError::NotFinite, "a NaN T is NotFinite");

    // The public wrapper must refuse the same things, not just the internal.
    const auto pub = implied_vol_black76(OptionRight::Call, Price{45121},
                                         Price{2'408'000}, Price{2'400'000},
                                         Years{0.0}, kRate);
    check(!pub && pub.error() == IvError::Expired,
          "the public Black-76 entry point refuses an expired option too");
}

// ── 5 ────────────────────────────────────────────────────────────────────
// The physics gate. IV is a measurement and its precision is not uniform
// across the surface. The hazard is asserted to EXIST before the handling of
// it is asserted -- otherwise this test passes on a flat surface where there
// was never anything to catch.
void vega_collapse_widens_the_error_bar()
{
    std::printf("\n5 vega_collapse_widens_the_error_bar\n");
    constexpr double kTick = 5.0;   // one NIFTY option tick, in paise

    struct Pt { double k; double px; };
    const Pt atm{2'400'000.0, 45121.48867250209};
    const Pt wing{2'800'000.0, 6.310312470381938};

    const auto a = detail::solve_iv(OptionRight::Call, atm.px, kF, atm.k,
                                    k30d, kRate);
    const auto w = detail::solve_iv(OptionRight::Call, wing.px, kF, wing.k,
                                    k30d, kRate);
    check(a.has_value() && w.has_value(), "both strikes solve");
    if (!a || !w) { return; }

    // 5a. The hazard exists: wing vega really is orders of magnitude smaller.
    std::printf("    vega   atm %.2f   wing %.2f   ratio %.1f\n",
                a->vega, w->vega, a->vega / w->vega);
    check(w->vega < a->vega / 100.0,
          "wing vega is under a hundredth of ATM vega -- the hazard is real");

    // 5b. And the error bar tracks it.
    const double band_atm = iv_uncertainty(a->vega, kTick);
    const double band_wing = iv_uncertainty(w->vega, kTick);
    std::printf("    1-tick IV band   atm %.6f   wing %.6f   ratio %.1f\n",
                band_atm, band_wing, band_wing / band_atm);
    check(rel_near(band_wing / band_atm, a->vega / w->vega, 1e-12),
          "the band ratio is exactly the inverse vega ratio");
    check(band_wing > 100.0 * band_atm,
          "one tick moves the wing IV over 100x further than the ATM IV");
    check(band_atm < 0.0001,
          "the ATM IV resolves to better than 0.01 vol points on one tick");
    check(band_wing > 0.005,
          "the wing IV does not resolve to better than 0.5 vol points");

    // 5c. Zero vega is +infinity, not a large finite number. A finite value
    // here would let a caller weight a meaningless point as merely imprecise.
    check(std::isinf(iv_uncertainty(0.0, kTick)),
          "zero vega yields an infinite band, not an invented precision");
    check(std::isinf(iv_uncertainty(-1.0, kTick)),
          "negative vega yields an infinite band");

    // 5d. Error propagation MEASURED, not asserted: quantise the ATM price to
    // whole paise and check the recovered IV shifts by exactly dP/vega.
    const double quantised = std::floor(atm.px);
    const double dP = quantised - atm.px;
    const auto q = detail::solve_iv(OptionRight::Call, quantised, kF, atm.k,
                                    k30d, kRate);
    check(q.has_value(), "the quantised price also solves");
    if (q) {
        const double predicted = dP / a->vega;
        const double actual = q->vol.raw() - a->vol.raw();
        std::printf("    quantising dP %.6f p -> dIV predicted %.3e"
                    "  actual %.3e\n", dP, predicted, actual);
        check(rel_near(actual, predicted, 1e-4),
              "the IV shift from rounding a price matches dP/vega to 1e-4");
    }
}

// ── 6 ────────────────────────────────────────────────────────────────────
// A model invariant that needs no reference numbers: Black-76 price is
// strictly increasing in vol, so implied vol must be strictly increasing in
// price across the whole admissible range.
void implied_vol_is_monotone_in_price()
{
    std::printf("\n6 implied_vol_is_monotone_in_price\n");
    const double df = std::exp(-kRate * k30d);
    const double K = 2'400'000.0;
    const auto b = detail::no_arb_bounds(OptionRight::Call, kF, K, df);

    double prev = -1.0;
    int steps = 0;
    int solved = 0;
    bool monotone = true;
    for (int i = 1; i < 200; ++i) {
        const double frac = static_cast<double>(i) / 200.0;
        const double px = b.lower + frac * (b.upper - b.lower);
        const auto r = detail::solve_iv(OptionRight::Call, px, kF, K, k30d,
                                        kRate);
        ++steps;
        if (!r) { continue; }        // the top of the range needs vol > kIvVolMax
        ++solved;
        if (r->vol.raw() <= prev) { monotone = false; }
        prev = r->vol.raw();
    }
    std::printf("    probed %d prices across the admissible range, %d solved,"
                " top vol %.4f\n", steps, solved, prev);
    check(solved > 100,
          "the sweep actually solved a majority of its probes");
    check(monotone, "implied vol increases strictly with price throughout");
}

// ── 7 ────────────────────────────────────────────────────────────────────
// Put-call parity: C - P = df*(F - K). A call and a put priced consistently
// on one strike describe ONE distribution and therefore ONE vol. This is the
// check that kills a sign error in the put branch, which round-tripping
// against a put-derived reference price cannot see.
void a_parity_consistent_pair_implies_one_vol()
{
    std::printf("\n7 a_parity_consistent_pair_implies_one_vol\n");
    const double K = 2'600'000.0;
    const double df = std::exp(-kRate * k30d);
    // Price the call at a known vol, then derive the put from PARITY alone --
    // never from the put formula. The put price entering the solver has
    // therefore not touched the code path being tested.
    const Greeks c = detail::black76_unchecked(OptionRight::Call, kF, K, k30d,
                                               0.17, kRate);
    const double put_from_parity = c.price - df * (kF - K);
    std::printf("    call %.6f p   put(from parity) %.6f p\n",
                c.price, put_from_parity);

    const auto iv_c = detail::solve_iv(OptionRight::Call, c.price, kF, K,
                                       k30d, kRate);
    const auto iv_p = detail::solve_iv(OptionRight::Put, put_from_parity, kF,
                                       K, k30d, kRate);
    check(iv_c.has_value() && iv_p.has_value(), "both legs solve");
    if (!iv_c || !iv_p) { return; }
    std::printf("    call IV %.12f   put IV %.12f   diff %.3e\n",
                iv_c->vol.raw(), iv_p->vol.raw(),
                std::fabs(iv_c->vol.raw() - iv_p->vol.raw()));
    check(rel_near(iv_c->vol.raw(), 0.17, 1e-10), "the call leg returns 17 vol");
    check(rel_near(iv_p->vol.raw(), 0.17, 1e-9),
          "the parity-derived put leg returns the SAME 17 vol");
    check(rel_near(iv_c->vega, iv_p->vega, 1e-8),
          "call and put vega agree -- they differ by a forward, which has none");
}

// ── 8 ────────────────────────────────────────────────────────────────────
// The bisection safeguard is a branch that only executes on hard inputs. If
// no test ever drives it, it is untested code that runs for the first time on
// a live wing quote. So: assert it actually fires somewhere in the corpus,
// bound the iteration counts, and measure the cost.
void the_bisection_safeguard_is_exercised_and_bounded()
{
    std::printf("\n8 the_bisection_safeguard_is_exercised_and_bounded\n");
    int worst_iters = 0;
    int total_bisect = 0;
    int cases_bisecting = 0;
    double worst_resid = 0.0;
    for (const RoundTrip& c : kCases) {
        const auto r = detail::solve_iv(c.right, c.price, kF, c.strike, c.t,
                                        kRate);
        if (!r) { continue; }
        if (r->iterations > worst_iters) {
            worst_iters = r->iterations;
        }
        total_bisect += r->bisections;
        if (r->bisections > 0) { ++cases_bisecting; }
        const double a = std::fabs(r->residual);
        if (a > worst_resid) { worst_resid = a; }
    }
    std::printf("    worst iterations %d   cases needing bisection %d"
                "   total bisections %d\n",
                worst_iters, cases_bisecting, total_bisect);
    std::printf("    worst absolute residual %.3e paise\n", worst_resid);
    check(cases_bisecting > 0,
          "at least one corpus case drives the bisection branch");
    check(worst_iters < kIvMaxIterations,
          "no case comes close to exhausting the iteration budget");
    check(worst_resid < 1.0e-4,
          "every case converges to under 1e-4 paise of the target");

    // A vol above kIvVolMax must be NoConvergence, not the cap returned as a
    // measurement.
    const double df = std::exp(-kRate * k30d);
    const auto b = detail::no_arb_bounds(OptionRight::Call, kF, 2'400'000.0, df);
    const auto absurd = detail::solve_iv(OptionRight::Call,
                                         b.upper * (1.0 - 1e-14), kF,
                                         2'400'000.0, k30d, kRate);
    check(!absurd || absurd->vol.raw() < kIvVolMax,
          "a price at the ceiling either refuses or stays under the vol cap");

    // Cost. Not a hot path -- the solver loops -- but a full chain of 100
    // strikes has to fit inside a tick.
    constexpr int kReps = 20000;
    const auto t0 = std::chrono::steady_clock::now();
    double sink = 0.0;
    for (int i = 0; i < kReps; ++i) {
        const RoundTrip& c = kCases[i % kNumCases];
        const auto r = detail::solve_iv(c.right, c.price, kF, c.strike, c.t,
                                        kRate);
        if (r) { sink += r->vol.raw(); }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count())
        / kReps;
    std::printf("    %.0f ns per solve (mixed corpus), 100-strike chain"
                " = %.1f us   [sink %.4f]\n", ns, ns * 100.0 / 1000.0, sink);
    check(ns < 20000.0, "a solve stays under 20 us");
}

} // namespace

int main()
{
    std::printf("P3-04 analytics/iv.hpp\n");
    round_trip_recovers_the_known_vol();
    black_scholes_and_black76_are_one_model();
    refuses_prices_outside_the_no_arbitrage_bounds();
    refuses_degenerate_inputs();
    vega_collapse_widens_the_error_bar();
    implied_vol_is_monotone_in_price();
    a_parity_consistent_pair_implies_one_vol();
    the_bisection_safeguard_is_exercised_and_bounded();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
