// P3-03 acceptance tests for analytics/american.hpp.
//
// The reference here is a CRR BINOMIAL TREE, built in this file. That is the
// right oracle for an American option: it shares no algebra with
// Bjerksund-Stensland, it converges to the true value, and it handles the
// early-exercise decision by actually making it at every node rather than by
// assuming a boundary. A closed form checked only against published constants
// is checked against someone else's typing.
//
// The property that matters most is not accuracy but DIRECTION: the
// approximation assumes a sub-optimal exercise rule, so it can only
// under-price, and the tests assert that bound holds at every point sampled.
// Safe when buying, dangerous when selling, and silent either way.
//
// No check description here may contain the substring FAIL.

#include <analytics/american.hpp>

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

/// Cox-Ross-Rubinstein binomial, in the cost-of-carry parameterisation.
/// `american` selects whether early exercise is permitted at each node.
constexpr int kSteps = 1500;
double g_tree[kSteps + 1];

double crr(altair::OptionRight right, double S, double K, double T, double v,
           double r, double b, bool american)
{
    const double dt = T / kSteps;
    const double u = std::exp(v * std::sqrt(dt));
    const double d = 1.0 / u;
    const double p = (std::exp(b * dt) - d) / (u - d);
    const double disc = std::exp(-r * dt);
    const bool call = (right == altair::OptionRight::Call);

    for (int i = 0; i <= kSteps; ++i) {
        const double ST = S * std::pow(u, i) * std::pow(d, kSteps - i);
        g_tree[i] = call ? (ST > K ? ST - K : 0.0) : (K > ST ? K - ST : 0.0);
    }
    for (int step = kSteps - 1; step >= 0; --step) {
        for (int i = 0; i <= step; ++i) {
            g_tree[i] = disc * (p * g_tree[i + 1] + (1.0 - p) * g_tree[i]);
            if (american) {
                const double ST = S * std::pow(u, i) * std::pow(d, step - i);
                const double ex = call ? (ST - K) : (K - ST);
                if (ex > g_tree[i]) { g_tree[i] = ex; }
            }
        }
    }
    return g_tree[0];
}

} // namespace

using namespace altair;

namespace {

/// Convenience: price at doubles, returning the struct.
AmericanValue amer(OptionRight right, double S, double K, double T, double v,
                   double r, double b)
{
    return *american_value(right, Price{static_cast<std::int64_t>(S)},
                           Price{static_cast<std::int64_t>(K)}, Years{T},
                           Vol{v}, r, b);
}

// ── 1 ────────────────────────────────────────────────────────────────────
// The exact identity, which is a fact about carry rather than about numerics.
void a_call_with_carry_above_the_rate_is_european()
{
    std::printf("\n1 a_call_with_carry_above_the_rate_is_european\n");
    // b = r is a non-dividend-paying stock. Early exercise is never optimal.
    int exact = 0, tested = 0;
    for (double S : {90.0, 100.0, 110.0, 130.0}) {
        const AmericanValue a = amer(OptionRight::Call, S * 100.0, 10'000.0,
                                     0.5, 0.25, 0.06, 0.06);
        ++tested;
        if (a.price == a.european && a.early_exercise_premium == 0.0) {
            ++exact;
        }
    }
    check(tested == 4 && exact == 4,
          "an American call with b = r equals the European call EXACTLY, at"
          " every spot -- not approximately");
    check(!early_exercise_possible(OptionRight::Call, 0.06, 0.06),
          "and early_exercise_possible says so before any pricing happens");
    check(early_exercise_possible(OptionRight::Call, 0.10, -0.05),
          "while a call on a high-dividend asset can be exercised early");
    check(!early_exercise_possible(OptionRight::Put, 0.0, 0.0),
          "a put on a zero-rate asset is never exercised early either");
    check(early_exercise_possible(OptionRight::Put, 0.06, 0.0),
          "but at a positive rate it can be");

    // b > r as well -- a negative dividend, or a currency with a higher
    // foreign rate. Same conclusion.
    const AmericanValue a = amer(OptionRight::Call, 10'000.0, 10'000.0, 0.5,
                                 0.25, 0.06, 0.09);
    check(a.price == a.european, "and b > r gives the same exact identity");
    check(a.exercise_boundary == 0.0,
          "with no exercise boundary reported, because there is none");
}

// ── 2 ────────────────────────────────────────────────────────────────────
// Against the binomial. The oracle shares no algebra with the approximation.
void it_tracks_a_binomial_tree()
{
    std::printf("\n2 it_tracks_a_binomial_tree\n");
    struct Case {
        const char* name;
        OptionRight right;
        double S, K, T, v, r, b;
    };
    const Case cases[] = {
        {"call S=42  K=40  v=.35 b=-.04", OptionRight::Call,
         4200.0, 4000.0, 0.75, 0.35, 0.08, -0.04},
        {"call S=90  K=100 v=.15 b=-.05", OptionRight::Call,
         9000.0, 10000.0, 0.50, 0.15, 0.10, -0.05},
        {"call S=100 K=100 v=.25 b=-.05", OptionRight::Call,
         10000.0, 10000.0, 0.50, 0.25, 0.10, -0.05},
        {"call S=110 K=100 v=.35 b=-.05", OptionRight::Call,
         11000.0, 10000.0, 0.50, 0.35, 0.10, -0.05},
        {"put  S=36  K=40  v=.30 b= .00", OptionRight::Put,
         3600.0, 4000.0, 0.50, 0.30, 0.10, 0.0},
        {"put  S=40  K=40  v=.30 b= .00", OptionRight::Put,
         4000.0, 4000.0, 0.50, 0.30, 0.10, 0.0},
        {"put  S=44  K=40  v=.30 b= .00", OptionRight::Put,
         4400.0, 4000.0, 0.50, 0.30, 0.10, 0.0},
    };

    std::printf("    case                             BS-1993     binomial"
                "    error     rel\n");
    int below = 0, within = 0;
    double worst_rel = 0.0, worst_rel_price = 0.0;
    for (const Case& c : cases) {
        const AmericanValue a = amer(c.right, c.S, c.K, c.T, c.v, c.r, c.b);
        const double tree = crr(c.right, c.S, c.K, c.T, c.v, c.r, c.b, true);
        const double err = a.price - tree;
        std::printf("    %s  %10.4f  %10.4f  %+8.4f  %+7.3f%%\n",
                    c.name, a.price, tree, err, 100.0 * err / tree);
        if (err <= 1e-9) { ++below; }
        if (std::fabs(err / tree) < 0.03) { ++within; }
        if (std::fabs(err / tree) > std::fabs(worst_rel)) {
            worst_rel = err / tree;
            worst_rel_price = tree;
        }
    }
    check(below == 7,
          "the approximation is at or below the binomial in EVERY case --"
          " a sub-optimal exercise rule cannot be worth more than the optimal"
          " one, and that direction is the safety property");
    check(within == 7, "and within 3% of it in every case");

    // Where the relative error is worst matters more than how big it is. The
    // approximation is least accurate on deep out-of-the-money low-vol
    // options, and those are precisely the ones whose absolute value is
    // smallest -- so the worst percentage lands on the smallest number.
    constexpr double kTickPaise = 5.0;      // one NIFTY option tick
    const double worst_abs = std::fabs(worst_rel * worst_rel_price);
    std::printf("    worst relative error %+.3f%% falls on the CHEAPEST"
                " option (%.2f paise),\n    where it is %.2f paise --"
                " %.2f of a %.0f-paise tick\n",
                100.0 * worst_rel, worst_rel_price, worst_abs,
                worst_abs / kTickPaise, kTickPaise);
    check(worst_abs < kTickPaise,
          "and in absolute terms the worst case is under one tick, which is"
          " below the resolution anything could be quoted at anyway");
    check(below == 7 && within == 7,
          "so it is usable for buying and must not be quoted when selling");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// The no-arbitrage floor, which owes nothing to any model.
void an_american_option_is_never_worth_less_than_a_european_one()
{
    std::printf("\n3 an_american_option_is_never_worth_less_than_a_european_one\n");
    int violations = 0, sampled = 0;
    double max_premium = 0.0;
    for (double S = 6000.0; S <= 14000.0; S += 250.0) {
        for (double v : {0.10, 0.25, 0.50}) {
            for (double b : {-0.08, -0.04, 0.0, 0.04}) {
                for (OptionRight r : {OptionRight::Call, OptionRight::Put}) {
                    const AmericanValue a = amer(r, S, 10'000.0, 0.5, v,
                                                 0.08, b);
                    ++sampled;
                    if (a.price < a.european - 1e-9) { ++violations; }
                    if (a.early_exercise_premium > max_premium) {
                        max_premium = a.early_exercise_premium;
                    }
                }
            }
        }
    }
    std::printf("    %d parameter combinations sampled, %d violations,"
                " largest premium %.4f paise\n",
                sampled, violations, max_premium);
    check(sampled > 700, "the sweep actually covers the surface");
    check(violations == 0,
          "the American value never falls below the European one anywhere on"
          " the surface");
    check(max_premium > 0.0,
          "and the premium is genuinely positive somewhere, so the check is"
          " not passing on a degenerate surface where the two always agree");
}

// ── 4 ────────────────────────────────────────────────────────────────────
void a_deep_in_the_money_put_approaches_intrinsic()
{
    std::printf("\n4 a_deep_in_the_money_put_approaches_intrinsic\n");
    // Far below the strike, an American put should be exercised now, so its
    // value is the intrinsic K - S. A European put is worth LESS, because it
    // cannot be.
    std::printf("    spot   American    intrinsic   European   A-E premium\n");
    int at_intrinsic = 0;
    for (double S : {2000.0, 1000.0, 500.0}) {
        const AmericanValue a = amer(OptionRight::Put, S, 4000.0, 0.5, 0.30,
                                     0.10, 0.0);
        const double intrinsic = 4000.0 - S;
        std::printf("    %5.0f  %9.4f  %11.1f  %9.4f  %11.4f\n",
                    S, a.price, intrinsic, a.european,
                    a.early_exercise_premium);
        if (rel_near(a.price, intrinsic, 1e-9)) { ++at_intrinsic; }
    }
    check(at_intrinsic == 3,
          "a deep in-the-money American put is worth exactly its intrinsic"
          " value -- exercise now");
    const AmericanValue deep = amer(OptionRight::Put, 1000.0, 4000.0, 0.5,
                                    0.30, 0.10, 0.0);
    check(deep.european < deep.price,
          "while the European put is worth strictly less, because it cannot"
          " be exercised until expiry");
    check(deep.immediate_exercise,
          "and the result flags that the boundary was already crossed");

    // The mirror: a deep ITM CALL on a high-dividend asset.
    const AmericanValue c = amer(OptionRight::Call, 20'000.0, 10'000.0, 0.5,
                                 0.30, 0.10, -0.15);
    std::printf("    deep ITM call on a heavy dividend: %.4f vs intrinsic"
                " %.1f\n", c.price, 10'000.0);
    check(c.price >= 10'000.0 - 1e-9,
          "a deep in-the-money call on a heavily dividend-paying asset is"
          " worth at least intrinsic too");
}

// ── 5 ────────────────────────────────────────────────────────────────────
// The put is the transformed call, so it must agree with a binomial PUT.
void the_put_transformation_is_exact()
{
    std::printf("\n5 the_put_transformation_is_exact\n");
    int ok_count = 0, n = 0;
    double worst = 0.0;
    for (double S = 3000.0; S <= 5000.0; S += 250.0) {
        const AmericanValue a = amer(OptionRight::Put, S, 4000.0, 0.5, 0.30,
                                     0.10, 0.02);
        const double tree = crr(OptionRight::Put, S, 4000.0, 0.5, 0.30, 0.10,
                                0.02, true);
        const double rel = (a.price - tree) / tree;
        if (std::fabs(rel) > std::fabs(worst)) { worst = rel; }
        ++n;
        if (a.price <= tree + 1e-9 && std::fabs(rel) < 0.03) { ++ok_count; }
    }
    std::printf("    %d puts across the money: worst relative error %+.4f%%\n",
                n, 100.0 * worst);
    check(ok_count == n,
          "every put is at or below its binomial and within 3% of it, so the"
          " call-to-put transformation carries the accuracy across");
    check(worst <= 0.0,
          "and the lower-bound direction survives the transformation -- the"
          " put inherits it rather than needing its own proof");
}

// ── 6 ────────────────────────────────────────────────────────────────────
void degenerate_inputs_are_refused()
{
    std::printf("\n6 degenerate_inputs_are_refused\n");
    auto e = [](double T, double v, std::int64_t S, std::int64_t K) {
        const auto r = american_value(OptionRight::Put, Price{S}, Price{K},
                                      Years{T}, Vol{v}, 0.10, 0.0);
        return r ? GreekError::NotFinite : r.error();
    };
    check(e(0.0, 0.30, 4000, 4000) == GreekError::Expired,
          "T = 0 is Expired -- at expiry the option is an exercise decision,"
          " not a pricing problem");
    check(e(-1.0, 0.30, 4000, 4000) == GreekError::Expired,
          "and so is a negative T");
    check(e(0.5, 0.0, 4000, 4000) == GreekError::NonPositiveVol,
          "sigma = 0 is refused");
    check(e(0.5, 0.30, 0, 4000) == GreekError::NonPositivePrice,
          "a zero spot is refused");
    check(e(0.5, 0.30, 4000, 0) == GreekError::NonPositivePrice,
          "a zero strike is refused");
    const double nan = std::nan("");
    const auto bad = american_value(OptionRight::Call, Price{4000},
                                    Price{4000}, Years{0.5}, Vol{0.3}, nan,
                                    0.0);
    check(!bad && bad.error() == GreekError::NotFinite,
          "a NaN rate is refused");
}

} // namespace

int main()
{
    std::printf("altair analytics american tests\n");
    a_call_with_carry_above_the_rate_is_european();
    it_tracks_a_binomial_tree();
    an_american_option_is_never_worth_less_than_a_european_one();
    a_deep_in_the_money_put_approaches_intrinsic();
    the_put_transformation_is_exact();
    degenerate_inputs_are_refused();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
