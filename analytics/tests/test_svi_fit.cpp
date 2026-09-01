// P3-05b acceptance tests for analytics/svi_fit.hpp.
//
// What is actually being checked:
//
//   * that the fit reproduces the OBSERVED SURFACE, not that it recovers the
//     generating parameters. SVI is close to unidentifiable along one
//     direction -- several (a,b,rho,m,sigma) give nearly the same w(k) -- so
//     asserting parameter equality would be asserting something that is not
//     true and does not matter. What matters is w.
//
//   * that inverse-variance weighting from P3-04 measurably beats equal
//     weighting on the at-the-money strikes, which is the entire argument for
//     carrying the error bar through Phase 3.
//
//   * that the inner problem really is linear -- checked by perturbing (a,c,d)
//     away from the solve and confirming the objective is worse in every
//     direction.
//
// No check description here may contain the substring FAIL.

#include <analytics/svi_fit.hpp>
#include <analytics/greeks.hpp>
#include <analytics/iv.hpp>

#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cmath>
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

bool rel_near(double a, double b, double tol)
{
    const double d = std::fabs(a - b);
    const double m = std::fabs(b) > 1.0 ? std::fabs(b) : 1.0;
    return d / m <= tol;
}

class Lcg {
public:
    explicit Lcg(std::uint64_t seed) noexcept : s_(seed) {}
    /// Uniform in [-1, 1].
    double sym() noexcept {
        s_ = s_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return 2.0 * (static_cast<double>(s_ >> 11)
                      * (1.0 / 9007199254740992.0)) - 1.0;
    }
private:
    std::uint64_t s_;
};

} // namespace

using namespace altair;

namespace {

// The same realistic NIFTY 30-day slice P3-05a uses.
constexpr SviParams kTruth{0.0002, 0.019, -0.7, 0.01, 0.08};
const Years kT{30.0 / 365.0};
constexpr double kF = 2'408'000.0;
constexpr double kRate = 0.065;

constexpr int kStrikes = 21;
SviFitPoint g_exact[kStrikes];
SviFitPoint g_noisy_equal[kStrikes];
SviFitPoint g_noisy_weighted[kStrikes];
double g_true_w[kStrikes];
double g_iv_error[kStrikes];

/// Build a strike ladder spanning k in [-0.25, 0.25], the way a NIFTY chain
/// does, and give every point the error bar P3-04 says its quote actually has.
void build_ladder()
{
    Lcg rng{0x5717F17ULL};
    for (int i = 0; i < kStrikes; ++i) {
        const double k = -0.25 + 0.025 * static_cast<double>(i);
        const double w = svi_total_variance(kTruth, k);
        const double vol = std::sqrt(w / kT.raw());
        const double K = kF * std::exp(k);

        // Vega of this strike, and hence the IV uncertainty a one-tick price
        // uncertainty implies -- straight out of P3-04.
        const Greeks g = detail::black76_unchecked(OptionRight::Call, kF, K,
                                                   kT.raw(), vol, kRate);
        const double iv_err = iv_uncertainty(g.vega, 5.0);   // one 5-paise tick
        g_true_w[i] = w;
        g_iv_error[i] = iv_err;

        g_exact[i] = SviFitPoint{k, w, 1.0};

        // Perturb each quote by ONE TICK's worth of implied vol -- so the
        // wings, where vega has collapsed, move far more than the money
        // strikes. That is what a real chain looks like.
        //
        // CAPPED AT 5 VOL POINTS, and the cap is not cosmetic. iv_uncertainty
        // is a local linearisation, dsigma = dPrice/vega, and it stops meaning
        // anything once vega is small enough that a tick spans a large part of
        // the option's whole value: at k = +0.25 it reports an error bar of
        // 485 vol. Perturbing by that would build a test that no weighting
        // scheme could lose, and prove nothing. A real wing quote is wrong by
        // a few vol points, so that is what is simulated -- while the WEIGHT
        // still uses the true uncorrected error bar, since that is the
        // information a caller actually has.
        double dvol = iv_err * rng.sym();
        if (dvol > 0.05) { dvol = 0.05; }
        if (dvol < -0.05) { dvol = -0.05; }
        const double vol_obs = vol + dvol;
        const double w_obs = vol_obs * vol_obs * kT.raw();

        g_noisy_equal[i] = SviFitPoint{k, w_obs, 1.0};
        g_noisy_weighted[i] = SviFitPoint{
            k, w_obs, svi_weight_from_iv_error(iv_err, Vol{vol}, kT)};
    }
}

// ── 1 ────────────────────────────────────────────────────────────────────
void exact_data_is_fitted_exactly()
{
    std::printf("\n1 exact_data_is_fitted_exactly\n");
    const auto r = svi_fit(g_exact, kStrikes);
    check(r.has_value(), "a 21-strike ladder fits");
    if (!r) { return; }
    std::printf("    fitted a=%.8f b=%.6f rho=%+.4f m=%+.5f sigma=%.5f\n",
                r->params.a, r->params.b, r->params.rho, r->params.m,
                r->params.sigma);
    std::printf("    truth  a=%.8f b=%.6f rho=%+.4f m=%+.5f sigma=%.5f\n",
                kTruth.a, kTruth.b, kTruth.rho, kTruth.m, kTruth.sigma);
    std::printf("    weighted RMSE %.3e   worst residual %.3e at k=%+.3f"
                "   %d iterations\n",
                r->rmse, r->worst_residual, r->worst_k, r->iterations);

    check(svi_check_params(r->params).has_value(),
          "the fitted parameters satisfy every domain condition");
    check(r->rmse < 1e-9,
          "and reproduce exact data to under 1e-9 in total variance");

    // What matters is the SURFACE, not the parameters. Check w pointwise,
    // including between the fitted strikes.
    double worst = 0.0;
    for (int i = -300; i <= 300; ++i) {
        const double k = i * 0.001;
        const double e = std::fabs(svi_total_variance(r->params, k)
                                   - svi_total_variance(kTruth, k));
        if (e > worst) { worst = e; }
    }
    std::printf("    worst |w_fit - w_true| over k in [-0.3,0.3]: %.3e\n", worst);
    check(worst < 1e-8,
          "the fitted surface matches the true one BETWEEN the strikes too,"
          " not only at them");
    check(r->points == kStrikes, "every point carried weight");
    check(r->projections == 0,
          "and the final inner solve needed no projection into the domain");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// The card's reason for existing: weights from P3-04.
void inverse_variance_weighting_beats_equal_weighting()
{
    std::printf("\n2 inverse_variance_weighting_beats_equal_weighting\n");
    std::printf("    the quotes, with the error bar P3-04 gives each:\n");
    for (int i = 0; i < kStrikes; i += 5) {
        std::printf("      k=%+.3f  true vol %6.2f%%  IV error +/-%.4f"
                    "  weight %.3e\n",
                    g_exact[i].k,
                    100.0 * std::sqrt(g_true_w[i] / kT.raw()),
                    g_iv_error[i], g_noisy_weighted[i].weight);
    }
    // The certainty spread across the board. Note it is NOT symmetric: the
    // put wing carries a 32.8 vol, so it still has real vega and a tight error
    // bar, while the call wing at 14.9 vol has almost none. Skew makes the
    // reliable half of a chain the expensive half.
    double best = g_iv_error[0], worst = g_iv_error[0];
    for (int i = 1; i < kStrikes; ++i) {
        if (g_iv_error[i] < best) { best = g_iv_error[i]; }
        if (g_iv_error[i] > worst) { worst = g_iv_error[i]; }
    }
    std::printf("    tightest error bar %.6f, loosest %.4f -- a factor of"
                " %.3e across one chain\n", best, worst, worst / best);
    check(worst > 1000.0 * best,
          "the least certain quote on the board is over a thousand times"
          " looser than the most certain -- the hazard is real");
    check(g_iv_error[kStrikes - 1] > g_iv_error[0],
          "and the CALL wing is the uncertain one here, not the put wing,"
          " because skew leaves the puts with vega and the calls without");

    const auto eq = svi_fit(g_noisy_equal, kStrikes);
    const auto wt = svi_fit(g_noisy_weighted, kStrikes);
    check(eq && wt, "both fits converge");
    if (!eq || !wt) { return; }

    // Compare where it matters: the at-the-money strikes, in VOL, against the
    // truth the quotes were generated from.
    auto atm_error = [&](const SviParams& p) {
        double acc = 0.0;
        int cnt = 0;
        for (double k = -0.05; k <= 0.0500001; k += 0.005) {
            const double vf = std::sqrt(svi_total_variance(p, k) / kT.raw());
            const double vt = std::sqrt(svi_total_variance(kTruth, k)
                                        / kT.raw());
            acc += (vf - vt) * (vf - vt);
            ++cnt;
        }
        return std::sqrt(acc / cnt);
    };
    const double e_eq = atm_error(eq->params);
    const double e_wt = atm_error(wt->params);
    std::printf("    at-the-money vol RMSE vs truth:  equal weights %.6f"
                "   inverse-variance %.6f\n", e_eq, e_wt);
    std::printf("    -> weighting improves the money strikes by %.1fx\n",
                e_eq / e_wt);
    check(e_wt < e_eq,
          "inverse-variance weighting fits the money strikes better than"
          " equal weighting on the SAME quotes");
    check(e_wt < 0.5 * e_eq,
          "and by at least a factor of two, which is the cost of letting the"
          " least reliable quotes on the board pull hardest");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// The weight conversion. Fitting w but weighting by 1/dsigma^2 tilts the fit
// by a factor of sigma^2 -- four to one across a normal equity smile, and
// invisible in the output.
void the_weight_converts_from_vol_error_to_variance_error()
{
    std::printf("\n3 the_weight_converts_from_vol_error_to_variance_error\n");
    const Vol v{0.15};
    const double err = 0.01;                       // one vol point
    const double got = svi_weight_from_iv_error(err, v, kT);
    // dw = 2*sigma*T*dsigma; weight = 1/dw^2.
    const double dw = 2.0 * 0.15 * kT.raw() * err;
    std::printf("    vol 15%%, +/-1 vol point: dw = %.8f, weight = %.4e\n",
                dw, got);
    check(rel_near(got, 1.0 / (dw * dw), 1e-12),
          "the weight is 1/(2*sigma*T*dsigma)^2");

    // The tilt this avoids: at 30 vol versus 15 vol with the SAME vol error,
    // the correct weight differs by 4x. Weighting by 1/dsigma^2 would make
    // them equal.
    const double w15 = svi_weight_from_iv_error(err, Vol{0.15}, kT);
    const double w30 = svi_weight_from_iv_error(err, Vol{0.30}, kT);
    std::printf("    same vol error at 15%% and 30%%: weights differ %.2fx\n",
                w15 / w30);
    check(rel_near(w15 / w30, 4.0, 1e-12),
          "the same vol uncertainty on a 30 vol strike is a 4x larger"
          " variance uncertainty -- the tilt that skipping the conversion"
          " would bake in");

    check(svi_weight_from_iv_error(0.0, v, kT) == 0.0,
          "a zero error bar gives zero weight rather than infinity");
    const double inf = std::numeric_limits<double>::infinity();
    check(svi_weight_from_iv_error(inf, v, kT) == 0.0,
          "and an infinite one drops the point -- which is exactly what"
          " iv_uncertainty returns where vega has underflowed");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void a_fit_is_not_returned_without_its_arbitrage_scan()
{
    std::printf("\n4 a_fit_is_not_returned_without_its_arbitrage_scan\n");
    const auto r = svi_fit(g_noisy_weighted, kStrikes);
    check(r.has_value(), "the fit converges");
    if (!r) { return; }
    std::printf("    butterfly scan of the fit: min g %+.6f at k=%+.3f"
                " over %d points -> %s\n",
                r->butterfly.worst, r->butterfly.worst_k,
                r->butterfly.points, r->butterfly.clean ? "clean" : "DIRTY");
    check(r->butterfly.points > 0,
          "the result carries a scan that was actually run");
    check(r->butterfly.clean,
          "and this fit is butterfly free");

    // The scan must default to WIDER than the fitted data, because a surface
    // is invariably asked for a vol just outside the strikes it saw.
    const auto narrow = svi_butterfly_scan(r->params, -0.25, 0.25, 401);
    const auto wide = svi_butterfly_scan(r->params, -0.3125, 0.3125, 401);
    check(narrow && wide, "both explicit scans run");
    if (narrow && wide) {
        std::printf("    fitted range [-0.250,0.250] min g %+.6f;"
                    " padded [-0.3125,0.3125] min g %+.6f\n",
                    narrow->worst, wide->worst);
        check(wide->worst <= narrow->worst,
              "the padded scan is at least as severe as the fitted range --"
              " which is why the default pads");
    }
}

// ── 5 ────────────────────────────────────────────────────────────────────
// The inner problem is a linear least squares, so at fixed (m, sigma) its
// solution must be a strict minimum in every direction of (a, c, d).
void the_inner_problem_is_solved_exactly()
{
    std::printf("\n5 the_inner_problem_is_solved_exactly\n");
    const double m = kTruth.m, s = kTruth.sigma;
    const detail::SviInner in = detail::svi_inner_solve(g_exact, kStrikes, m, s);
    std::printf("    at the true (m,sigma): a=%.8f c=%.8f d=%.8f  sse=%.3e\n",
                in.a, in.c, in.d, in.sse);
    check(!in.projected, "the solve at the true (m,sigma) needs no projection");
    check(in.sse < 1e-20, "and reproduces exact data to machine precision");

    // c = b*rho*sigma and d = b*sigma must invert back to the truth.
    check(rel_near(in.d / s, kTruth.b, 1e-8), "d/sigma recovers b");
    check(rel_near(in.c / in.d, kTruth.rho, 1e-8), "c/d recovers rho");
    check(rel_near(in.a, kTruth.a, 1e-6), "and a is a directly");

    // Perturb in all six directions; every one must be worse.
    auto sse_at = [&](double a, double c, double d) {
        double acc = 0.0;
        for (int i = 0; i < kStrikes; ++i) {
            const double y = (g_exact[i].k - m) / s;
            const double z = std::sqrt(y * y + 1.0);
            const double r = a + c * y + d * z - g_exact[i].w;
            acc += g_exact[i].weight * r * r;
        }
        return acc;
    };
    const double h = 1e-6;
    int worse = 0;
    const double base = sse_at(in.a, in.c, in.d);
    if (sse_at(in.a + h, in.c, in.d) > base) { ++worse; }
    if (sse_at(in.a - h, in.c, in.d) > base) { ++worse; }
    if (sse_at(in.a, in.c + h, in.d) > base) { ++worse; }
    if (sse_at(in.a, in.c - h, in.d) > base) { ++worse; }
    if (sse_at(in.a, in.c, in.d + h) > base) { ++worse; }
    if (sse_at(in.a, in.c, in.d - h) > base) { ++worse; }
    check(worse == 6,
          "every perturbation of (a,c,d) increases the objective, so the"
          " linear solve found the true minimum and not a nearby point");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void the_domain_projection_keeps_infeasible_solves_legal()
{
    std::printf("\n6 the_domain_projection_keeps_infeasible_solves_legal\n");
    // A downward-sloping "smile": the unconstrained solve wants d < 0, which
    // is b < 0 and inverts the model. It must come back projected and legal.
    SviFitPoint down[9];
    for (int i = 0; i < 9; ++i) {
        const double k = -0.2 + 0.05 * i;
        down[i] = SviFitPoint{k, 0.004 - 0.010 * (k + 0.2), 1.0};
    }
    const detail::SviInner in = detail::svi_inner_solve(down, 9, 0.0, 0.1);
    std::printf("    monotone-decreasing data -> a=%.6f c=%.6f d=%.6f"
                " projected=%s\n", in.a, in.c, in.d,
                in.projected ? "yes" : "no");
    check(in.projected, "the solve reports that it had to project");
    check(in.d >= 0.0, "b is not allowed to come back negative");
    check(std::fabs(in.c) <= in.d + 1e-15, "and |rho| is not allowed past 1");
    const SviParams p = detail::svi_from_inner(in, 0.0, 0.1);
    check(svi_check_params(p).has_value(),
          "the projected parameters pass every domain condition");
    check(svi_slopes_within_lee(p), "and Lee's slope bound");

    // A flat slice has no identifiable skew, and rho must not be read off it.
    SviFitPoint flat[7];
    for (int i = 0; i < 7; ++i) {
        flat[i] = SviFitPoint{-0.15 + 0.05 * i, 0.002, 1.0};
    }
    const auto fr = svi_fit(flat, 7);
    check(fr.has_value(), "a flat slice fits");
    if (fr) {
        std::printf("    flat data -> b=%.3e rho=%+.4f\n",
                    fr->params.b, fr->params.rho);
        check(fr->params.b < 1e-6,
              "a flat slice fits with essentially zero slope");
        check(svi_check_params(fr->params).has_value(),
              "and is still a legal slice");
    }
}

// ── 7 ────────────────────────────────────────────────────────────────────
void degenerate_input_is_refused()
{
    std::printf("\n7 degenerate_input_is_refused\n");
    check(!svi_fit(g_exact, 4)
          && svi_fit(g_exact, 4).error() == SviFitError::TooFewPoints,
          "four points cannot fit five parameters");

    SviFitPoint same[8];
    for (auto& p : same) { p = SviFitPoint{0.01, 0.002, 1.0}; }
    check(!svi_fit(same, 8)
          && svi_fit(same, 8).error() == SviFitError::Degenerate,
          "eight quotes at ONE moneyness leave m and sigma unidentifiable");

    SviFitPoint bad[8];
    for (int i = 0; i < 8; ++i) {
        bad[i] = SviFitPoint{-0.1 + 0.03 * i, 0.002, 1.0};
    }
    bad[3].w = -0.001;
    check(!svi_fit(bad, 8)
          && svi_fit(bad, 8).error() == SviFitError::BadObservation,
          "a negative total variance is refused, not fitted around");
    bad[3].w = 0.002;
    bad[5].k = std::nan("");
    check(!svi_fit(bad, 8)
          && svi_fit(bad, 8).error() == SviFitError::BadObservation,
          "and so is a NaN moneyness");

    bad[5].k = 0.05;
    for (auto& p : bad) { p.weight = 0.0; }
    check(!svi_fit(bad, 8),
          "all-zero weights leave nothing to fit to");
}

// ── 8 ────────────────────────────────────────────────────────────────────
void the_fitted_slice_reprices_the_chain()
{
    std::printf("\n8 the_fitted_slice_reprices_the_chain\n");
    const auto r = svi_fit(g_noisy_weighted, kStrikes);
    check(r.has_value(), "the fit converges");
    if (!r) { return; }

    // End to end: fit -> vol -> black76 price -> P3-04 solver -> vol.
    int round_tripped = 0;
    double worst_vol_err = 0.0;
    for (int i = 0; i < kStrikes; ++i) {
        const double k = g_exact[i].k;
        const double K = kF * std::exp(k);
        const auto vol = svi_implied_vol(r->params, k, kT);
        if (!vol) { continue; }
        const Greeks g = detail::black76_unchecked(OptionRight::Call, kF, K,
                                                   kT.raw(), vol->raw(), kRate);
        const auto back = detail::solve_iv(OptionRight::Call, g.price, kF, K,
                                           kT.raw(), kRate);
        if (back && std::fabs(back->vol.raw() - vol->raw()) < 1e-9) {
            ++round_tripped;
        }
        const double truth = std::sqrt(svi_total_variance(kTruth, k)
                                       / kT.raw());
        const double e = std::fabs(vol->raw() - truth);
        if (e > worst_vol_err) { worst_vol_err = e; }
    }
    std::printf("    %d/%d strikes round-trip through black76 and the IV"
                " solver\n", round_tripped, kStrikes);
    std::printf("    worst vol error against the generating slice: %.5f"
                " (%.2f vol points)\n", worst_vol_err, worst_vol_err * 100.0);
    check(round_tripped == kStrikes,
          "every fitted strike prices and inverts back to its own vol");
    check(worst_vol_err < 0.05,
          "and the whole chain is within 5 vol points of the truth despite"
          " one-tick noise on every quote");
    std::printf("    %d Nelder-Mead iterations on (m, log sigma)\n",
                r->iterations);
    check(r->iterations < 400, "the search converged inside its budget");
}

} // namespace

int main()
{
    std::printf("altair analytics svi_fit tests\n");
    build_ladder();
    exact_data_is_fitted_exactly();
    inverse_variance_weighting_beats_equal_weighting();
    the_weight_converts_from_vol_error_to_variance_error();
    a_fit_is_not_returned_without_its_arbitrage_scan();
    the_inner_problem_is_solved_exactly();
    the_domain_projection_keeps_infeasible_solves_legal();
    degenerate_input_is_refused();
    the_fitted_slice_reprices_the_chain();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
