// P3-08 acceptance tests for analytics/vix.hpp.
//
// The load-bearing check is that a FLAT surface at a known vol returns that
// vol. VIX is a variance-swap replication, so on a flat 15-vol surface the
// answer is exactly 15 -- no model, no fitting, nothing to tune. Anything else
// is discretisation, truncation, or a bug, and the tests separate the three by
// varying one at a time.
//
// Chains are built with black76 from a KNOWN vol surface, which is the only
// way to have a right answer to compare against. That is a legitimate use of
// the pricer here and not circular: nothing in vix.hpp prices an option, so
// the two share no code and no assumptions.
//
// No check description here may contain the substring FAIL.

#include <analytics/vix.hpp>
#include <analytics/greeks.hpp>
#include <analytics/svi.hpp>

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

constexpr double kF = 2'408'000.0;      // NIFTY forward, paise
constexpr double kRate = 0.065;
constexpr double kT30 = 30.0 / 365.0;

constexpr std::size_t kMaxStrikes = 1200;
VixQuote g_chain[kMaxStrikes];

/// Build a chain from a vol function. `step` in paise, span in log-moneyness.
/// Returns the number of strikes written.
template <typename VolFn>
std::size_t build_chain(VixQuote* out, double step, double span, double T,
                        VolFn vol_at)
{
    const double lo = kF * std::exp(-span);
    const double hi = kF * std::exp(span);
    double k = std::floor(lo / step) * step;
    std::size_t n = 0;
    while (k <= hi && n < kMaxStrikes) {
        if (k > 0.0) {
            const double s = vol_at(std::log(k / kF));
            const Greeks c = detail::black76_unchecked(OptionRight::Call, kF, k,
                                                       T, s, kRate);
            const Greeks p = detail::black76_unchecked(OptionRight::Put, kF, k,
                                                       T, s, kRate);
            out[n].strike = Price{static_cast<std::int64_t>(k)};
            out[n].call = Price{static_cast<std::int64_t>(c.price + 0.5)};
            out[n].put = Price{static_cast<std::int64_t>(p.price + 0.5)};
            out[n].has_call = true;
            out[n].has_put = true;
            ++n;
        }
        k += step;
    }
    return n;
}

auto flat15 = [](double) { return 0.15; };

// The realistic NIFTY slice from P3-05: 15.06 at the money, with skew.
constexpr SviParams kSlice{0.0002, 0.019, -0.7, 0.01, 0.08};
auto skewed = [](double k) {
    return std::sqrt(svi_total_variance(kSlice, k) / kT30);
};

// ── 1 ────────────────────────────────────────────────────────────────────
// A flat 15-vol surface must return 15.00. No model, no fitting.
void a_flat_surface_returns_its_own_vol()
{
    std::printf("\n1 a_flat_surface_returns_its_own_vol\n");
    const std::size_t n = build_chain(g_chain, 5'000.0, 0.30, kT30, flat15);
    const auto t = vix_term_variance(g_chain, n, Years{kT30}, kRate);
    check(t.has_value(), "a 295-strike flat chain evaluates");
    if (!t) { return; }
    const double vix = 100.0 * std::sqrt(t->variance);
    std::printf("    %zu strikes at Rs 50: VIX %.4f  (truth 15.00,"
                " error %+.4f)\n", n, vix, vix - 15.0);
    std::printf("    forward %.2f (truth %.2f)   K0 %.0f   correction %.3e\n",
                t->forward, kF, t->k0, t->correction);
    check(std::fabs(vix - 15.0) < 0.01,
          "the replication returns 15.00 to within 0.01 vol points");
    check(rel_near(t->forward, kF, 1e-6),
          "and the parity forward recovers the true forward");
    check(t->k0 <= t->forward,
          "K0 is at or below the forward, as the method requires");
    check(t->correction < 0.0,
          "the K0 correction is negative -- it always is, and dropping it"
          " biases the index upward");
    check(t->strikes_used == static_cast<int>(n),
          "every strike entered the strip on a fully quoted chain");
    check(t->truncated_low == 0 && t->truncated_high == 0,
          "and none was truncated");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// Discretisation. The integral is approximated by whatever strikes exist, so
// the answer depends on the ladder, and the bias is UPWARD.
void a_coarse_strike_ladder_overstates_the_index()
{
    std::printf("\n2 a_coarse_strike_ladder_overstates_the_index\n");
    struct Row { double step_rs; double vix; };
    Row rows[5];
    int i = 0;
    for (double step_rs : {50.0, 100.0, 250.0, 500.0, 1000.0}) {
        const std::size_t n = build_chain(g_chain, step_rs * 100.0, 0.50,
                                          kT30, flat15);
        const auto t = vix_term_variance(g_chain, n, Years{kT30}, kRate);
        if (!t) { check(false, "coarse chain evaluates"); return; }
        rows[i] = Row{step_rs, 100.0 * std::sqrt(t->variance)};
        std::printf("    Rs %6.0f step, %4zu strikes: VIX %8.4f   bias %+.4f\n",
                    step_rs, n, rows[i].vix, rows[i].vix - 15.0);
        ++i;
    }
    check(rows[0].vix < rows[3].vix,
          "a coarser ladder reports a higher index on identical vol");
    check(std::fabs(rows[0].vix - 15.0) < 0.01,
          "NIFTY's real Rs 50 step is accurate to 0.01 vol points");
    check(rows[3].vix - 15.0 > 0.2,
          "a Rs 500 step is out by more than 0.2 -- a thin chain reports"
          " more fear than there is");
    check(rows[4].vix > rows[3].vix,
          "and it keeps getting worse, monotonically, as the ladder thins");
    bool monotone = true;
    for (int j = 1; j < 5; ++j) {
        if (rows[j].vix <= rows[j - 1].vix) { monotone = false; }
    }
    check(monotone, "the bias is monotone in strike spacing, so it is"
                    " discretisation and not noise");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// The fact people read wrong.
void the_index_is_not_the_at_the_money_vol()
{
    std::printf("\n3 the_index_is_not_the_at_the_money_vol\n");
    const std::size_t n = build_chain(g_chain, 5'000.0, 0.50, kT30, skewed);
    const auto t = vix_term_variance(g_chain, n, Years{kT30}, kRate);
    check(t.has_value(), "the skewed chain evaluates");
    if (!t) { return; }
    const double vix = 100.0 * std::sqrt(t->variance);
    const double atm = 100.0 * std::sqrt(svi_total_variance(kSlice, 0.0) / kT30);
    std::printf("    ATM vol %.4f   VIX %.4f   uplift %+.4f\n",
                atm, vix, vix - atm);
    check(vix > atm,
          "on a skewed surface the index EXCEEDS the at-the-money vol");
    check(vix - atm > 1.0,
          "by more than a vol point -- that gap IS the skew, not a signal");

    // Prove it is the skew by removing the skew: a flat surface at the SAME
    // at-the-money vol must give back roughly that vol.
    auto flat_at_atm = [&](double) {
        return std::sqrt(svi_total_variance(kSlice, 0.0) / kT30);
    };
    const std::size_t n2 = build_chain(g_chain, 5'000.0, 0.50, kT30,
                                       flat_at_atm);
    const auto t2 = vix_term_variance(g_chain, n2, Years{kT30}, kRate);
    check(t2.has_value(), "the flattened chain evaluates");
    if (t2) {
        const double vix_flat = 100.0 * std::sqrt(t2->variance);
        std::printf("    same ATM vol with the skew REMOVED: VIX %.4f\n",
                    vix_flat);
        check(std::fabs(vix_flat - atm) < 0.02,
              "with no skew the index and the at-the-money vol agree, which"
              " is what identifies the gap above as the skew");
    }
}

// ── 4 ────────────────────────────────────────────────────────────────────
// Truncation always loses variance. The wings are cheap and easy to dismiss,
// and they are where a variance swap gets its convexity.
void truncating_the_wings_understates_the_index()
{
    std::printf("\n4 truncating_the_wings_understates_the_index\n");
    // The span that matters is not in log-moneyness, it is in STANDARD
    // DEVIATIONS, sigma*sqrt(T). At 30 days and 15 vol that is 0.043, so a
    // NIFTY chain listed out to +/-0.5 is more than eleven sigma wide and
    // truncation costs nothing at all. The loss only appears once the chain is
    // cut inside about two sigma -- which is where a weekly on a volatile day,
    // or a single stock with a short ladder, actually sits.
    const double sd = 0.15 * std::sqrt(kT30);
    std::printf("    one standard deviation is %.4f in log-moneyness\n", sd);
    struct Row { double sigmas; double vix; int used; };
    Row rows[5];
    int i = 0;
    for (double sigmas : {11.6, 2.8, 1.9, 1.4, 0.9}) {
        const std::size_t n = build_chain(g_chain, 5'000.0, sigmas * sd,
                                          kT30, flat15);
        const auto t = vix_term_variance(g_chain, n, Years{kT30}, kRate);
        if (!t) { check(false, "narrow chain evaluates"); return; }
        rows[i] = Row{sigmas, 100.0 * std::sqrt(t->variance), t->strikes_used};
        std::printf("    range +/-%5.1f sigma, %3d strikes:"
                    " VIX %8.4f   %+.4f\n",
                    sigmas, rows[i].used, rows[i].vix, rows[i].vix - 15.0);
        ++i;
    }
    check(std::fabs(rows[1].vix - rows[0].vix) < 0.02,
          "cutting a 30-day NIFTY chain at 2.8 sigma costs almost nothing --"
          " the wings out there really are worthless");
    check(rows[4].vix < rows[0].vix - 0.5,
          "but cutting it at 0.9 sigma loses over half a vol point");
    check(rows[4].vix < 15.0,
          "truncation biases DOWNWARD, the opposite direction from coarse"
          " strikes, so the two do not cancel and must be judged separately");
    bool monotone = true;
    for (int j = 1; j < 5; ++j) {
        if (rows[j].vix > rows[j - 1].vix + 1e-9) { monotone = false; }
    }
    check(monotone,
          "and the loss grows monotonically as the range narrows, so it is"
          " truncation and not noise");

    // The truncation counters must actually fire when quotes go missing.
    const std::size_t n = build_chain(g_chain, 5'000.0, 0.50, kT30, flat15);
    for (std::size_t j = 0; j < 6; ++j) { g_chain[j].has_put = false; }
    for (std::size_t j = n - 6; j < n; ++j) { g_chain[j].has_call = false; }
    const auto t = vix_term_variance(g_chain, n, Years{kT30}, kRate);
    check(t.has_value(), "a chain with dead wings still evaluates");
    if (t) {
        std::printf("    with 6 dead strikes each side: truncated %d low,"
                    " %d high, %d used\n",
                    t->truncated_low, t->truncated_high, t->strikes_used);
        check(t->truncated_low >= kVixTruncateAfter
              && t->truncated_high >= kVixTruncateAfter,
              "missing quotes are counted, on both sides");
        check(t->strikes_used < static_cast<int>(n),
              "and the strip is shorter than the chain");
    }
}

// ── 5 ────────────────────────────────────────────────────────────────────
// A single hole near the money must NOT end the strip. That is why the rule
// is two consecutive gaps rather than one.
void one_illiquid_strike_does_not_end_the_strip()
{
    std::printf("\n5 one_illiquid_strike_does_not_end_the_strip\n");
    const std::size_t n = build_chain(g_chain, 5'000.0, 0.30, kT30, flat15);
    const auto full = vix_term_variance(g_chain, n, Years{kT30}, kRate);
    check(full.has_value(), "the full chain evaluates");
    if (!full) { return; }

    // Punch a single hole a few strikes below the money.
    std::size_t hole = 0;
    for (std::size_t j = 0; j < n; ++j) {
        if (static_cast<double>(g_chain[j].strike.raw()) > kF * 0.97) {
            hole = j; break;
        }
    }
    g_chain[hole].has_put = false;
    const auto holed = vix_term_variance(g_chain, n, Years{kT30}, kRate);
    check(holed.has_value(), "a chain with one hole evaluates");
    if (!holed) { return; }
    std::printf("    full %d strikes -> VIX %.4f;  one hole -> %d strikes,"
                " VIX %.4f\n", full->strikes_used,
                100.0 * std::sqrt(full->variance), holed->strikes_used,
                100.0 * std::sqrt(holed->variance));
    check(holed->strikes_used == full->strikes_used - 1,
          "exactly one strike is lost, not the whole wing below it");
    check(std::fabs(100.0 * std::sqrt(holed->variance)
                  - 100.0 * std::sqrt(full->variance)) < 0.30,
          "and the index barely moves -- a single gap is a gap, not an end");

    // Two ADJACENT holes must end it.
    g_chain[hole - 1].has_put = false;
    const auto ended = vix_term_variance(g_chain, n, Years{kT30}, kRate);
    check(ended.has_value(), "the doubly holed chain evaluates");
    if (ended) {
        std::printf("    two adjacent holes -> %d strikes, VIX %.4f\n",
                    ended->strikes_used,
                    100.0 * std::sqrt(ended->variance));
        check(ended->strikes_used < holed->strikes_used - 1,
              "two consecutive gaps DO end the strip, dropping the whole"
              " wing beyond them");
    }
}

// ── 6 ────────────────────────────────────────────────────────────────────
void the_thirty_day_interpolation_is_in_total_variance()
{
    std::printf("\n6 the_thirty_day_interpolation_is_in_total_variance\n");
    const double t1 = 16.0 / 365.0;
    const double t2 = 44.0 / 365.0;

    // Two flat terms at DIFFERENT vols, so the term structure is not flat and
    // the interpolation actually has to do something.
    auto flat = [](double v) { return [v](double) { return v; }; };
    const std::size_t n1 = build_chain(g_chain, 5'000.0, 0.30, t1, flat(0.18));
    const auto near_t = vix_term_variance(g_chain, n1, Years{t1}, kRate);
    const std::size_t n2 = build_chain(g_chain, 5'000.0, 0.30, t2, flat(0.14));
    const auto next_t = vix_term_variance(g_chain, n2, Years{t2}, kRate);
    check(near_t && next_t, "both terms evaluate");
    if (!near_t || !next_t) { return; }

    const auto vix = india_vix(*near_t, Years{t1}, *next_t, Years{t2});
    check(vix.has_value(), "the interpolation produces a value");
    if (!vix) { return; }

    // The right answer, computed here from the definition rather than from
    // the implementation: interpolate TOTAL variance, then annualise.
    const double w1 = (t2 - kVixTargetYears) / (t2 - t1);
    const double w2 = (kVixTargetYears - t1) / (t2 - t1);
    const double want = 100.0 * std::sqrt(
        (t1 * near_t->variance * w1 + t2 * next_t->variance * w2)
        / kVixTargetYears);
    // What interpolating VOL instead would have given -- the standard error.
    const double naive = 100.0 * (w1 * std::sqrt(near_t->variance)
                                + w2 * std::sqrt(next_t->variance));
    std::printf("    16d at 18 vol, 44d at 14 vol -> 30d index %.4f\n", *vix);
    std::printf("    interpolating VOL instead would give %.4f, a %+.4f error\n",
                naive, naive - *vix);
    check(rel_near(*vix, want, 1e-12),
          "the index matches the total-variance interpolation exactly");
    check(*vix > 14.0 && *vix < 18.0,
          "and lands between the two terms it interpolates");
    check(std::fabs(naive - *vix) > 0.01,
          "while interpolating vol directly is measurably different --"
          " total variance is what is additive in time, not vol");
    check(w1 > 0.0 && w2 > 0.0,
          "with 30 days inside the bracket, both weights are positive");
}

// ── 7 ────────────────────────────────────────────────────────────────────
void degenerate_chains_are_refused()
{
    std::printf("\n7 degenerate_chains_are_refused\n");
    const std::size_t n = build_chain(g_chain, 5'000.0, 0.30, kT30, flat15);

    check(!vix_term_variance(g_chain, 2, Years{kT30}, kRate)
          && vix_term_variance(g_chain, 2, Years{kT30}, kRate).error()
             == VixError::TooFewStrikes,
          "two strikes cannot define a spacing");
    check(!vix_term_variance(g_chain, n, Years{0.0}, kRate),
          "zero time to expiry is refused");
    check(!vix_term_variance(g_chain, n, Years{-1.0}, kRate),
          "and so is negative time");

    // Unsorted strikes are refused rather than sorted in place.
    static VixQuote scratch[64];
    for (std::size_t i = 0; i < 32; ++i) { scratch[i] = g_chain[i]; }
    const Price tmp = scratch[5].strike;
    scratch[5].strike = scratch[6].strike;
    scratch[6].strike = tmp;
    check(!vix_term_variance(scratch, 32, Years{kT30}, kRate)
          && vix_term_variance(scratch, 32, Years{kT30}, kRate).error()
             == VixError::UnsortedStrikes,
          "out-of-order strikes are refused, not silently sorted -- the"
          " caller's order is information about a broken chain builder");

    // No strike carries both sides, so no parity forward exists.
    for (std::size_t i = 0; i < 32; ++i) {
        scratch[i] = g_chain[i];
        scratch[i].has_put = (i % 2 == 0);
        scratch[i].has_call = (i % 2 == 1);
    }
    check(!vix_term_variance(scratch, 32, Years{kT30}, kRate)
          && vix_term_variance(scratch, 32, Years{kT30}, kRate).error()
             == VixError::NoParityStrike,
          "a chain where no strike quotes both sides has no parity forward");

    // Interpolation guards.
    const auto term = vix_term_variance(g_chain, n, Years{kT30}, kRate);
    if (term) {
        check(!india_vix(*term, Years{0.12}, *term, Years{0.05}),
              "terms given out of order are refused");
        check(!india_vix(*term, Years{0.0}, *term, Years{0.12}),
              "and a zero near term is refused");
    }
    check(vix_should_roll(Years{7.0 / 365.0}),
          "a near term at 7 days rolls");
    check(!vix_should_roll(Years{8.0 / 365.0}),
          "and one at 8 days does not");
}

// ── 8 ────────────────────────────────────────────────────────────────────
// The cross-check that makes this card worth having: VIX and the SVI surface
// share no code and no assumptions, so agreement is real evidence.
void the_replication_agrees_with_the_svi_surface()
{
    std::printf("\n8 the_replication_agrees_with_the_svi_surface\n");
    // Integrate the variance-swap kernel against the SVI surface directly,
    // finely, with no strike ladder at all. This is the continuous answer the
    // discrete strip is approximating.
    constexpr int kN = 400'000;
    const double lo = -1.2, hi = 1.2;
    const double h = (hi - lo) / kN;
    double integral = 0.0;
    for (int i = 0; i < kN; ++i) {
        const double k = lo + h * (i + 0.5);
        const double K = kF * std::exp(k);
        const double s = std::sqrt(svi_total_variance(kSlice, k) / kT30);
        const OptionRight r = (K < kF) ? OptionRight::Put : OptionRight::Call;
        const Greeks g = detail::black76_unchecked(r, kF, K, kT30, s, kRate);
        // dK = K dk, so dK/K^2 = dk/K.
        integral += g.price / K * std::exp(kRate * kT30) * K * h / K;
    }
    const double continuous = 100.0 * std::sqrt((2.0 / kT30) * integral);

    const std::size_t n = build_chain(g_chain, 5'000.0, 1.2, kT30, skewed);
    const auto t = vix_term_variance(g_chain, n, Years{kT30}, kRate);
    check(t.has_value(), "the wide skewed chain evaluates");
    if (!t) { return; }
    const double discrete = 100.0 * std::sqrt(t->variance);
    std::printf("    continuous integral over the SVI surface: %.4f\n",
                continuous);
    std::printf("    discrete Rs 50 strip over the same surface: %.4f"
                "   difference %+.4f\n", discrete, discrete - continuous);
    check(std::fabs(discrete - continuous) < 0.05,
          "the discrete strip reproduces the continuous replication to under"
          " 0.05 vol points -- two computations sharing no code");
}

} // namespace

int main()
{
    std::printf("altair analytics vix tests\n");
    a_flat_surface_returns_its_own_vol();
    a_coarse_strike_ladder_overstates_the_index();
    the_index_is_not_the_at_the_money_vol();
    truncating_the_wings_understates_the_index();
    one_illiquid_strike_does_not_end_the_strip();
    the_thirty_day_interpolation_is_in_total_variance();
    degenerate_chains_are_refused();
    the_replication_agrees_with_the_svi_surface();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
