// Tests for analytics/gamma_trading.hpp -- held to the worked examples in
// Gleadall's Volcube guides, so a sign or a factor of two cannot slip in.

#include <analytics/gamma_trading.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using namespace altair;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

void test_breakeven_and_quadratic_pnl() {
    // Option Gamma Trading, section 3: 50 gamma, $75 theta, breakeven ~ $1.73.
    check(near(gamma_breakeven_move(75.0, 50.0), std::sqrt(3.0), 1e-12), "breakeven move sqrt(2*75/50) = 1.73");
    check(near(gamma_breakeven_move(-75.0, -50.0), std::sqrt(3.0), 1e-12), "breakeven ignores the signs");
    check(std::isnan(gamma_breakeven_move(75.0, 0.0)), "no gamma: no breakeven");
    // 1/2 * 50 * 2^2 = 100; doubling the move quadruples it.
    check(near(gamma_hedge_pnl(50.0, 2.0), 100.0, 1e-12), "half gamma move squared");
    check(near(gamma_hedge_pnl(50.0, 4.0), 4.0 * gamma_hedge_pnl(50.0, 2.0), 1e-12), "double the move, four times the P&L");
    check(gamma_hedge_pnl(-50.0, 2.0) < 0.0, "short gamma loses on the hedge");
    // A hedge at the breakeven pays exactly the day's theta.
    const double b = gamma_breakeven_move(75.0, 50.0);
    check(near(gamma_hedge_pnl(50.0, b), 75.0, 1e-9), "a hedge at the breakeven pays one day's theta");
}

void test_bands() {
    const auto b = short_gamma_bands(75.0, -50.0);   // default half the breakeven each side
    check(near(b.up, 0.5 * std::sqrt(3.0), 1e-12) && near(b.down, b.up, 1e-12), "default bands are half the breakeven");
    // Four half-distance hedges cost one day's theta.
    check(near(4.0 * std::fabs(gamma_hedge_pnl(-50.0, b.up)), 75.0, 1e-9), "four half-band hedges = one day's theta");
    const auto a = short_gamma_bands(75.0, -50.0, 0.6, 0.3);
    check(a.down < a.up, "tighter downside band when asked");
    check(!outside_band(a, 0.5 * a.up) && outside_band(a, a.up) && outside_band(a, -a.down)
              && !outside_band(a, -0.5 * a.down),
          "outside_band honours each side");
    check(outside_band(HedgeBands{}, 0.0), "a failed band says re-hedge");
}

void test_ratios_and_vol_readbacks() {
    // Exercise 4.7: short 300 gamma for $500 vs short 400 for $700 -- the second is
    // (marginally) more efficient for a seller, i.e. a LOWER ratio.
    check(gamma_theta_ratio(-400.0, 700.0) < gamma_theta_ratio(-300.0, 500.0), "seller prefers the lower gamma/theta");
    // Trading Implied Volatility: sold 25%, short $10,000 vega, made $15,000 -> 23.5%.
    check(near(locked_in_vol(0.25, 15'000.0, 10'000.0), 0.235, 1e-12), "locked-in vol 25 - 1.5 = 23.5");
    check(near(locked_in_vol(0.25, -5'000.0, -10'000.0), 0.255, 1e-12), "a loss locks in above the sale");
    // 10,000 of month-2 vega at T=0.18 is ~7,453 of month-1 (T=0.1) vega.
    check(near(time_weighted_vega(10'000.0, 0.18, 0.1), 10'000.0 * std::sqrt(0.1 / 0.18), 1e-9)
              && near(time_weighted_vega(10'000.0, 0.18, 0.1), 7453.56, 0.01),
          "time-weighted vega 7,453");
    // Option Gamma Trading: $100 at 25% over sqrt(250) is about $1.58 a day.
    check(near(implied_daily_move(100.0, 0.25, 250.0), 1.5811, 1e-4), "implied daily move 1.58");
    // Market Making: 8 cents wide on 16 vega is half a vol.
    check(near(spread_in_vols(1.16, 1.24, 0.16), 0.5, 1e-9), "8 cents on 16 vega is half a vol wide");
    check(std::isnan(spread_in_vols(1.24, 1.16, 0.16)), "a crossed quote is refused");
    check(near(premium_in_vol(4.0, 20.0), 0.002, 1e-12), "4 rupees on 20 vega per point = 0.2 vol points");
    check(near(linked_vol_points(-2.0, 1.5), 3.0, 1e-12) && near(linked_vol_points(2.0, 1.5), -3.0, 1e-12),
          "a 2% fall adds 3 vol points at 1.5 per %");
}

void test_vega_beta() {
    std::vector<double> bench, front;
    for (int i = 0; i < 60; ++i) {
        const double x = std::sin(0.7 * i) * 0.01;
        bench.push_back(x);
        front.push_back(1.8 * x);   // front IV moves 1.8x the benchmark
    }
    check(near(vega_beta(front, bench), 1.8, 1e-12), "measured vega beta");
    front[3] = std::nan("");
    check(near(vega_beta(front, bench), 1.8, 1e-12), "NaN pairs are skipped");
    check(std::isnan(vega_beta(std::span<const double>(front).first(5), std::span<const double>(bench).first(5))),
          "too few pairs: no beta");
}

} // namespace

int main() {
    test_breakeven_and_quadratic_pnl();
    test_bands();
    test_ratios_and_vol_readbacks();
    test_vega_beta();
    std::printf(failures == 0 ? "gamma_trading: all passed\n" : "gamma_trading: %d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
