// P3-10 acceptance tests for risk/slippage.hpp.
//
// The load-bearing experiment is test 3. Orders are generated from a KNOWN
// eta, adverse ones are allowed to miss the way a passive order really does,
// and the model is fitted twice -- once on the survivors alone, once on every
// order sent. The first comes back materially low. That is survivorship bias
// in execution data, and it is the reason `add` takes the whole order.
//
// Everything else is checked against data generated from a known coefficient,
// so there is a right answer rather than a plausible one.
//
// No check description here may contain the substring FAIL.

#include <risk/slippage.hpp>

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
    const double m = std::fabs(b) > 1e-12 ? std::fabs(b) : 1e-12;
    return d / m <= tol;
}

class Lcg {
public:
    explicit Lcg(std::uint64_t seed) noexcept : s_(seed) {}
    /// Uniform in [0, 1).
    double u01() noexcept {
        s_ = s_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(s_ >> 11) * (1.0 / 9007199254740992.0);
    }
    double sym() noexcept { return 2.0 * u01() - 1.0; }
private:
    std::uint64_t s_;
};

} // namespace

using namespace altair;

namespace {

constexpr double kMid = 2'408'000.0;     // NIFTY, paise
constexpr double kTrueEta = 0.90;
constexpr double kVol = 0.012;           // 1.2% a day

/// An order generated from the square-root law, with a given participation.
/// `noise_bps` perturbs the realised impact; `delay_bps` moves the arrival.
OrderOutcome make_order(double participation, double noise_bps,
                        double delay_bps, Side side, double fill_ratio)
{
    const double sgn = adverse_sign(side);
    const double impact_bps = kTrueEta * kVol * 10'000.0
                            * std::sqrt(participation) + noise_bps;
    OrderOutcome o{};
    o.side = side;
    o.participation = participation;
    o.daily_vol = kVol;
    o.decision_price = Price{static_cast<std::int64_t>(kMid)};
    const double arrival = kMid * (1.0 + sgn * delay_bps / 10'000.0);
    o.arrival_price = Price{static_cast<std::int64_t>(arrival + 0.5)};
    const double fill = arrival * (1.0 + sgn * impact_bps / 10'000.0);
    o.fill_price = Price{static_cast<std::int64_t>(fill + 0.5)};
    o.final_price = o.arrival_price;
    o.ordered = Qty{10'000};
    o.filled = Qty{static_cast<std::int64_t>(10'000.0 * fill_ratio)};
    return o;
}

// ── 1 ────────────────────────────────────────────────────────────────────
void a_known_eta_is_recovered()
{
    std::printf("\n1 a_known_eta_is_recovered\n");
    SlippageModel m;
    Lcg rng{0x51199AULL};
    int added = 0;
    for (int i = 0; i < 500; ++i) {
        const double p = 0.0005 + 0.03 * rng.u01();      // 0.05% to 3% of ADV
        const Side s = (i % 2 == 0) ? Side::Buy : Side::Sell;
        if (m.add(make_order(p, 1.5 * rng.sym(), 0.0, s, 1.0))) { ++added; }
    }
    check(added == 500, "every order is accepted");
    const auto e = m.eta();
    const auto se = m.eta_std_error();
    check(e && se, "eta and its standard error are available");
    if (!e || !se) { return; }
    std::printf("    fitted eta %.6f +/- %.6f   (truth %.2f, %llu orders)\n",
                *e, *se, kTrueEta,
                static_cast<unsigned long long>(m.count()));
    check(std::fabs(*e - kTrueEta) < 0.05,
          "the fitted coefficient recovers the true one");
    check(std::fabs(*e - kTrueEta) < 3.0 * *se,
          "and lands inside three standard errors of it, so the error bar is"
          " honest rather than decorative");
    check(m.misses() == 0 && m.mean_unfilled() < 1e-9,
          "with every order fully filled, nothing is reported as missed");

    // Prediction must reproduce the law it was fitted to.
    const auto p1 = m.predict(kVol, 0.01);
    const auto p4 = m.predict(kVol, 0.04);
    check(p1 && p4, "predictions are available");
    if (p1 && p4) {
        std::printf("    1%% of ADV costs %.3f bps, 4%% costs %.3f bps"
                    "  -- ratio %.4f\n", *p1, *p4, *p4 / *p1);
        check(rel_near(*p4 / *p1, 2.0, 1e-12),
              "quadrupling the size DOUBLES the cost -- the square root, not"
              " a linear scaling");
    }
}

// ── 2 ────────────────────────────────────────────────────────────────────
// A linear model fitted to square-root reality is wrong in both tails, and
// both errors push a sizing rule the wrong way.
void a_linear_model_is_wrong_at_both_ends()
{
    std::printf("\n2 a_linear_model_is_wrong_at_both_ends\n");
    // Fit slippage = k * participation, through the origin, on the same law.
    double sxy = 0.0, sxx = 0.0;
    for (int i = 0; i < 2000; ++i) {
        const double p = 0.0005 + 0.03 * (static_cast<double>(i) / 2000.0);
        const double y = kTrueEta * kVol * 10'000.0 * std::sqrt(p);
        sxy += p * y;
        sxx += p * p;
    }
    const double k_linear = sxy / sxx;
    std::printf("    linear fit: %.3f bps per unit participation\n", k_linear);

    // A through-origin line crosses a square root exactly ONCE, at
    // p* = (c/k)^2, so it is wrong on both sides of that point in opposite
    // directions. Computing the crossover rather than guessing at test points
    // is the difference between a real check and a lucky one -- my first draft
    // guessed 2% and landed on the wrong side of a crossover at 2.12%.
    const double c = kTrueEta * kVol * 10'000.0;
    const double p_cross = (c / k_linear) * (c / k_linear);
    std::printf("    the two agree at exactly one size: %.4f%% of ADV\n",
                100.0 * p_cross);

    std::printf("    participation    truth     linear    error\n");
    int small_under = 0, large_over = 0, tested_small = 0, tested_large = 0;
    for (double p : {0.0005, 0.001, 0.005, 0.028, 0.030}) {
        const double truth = c * std::sqrt(p);
        const double lin = k_linear * p;
        std::printf("    %10.4f%%   %8.4f  %8.4f  %+8.4f\n",
                    100.0 * p, truth, lin, lin - truth);
        if (p < p_cross) { ++tested_small; if (lin < truth) { ++small_under; } }
        if (p > p_cross) { ++tested_large; if (lin > truth) { ++large_over; } }
    }
    check(tested_small > 0 && small_under == tested_small,
          "below the crossover the linear model UNDERSTATES every size --"
          " and small orders are most of the count");
    check(tested_large > 0 && large_over == tested_large,
          "above it the linear model OVERSTATES every size -- and large"
          " orders are most of the cost");
    check(small_under > 0 && large_over > 0,
          "so it is wrong on both sides at once: no rescaling repairs it,"
          " only the correct functional form");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// THE experiment. Fills are not a random sample of orders.
void fitting_only_on_fills_understates_the_cost()
{
    std::printf("\n3 fitting_only_on_fills_understates_the_cost\n");
    SlippageModel survivors;   // fed only the orders that filled
    SlippageModel everything;  // fed every order sent
    Lcg rng{0xB1A5EDULL};
    int sent = 0, filled = 0;

    // The mechanism, stated exactly. The true cost of trading size p is
    // c(p) = eta*vol_bps*sqrt(p). While a passive order rests, the market
    // moves by m, symmetric about zero. The realised cost of the decision is
    // c(p) + m EITHER WAY:
    //
    //   * m < 0, the market came to you: the order FILLS, at a price better
    //     than c(p) by |m|.
    //   * m > 0, the market ran away: the order MISSES, and you are left
    //     needing the position at a price worse than c(p) by m. That is the
    //     opportunity cost, and it is exactly as real as a fill.
    //
    // So the full sample is unbiased -- E[m] = 0 -- while the fills are a
    // sample conditioned on m < 0 and are therefore cheap by construction.
    for (int i = 0; i < 800; ++i) {
        const double p = 0.0005 + 0.03 * rng.u01();
        const Side s = (i % 2 == 0) ? Side::Buy : Side::Sell;
        const double sgn = adverse_sign(s);
        const double m = 8.0 * rng.sym();                // bps, symmetric
        const bool did_fill = (m <= 0.0);
        const double cost_bps = kTrueEta * kVol * 10'000.0 * std::sqrt(p) + m;
        ++sent;

        OrderOutcome o{};
        o.side = s;
        o.participation = p;
        o.daily_vol = kVol;
        o.decision_price = Price{static_cast<std::int64_t>(kMid)};
        o.arrival_price = o.decision_price;
        o.ordered = Qty{10'000};
        const double moved = kMid * (1.0 + sgn * cost_bps / 10'000.0);
        if (did_fill) {
            ++filled;
            o.filled = Qty{10'000};
            o.fill_price = Price{static_cast<std::int64_t>(moved + 0.5)};
            o.final_price = o.arrival_price;
            (void)survivors.add(o);
        } else {
            o.filled = Qty{0};
            o.fill_price = o.arrival_price;
            o.final_price = Price{static_cast<std::int64_t>(moved + 0.5)};
        }
        (void)everything.add(o);
    }

    const auto e_surv = survivors.eta();
    const auto e_all = everything.eta();
    check(e_surv && e_all, "both models fit");
    if (!e_surv || !e_all) { return; }
    std::printf("    %d orders sent, %d filled (%.0f%%)\n",
                sent, filled, 100.0 * filled / sent);
    std::printf("    eta fitted on FILLS ONLY   : %.4f\n", *e_surv);
    std::printf("    eta fitted on EVERY ORDER  : %.4f\n", *e_all);
    std::printf("    the truth is               : %.4f\n", kTrueEta);
    check(*e_surv < *e_all,
          "the fills-only model reports a LOWER cost than the full sample");
    check(*e_all > *e_surv * 1.05,
          "and the gap is material, not rounding -- the orders that got away"
          " are systematically the expensive ones");
    check(std::fabs(*e_all - kTrueEta) < std::fabs(*e_surv - kTrueEta),
          "the full sample is the accurate one; the fills are a sample"
          " conditioned on the market having come to you");
    check(everything.misses() > 0,
          "the full model has actually seen misses, so the comparison is"
          " between a biased sample and a complete one");
    std::printf("    mean unfilled fraction across all orders: %.1f%%\n",
                100.0 * everything.mean_unfilled());
    check(everything.mean_unfilled() > 0.1,
          "and a substantial share of the order flow never traded at all");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void the_shortfall_decomposition_adds_up()
{
    std::printf("\n4 the_shortfall_decomposition_adds_up\n");
    // A buy: decided at 24,080, arrived 3 bps worse, filled 5 bps worse than
    // arrival on 60% of the order, cancelled with the market 12 bps away.
    OrderOutcome o{};
    o.side = Side::Buy;
    o.decision_price = Price{2'408'000};
    o.arrival_price = Price{static_cast<std::int64_t>(2'408'000 * 1.0003)};
    o.fill_price = Price{static_cast<std::int64_t>(2'408'000 * 1.0003 * 1.0005)};
    o.final_price = Price{static_cast<std::int64_t>(2'408'000 * 1.0012)};
    o.ordered = Qty{10'000};
    o.filled = Qty{6'000};
    o.participation = 0.01;
    o.daily_vol = kVol;

    const auto s = implementation_shortfall(o);
    check(s.has_value(), "the decomposition is available");
    if (!s) { return; }
    std::printf("    delay %+.4f  impact %+.4f  opportunity %+.4f"
                "  =  total %+.4f bps\n",
                s->delay_bps, s->impact_bps, s->opportunity_bps, s->total_bps);
    check(rel_near(s->delay_bps + s->impact_bps + s->opportunity_bps,
                   s->total_bps, 1e-12),
          "the three components sum exactly to the total");
    check(rel_near(s->delay_bps, 3.0, 0.01), "delay is the 3 bps to arrival");
    check(rel_near(s->impact_bps, 5.0 * 0.6, 0.01),
          "impact is 5 bps scaled by the 60% that actually filled");
    check(rel_near(s->opportunity_bps, 12.0 * 0.4, 0.01),
          "and opportunity is 12 bps scaled by the 40% that did not");
    check(rel_near(s->unfilled_ratio, 0.4, 1e-12),
          "the unfilled fraction is reported");
    check(s->opportunity_bps > s->impact_bps,
          "here the order that got away cost MORE than the one that traded --"
          " which a fills-only cost report would show as zero");

    // A SELL with the market falling is adverse in the other direction, and
    // must come out positive too. Getting this sign wrong makes every short
    // look profitable.
    OrderOutcome p = o;
    p.side = Side::Sell;
    p.arrival_price = Price{static_cast<std::int64_t>(2'408'000 * 0.9997)};
    p.fill_price = Price{static_cast<std::int64_t>(2'408'000 * 0.9997 * 0.9995)};
    p.final_price = Price{static_cast<std::int64_t>(2'408'000 * 0.9988)};
    const auto ps = implementation_shortfall(p);
    check(ps.has_value(), "the sell decomposes");
    if (ps) {
        std::printf("    sell side: delay %+.4f  impact %+.4f"
                    "  opportunity %+.4f\n",
                    ps->delay_bps, ps->impact_bps, ps->opportunity_bps);
        check(ps->delay_bps > 0.0 && ps->impact_bps > 0.0
              && ps->opportunity_bps > 0.0,
              "a falling market is ADVERSE for a seller, and all three terms"
              " are positive -- the sign convention holds on both sides");
        check(rel_near(ps->total_bps, s->total_bps, 0.02),
              "and a mirror-image sell costs the same as the buy");
    }
}

// ── 5 ────────────────────────────────────────────────────────────────────
void sizing_uses_the_pessimistic_bound()
{
    std::printf("\n5 sizing_uses_the_pessimistic_bound\n");
    // The premium the pessimistic bound charges is itself a measurement: it
    // is large when the model has seen little and shrinks as it learns. That
    // shrinkage is the property worth pinning, because a bound that stayed
    // wide forever would just be a tax and one that started narrow would be a
    // lie about a fresh model.
    struct Row { int n; double eta; double se; double mid; double up; };
    Row rows[3];
    int idx = 0;
    for (int n : {12, 60, 600}) {
        SlippageModel m;
        Lcg rng{0x512E00ULL};
        for (int i = 0; i < n; ++i) {
            const double p = 0.0005 + 0.03 * rng.u01();
            const Side s = (i % 2 == 0) ? Side::Buy : Side::Sell;
            (void)m.add(make_order(p, 8.0 * rng.sym(), 0.0, s, 1.0));
        }
        const auto e = m.eta();
        const auto se = m.eta_std_error();
        const auto mid = m.predict(kVol, 0.02);
        const auto up = m.predict_upper(kVol, 0.02, 2.0);
        if (!e || !se || !mid || !up) { check(false, "model reports"); return; }
        rows[idx] = Row{n, *e, *se, *mid, *up};
        std::printf("    %3d orders: eta %.4f +/- %.4f   2%% of ADV costs"
                    " %.3f bps, 2-sigma upper %.3f  (+%.1f%%)\n",
                    n, *e, *se, *mid, *up, 100.0 * (*up / *mid - 1.0));
        ++idx;
    }
    check(rows[0].up > rows[0].mid && rows[2].up > rows[2].mid,
          "the pessimistic bound always exceeds the point estimate");
    check(rows[0].up / rows[0].mid > 1.15,
          "on 12 orders it is more than 15% higher -- sizing on the point"
          " estimate there assumes a precision the sample does not have");
    check(rows[2].up / rows[2].mid < rows[0].up / rows[0].mid,
          "and the premium shrinks as the model learns, so it is an error"
          " bar rather than a permanent tax");
    check(rows[2].se < rows[0].se * 0.5,
          "because the standard error itself falls with the sample");

    SlippageModel m;
    Lcg rng{0x512E00ULL};
    for (int i = 0; i < 60; ++i) {
        const double p = 0.0005 + 0.03 * rng.u01();
        const Side s = (i % 2 == 0) ? Side::Buy : Side::Sell;
        (void)m.add(make_order(p, 8.0 * rng.sym(), 0.0, s, 1.0));
    }

    // The inversion a sizing rule actually needs, and its square.
    const auto p_budget = m.max_participation(kVol, 3.0, 2.0);
    const auto p_half = m.max_participation(kVol, 1.5, 2.0);
    check(p_budget && p_half, "the size budget inverts");
    if (p_budget && p_half) {
        std::printf("    a 3.0 bps cost budget allows %.4f%% of ADV;"
                    " 1.5 bps allows %.4f%%\n",
                    100.0 * *p_budget, 100.0 * *p_half);
        check(rel_near(*p_half / *p_budget, 0.25, 1e-9),
              "HALVING the cost budget QUARTERS the tradable size -- the"
              " square-root law inverts to a square, and a linear model would"
              " have allowed twice as much");
        // And the budget must actually be met at that size.
        const auto cost = m.predict_upper(kVol, *p_budget, 2.0);
        if (cost) {
            check(rel_near(*cost, 3.0, 1e-9),
                  "and trading exactly that size costs exactly the budget");
        }
    }
}

// ── 6 ────────────────────────────────────────────────────────────────────
void degenerate_input_is_refused()
{
    std::printf("\n6 degenerate_input_is_refused\n");
    SlippageModel m;
    check(!m.eta() && m.eta().error() == SlippageError::TooFewFills,
          "an empty model has no eta, rather than zero");
    check(!m.predict(kVol, 0.01), "and cannot predict");

    OrderOutcome o = make_order(0.01, 0.0, 0.0, Side::Buy, 1.0);
    OrderOutcome bad = o;
    bad.participation = 0.0;
    check(!m.add(bad) && m.add(bad).error() == SlippageError::BadObservation,
          "a zero participation is refused -- an order of no size has no"
          " impact to learn from");
    bad = o; bad.daily_vol = 0.0;
    check(!m.add(bad), "a zero volatility is refused");
    bad = o; bad.ordered = Qty{0};
    check(!m.add(bad), "a zero order quantity is refused");
    bad = o; bad.filled = Qty{20'000};      // more than ordered
    check(!m.add(bad),
          "filling more than was ordered is refused, not clamped");
    bad = o; bad.decision_price = Price{0};
    check(!m.add(bad), "a zero decision price is refused");

    // Two observations give a slope with no residual degrees of freedom.
    SlippageModel two;
    (void)two.add(o);
    (void)two.add(make_order(0.02, 0.0, 0.0, Side::Sell, 1.0));
    check(!two.eta() && two.eta().error() == SlippageError::TooFewFills,
          "two orders give a slope with no error bar, which is refused --"
          " a cost estimate with no uncertainty is the dangerous kind");
    (void)two.add(make_order(0.005, 0.0, 0.0, Side::Buy, 1.0));
    check(two.eta().has_value() && two.eta_std_error().has_value(),
          "three orders is the minimum that reports both");
    check(m.count() == 0, "and the refused observations were never counted");
}

} // namespace

int main()
{
    std::printf("altair risk slippage tests\n");
    a_known_eta_is_recovered();
    a_linear_model_is_wrong_at_both_ends();
    fitting_only_on_fills_understates_the_cost();
    the_shortfall_decomposition_adds_up();
    sizing_uses_the_pessimistic_bound();
    degenerate_input_is_refused();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
