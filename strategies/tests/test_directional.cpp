// P21-01/02 acceptance tests for strategies/momentum.hpp and
// strategies/meanrev.hpp.
//
// Eight tests, and the shape of them is the point.
//
// Test 1 is causality, and it is the one that has to pass before any other
// result means anything. It does not read the code; it PERTURBS the future and
// checks the past did not move. A strategy that fails this can post any Sharpe
// it likes and none of it is real.
//
// Test 2 is turnover accounting -- specifically that a full flip from +1 to -1
// is charged as TWO trades, not one. Halving that cost is a silent way to make
// exactly the highest-turnover strategies look affordable.
//
// Tests 3 and 4 are the two strategies against series built to suit them:
// momentum against a trend, mean reversion against an AR(1) with negative
// autocorrelation. They establish the machinery detects what it claims to.
//
// Test 5 is THE CONTROL, and it is the test expected to embarrass both. On a
// random walk neither has any gross edge, and both still pay their turnover,
// so the POOLED net over 20 paths must be negative and the pooled gross must
// not be significant.
//
// It asserts on the pooled statistic rather than on every path, because the
// first version of it -- "every path must lose" -- FAILED, and was wrong to
// have been written: 3 of 8 momentum paths came out net positive on pure
// noise. One path is one draw. The test now reports that scatter deliberately,
// because it is the most useful thing in this file: a single backtest that
// landed on one of those paths would have shown a profit on a series with
// nothing in it.
//
// Test 6 is the lookback sweep. The parameters here are choices rather than
// fits, so there is no in-sample optimism to deflate -- but picking a lookback
// by trying twelve and keeping the best would put it straight back, so the
// test prints the WHOLE curve and asserts only that the curve exists.
//
// Test 7 is hysteresis: that the dead band actually cuts turnover, measured.
//
// Test 8 is the cost hurdle arithmetic and the guards, including the one that
// refuses a mean-reversion spec whose dead band is empty.
//
// No check description here may contain the substring FAIL.

#include <strategies/meanrev.hpp>
#include <strategies/momentum.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
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

/// White noise in bps. The control series: no trend, no reversion, nothing to
/// find.
std::vector<double> random_walk(std::size_t n, double sd_bps, std::uint64_t seed)
{
    Lcg g{seed};
    std::vector<double> r(n);
    for (std::size_t i = 0; i < n; ++i) { r[i] = g.normal() * sd_bps; }
    return r;
}

/// Returns with POSITIVE autocorrelation -- a trend that persists. AR(1) on
/// the return itself with phi > 0.
std::vector<double> trending(std::size_t n, double phi, double sd_bps,
                             std::uint64_t seed)
{
    Lcg g{seed};
    std::vector<double> r(n);
    double prev = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = phi * prev + g.normal() * sd_bps;
        r[i] = x;
        prev = x;
    }
    return r;
}

/// A LEVEL that pulls back to a moving anchor -- Ornstein-Uhlenbeck in log
/// space, differenced back to returns. This is what mean reversion is
/// supposed to find, and building it as a level rather than as a negatively
/// autocorrelated return matters: the fade trades the level, so the series
/// has to mean-revert in the quantity the strategy actually measures.
std::vector<double> reverting(std::size_t n, double kappa, double sd_bps,
                              std::uint64_t seed)
{
    Lcg g{seed};
    std::vector<double> r(n);
    double level = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double dx = -kappa * level + g.normal() * sd_bps;
        level += dx;
        r[i] = dx;
    }
    return r;
}

constexpr double kCostBps = 5.5;      // the project-wide round-trip hurdle
constexpr double kBarsPerYear = 250.0;

void report(const char* name, const altair::StrategyResult& s)
{
    std::printf("        %-26s gross %+8.4f  net %+8.4f  turnover %6.4f  "
                "drag %7.4f  netSharpe %+7.3f  t %+6.2f\n",
                name, s.gross_bps, s.net_bps, s.turnover, s.cost_drag_bps(),
                s.net_sharpe, s.net_t());
}

} // namespace

using namespace altair;

// ---------------------------------------------------------------------------
// 1. Causality. Perturb the future; the past must not move.
// ---------------------------------------------------------------------------
namespace {

void test_causality()
{
    std::printf("\n[1] causality -- perturbing bar k must not move any "
                "position before k\n");

    const std::size_t n = 900;
    const std::size_t k = 700;
    std::vector<double> a = random_walk(n, 40.0, 11u);
    std::vector<double> b = a;
    // Replace the entire tail from k onward with something wildly different.
    for (std::size_t i = k; i < n; ++i) { b[i] = 500.0; }

    MomentumSpec ms;
    const auto pa = momentum_positions(a, ms);
    const auto pb = momentum_positions(b, ms);
    check(pa.has_value() && pb.has_value(), "momentum produced both series");

    MeanRevSpec vs;
    const auto qa = meanrev_positions(a, vs);
    const auto qb = meanrev_positions(b, vs);
    check(qa.has_value() && qb.has_value(), "mean reversion produced both");

    if (!pa || !pb || !qa || !qb) { return; }

    std::size_t mom_diff = 0, rev_diff = 0;
    // position[i] is decided from data through i-1, so identical inputs up to
    // k-1 must give identical positions up to and INCLUDING k-1... and also
    // at k, because position[k] reads r[..k-1] only.
    for (std::size_t i = 0; i <= k; ++i) {
        if ((*pa)[i] != (*pb)[i]) { ++mom_diff; }
        if ((*qa)[i] != (*qb)[i]) { ++rev_diff; }
    }
    std::printf("        momentum positions differing at or before k: %zu\n",
                mom_diff);
    std::printf("        meanrev  positions differing at or before k: %zu\n",
                rev_diff);
    check(mom_diff == 0, "momentum position[0..k] is blind to r[k..]");
    check(rev_diff == 0, "meanrev  position[0..k] is blind to r[k..]");

    // And the converse: the tail MUST react, or the test above is vacuous
    // because the strategy ignores its input entirely.
    std::size_t tail_diff = 0;
    for (std::size_t i = k + 1; i < n; ++i) {
        if ((*pa)[i] != (*pb)[i]) { ++tail_diff; }
    }
    std::printf("        momentum positions differing after k:        %zu\n",
                tail_diff);
    check(tail_diff > 0, "the tail does react, so test 1 is not vacuous");
}

// ---------------------------------------------------------------------------
// 2. Turnover accounting. A full flip is two trades.
// ---------------------------------------------------------------------------
void test_turnover_accounting()
{
    std::printf("\n[2] turnover -- a +1 to -1 flip is charged as TWO trades\n");

    const std::size_t n = 200;
    std::vector<double> r(n, 0.0);        // zero returns: cost only
    std::vector<double> pos(n, 0.0);

    // One entry to +1 at bar 10, one flip to -1 at bar 20. Nothing else.
    for (std::size_t i = 10; i < 20; ++i) { pos[i] = 1.0; }
    for (std::size_t i = 20; i < n; ++i) { pos[i] = -1.0; }

    const auto s = evaluate(pos, r, kCostBps, kBarsPerYear);
    check(s.has_value(), "evaluate accepted the hand-built series");
    if (!s) { return; }

    // Turnover units: 1.0 entering at bar 10, 2.0 flipping at bar 20 = 3.0.
    const double expect_units = 3.0;
    const double got_units = s->turnover * static_cast<double>(n);
    std::printf("        turnover units  expected %.1f   got %.6f\n",
                expect_units, got_units);
    check(std::fabs(got_units - expect_units) < 1e-9,
          "the flip counted 2.0 units, not 1.0");

    const double expect_cost = expect_units * kCostBps / static_cast<double>(n);
    std::printf("        cost drag/bar   expected %+.6f  got %+.6f\n",
                expect_cost, s->cost_drag_bps());
    check(std::fabs(s->cost_drag_bps() - expect_cost) < 1e-9,
          "cost drag equals turnover times cost exactly");
    check(std::fabs(s->gross_bps) < 1e-12,
          "gross is exactly zero on a zero-return series");
    check(s->net_bps < 0.0, "net is negative: cost with nothing to pay it");
}

// ---------------------------------------------------------------------------
// 3. Momentum on a trend.
// ---------------------------------------------------------------------------
void test_momentum_on_trend()
{
    std::printf("\n[3] momentum on a POSITIVELY autocorrelated series\n");

    const auto r = trending(4000, 0.55, 30.0, 21u);
    MomentumSpec ms;
    ms.lookback = 20;
    ms.vol_window = 120;
    const auto pos = momentum_positions(r, ms);
    check(pos.has_value(), "momentum produced a position series");
    if (!pos) { return; }

    const auto s = evaluate(*pos, r, kCostBps, kBarsPerYear);
    check(s.has_value(), "evaluate scored it");
    if (!s) { return; }
    report("momentum, phi=0.55", *s);

    check(s->gross_bps > 0.0,
          "gross edge is positive where a trend genuinely exists");
    check(s->active > 0, "the strategy actually held positions");
    std::printf("        round trips: %zu (a strategy that entered once and "
                "held is not one)\n", s->round_trips);
    check(s->round_trips > 1, "it traded more than once");

    // Against the control that matters for anything long-biased.
    const auto bh = buy_and_hold(r.size());
    const auto bhs = evaluate(bh, r, kCostBps, kBarsPerYear);
    const auto ex = excess_over(*pos, bh, r, kCostBps);
    check(bhs.has_value() && ex.has_value(), "buy-and-hold scored too");
    if (bhs && ex) {
        report("buy-and-hold", *bhs);
        std::printf("        momentum OVER buy-and-hold: %+.4f bps/bar "
                    "(t %+.2f)\n", ex->mean_bps, ex->t);
        check(std::fabs((s->net_bps - bhs->net_bps) - ex->mean_bps) < 1e-9,
              "the paired excess equals the difference of the two nets");
    }
    // The honest half of the result: whether the gross edge SURVIVES.
    std::printf("        hurdle at this turnover: %.4f bps/bar   gross: "
                "%.4f bps/bar   %s\n",
                required_gross_bps(s->turnover, kCostBps), s->gross_bps,
                s->net_bps > 0.0 ? "clears" : "does NOT clear");
}

// ---------------------------------------------------------------------------
// 4. Mean reversion on a reverting level.
// ---------------------------------------------------------------------------
void test_meanrev_on_reverting()
{
    std::printf("\n[4] mean reversion on an Ornstein-Uhlenbeck level\n");

    const auto r = reverting(4000, 0.04, 30.0, 31u);
    MeanRevSpec vs;
    vs.window = 60;
    vs.entry_z = 1.5;
    vs.exit_z = 0.5;
    const auto pos = meanrev_positions(r, vs);
    check(pos.has_value(), "meanrev produced a position series");
    if (!pos) { return; }

    const auto s = evaluate(*pos, r, kCostBps, kBarsPerYear);
    check(s.has_value(), "evaluate scored it");
    if (!s) { return; }
    report("meanrev, kappa=0.04", *s);

    check(s->gross_bps > 0.0,
          "gross edge is positive where the level genuinely reverts");
    std::printf("        hurdle at this turnover: %.4f bps/bar   gross: "
                "%.4f bps/bar   %s\n",
                required_gross_bps(s->turnover, kCostBps), s->gross_bps,
                s->net_bps > 0.0 ? "clears" : "does NOT clear");
}

// ---------------------------------------------------------------------------
// 5. THE CONTROL. Both must lose on noise.
// ---------------------------------------------------------------------------
void test_random_walk_control()
{
    std::printf("\n[5] CONTROL -- white noise, 20 independent paths\n");
    std::printf("        There is no edge to find and the turnover is still\n"
                "        charged, so the POOLED net must be negative and the\n"
                "        POOLED gross must be indistinguishable from zero.\n");

    constexpr int kTrials = 20;
    double mg[2] = {0.0, 0.0}, mn[2] = {0.0, 0.0};
    double sg[2] = {0.0, 0.0};
    int pos_net[2] = {0, 0};
    std::vector<double> g0, g1;
    g0.reserve(kTrials);
    g1.reserve(kTrials);

    for (int t = 0; t < kTrials; ++t) {
        const auto r = random_walk(4000, 40.0,
                                   1000u + static_cast<std::uint64_t>(t));
        const auto mp = momentum_positions(r, MomentumSpec{});
        const auto vp = meanrev_positions(r, MeanRevSpec{});
        if (!mp || !vp) { continue; }
        const auto ms = evaluate(*mp, r, kCostBps, kBarsPerYear);
        const auto vs = evaluate(*vp, r, kCostBps, kBarsPerYear);
        if (!ms || !vs) { continue; }
        if (t == 0) {
            report("momentum, path 0", *ms);
            report("meanrev,  path 0", *vs);
        }
        g0.push_back(ms->gross_bps);
        g1.push_back(vs->gross_bps);
        mg[0] += ms->gross_bps;   mn[0] += ms->net_bps;
        mg[1] += vs->gross_bps;   mn[1] += vs->net_bps;
        if (ms->net_bps > 0.0) { ++pos_net[0]; }
        if (vs->net_bps > 0.0) { ++pos_net[1]; }
    }
    const double n = static_cast<double>(g0.size());
    check(g0.size() == kTrials && g1.size() == kTrials,
          "all 20 noise paths produced a scored result");
    if (g0.size() != kTrials) { return; }

    for (int k = 0; k < 2; ++k) { mg[k] /= n; mn[k] /= n; }
    for (const double v : g0) { sg[0] += (v - mg[0]) * (v - mg[0]); }
    for (const double v : g1) { sg[1] += (v - mg[1]) * (v - mg[1]); }
    for (int k = 0; k < 2; ++k) { sg[k] = std::sqrt(sg[k] / (n - 1.0)); }

    const char* names[2] = {"momentum", "meanrev "};
    bool ok_gross = true, ok_net = true;
    for (int k = 0; k < 2; ++k) {
        const double se = sg[k] / std::sqrt(n);
        const double tstat = se > 0.0 ? mg[k] / se : 0.0;
        std::printf("        %s  pooled gross %+7.4f (sd across paths %6.4f, "
                    "t %+6.2f)   pooled net %+7.4f\n",
                    names[k], mg[k], sg[k], tstat, mn[k]);
        if (std::fabs(tstat) >= 3.0) { ok_gross = false; }
        if (!(mn[k] < 0.0)) { ok_net = false; }
    }
    check(ok_gross, "neither pooled gross edge is significant on noise");
    check(ok_net, "both pooled NET returns are negative: the drag is real");

    // THE FINDING, and it is the reason this test exists in this form.
    //
    // The first version of this test asserted that EVERY path lost money. It
    // failed: 3 of 8 momentum paths and 2 of 8 mean-reversion paths came out
    // net POSITIVE on pure noise. The assertion was wrong, not the strategy --
    // a single path is one draw, and its standard error here is comparable to
    // the whole cost drag.
    //
    // Which is the point worth keeping: if a single backtest had been run and
    // it happened to land on one of those paths, the result would have been a
    // positive net on a series with NOTHING IN IT. That is what one backtest
    // is worth.
    std::printf("        paths with POSITIVE net, by chance alone: "
                "momentum %d/%d, meanrev %d/%d\n",
                pos_net[0], kTrials, pos_net[1], kTrials);
    std::printf("        A SINGLE backtest landing on one of those would have\n"
                "        shown a profit on a series containing no signal.\n");
    check(pos_net[0] > 0 || pos_net[1] > 0,
          "some individual noise paths DO show a profit -- one path is not "
          "evidence");
}

// ---------------------------------------------------------------------------
// 6. The lookback sweep -- the whole curve, not the best point.
// ---------------------------------------------------------------------------
void test_lookback_sweep()
{
    std::printf("\n[6] lookback sweep on the trending series\n");
    std::printf("        Printed in full BECAUSE reporting only the best "
                "lookback\n"
                "        is how a choice becomes a fit.\n");

    const auto r = trending(4000, 0.55, 30.0, 21u);
    const std::size_t lookbacks[] = {5, 10, 20, 40, 60, 120, 250};
    std::size_t produced = 0;
    for (const std::size_t lb : lookbacks) {
        MomentumSpec ms;
        ms.lookback = lb;
        const auto pos = momentum_positions(r, ms);
        if (!pos) { continue; }
        const auto s = evaluate(*pos, r, kCostBps, kBarsPerYear);
        if (!s) { continue; }
        ++produced;
        std::printf("        lookback %4zu   gross %+8.4f   net %+8.4f   "
                    "turnover %6.4f   netSharpe %+7.3f\n",
                    lb, s->gross_bps, s->net_bps, s->turnover, s->net_sharpe);
    }
    check(produced == sizeof(lookbacks) / sizeof(lookbacks[0]),
          "every lookback in the sweep produced a scored result");
}

// ---------------------------------------------------------------------------
// 7. Hysteresis cuts turnover, measured.
// ---------------------------------------------------------------------------
void test_hysteresis()
{
    std::printf("\n[7] hysteresis -- the dead band must cut turnover\n");

    const auto r = reverting(4000, 0.04, 30.0, 31u);

    MeanRevSpec narrow;                  // dead band almost empty
    narrow.entry_z = 1.5;
    narrow.exit_z = 1.4999;
    MeanRevSpec wide;                    // real dead band
    wide.entry_z = 1.5;
    wide.exit_z = 0.25;

    const auto np = meanrev_positions(r, narrow);
    const auto wp = meanrev_positions(r, wide);
    check(np.has_value() && wp.has_value(), "both specs produced positions");
    if (!np || !wp) { return; }
    const auto ns = evaluate(*np, r, kCostBps, kBarsPerYear);
    const auto ws = evaluate(*wp, r, kCostBps, kBarsPerYear);
    if (!ns || !ws) { return; }

    report("exit_z 1.4999 (no band)", *ns);
    report("exit_z 0.25   (wide band)", *ws);
    std::printf("        turnover cut: %6.4f -> %6.4f  (%.1f%% less)\n",
                ns->turnover, ws->turnover,
                ns->turnover > 0.0
                    ? 100.0 * (1.0 - ws->turnover / ns->turnover) : 0.0);
    check(ws->turnover < ns->turnover,
          "the wider dead band traded strictly less");
    check(ws->cost_drag_bps() < ns->cost_drag_bps(),
          "and therefore paid strictly less cost");
}

// ---------------------------------------------------------------------------
// 8. Hurdle arithmetic and the guards.
// ---------------------------------------------------------------------------
void test_hurdle_and_guards()
{
    std::printf("\n[8] cost hurdle, and the specs that must be refused\n");

    check(std::fabs(required_gross_bps(0.4, 5.5) - 2.2) < 1e-12,
          "0.4 turnover at 5.5 bps needs 2.2 bps per bar");
    check(std::fabs(required_gross_bps(0.0, 5.5)) < 1e-12,
          "a strategy that never trades has a zero hurdle");

    // 20 bps average win, 20 bps average loss, 0.4 turnover at 5.5 bps.
    const double h = breakeven_hit_rate(20.0, 20.0, 0.4, 5.5);
    std::printf("        breakeven hit rate at +-20 bps, 0.4 turnover: "
                "%.4f\n", h);
    check(std::fabs(h - 0.555) < 1e-9,
          "breakeven hit rate is (loss + hurdle) / (win + loss)");
    std::printf("        the reality-check ceiling for 10-minute direction "
                "is 0.52-0.55.\n"
                "        A rule needing 0.5550 is asking for more than the "
                "horizon gives.\n");
    check(std::isnan(breakeven_hit_rate(0.0, 0.0, 0.4, 5.5)),
          "a degenerate payoff returns NaN, not a plausible number");

    // The empty dead band is the naive fade wearing two parameters.
    MeanRevSpec bad;
    bad.entry_z = 1.0;
    bad.exit_z = 1.0;
    check(!bad.valid(), "exit_z == entry_z is refused");
    bad.exit_z = 1.5;
    check(!bad.valid(), "exit_z > entry_z is refused");
    // The other end, and the one that cost a wrong number in a table: |z| < 0
    // is unsatisfiable, so the position enters once and never exits. That is
    // buy-and-hold with a random sign, not a fade.
    bad.exit_z = 0.0;
    check(!bad.valid(), "exit_z == 0 is refused: the exit can never fire");

    const auto r = random_walk(400, 40.0, 7u);
    const auto refused = meanrev_positions(r, bad);
    check(!refused.has_value()
              && refused.error() == StratError::BadParameter,
          "and meanrev_positions refuses it rather than silently repairing");

    MomentumSpec ms;
    ms.lookback = 1;
    check(!ms.valid(), "a one-bar lookback is refused");

    const auto too_short = momentum_positions(random_walk(50, 40.0, 9u),
                                              MomentumSpec{});
    check(!too_short.has_value()
              && too_short.error() == StratError::TooFewSamples,
          "50 bars is refused as too few, not scored on 3 observations");

    // rolling_z leaves NaN where it cannot measure -- absence is not zero.
    const std::vector<double> flat(300, 42.0);
    const auto z = rolling_z(flat, 60);
    std::size_t nans = 0;
    for (std::size_t i = 60; i < z.size(); ++i) {
        if (std::isnan(z[i])) { ++nans; }
    }
    std::printf("        flat series: %zu of %zu in-window z values are NaN\n",
                nans, z.size() - 60);
    check(nans == z.size() - 60,
          "a flat window yields NaN, never a confident z of 0.0");
}

} // namespace

int main()
{
    std::printf("P21-01/02 -- momentum and mean reversion\n");
    std::printf("Cost: %.1f bps per unit of turnover. %.0f bars per year.\n",
                kCostBps, kBarsPerYear);

    test_causality();
    test_turnover_accounting();
    test_momentum_on_trend();
    test_meanrev_on_reverting();
    test_random_walk_control();
    test_lookback_sweep();
    test_hysteresis();
    test_hurdle_and_guards();

    std::printf("\n%s -- %d failing check(s)\n",
                failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
