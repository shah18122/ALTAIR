// P3-02 acceptance tests for analytics/greeks2.hpp.
//
// Higher-order greeks are the easiest thing in this repo to get subtly wrong,
// because nothing about a wrong one looks wrong. So every single greek here is
// checked TWICE by independent means:
//
//   * against a reference value computed in Python, which catches a
//     transcription slip;
//   * against a FINITE DIFFERENCE of the P3-01 first-order greeks, which
//     catches a wrong formula -- a dropped term or a flipped sign that a
//     reference computed the same wrong way would happily confirm.
//
// Vanna gets a third check that neither of those provides: it is a mixed
// second partial, so d(vega)/dF and d(delta)/dsigma must be the same number
// (Clairaut). Computing it both ways from the price function and getting one
// answer is strong evidence the whole construction is right.
//
// No check description here may contain the substring FAIL.

#include <analytics/greeks2.hpp>

#include <cstdio>
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
    const double m = std::fabs(b) > 1e-300 ? std::fabs(b) : 1e-300;
    return d / m <= tol;
}

} // namespace

using namespace altair;

namespace {

// The same NIFTY frame as P3-01: forward 24'080, strike 24'000, 30 days,
// 15 vol, 6.5% rate. Everything in paise.
constexpr double kF = 2'408'000.0;
constexpr double kK = 2'400'000.0;
constexpr double kTv = 30.0 / 365.0;
constexpr double kVol = 0.15;
constexpr double kRate = 0.065;

HigherGreeks higher(OptionRight r)
{
    return *black76_higher(r, Price{2'408'000}, Price{2'400'000}, Years{kTv},
                           Vol{kVol}, kRate);
}

/// First-order greeks at arbitrary (F, T, sigma), in doubles, so a finite
/// difference can be taken in any direction without quantising the forward.
Greeks g_at(OptionRight r, double F, double T, double s)
{
    return detail::black76_unchecked(r, F, kK, T, s, kRate);
}

// ── 1 ────────────────────────────────────────────────────────────────────
void matches_independent_reference_values()
{
    std::printf("\n1 matches_independent_reference_values\n");
    const HigherGreeks c = higher(OptionRight::Call);
    const HigherGreeks p = higher(OptionRight::Put);

    check(rel_near(c.vanna, -0.1471118062312705, 1e-12), "vanna");
    check(rel_near(c.volga, 10042.72856754042, 1e-12), "volga");
    check(rel_near(c.speed, -5.225074646492149e-12, 1e-12), "speed");
    check(rel_near(c.zomma, -2.5281729435716006e-05, 1e-12), "zomma");
    check(rel_near(c.ultima, -222890.66604954892, 1e-12), "ultima");
    check(rel_near(c.charm, 0.16911277239596015, 1e-12), "charm, call");
    check(rel_near(p.charm, 0.10445910670658182, 1e-12), "charm, put");
    check(rel_near(c.veta, -1649804.4943682018, 1e-12), "veta");
    check(rel_near(c.colour, 2.331744466663385e-05, 1e-12), "colour");
    check(c.basis == GreekBasis::Forward && p.basis == GreekBasis::Forward,
          "the basis is stamped Forward, never left Unspecified");
    check(HigherGreeks{}.basis == GreekBasis::Unspecified,
          "and a default-constructed struct does not read as a valid basis");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// Every greek against a central difference of the P3-01 functions. This is
// the check that catches a WRONG FORMULA rather than a mistyped digit.
void every_greek_matches_a_finite_difference()
{
    std::printf("\n2 every_greek_matches_a_finite_difference\n");
    const HigherGreeks c = higher(OptionRight::Call);

    const double hF = kF * 1e-5;
    const double hs = 1e-6;
    const double hT = 1e-7;

    // vanna = d(vega)/dF
    const double fd_vanna_F =
        (g_at(OptionRight::Call, kF + hF, kTv, kVol).vega
       - g_at(OptionRight::Call, kF - hF, kTv, kVol).vega) / (2.0 * hF);
    // vanna = d(delta)/dsigma -- the SAME mixed partial, the other way round
    const double fd_vanna_s =
        (g_at(OptionRight::Call, kF, kTv, kVol + hs).delta
       - g_at(OptionRight::Call, kF, kTv, kVol - hs).delta) / (2.0 * hs);
    std::printf("    vanna  analytic %.12e   d(vega)/dF %.12e   d(delta)/ds %.12e\n",
                c.vanna, fd_vanna_F, fd_vanna_s);
    check(rel_near(c.vanna, fd_vanna_F, 1e-5), "vanna equals d(vega)/dF");
    check(rel_near(c.vanna, fd_vanna_s, 1e-5), "vanna equals d(delta)/dsigma");
    check(rel_near(fd_vanna_F, fd_vanna_s, 1e-4),
          "and the two mixed partials agree with EACH OTHER -- Clairaut holds,"
          " so the construction is consistent independently of my algebra");

    const double fd_volga =
        (g_at(OptionRight::Call, kF, kTv, kVol + hs).vega
       - g_at(OptionRight::Call, kF, kTv, kVol - hs).vega) / (2.0 * hs);
    check(rel_near(c.volga, fd_volga, 1e-5), "volga equals d(vega)/dsigma");

    const double fd_speed =
        (g_at(OptionRight::Call, kF + hF, kTv, kVol).gamma
       - g_at(OptionRight::Call, kF - hF, kTv, kVol).gamma) / (2.0 * hF);
    check(rel_near(c.speed, fd_speed, 1e-4), "speed equals d(gamma)/dF");

    const double fd_zomma =
        (g_at(OptionRight::Call, kF, kTv, kVol + hs).gamma
       - g_at(OptionRight::Call, kF, kTv, kVol - hs).gamma) / (2.0 * hs);
    check(rel_near(c.zomma, fd_zomma, 1e-5), "zomma equals d(gamma)/dsigma");

    // The time greeks carry a SIGN FLIP: they are per calendar time, so they
    // are minus the T-derivative. Getting this backwards is invisible in the
    // magnitude and wrong in every hedge.
    const double fd_charm = -(g_at(OptionRight::Call, kF, kTv + hT, kVol).delta
                            - g_at(OptionRight::Call, kF, kTv - hT, kVol).delta)
                          / (2.0 * hT);
    check(rel_near(c.charm, fd_charm, 1e-4),
          "charm equals MINUS d(delta)/dT, the calendar-time convention");

    const double fd_veta = -(g_at(OptionRight::Call, kF, kTv + hT, kVol).vega
                           - g_at(OptionRight::Call, kF, kTv - hT, kVol).vega)
                         / (2.0 * hT);
    check(rel_near(c.veta, fd_veta, 1e-4), "veta equals MINUS d(vega)/dT");

    const double fd_colour = -(g_at(OptionRight::Call, kF, kTv + hT, kVol).gamma
                             - g_at(OptionRight::Call, kF, kTv - hT, kVol).gamma)
                           / (2.0 * hT);
    check(rel_near(c.colour, fd_colour, 1e-4), "colour equals MINUS d(gamma)/dT");

    // Ultima is third order: difference volga, which is itself second order.
    auto volga_at = [&](double s) {
        const double a = g_at(OptionRight::Call, kF, kTv, s + hs).vega;
        const double b = g_at(OptionRight::Call, kF, kTv, s - hs).vega;
        return (a - b) / (2.0 * hs);
    };
    const double fd_ultima = (volga_at(kVol + 1e-4) - volga_at(kVol - 1e-4))
                           / (2.0 * 1e-4);
    std::printf("    ultima analytic %.6e   finite difference %.6e\n",
                c.ultima, fd_ultima);
    check(rel_near(c.ultima, fd_ultima, 1e-3),
          "ultima equals d(volga)/dsigma to third-order difference accuracy");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// The asymmetry. Six greeks are right-independent; charm is not, by exactly
// r*df. A "they are all right-independent" simplification looks harmless.
void only_charm_depends_on_the_right()
{
    std::printf("\n3 only_charm_depends_on_the_right\n");
    const HigherGreeks c = higher(OptionRight::Call);
    const HigherGreeks p = higher(OptionRight::Put);

    check(c.vanna == p.vanna, "vanna is identical for call and put");
    check(c.volga == p.volga, "volga is identical");
    check(c.speed == p.speed, "speed is identical");
    check(c.zomma == p.zomma, "zomma is identical");
    check(c.colour == p.colour, "colour is identical");
    check(c.ultima == p.ultima, "ultima is identical");
    check(c.veta == p.veta, "veta is identical");

    const double df = std::exp(-kRate * kTv);
    std::printf("    charm call %.12f   put %.12f   difference %.12f"
                "   r*df %.12f\n",
                c.charm, p.charm, c.charm - p.charm, kRate * df);
    check(c.charm != p.charm,
          "charm is NOT -- the hazard exists rather than being hypothetical");
    check(rel_near(c.charm - p.charm, kRate * df, 1e-12),
          "and the difference is exactly r*df, because call and put delta"
          " differ by the discount factor, which itself moves with time");

    // The reason the other six ARE identical: a call minus a put is a forward,
    // which has no convexity, no vol sensitivity, and a delta of df.
    const Greeks gc = g_at(OptionRight::Call, kF, kTv, kVol);
    const Greeks gp = g_at(OptionRight::Put, kF, kTv, kVol);
    check(rel_near(gc.delta - gp.delta, df, 1e-14),
          "call delta minus put delta is the discount factor, which is the"
          " whole reason for the asymmetry");
    check(rel_near(gc.gamma, gp.gamma, 1e-15)
          && rel_near(gc.vega, gp.vega, 1e-15),
          "while gamma and vega are already right-independent at first order");
}

// ── 4 ────────────────────────────────────────────────────────────────────
// The greeks that exist because a hedge is not linear. Checked against the
// actual re-priced position rather than against themselves.
void the_hedge_corrections_do_what_they_claim()
{
    std::printf("\n4 the_hedge_corrections_do_what_they_claim\n");
    const HigherGreeks c = higher(OptionRight::Call);
    const Greeks base = g_at(OptionRight::Call, kF, kTv, kVol);

    // A 3-vol move: what the delta hedge actually needs to become.
    const double dvol = 0.03;
    const Greeks moved = g_at(OptionRight::Call, kF, kTv, kVol + dvol);
    const double predicted = delta_after_vol_move(base.delta, c.vanna, dvol);
    std::printf("    delta at 15 vol %.6f, at 18 vol %.6f, vanna predicts"
                " %.6f\n", base.delta, moved.delta, predicted);
    check(std::fabs(predicted - moved.delta)
          < std::fabs(base.delta - moved.delta),
          "the vanna correction moves the delta toward its true value");
    const double err_raw = std::fabs(base.delta - moved.delta);
    const double err_fix = std::fabs(predicted - moved.delta);
    std::printf("      uncorrected error %.6f, vanna-corrected %.6f"
                " -- %.1fx better\n", err_raw, err_fix, err_raw / err_fix);
    check(err_fix < 0.5 * err_raw,
          "and at least halves the error -- a book re-hedged on price alone"
          " drifts systematically every time vol moves");

    // Volga: vega is not linear in vol.
    const double vega_pred = vega_after_vol_move(base.vega, c.volga, dvol);
    std::printf("    vega at 15 vol %.1f, at 18 vol %.1f, volga predicts"
                " %.1f\n", base.vega, moved.vega, vega_pred);
    check(std::fabs(vega_pred - moved.vega)
          < std::fabs(base.vega - moved.vega),
          "the volga correction moves vega toward its true value");
    check(std::fabs(base.vega - moved.vega) > 200.0,
          "and the linear error over 3 vols is not negligible -- a"
          " vega-neutral book is not vol-neutral");

    // Charm: the overnight re-hedge, with nothing having happened.
    const double one_day = 1.0 / 365.0;
    const Greeks tomorrow = g_at(OptionRight::Call, kF, kTv - one_day, kVol);
    std::printf("    delta today %.6f, tomorrow %.6f (same price, same vol);"
                " charm/day predicts %+.6f\n",
                base.delta, tomorrow.delta, per_day(c.charm));
    // 5%, not tighter: charm per day extrapolates linearly across a whole
    // day, and charm itself changes over that day. The residual is the
    // second-order term, not an error in charm -- halving the step halves it,
    // which is what the second check confirms.
    check(rel_near(tomorrow.delta - base.delta, per_day(c.charm), 0.05),
          "charm per day predicts the overnight delta drift to 5%");
    const double half = 0.5 / 365.0;
    const Greeks midday = g_at(OptionRight::Call, kF, kTv - half, kVol);
    const double e_full = std::fabs((tomorrow.delta - base.delta)
                                    - per_day(c.charm));
    const double e_half = std::fabs((midday.delta - base.delta)
                                    - 0.5 * per_day(c.charm));
    std::printf("      extrapolation error over 1 day %.3e, over half a day"
                " %.3e -- ratio %.2f\n", e_full, e_half, e_full / e_half);
    check(e_half < 0.75 * e_full,
          "and halving the step shrinks the residual, confirming it is the"
          " second-order term rather than a wrong charm");
    check(std::fabs(tomorrow.delta - base.delta) > 0.0002,
          "and that drift is real: a position flat at the close is not flat"
          " at the open with nothing having happened");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void degenerate_inputs_are_refused()
{
    std::printf("\n5 degenerate_inputs_are_refused\n");
    auto e = [](double T, double s, std::int64_t F, std::int64_t K) {
        const auto r = black76_higher(OptionRight::Call, Price{F}, Price{K},
                                      Years{T}, Vol{s}, kRate);
        return r ? GreekError::NotFinite : r.error();
    };
    check(e(0.0, kVol, 2'408'000, 2'400'000) == GreekError::Expired,
          "T = 0 is Expired -- the higher greeks diverge faster than gamma");
    check(e(-1.0, kVol, 2'408'000, 2'400'000) == GreekError::Expired,
          "and so is a negative T");
    check(e(kTv, 0.0, 2'408'000, 2'400'000) == GreekError::NonPositiveVol,
          "sigma = 0 is refused");
    check(e(kTv, kVol, 0, 2'400'000) == GreekError::NonPositivePrice,
          "a zero forward is refused");
    check(e(kTv, kVol, 2'408'000, 0) == GreekError::NonPositivePrice,
          "a zero strike is refused");
    check(e(kTv, kVol, -1, 2'400'000) == GreekError::NonPositivePrice,
          "a negative forward is refused");
}

// ── 6 ────────────────────────────────────────────────────────────────────
// Behaviour across the surface, where a sign error hides. These are shape
// facts that hold for any correct implementation, checked without reference
// numbers.
void the_shapes_are_right_across_the_surface()
{
    std::printf("\n6 the_shapes_are_right_across_the_surface\n");
    // Volga vanishes at the money and is positive in both wings: a
    // near-the-money option has almost linear vega, a wing option does not.
    // That is why wing options are the vol-convexity instrument.
    const double atm_k = kK * std::exp(0.5 * kVol * kVol * kTv);  // d1=d2=0 pt
    int wings_positive = 0;
    double atm_volga = 1e9;
    for (double mult : {0.80, 0.90, 1.10, 1.25}) {
        const auto h = black76_higher(
            OptionRight::Call, Price{static_cast<std::int64_t>(kF * mult)},
            Price{2'400'000}, Years{kTv}, Vol{kVol}, kRate);
        if (h && h->volga > 0.0) { ++wings_positive; }
    }
    // On the EXACT forward. Rounding it to whole paise moves d2 off zero by
    // 2e-6 and volga -- which is proportional to d2 -- from 7e-11 to 0.155.
    // That is the reason black76_higher_unchecked exists.
    atm_volga = std::fabs(detail::black76_higher_unchecked(
        OptionRight::Call, atm_k, kK, kTv, kVol, kRate).volga);
    std::printf("    volga: %d of 4 wing strikes positive; at the forward-atm"
                " point |volga| = %.3e\n", wings_positive, atm_volga);
    check(wings_positive == 4, "volga is positive in both wings");
    check(atm_volga < 1e-9,
          "and vanishes where d1 = d2 = 0 -- vega is locally linear at the"
          " money, which is why the wings carry the vol convexity");

    // Vanna changes sign across the money: raising vol lifts an OTM call
    // delta and lowers an ITM one.
    const auto otm = black76_higher(OptionRight::Call, Price{2'408'000},
                                    Price{2'800'000}, Years{kTv}, Vol{kVol},
                                    kRate);
    const auto itm = black76_higher(OptionRight::Call, Price{2'408'000},
                                    Price{2'000'000}, Years{kTv}, Vol{kVol},
                                    kRate);
    check(otm && itm, "both wings evaluate");
    if (otm && itm) {
        std::printf("    vanna: OTM call %+.6e, ITM call %+.6e\n",
                    otm->vanna, itm->vanna);
        check(otm->vanna > 0.0 && itm->vanna < 0.0,
              "vanna is positive for an OTM call and negative for an ITM one"
              " -- more vol makes the far strike more likely and the near"
              " strike less certain");
    }

    // Gamma is not constant in the forward, which is what speed says. A gamma
    // limit set at one price is not the limit that binds at another.
    const Greeks near = g_at(OptionRight::Call, kF, kTv, kVol);
    const Greeks up = g_at(OptionRight::Call, kF * 1.02, kTv, kVol);
    std::printf("    gamma at F %.4e, at F*1.02 %.4e -- a %.0f%% change\n",
                near.gamma, up.gamma, 100.0 * (up.gamma / near.gamma - 1.0));
    check(std::fabs(up.gamma / near.gamma - 1.0) > 0.05,
          "a 2% move changes gamma by more than 5%, so a gamma limit set at"
          " one price is not the limit that binds at another");
}

} // namespace

int main()
{
    std::printf("altair analytics greeks2 tests\n");
    matches_independent_reference_values();
    every_greek_matches_a_finite_difference();
    only_charm_depends_on_the_right();
    the_hedge_corrections_do_what_they_claim();
    degenerate_inputs_are_refused();
    the_shapes_are_right_across_the_surface();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
