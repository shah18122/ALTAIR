// P4-01 acceptance tests for risk/sizing.hpp.
//
// The load-bearing test is 1: the Kelly overbetting table, COMPUTED from the
// expected-log-growth function rather than quoted from the header. A divisor
// of four is either justified by that table or it is superstition, and a
// justification that cannot be evaluated is a slogan.
//
// Everything else checks a property that has a right answer independent of any
// number I chose: rounding direction, which constraint binds, and what happens
// when an edge is indistinguishable from zero.
//
// No check description here may contain the substring FAIL.

#include <risk/sizing.hpp>

#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <algorithm>

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

} // namespace

using namespace altair;

namespace {

// A realistic NIFTY futures frame, everything in paise.
//   capital   Rs 1 crore          = 1e9 paise
//   price     Rs 24,080 per unit  = 2,408,000 paise
//   lot       75 units            -> Rs 18.06 lakh of notional per lot
constexpr std::int64_t kCapital = 1'000'000'000;
constexpr std::int64_t kPrice = 2'408'000;
constexpr std::int64_t kLot = 75;
constexpr std::int64_t kNotionalPerLot = kPrice * kLot;   // 180,600,000

SizingInputs base()
{
    SizingInputs in{};
    in.capital = Notional{kCapital};
    in.price = Price{kPrice};
    in.lot_size = LotSize{kLot};
    in.freeze_qty = Qty{1800};              // NSE NIFTY freeze, 24 lots
    in.edge_bps = Bps{25.0};                // 25 bps net of all cost
    in.edge_std_error_bps = Bps{5.0};
    in.edge_sigmas = 2.0;
    in.period_vol = Vol{0.012};             // 1.2% over the holding period
    in.risk_fraction = 0.01;
    in.stop_distance = Price{12'000};       // Rs 120 per unit
    in.kelly_divisor = 4.0;
    in.target_vol = 0.010;
    in.max_leverage = 1.0;
    return in;
}

// ── 1 ────────────────────────────────────────────────────────────────────
// The table that justifies the divisor. Computed here, not quoted.
void the_kelly_divisor_is_justified_by_its_own_arithmetic()
{
    std::printf("\n1 the_kelly_divisor_is_justified_by_its_own_arithmetic\n");
    // Work in units of mu^2/sigma^2 by setting mu = sigma = 1: then betting
    // k*mu/sigma^2 is just betting k, and g(k) = k - k^2/2.
    constexpr double mu = 1.0, sigma = 1.0;
    auto g = [&](double k) { return log_growth(k, mu, sigma); };

    const double full_true = g(1.0);
    const double full_2x = g(2.0);
    const double half_true = g(0.5);
    const double quarter_true = g(0.25);
    const double quarter_2x = g(0.5);
    const double quarter_8x = g(2.0);

    std::printf("    expected log growth, in units of mu^2/sigma^2:\n");
    std::printf("      full    Kelly, true edge      %.5f  (the optimum)\n",
                full_true);
    std::printf("      full    Kelly, edge 2x too big %.5f\n", full_2x);
    std::printf("      half    Kelly, true edge      %.5f\n", half_true);
    std::printf("      quarter Kelly, true edge      %.5f\n", quarter_true);
    std::printf("      quarter Kelly, edge 2x too big %.5f\n", quarter_2x);
    std::printf("      quarter Kelly, edge 8x too big %.5f\n", quarter_8x);

    check(rel_near(full_true, 0.5, 1e-12),
          "full Kelly at the true edge is the growth optimum, 0.5");
    check(std::fabs(full_2x) < 1e-12,
          "full Kelly with the edge overestimated 2x gives EXACTLY ZERO"
          " growth -- the whole edge consumed by variance drag");
    check(g(3.0) < 0.0,
          "and a 3x overestimate at full Kelly gives NEGATIVE growth");
    check(quarter_2x > quarter_true,
          "quarter Kelly with a 2x overestimate is still better than quarter"
          " Kelly at the true edge, because it lands on half Kelly");
    check(std::fabs(quarter_8x) < 1e-12,
          "quarter Kelly tolerates an EIGHT-fold overestimate before growth"
          " reaches zero, against full Kelly's twofold");
    check(quarter_true > 0.43 * full_true && quarter_true < 0.45 * full_true,
          "the price is giving up 56% of the theoretical growth rate");
    std::printf("    -> a 4x divisor buys a 4x increase in how wrong the edge"
                " may be,\n       for 56%% of the theoretical growth."
                " That is the trade.\n");

    // The growth function must actually peak at full Kelly, or the whole
    // argument above is about the wrong curve.
    bool peaks_at_one = true;
    for (double k = 0.05; k < 3.0; k += 0.05) {
        if (g(k) > full_true + 1e-12) { peaks_at_one = false; }
    }
    check(peaks_at_one, "and g(f) peaks at f = 1 (full Kelly), as it must");
}

// ── 2 ────────────────────────────────────────────────────────────────────
void everything_rounds_down_to_whole_lots()
{
    std::printf("\n2 everything_rounds_down_to_whole_lots\n");
    // A budget of 2.9 lots must give 2, not 3. Rounding to nearest would put
    // most of an extra Rs 18 lakh lot on the book.
    const Notional budget{static_cast<std::int64_t>(2.9 * kNotionalPerLot)};
    const auto l = lots_within(budget, Price{kPrice}, LotSize{kLot});
    check(l.has_value(), "a 2.9-lot budget sizes");
    if (!l) { return; }
    std::printf("    budget %lld paise = %.3f lots -> %lld lots\n",
                static_cast<long long>(budget.raw()),
                static_cast<double>(budget.raw()) / kNotionalPerLot,
                static_cast<long long>(l->raw()));
    check(l->raw() == 2, "2.9 lots rounds DOWN to 2");
    const double unused = static_cast<double>(budget.raw())
                        - 2.0 * kNotionalPerLot;
    std::printf("    leaves %.0f paise (Rs %.0f) of budget unused; rounding up"
                " would have exceeded it by Rs %.0f\n",
                unused, unused / 100.0,
                (kNotionalPerLot - unused) / 100.0);
    check(unused > 0.0,
          "and the leftover is unused capacity, which cannot blow up");

    // Just under one lot is zero lots, not one.
    const Notional almost{kNotionalPerLot - 1};
    check(lots_within(almost, Price{kPrice}, LotSize{kLot})->raw() == 0,
          "one paise short of a lot is ZERO lots");
    check(lots_within(Notional{kNotionalPerLot}, Price{kPrice},
                      LotSize{kLot})->raw() == 1,
          "and exactly one lot's notional is one lot");
    check(lots_within(Notional{0}, Price{kPrice}, LotSize{kLot})->raw() == 0,
          "a zero budget is zero lots, not an error");
    check(lots_within(Notional{-5}, Price{kPrice}, LotSize{kLot})->raw() == 0,
          "and a negative budget is zero rather than a negative position");
}

// ── 3 ────────────────────────────────────────────────────────────────────
// The minimum binds, and it says which one.
void the_smallest_constraint_binds_and_is_named()
{
    std::printf("\n3 the_smallest_constraint_binds_and_is_named\n");
    const auto d = size_position(base());
    check(d.has_value(), "the base case sizes");
    if (!d) { return; }
    std::printf("    fixed-fractional %lld   kelly %lld   vol-target %lld"
                "   freeze %lld\n",
                static_cast<long long>(d->by_fixed_fractional.raw()),
                static_cast<long long>(d->by_kelly.raw()),
                static_cast<long long>(d->by_vol_target.raw()),
                static_cast<long long>(d->by_freeze.raw()));
    std::printf("    chosen %lld lots (%lld units, %lld paise), bound by %d\n",
                static_cast<long long>(d->lots.raw()),
                static_cast<long long>(d->qty.raw()),
                static_cast<long long>(d->notional.raw()),
                static_cast<int>(d->binding));

    const std::int64_t smallest =
        std::min(std::min(d->by_fixed_fractional.raw(), d->by_kelly.raw()),
                 std::min(d->by_vol_target.raw(), d->by_freeze.raw()));
    check(d->lots.raw() == smallest,
          "the chosen size is the MINIMUM of the four, never an average");
    check(d->binding != SizingBound::Unset,
          "and the binding constraint is named");
    check(d->qty.raw() == d->lots.raw() * kLot,
          "quantity is lots times the lot size from the spec store");
    check(d->notional.raw() == d->qty.raw() * kPrice,
          "and notional is price times quantity, in paise");

    // Make each constraint bind in turn, and check it is the one reported.
    SizingInputs in = base();
    in.risk_fraction = 0.0005;                       // a very tight risk budget
    const auto a = size_position(in);
    check(a && a->binding == SizingBound::FixedFractional,
          "tightening the risk fraction makes fixed-fractional bind");

    in = base();
    in.freeze_qty = Qty{150};                        // 2 lots
    const auto b = size_position(in);
    check(b && b->binding == SizingBound::FreezeQuantity && b->lots.raw() == 2,
          "a small exchange freeze quantity binds and caps at 2 lots");

    in = base();
    in.target_vol = 0.0005;                          // a very low vol target
    const auto c = size_position(in);
    check(c && c->binding == SizingBound::VolTarget,
          "a low volatility target binds");

    in = base();
    in.edge_bps = Bps{0.5};                          // almost no edge
    in.edge_std_error_bps = Bps{0.1};
    const auto e = size_position(in);
    check(e && e->binding == SizingBound::Kelly,
          "and a thin edge makes Kelly the binding constraint");
}

// ── 4 ────────────────────────────────────────────────────────────────────
// The ROADMAP section 3 rule, enforced by construction.
void an_edge_inside_its_own_error_bar_sizes_to_zero()
{
    std::printf("\n4 an_edge_inside_its_own_error_bar_sizes_to_zero\n");
    SizingInputs in = base();
    in.edge_bps = Bps{8.0};
    in.edge_std_error_bps = Bps{5.0};       // 8 - 2*5 = -2, straddles zero
    const auto d = size_position(in);
    check(d.has_value(), "it still returns a decision");
    if (!d) { return; }
    std::printf("    edge 8.0 +/- 5.0 bps, 2-sigma lower bound %.1f bps"
                " -> %lld lots\n", d->edge_lower_bps,
                static_cast<long long>(d->lots.raw()));
    check(d->edge_lower_bps < 0.0, "the lower bound is negative");
    check(d->by_kelly.raw() == 0, "so Kelly allows nothing");
    check(d->lots.raw() == 0, "and the position is zero lots");
    check(d->binding == SizingBound::NoEdge,
          "reported as NoEdge, not as a generic Kelly bind -- the cause is"
          " that there was no signal, not that the model was cautious");
    check(d->kelly_fraction == 0.0, "and the Kelly fraction is exactly zero");

    // The same edge with a tighter error bar IS tradable. Without this the
    // check above would pass on a sizer that never trades anything.
    in.edge_std_error_bps = Bps{1.0};       // 8 - 2 = 6 bps, clearly positive
    const auto ok = size_position(in);
    check(ok && ok->edge_lower_bps > 0.0 && ok->by_kelly.raw() > 0,
          "the same 8 bps edge measured more precisely does size a position");
    std::printf("    same edge at +/-1.0 bps: lower bound %.1f bps -> kelly"
                " allows %lld lots\n", ok->edge_lower_bps,
                static_cast<long long>(ok->by_kelly.raw()));

    // Zero lots is a DECISION, not an error.
    check(d.has_value() && d->lots.raw() == 0,
          "zero lots comes back as a value, so a caller cannot mistake"
          " do-not-trade for an exception to route around");
}

// ── 5 ────────────────────────────────────────────────────────────────────
void vol_targeting_scales_inversely_and_is_capped()
{
    std::printf("\n5 vol_targeting_scales_inversely_and_is_capped\n");
    SizingInputs in = base();
    in.max_leverage = 100.0;            // lift the cap to see the pure scaling
    std::printf("    period vol   implied leverage   lots\n");
    double prev_lots = -1.0;
    bool monotone = true;
    for (double v : {0.040, 0.020, 0.010, 0.005}) {
        in.period_vol = Vol{v};
        const auto l = size_vol_target(in);
        if (!l) { check(false, "vol target sizes"); return; }
        std::printf("    %8.3f      %14.2f   %4lld\n",
                    v, in.target_vol / v, static_cast<long long>(l->raw()));
        if (prev_lots >= 0.0 && static_cast<double>(l->raw()) < prev_lots) {
            monotone = false;
        }
        prev_lots = static_cast<double>(l->raw());
    }
    check(monotone, "halving volatility does not decrease the size");

    // Halving the vol doubles the BUDGET exactly. It does not double the LOT
    // COUNT, and expecting it to is a mistake worth pinning down: flooring
    // does not commute with doubling. Here 0.5x capital buys 2.768 lots ->
    // 2, and 1.0x buys 5.537 -> 5, not 4. floor(2x) >= 2*floor(x) always, so
    // the inequality is the exact statement and equality is the accident.
    in.period_vol = Vol{0.020};
    const auto a = size_vol_target(in);
    in.period_vol = Vol{0.010};
    const auto b = size_vol_target(in);
    check(a && b, "both volatilities size");
    if (a && b) {
        std::printf("    vol 0.020 -> %lld lots, vol 0.010 -> %lld lots"
                    " (not %lld: flooring does not commute with doubling)\n",
                    static_cast<long long>(a->raw()),
                    static_cast<long long>(b->raw()),
                    static_cast<long long>(2 * a->raw()));
        check(b->raw() >= 2 * a->raw(),
              "halving the volatility gives AT LEAST twice the lots, which is"
              " the exact statement once whole contracts are involved");
    }

    // The underlying scaling IS exact; it is the lot discretisation that
    // breaks it. Dropping to a lot size of ONE NIFTY UNIT is not enough to
    // show that -- a unit is still Rs 24,080, and 207.6 units floors to 207,
    // which is 0.5% off. A cheap instrument makes the granularity genuinely
    // negligible and the doubling reappears.
    {
        SizingInputs fine = base();
        fine.max_leverage = 100.0;
        fine.price = Price{100};            // a Rs 1 share
        fine.lot_size = LotSize{1};
        fine.period_vol = Vol{0.020};
        const auto fa = size_vol_target(fine);
        fine.period_vol = Vol{0.010};
        const auto fb = size_vol_target(fine);
        check(fa && fb, "the fine-grained case sizes");
        if (fa && fb) {
            const double ratio = static_cast<double>(fb->raw())
                               / static_cast<double>(fa->raw());
            std::printf("    a Rs 1 share, lot 1: %lld -> %lld units,"
                        " ratio %.9f\n",
                        static_cast<long long>(fa->raw()),
                        static_cast<long long>(fb->raw()), ratio);
            check(rel_near(ratio, 2.0, 1e-6),
                  "with the granularity negligible, halving the volatility"
                  " doubles the size exactly -- the scaling was never wrong,"
                  " the lot boundary was");
        }
    }

    // The cap is not optional: as vol goes to zero the unlevered answer goes
    // to infinity, and a quiet market is when that is most tempting.
    // Rebuilt from base() rather than reusing `in`, which the sweeps above
    // have been mutating -- a leaked lot_size from an earlier block is
    // exactly how a test ends up measuring something it did not mean to.
    in = base();
    in.max_leverage = 1.0;
    in.period_vol = Vol{1e-9};
    const auto capped = size_vol_target(in);
    check(capped.has_value(), "a near-zero vol still returns a size");
    if (capped) {
        const double lev = static_cast<double>(capped->raw() * kNotionalPerLot)
                         / static_cast<double>(kCapital);
        std::printf("    vol 1e-9 with max_leverage 1.0 -> %lld lots,"
                    " leverage %.3f\n",
                    static_cast<long long>(capped->raw()), lev);
        check(lev <= 1.0 + 1e-9,
              "the leverage cap holds even at a vanishing volatility, where"
              " the uncapped formula would size to infinity");
    }
}

// ── 6 ────────────────────────────────────────────────────────────────────
// Rule 1: the lot size comes from the spec store, and the sizer must respect
// whatever it says rather than assuming anything.
void the_lot_size_comes_from_the_spec_and_is_respected()
{
    std::printf("\n6 the_lot_size_comes_from_the_spec_and_is_respected\n");
    std::printf("    lot size   notional/lot (Rs)   lots at Rs 1 cr\n");
    std::int64_t prev = -1;
    bool non_increasing = true;
    for (std::int64_t ls : {1, 25, 75, 500, 1250}) {
        SizingInputs in = base();
        in.lot_size = LotSize{ls};
        in.freeze_qty = Qty{0};             // no exchange cap, isolate lot size
        in.risk_fraction = 1.0;             // and no risk-budget bind
        in.stop_distance = Price{100};
        in.target_vol = 100.0;              // nor a vol bind
        const auto d = size_position(in);
        if (!d) { check(false, "spec lot size sizes"); return; }
        std::printf("    %8lld   %17.0f   %14lld\n",
                    static_cast<long long>(ls),
                    static_cast<double>(kPrice * ls) / 100.0,
                    static_cast<long long>(d->lots.raw()));
        if (prev >= 0 && d->lots.raw() > prev) { non_increasing = false; }
        prev = d->lots.raw();
        // Whatever the lot size, the notional must never exceed the budget.
        check(d->notional.raw() <= kCapital,
              "the position never exceeds the capital at any lot size");
    }
    check(non_increasing,
          "a larger lot size gives no more lots -- the same money buys fewer,"
          " larger contracts");

    check(!size_position([]{ auto i = base(); i.lot_size = LotSize{0}; return i; }()),
          "a zero lot size is refused, because it means the spec store was"
          " read before it was loaded (rule 1)");
}

// ── 7 ────────────────────────────────────────────────────────────────────
void degenerate_inputs_are_refused()
{
    std::printf("\n7 degenerate_inputs_are_refused\n");
    auto err = [](SizingInputs in) {
        const auto r = size_position(in);
        return r ? SizingError::NotFinite : r.error();
    };
    SizingInputs in = base();
    in.capital = Notional{0};
    check(err(in) == SizingError::NonPositiveCapital, "zero capital is refused");
    in = base(); in.capital = Notional{-1};
    check(err(in) == SizingError::NonPositiveCapital,
          "and negative capital");
    in = base(); in.price = Price{0};
    check(err(in) == SizingError::NonPositivePrice, "a zero price is refused");
    in = base(); in.lot_size = LotSize{-1};
    check(err(in) == SizingError::BadLotSize, "a negative lot size is refused");
    in = base(); in.period_vol = Vol{0.0};
    check(err(in) == SizingError::NonPositiveVol,
          "zero volatility is refused -- it would size to infinity");
    in = base(); in.risk_fraction = 0.0;
    check(err(in) == SizingError::BadParameter,
          "a zero risk fraction is refused");
    in = base(); in.risk_fraction = 1.5;
    check(err(in) == SizingError::BadParameter,
          "and risking more than the whole account");
    in = base(); in.stop_distance = Price{0};
    check(err(in) == SizingError::BadParameter,
          "a zero stop distance is refused -- it would divide by zero and"
          " size the whole book on one trade");
    in = base(); in.kelly_divisor = 0.0;
    check(err(in) == SizingError::BadParameter, "a zero Kelly divisor");
    in = base(); in.max_leverage = 0.0;
    check(err(in) == SizingError::BadParameter, "a zero leverage cap");

    check(SizeDecision{}.binding == SizingBound::Unset,
          "and a default-constructed decision does not read as a real one");
}

} // namespace

int main()
{
    std::printf("altair risk sizing tests\n");
    the_kelly_divisor_is_justified_by_its_own_arithmetic();
    everything_rounds_down_to_whole_lots();
    the_smallest_constraint_binds_and_is_named();
    an_edge_inside_its_own_error_bar_sizes_to_zero();
    vol_targeting_scales_inversely_and_is_capped();
    the_lot_size_comes_from_the_spec_and_is_respected();
    degenerate_inputs_are_refused();

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
