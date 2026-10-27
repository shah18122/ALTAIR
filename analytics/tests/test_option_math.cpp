// Tests for analytics/option_math.hpp -- held to the worked numbers in Nations'
// Options Math for Traders, plus a synthetic asymmetric spot-vol series.

#include <analytics/option_math.hpp>

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

void test_vol_slope() {
    // IV up 1.2 points per 1% fall, down 0.5 per 1% rise, plus deterministic noise.
    std::vector<double> ret, dv;
    for (int i = 0; i < 400; ++i) {
        const double r = 0.05 * ((i * 37) % 81 - 40);   // -2% .. +2%
        const double noise = 0.02 * (((i * 13) % 11) - 5);
        ret.push_back(r);
        dv.push_back((r < 0.0 ? -1.2 * r : -0.5 * r) + noise);
    }
    const VolSlope s = vol_slope(ret, dv);
    check(near(s.per_pct_fall, 1.2, 0.05) && near(s.per_pct_rise, 0.5, 0.05), "recovers 1.2 on falls, 0.5 on rises");
    check(s.n_fall > 0 && s.n_rise > 0 && s.n_fall + s.n_rise <= ret.size(), "unchanged days are skipped");
    check(near(linked_vol_points_asym(-2.0, s), 2.0 * s.per_pct_fall, 1e-12), "a 2% fall adds 2x the fall slope");
    check(near(linked_vol_points_asym(3.0, s), -3.0 * s.per_pct_rise, 1e-12), "a 3% rise takes 3x the rise slope");
    check(linked_vol_points_asym(0.0, s) == 0.0, "no move, no shift");
    std::vector<double> few(5, -1.0), fewv(5, 1.0);
    const VolSlope t = vol_slope(few, fewv);
    check(std::isnan(t.per_pct_fall) && std::isnan(t.per_pct_rise), "too few days: NaN, not a number from noise");
    check(std::isnan(vol_slope(few, dv).per_pct_fall), "mismatched lengths: NaN");
}

void test_probabilities() {
    // Ch. 10: LMN 77, breakeven 73, 60% IV, 30 days -> "about a 41 percent likelihood".
    const double p = prob_below(77.0, 73.0, 30.0 / 365.0, 0.60);
    check(near(p, 0.41, 0.01), "LMN below 73 at expiry ~41%");
    check(prob_below(100.0, 0.0, 0.1, 0.2) == 0.0, "nothing ends below zero");
    check(std::isnan(prob_below(100.0, 90.0, 0.0, 0.2)), "no time: NaN");
    // A straddle sold at the money for 0 credit loses almost surely; a huge credit almost never.
    const double tight = straddle_loss_probability(100.0, 100.0, 30.0 / 365.0, 0.2, 0.01);
    const double wide = straddle_loss_probability(100.0, 100.0, 30.0 / 365.0, 0.2, 30.0);
    check(tight > 0.99 && wide < 1e-4, "loss probability falls as the credit widens");
    // Fair ATM straddle (~0.8 sd wide) loses a bit over 40% of the time under its own model.
    const double T = 7.0 / 365.0, iv = 0.14, F = 24'000.0;
    const double fair = 0.8 * F * iv * std::sqrt(T);
    const double pl = straddle_loss_probability(F, F, T, iv, fair);
    check(pl > 0.38 && pl < 0.46, "a fairly priced ATM straddle loses ~42% of expiries");
    check(std::isnan(straddle_loss_probability(F, F, T, iv, 0.0)), "no credit: NaN");
}

void test_skew_premium_and_yield() {
    // An OTM put priced 6 vol points over ATM is worth more; at the ATM IV, nothing.
    const double F = 140.40, T = 30.0 / 365.0;
    const double prem = skew_premium(false, F, 130.0, T, 0.25, 0.19);
    check(prem > 0.0, "put skew adds rupees");
    check(near(skew_premium(false, F, 130.0, T, 0.19, 0.19), 0.0, 1e-12), "no skew, no premium");
    // Nations' point: the largest IV gap is not the largest rupee gap far from the money.
    const double far = skew_premium(false, F, 110.0, T, 0.40, 0.19);
    const double nearer = skew_premium(false, F, 134.0, T, 0.24, 0.19);
    check(nearer > far, "a smaller IV gap nearer the money pays more rupees");
    // Appendix: $4 on $77 a month is 83.7% compounded, not 62.4%.
    check(near(annualised_yield(4.0 / 77.0, 12.0), 0.837, 0.001), "5.2% a month compounds to 83.7%");
    check(std::isnan(annualised_yield(-1.5, 12.0)), "a yield below -100% is refused");
}

} // namespace

int main() {
    test_vol_slope();
    test_probabilities();
    test_skew_premium_and_yield();
    if (failures) { std::printf("%d check(s) FAILED\n", failures); return 1; }
    std::printf("all option_math checks passed\n");
    return 0;
}
