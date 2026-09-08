// P22-01 acceptance tests for strategies/overnight.hpp.
//
// Eight tests, on constructed sessions where the answer is known by
// construction rather than measured.
//
// Test 1 is the conservation identity: overnight plus session equals close to
// close, EXACTLY, for every session. Everything else in the file rests on it,
// because the benchmark arithmetic is derived from it rather than fitted.
//
// Test 2 is causality. The filter reads session i's own return, which is
// complete at the close, and night i begins after that. The test perturbs the
// night's OUTCOME and checks the decision to take it did not move.
//
// Test 3 is the claim that trading less often cannot make the strategy
// affordable: the break-even round trip equals the conditional mean and is
// independent of how many nights are taken.
//
// Test 4 is the asymmetry that follows -- the break-even AGAINST BUY AND HOLD
// does depend on frequency, because skipping a night means being flat and
// being flat on a drifting index forgoes the drift.
//
// Test 5 is the execution assumption, which is the whole risk in this
// strategy: a constructed series that gives half the gap back in the first
// five minutes must show `Exit::FirstBar` measuring half the edge.
//
// Test 6 is the cost sweep, which must be monotone and must cross zero at the
// break-even the result reports.
//
// Test 7 is a series with NO overnight drift: break-even zero, and a loss at
// any positive cost.
//
// Test 8 is the guards, including the one that refuses `Exit::FirstBar` on
// data that has no first-bar price rather than quietly substituting the open.
//
// No check description here may contain the substring FAIL.

#include <strategies/overnight.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

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

struct Lcg {
    std::uint64_t s;
    double uniform()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & ((1ULL << 53) - 1))
               / static_cast<double>(1ULL << 53);
    }
    double normal()
    {
        const double u1 = uniform();
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1 + 1e-300))
               * std::cos(6.283185307179586 * u2);
    }
};

/// Sessions built from an explicit overnight drift and session drift, in bps,
/// so every quantity the strategy reports is known before it is measured.
///
/// `giveback` is the FRACTION of the overnight gap handed back by the close of
/// the first intraday bar -- the thing that decides whether this strategy
/// survives being unable to trade the auction.
std::vector<altair::SessionBar> make_sessions(std::size_t n,
                                              double overnight_bps,
                                              double session_bps,
                                              double noise_bps,
                                              double giveback,
                                              std::uint64_t seed)
{
    Lcg g{seed};
    std::vector<altair::SessionBar> out;
    out.reserve(n);
    double px = 10000.0;
    // Session 0 has no previous close, so only its close is ever read.
    out.push_back({px, px, px});
    for (std::size_t i = 1; i < n; ++i) {
        const double on = (overnight_bps + noise_bps * g.normal()) / 10000.0;
        const double se = (session_bps + noise_bps * g.normal()) / 10000.0;
        altair::SessionBar b;
        b.open = px * std::exp(on);
        b.first_bar_close = px * std::exp(on * (1.0 - giveback));
        b.close = b.open * std::exp(se);
        px = b.close;
        out.push_back(b);
    }
    return out;
}

constexpr double kEps = 1e-9;

} // namespace

using namespace altair;

namespace {

// ---------------------------------------------------------------------------
// 1. Conservation.
// ---------------------------------------------------------------------------
void test_conservation()
{
    std::printf("\n[1] overnight + session == close-to-close, every session\n");

    const auto bars = make_sessions(1200, 8.0, -5.0, 40.0, 0.4, 7u);
    const auto r = session_returns(bars);
    check(r.has_value(), "the decomposition was produced");
    if (!r) { return; }

    double worst = 0.0;
    for (std::size_t i = 0; i < r->overnight.size(); ++i) {
        const double d = std::fabs(r->overnight[i] + r->session[i]
                                   - r->close_to_close[i]);
        worst = std::max(worst, d);
    }
    std::printf("        %zu sessions, worst residual %.3e bps\n",
                r->overnight.size(), worst);
    check(worst < 1e-9,
          "the two halves of the day reconstruct the whole day exactly");
    check(r->overnight.size() == bars.size() - 1,
          "one return per session after the first, which has no predecessor");
}

// ---------------------------------------------------------------------------
// 2. Causality.
// ---------------------------------------------------------------------------
void test_causality()
{
    std::printf("\n[2] the take/skip decision cannot see the night's outcome\n");

    auto a = make_sessions(1200, 8.0, -5.0, 40.0, 0.4, 11u);
    auto b = a;
    // Change ONLY the outcome of night 600: move its open, and move its close
    // by the same factor so the SESSION return -- which is what the filter
    // reads -- is untouched.
    const double bump = std::exp(300.0 / 10000.0);
    b[600].open *= bump;
    b[600].close *= bump;
    b[600].first_bar_close *= bump;

    OvernightSpec spec;
    spec.session_below_bps = 0.0;        // a filter that actually excludes
    const auto ra = run_overnight(a, spec);
    const auto rb = run_overnight(b, spec);
    check(ra.has_value() && rb.has_value(), "both runs produced a result");
    if (!ra || !rb) { return; }

    std::printf("        nights taken: unperturbed %zu, perturbed %zu\n",
                ra->taken, rb->taken);
    check(ra->taken == rb->taken,
          "moving the night's outcome did not change which nights were taken");
    // And the converse, so the test is not vacuous: the RESULT must move.
    std::printf("        mean over taken nights: %+.4f -> %+.4f\n",
                ra->mean_taken_bps, rb->mean_taken_bps);
    check(std::fabs(ra->mean_taken_bps - rb->mean_taken_bps) > kEps,
          "but the measured return did move, so the test is not vacuous");
}

// ---------------------------------------------------------------------------
// 3. Break-even is independent of frequency.
// ---------------------------------------------------------------------------
void test_breakeven_independent_of_frequency()
{
    std::printf("\n[3] trading less often cannot lower the break-even cost\n");
    std::printf("        Half the gross and half the cost leaves the ratio\n"
                "        alone. Only a higher CONDITIONAL MEAN helps.\n");

    // Overnight return independent of the preceding session, so filtering on
    // the session changes HOW OFTEN, not the conditional mean.
    const auto bars = make_sessions(4000, 9.0, -4.0, 50.0, 0.0, 23u);
    const double thresholds[] = {1e18, 40.0, 0.0, -40.0};
    double first_be = 0.0;
    std::size_t rows = 0, agree = 0;
    for (const double th : thresholds) {
        OvernightSpec spec;
        spec.session_below_bps = th;
        const auto res = run_overnight(bars, spec);
        if (!res) { continue; }
        if (rows == 0) { first_be = res->breakeven_rt_bps(); }
        ++rows;
        const double share = 100.0 * static_cast<double>(res->taken)
                           / static_cast<double>(res->sessions);
        std::printf("        session < %10.1f  took %5zu (%5.1f%%)  "
                    "mean %+7.4f  breakeven RT %+7.4f\n",
                    th, res->taken, share, res->mean_taken_bps,
                    res->breakeven_rt_bps());
        // The conditional mean is a sample statistic, so it wanders; what must
        // hold exactly is that break-even IS the conditional mean.
        if (std::fabs(res->breakeven_rt_bps() - res->mean_taken_bps) < kEps) {
            ++agree;
        }
    }
    check(rows == 4, "every threshold produced a result");
    check(agree == rows,
          "break-even equals the conditional mean at every frequency");
    std::printf("        (the four break-evens differ only by sampling: "
                "%.4f at the top)\n", first_be);
}

// ---------------------------------------------------------------------------
// 4. But break-even AGAINST HOLDING does depend on frequency.
// ---------------------------------------------------------------------------
void test_selectivity_costs_against_hold()
{
    std::printf("\n[4] being flat is not free -- selectivity loses to the "
                "drift\n");

    const auto bars = make_sessions(4000, 9.0, -4.0, 50.0, 0.0, 23u);
    OvernightSpec all;
    OvernightSpec few;
    few.session_below_bps = -40.0;
    const auto ra = run_overnight(bars, all);
    const auto rf = run_overnight(bars, few);
    check(ra.has_value() && rf.has_value(), "both specs produced results");
    if (!ra || !rf) { return; }

    std::printf("        every night : took %5zu  breakeven vs hold %+7.4f\n",
                ra->taken, ra->breakeven_rt_vs_hold_bps());
    std::printf("        selective   : took %5zu  breakeven vs hold %+7.4f\n",
                rf->taken, rf->breakeven_rt_vs_hold_bps());
    check(rf->taken < ra->taken, "the selective spec did trade less");
    check(rf->breakeven_rt_vs_hold_bps() < ra->breakeven_rt_vs_hold_bps(),
          "and its bar against buy-and-hold is strictly harder to clear");

    // The identity the header claims, for the take-everything case:
    //   breakeven vs hold == mean overnight - mean close-to-close == -session
    const double identity = ra->mean_taken_bps - ra->hold_bps;
    std::printf("        identity: mean_overnight - hold = %+.4f, "
                "-session = %+.4f\n", identity, -ra->session_bps);
    check(std::fabs(identity - ra->breakeven_rt_vs_hold_bps()) < 1e-9,
          "taking every night, break-even vs hold is exactly that difference");
    check(std::fabs(identity + ra->session_bps) < 1e-9,
          "which is exactly the session drag the strategy avoids");
}

// ---------------------------------------------------------------------------
// 5. THE EXECUTION ASSUMPTION.
// ---------------------------------------------------------------------------
void test_exit_assumption()
{
    std::printf("\n[5] exiting five minutes late, on a series that gives back\n"
                "    exactly half the gap\n");

    const auto bars = make_sessions(4000, 12.0, -6.0, 0.0, 0.5, 31u);
    OvernightSpec at_open;
    OvernightSpec at_first;
    at_first.exit = Exit::FirstBar;
    const auto ro = run_overnight(bars, at_open);
    const auto rf = run_overnight(bars, at_first);
    check(ro.has_value() && rf.has_value(), "both exits produced results");
    if (!ro || !rf) { return; }

    std::printf("        exit at open      mean %+8.4f  breakeven RT %+8.4f\n",
                ro->mean_taken_bps, ro->breakeven_rt_bps());
    std::printf("        exit at 09:20     mean %+8.4f  breakeven RT %+8.4f\n",
                rf->mean_taken_bps, rf->breakeven_rt_bps());
    const double ratio = rf->mean_taken_bps / ro->mean_taken_bps;
    std::printf("        the later exit kept %.1f%% of the edge\n",
                100.0 * ratio);
    check(std::fabs(ratio - 0.5) < 1e-6,
          "a 50% giveback halves the edge, exactly as constructed");
    check(rf->breakeven_rt_bps() < ro->breakeven_rt_bps(),
          "so the cost the strategy can bear halves too");
}

// ---------------------------------------------------------------------------
// 6. The cost sweep.
// ---------------------------------------------------------------------------
void test_cost_sweep()
{
    std::printf("\n[6] net against round-trip cost, and where it crosses\n");

    const auto bars = make_sessions(4000, 9.0, -4.0, 50.0, 0.3, 41u);
    const auto res = run_overnight(bars, OvernightSpec{});
    check(res.has_value(), "the run produced a result");
    if (!res) { return; }

    std::printf("        buy-and-hold %+.4f bps/session; break-even RT "
                "%.4f (vs zero), %.4f (vs hold)\n",
                res->hold_bps, res->breakeven_rt_bps(),
                res->breakeven_rt_vs_hold_bps());
    std::printf("        RT bps      net   excess vs hold\n");
    double prev = std::numeric_limits<double>::infinity();
    bool monotone = true;
    for (const double rt : {0.0, 2.0, 4.0, 6.0, 8.0, 10.0, 12.0}) {
        const double net = res->net_at(rt);
        std::printf("        %6.1f  %+8.4f  %+8.4f\n",
                    rt, net, res->excess_at(rt));
        if (net > prev) { monotone = false; }
        prev = net;
    }
    check(monotone, "net falls monotonically as the round trip rises");
    check(std::fabs(res->net_at(res->breakeven_rt_bps())) < 1e-9,
          "net is exactly zero at the reported break-even");
    check(std::fabs(res->excess_at(res->breakeven_rt_vs_hold_bps())) < 1e-9,
          "and excess over hold is exactly zero at its own break-even");
}

// ---------------------------------------------------------------------------
// 7. No drift: nothing to find.
// ---------------------------------------------------------------------------
void test_no_drift()
{
    std::printf("\n[7] a series with NO overnight drift\n");

    const auto bars = make_sessions(4000, 0.0, 0.0, 50.0, 0.3, 53u);
    const auto res = run_overnight(bars, OvernightSpec{});
    check(res.has_value(), "the run produced a result");
    if (!res) { return; }

    std::printf("        mean %+.4f bps (t %+.2f), break-even RT %.4f\n",
                res->mean_taken_bps, res->t_taken, res->breakeven_rt_bps());
    check(std::fabs(res->t_taken) < 3.0,
          "the overnight mean is not significant, as constructed");
    check(res->net_at(6.0) < 0.0,
          "and at a realistic round trip the strategy loses money");
    std::printf("        hit rate %.3f\n", res->hit_rate);
    check(std::fabs(res->hit_rate - 0.5) < 0.05,
          "roughly half the nights are up, as a driftless series requires");
}

// ---------------------------------------------------------------------------
// 8. Guards.
// ---------------------------------------------------------------------------
void test_guards()
{
    std::printf("\n[8] the inputs that must be refused\n");

    const auto few = run_overnight(
        make_sessions(40, 8.0, -4.0, 30.0, 0.3, 61u), OvernightSpec{});
    check(!few.has_value()
              && few.error() == OvernightError::TooFewSessions,
          "40 sessions is refused, not scored");

    OvernightSpec bad;
    bad.size = 0.0;
    const auto zero_size = run_overnight(
        make_sessions(400, 8.0, -4.0, 30.0, 0.3, 61u), bad);
    check(!zero_size.has_value()
              && zero_size.error() == OvernightError::BadParameter,
          "a zero position size is refused");

    // Absence is not zero: a session series with no first-bar price cannot be
    // exited at the first bar, and must say so rather than using the open.
    auto stripped = make_sessions(400, 8.0, -4.0, 30.0, 0.3, 67u);
    for (auto& b : stripped) { b.first_bar_close = 0.0; }
    OvernightSpec at_first;
    at_first.exit = Exit::FirstBar;
    const auto missing = run_overnight(stripped, at_first);
    check(!missing.has_value()
              && missing.error() == OvernightError::MissingFirstBar,
          "a missing first-bar price is refused, never replaced by the open");
    // The same data at the open exit is fine, so the refusal is specific.
    check(run_overnight(stripped, OvernightSpec{}).has_value(),
          "and the same data still works for the open exit");

    auto broken = make_sessions(400, 8.0, -4.0, 30.0, 0.3, 71u);
    broken[200].open = 0.0;
    const auto bad_price = run_overnight(broken, OvernightSpec{});
    check(!bad_price.has_value()
              && bad_price.error() == OvernightError::BadPrice,
          "a non-positive price is refused rather than logged");

    // A filter that excludes everything reports zero taken rather than
    // dividing by it.
    OvernightSpec none;
    none.session_below_bps = -1e18;
    const auto empty = run_overnight(
        make_sessions(400, 8.0, -4.0, 30.0, 0.3, 73u), none);
    check(empty.has_value() && empty->taken == 0,
          "a filter that takes nothing returns zero taken, not a crash");
    if (empty) {
        check(std::fabs(empty->gross_per_session_bps()) < kEps,
              "and its gross per session is zero, not undefined");
    }
}

} // namespace

int main()
{
    std::printf("P22-01 -- the overnight gap strategy\n");
    std::printf("Cost is quoted as an ALL-IN ROUND TRIP: one in-and-out per "
                "night.\n");

    test_conservation();
    test_causality();
    test_breakeven_independent_of_frequency();
    test_selectivity_costs_against_hold();
    test_exit_assumption();
    test_cost_sweep();
    test_no_drift();
    test_guards();

    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
