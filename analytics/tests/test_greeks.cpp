// P3-01 acceptance tests for analytics/greeks.hpp.
//
// Two independent kinds of check, because they catch different mistakes:
//
//   * REFERENCE VALUES, computed in Python and pasted here, catch a
//     transcription slip.
//   * FINITE DIFFERENCES against the price function catch a WRONG FORMULA --
//     a dropped term or a flipped sign that a reference value computed from
//     the same wrong formula would happily confirm.
//
// The second is the one that matters. A greek with the wrong sign does not
// crash; it hedges the wrong way, and it does so quietly.
//
// No check description here may contain the substring FAIL.

#include <analytics/greeks.hpp>

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

bool rel_near(double a, double b, double tol)
{
    const double d = std::fabs(a - b);
    const double m = std::fabs(b) > 1.0 ? std::fabs(b) : 1.0;
    return d / m <= tol;
}

} // namespace

using namespace altair;

namespace {

// A realistic NIFTY case, everything in PAISE: forward 24'080, strike 24'000,
// 30 days, 15 vol, 6.5% rate.
constexpr Price kF{2'408'000};
constexpr Price kK{2'400'000};
const Years     kT{30.0 / 365.0};
const Vol       kVol{0.15};
constexpr double kRate = 0.065;

Greeks call_g()
{
    return *black76(OptionRight::Call, kF, kK, kT, kVol, kRate);
}
Greeks put_g()
{
    return *black76(OptionRight::Put, kF, kK, kT, kVol, kRate);
}

double px(OptionRight r, double F, double K, double T, double s, double rate)
{
    const auto g = black76(r, Price{static_cast<std::int64_t>(F)},
                           Price{static_cast<std::int64_t>(K)},
                           Years{T}, Vol{s}, rate);
    return g ? g->price : 0.0;
}

// ── 1 ────────────────────────────────────────────────────────────────────
void matches_independent_reference_values()
{
    std::printf("\n1 matches_independent_reference_values\n");
    const Greeks c = call_g();
    const Greeks p = put_g();

    std::printf("       call Rs %.2f  delta %.6f  vega/pt Rs %.2f  theta/day Rs %.2f\n",
                c.price / 100.0, c.delta,
                vega_per_vol_point(c.vega) / 100.0,
                theta_per_day(c.theta) / 100.0);

    check(rel_near(c.price, 45121.48867250209, 1e-12), "call price");
    check(rel_near(c.delta, 0.5365115263065505, 1e-12), "call delta");
    check(rel_near(c.gamma, 3.8133316391230524e-06, 1e-12), "call gamma");
    check(rel_near(c.vega, 272607.11789814255, 1e-12), "call vega");
    check(rel_near(c.theta, -245821.0983183424, 1e-12), "call theta");
    check(rel_near(c.rho, -3708.6155073289387, 1e-12), "call rho");

    check(rel_near(p.price, 37164.11443380938, 1e-12), "put price");
    check(rel_near(p.delta, -0.4581602535300388, 1e-12), "put delta");

    // Sanity that the numbers are the right SIZE, not just self-consistent.
    check(c.price > 40000.0 && c.price < 50000.0,
          "an ATM 30-day NIFTY call is around Rs 450, which it is");
    check(c.delta > 0.5 && c.delta < 0.6,
          "and its delta is a little over 0.5, as a slightly ITM call should be");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// The check that catches a wrong FORMULA rather than a wrong transcription.
void greeks_match_finite_differences()
{
    std::printf("\n2 greeks_match_finite_differences\n");
    const Greeks c = call_g();
    const double F = 2'408'000.0, K = 2'400'000.0;
    const double T = 30.0 / 365.0, s = 0.15;

    // delta = dV/dF, central difference.
    const double hF = 100.0;                       // one rupee
    const double fd_delta = (px(OptionRight::Call, F + hF, K, T, s, kRate)
                           - px(OptionRight::Call, F - hF, K, T, s, kRate))
                          / (2.0 * hF);
    check(rel_near(c.delta, fd_delta, 1e-6),
          "delta equals dV/dF measured by central difference");

    // gamma = d2V/dF2. Needs a wider step: the second difference cancels four
    // significant digits before it starts.
    const double hG = 20'000.0;                    // Rs 200
    const double fd_gamma = (px(OptionRight::Call, F + hG, K, T, s, kRate)
                           - 2.0 * px(OptionRight::Call, F, K, T, s, kRate)
                           + px(OptionRight::Call, F - hG, K, T, s, kRate))
                          / (hG * hG);
    check(rel_near(c.gamma, fd_gamma, 1e-4),
          "gamma equals d2V/dF2");

    // vega = dV/dsigma.
    const double hV = 1e-5;
    const double fd_vega = (px(OptionRight::Call, F, K, T, s + hV, kRate)
                          - px(OptionRight::Call, F, K, T, s - hV, kRate))
                         / (2.0 * hV);
    check(rel_near(c.vega, fd_vega, 1e-6),
          "vega equals dV/dsigma");

    // theta = dV/dt, and t runs OPPOSITE to T. Getting this sign wrong gives a
    // long option positive time decay, which no long option has.
    const double hT = 1e-6;
    const double fd_theta = -(px(OptionRight::Call, F, K, T + hT, s, kRate)
                            - px(OptionRight::Call, F, K, T - hT, s, kRate))
                           / (2.0 * hT);
    check(rel_near(c.theta, fd_theta, 1e-4),
          "theta equals -dV/dT -- the sign that decides whether a long option "
          "decays or accretes");
    check(c.theta < 0.0, "and it is NEGATIVE for a long option, as it must be");

    // rho = dV/dr.
    const double hR = 1e-8;
    const double fd_rho = (px(OptionRight::Call, F, K, T, s, kRate + hR)
                         - px(OptionRight::Call, F, K, T, s, kRate - hR))
                        / (2.0 * hR);
    check(rel_near(c.rho, fd_rho, 1e-4),
          "rho equals dV/dr -- under Black-76 the only rate dependence is the "
          "discount factor, so rho is -T*V and NOT the Black-Scholes-on-spot "
          "form, which would double-count the drift");
}

// ── 3 ────────────────────────────────────────────────────────────────────
void put_call_parity_holds()
{
    std::printf("\n3 put_call_parity_holds\n");
    const Greeks c = call_g();
    const Greeks p = put_g();

    const double resid = parity_residual(c.price, p.price, kF, kK, kT, kRate);
    check(std::fabs(resid) < 1e-6,
          "C - P equals df*(F - K) to within a millionth of a paisa");

    const double df = std::exp(-kRate * kT.raw());
    check(rel_near(c.delta - p.delta, df, 1e-12),
          "and delta_call - delta_put equals the discount factor exactly -- "
          "the call and put differ by a forward, whose delta IS df");

    check(c.gamma == p.gamma,
          "gamma is identical for call and put, bit for bit: they differ by a "
          "forward, which has no convexity");
    check(c.vega == p.vega,
          "and so is vega -- a forward has no vol sensitivity either");
}

// ── 4 ────────────────────────────────────────────────────────────────────
// Why norm_cdf is built on erfc.
void deep_tail_survives_cancellation()
{
    std::printf("\n4 deep_tail_survives_cancellation\n");

    // A call struck at 40'000 against a 24'080 forward: d1 is about -11.8.
    const auto g = black76(OptionRight::Call, kF, Price{4'000'000}, kT, kVol,
                           kRate);
    check(g.has_value(), "a far out-of-the-money call still prices");
    if (!g) { return; }

    check(g->delta > 0.0,
          "and its delta is strictly POSITIVE, not flushed to zero");
    check(rel_near(g->delta, 2.46825382849156e-32, 1e-10),
          "matching the reference to 2.47e-32");

    // What the textbook form would have produced at the same point.
    const double d1 = -11.779767948165478;
    const double naive = 0.5 * (1.0 + std::erf(d1 / std::sqrt(2.0)));
    const double ours  = detail::norm_cdf(d1);
    std::printf("       N(-11.78): erfc form %.6e   naive 0.5*(1+erf) %.6e\n",
                ours, naive);
    check(naive == 0.0,
          "0.5*(1 + erf(d1)) returns EXACTLY ZERO here -- every significant "
          "digit lost to cancellation");
    check(ours > 0.0,
          "while the erfc form keeps the value, which is the whole reason it "
          "is used: a delta of 1e-32 versus 0 is the difference between a "
          "hedge existing and not");
    check(g->price > 0.0, "and the price stays positive too");
}

// ── 5 ────────────────────────────────────────────────────────────────────
// D4. At expiry an option is an exercise decision, not a derivative.
void degenerate_inputs_are_refused()
{
    std::printf("\n5 degenerate_inputs_are_refused\n");

    const auto expired = black76(OptionRight::Call, kF, kK, Years{0.0}, kVol,
                                 kRate);
    check(!expired.has_value() && expired.error() == GreekError::Expired,
          "T = 0 is refused, NOT given a limit value -- gamma at the money at "
          "expiry is genuinely infinite, and a large number there would size a "
          "position on infinity");
    check(!black76(OptionRight::Call, kF, kK, Years{-1.0}, kVol, kRate)
               .has_value(), "and a negative T likewise");

    const auto novol = black76(OptionRight::Call, kF, kK, kT, Vol{0.0}, kRate);
    check(!novol.has_value() && novol.error() == GreekError::NonPositiveVol,
          "zero vol is refused: with no vol there is no distribution");

    check(!black76(OptionRight::Call, Price{0}, kK, kT, kVol, kRate).has_value(),
          "a zero forward is refused -- Black-76 takes its log");
    check(!black76(OptionRight::Call, kF, Price{0}, kT, kVol, kRate).has_value(),
          "and a zero strike");

    // Expiry has its own, well-defined answer.
    check(intrinsic(OptionRight::Call, kF, kK) == Price{8'000},
          "intrinsic() handles expiry instead: a 24'080 forward against a "
          "24'000 call is Rs 80 in the money");
    check(intrinsic(OptionRight::Call, kK, kF) == Price{0},
          "and an out-of-the-money call is worth zero, never negative");
    check(intrinsic(OptionRight::Put, kK, kF) == Price{8'000},
          "the put mirrors it");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void conventions_are_named_not_baked_in()
{
    std::printf("\n6 conventions_are_named_not_baked_in\n");
    const Greeks c = call_g();

    check(rel_near(vega_per_vol_point(c.vega), c.vega * 0.01, 1e-15),
          "vega_per_vol_point scales by 0.01 -- one vol POINT is 0.01 of sigma");
    check(rel_near(theta_per_day(c.theta), c.theta / 365.0, 1e-15),
          "theta_per_day divides by 365 CALENDAR days, because a weekend "
          "decays an option");

    // The raw values are the mathematical derivatives, so a caller who forgets
    // to scale gets an obviously wrong magnitude rather than a subtly wrong one.
    check(std::fabs(c.vega) > std::fabs(vega_per_vol_point(c.vega)) * 50.0,
          "the raw vega is ~100x the per-point figure, so applying the scaling "
          "twice or not at all is visible rather than plausible");
    std::printf("       vega raw %.1f paise, per vol point %.1f paise\n",
                c.vega, vega_per_vol_point(c.vega));
}

// ── 7 ────────────────────────────────────────────────────────────────────
void monotonicity_and_bounds()
{
    std::printf("\n7 monotonicity_and_bounds\n");

    // A call is worth more as the forward rises; a put less.
    double prev_c = -1.0, prev_p = 1e18;
    bool call_up = true, put_down = true;
    for (std::int64_t f = 2'200'000; f <= 2'600'000; f += 20'000) {
        const auto c = black76(OptionRight::Call, Price{f}, kK, kT, kVol, kRate);
        const auto p = black76(OptionRight::Put, Price{f}, kK, kT, kVol, kRate);
        if (!c || !p) { continue; }
        if (c->price <= prev_c) { call_up = false; }
        if (p->price >= prev_p) { put_down = false; }
        prev_c = c->price;
        prev_p = p->price;
    }
    check(call_up, "call value rises monotonically with the forward");
    check(put_down, "and put value falls");

    // Delta is bounded by the discount factor, not by 1.
    const double df = std::exp(-kRate * kT.raw());
    bool bounded = true;
    for (std::int64_t f = 1'000'000; f <= 5'000'000; f += 100'000) {
        const auto c = black76(OptionRight::Call, Price{f}, kK, kT, kVol, kRate);
        if (c && (c->delta < 0.0 || c->delta > df + 1e-12)) { bounded = false; }
    }
    check(bounded,
          "call delta stays within [0, df] -- bounded by the DISCOUNT FACTOR "
          "and not by 1, because Black-76 delta is with respect to the forward");

    // Vega and gamma are non-negative everywhere.
    bool nonneg = true;
    for (std::int64_t f = 1'000'000; f <= 5'000'000; f += 100'000) {
        const auto c = black76(OptionRight::Call, Price{f}, kK, kT, kVol, kRate);
        if (c && (c->vega < 0.0 || c->gamma < 0.0)) { nonneg = false; }
    }
    check(nonneg, "vega and gamma are non-negative across the whole strip");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void stability_approaching_expiry()
{
    std::printf("\n8 stability_approaching_expiry\n");

    // Walk T down towards zero at the money and confirm nothing blows up
    // before the refusal boundary.
    bool all_finite = true;
    double last_gamma = 0.0;
    for (double T = 1e-2; T > 1e-8; T *= 0.1) {
        const auto g = black76(OptionRight::Call, kK, kK, Years{T}, kVol, kRate);
        if (!g) { all_finite = false; break; }
        if (!std::isfinite(g->price) || !std::isfinite(g->gamma)) {
            all_finite = false;
            break;
        }
        last_gamma = g->gamma;
    }
    check(all_finite,
          "every value stays finite down to T = 1e-8 years, about a "
          "third of a second");
    check(last_gamma > 0.0,
          "and gamma keeps growing rather than collapsing to zero or NaN");
    std::printf("       ATM gamma at T=1e-8y: %.6e per paise\n", last_gamma);

    check(!black76(OptionRight::Call, kK, kK, Years{0.0}, kVol, kRate).has_value(),
          "and T = 0 is where it stops, by refusal rather than by overflow");
}

} // namespace

int main()
{
    std::printf("altair analytics greeks tests\n");
    matches_independent_reference_values();
    greeks_match_finite_differences();
    put_call_parity_holds();
    deep_tail_survives_cancellation();
    degenerate_inputs_are_refused();
    conventions_are_named_not_baked_in();
    monotonicity_and_bounds();
    stability_approaching_expiry();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
