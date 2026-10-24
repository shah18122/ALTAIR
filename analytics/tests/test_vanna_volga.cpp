// Tests for analytics/vanna_volga.hpp -- the vanna-volga smile.

#include <analytics/sabr.hpp>
#include <analytics/vanna_volga.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

namespace vv = altair::vanna_volga;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}

double bs(bool call, double F, double K, double T, double s, double r) { return vv::detail::price(call, F, K, T, s, r); }

/// The vol that prices `c`, by bisection (double precision; no paise rounding).
double implied(bool call, double F, double K, double T, double r, double c) {
    double lo = 1e-4, hi = 3.0;
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        (bs(call, F, K, T, mid, r) < c ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

} // namespace

int main() {
    std::printf("Vanna-volga smile\n");
    // NIFTY-like: forward 25,000, 30 days, a put skew.
    const double F = 25'000.0, T = 30.0 / 365.0, r = 0.065;
    vv::Pillars p;
    p.k = {23'900.0, 25'000.0, 25'900.0};
    p.vol = {0.165, 0.125, 0.118};

    // 1. The pillars are reproduced: price and both vol forms.
    bool pillars_ok = true;
    for (std::size_t i = 0; i < 3; ++i) {
        const bool call = p.k[i] >= F;
        pillars_ok = pillars_ok && std::fabs(vv::price(call, F, p.k[i], T, r, p) - bs(call, F, p.k[i], T, p.vol[i], r)) < 1e-8
                  && std::fabs(vv::vol_first(p.k[i], p) - p.vol[i]) < 1e-12 && std::fabs(vv::vol(F, p.k[i], T, p) - p.vol[i]) < 1e-9;
    }
    check(pillars_ok, "at the three quoted strikes the vanna-volga price and vols are the market's");

    // 2. A flat smile stays flat.
    vv::Pillars flat = p;
    flat.vol = {0.14, 0.14, 0.14};
    bool flat_ok = true;
    for (double K = 22'000; K <= 28'000; K += 500)
        flat_ok = flat_ok && std::fabs(vv::vol(F, K, T, flat) - 0.14) < 1e-12
               && std::fabs(vv::price(K >= F, F, K, T, r, flat) - bs(K >= F, F, K, T, 0.14, r)) < 1e-8;
    check(flat_ok, "a flat smile gives the flat vol and the Black-76 price at every strike");

    // 3. The hedge matches vega, vanna and volga of the target at the ATM vol
    //    (the defining property of the weights), checked by finite differences.
    {
        const double s = p.vol[1], h = 1e-4, dF = 0.5;
        const auto vega = [&](double K) { return (bs(true, F, K, T, s + h, r) - bs(true, F, K, T, s - h, r)) / (2 * h); };
        const auto volga = [&](double K) {
            return (bs(true, F, K, T, s + h, r) - 2 * bs(true, F, K, T, s, r) + bs(true, F, K, T, s - h, r)) / (h * h);
        };
        const auto vanna = [&](double K) {
            return (bs(true, F + dF, K, T, s + h, r) - bs(true, F + dF, K, T, s - h, r) - bs(true, F - dF, K, T, s + h, r)
                    + bs(true, F - dF, K, T, s - h, r)) / (4 * dF * h);
        };
        double worst = 0.0;
        for (const double K : {23'500.0, 24'400.0, 25'300.0, 26'400.0}) {
            const auto x = vv::weights(F, K, T, p);
            const double tv = vega(K), ta = vanna(K), tg = volga(K);
            double hv = 0, ha = 0, hg = 0;
            for (std::size_t i = 0; i < 3; ++i) { hv += x[i] * vega(p.k[i]); ha += x[i] * vanna(p.k[i]); hg += x[i] * volga(p.k[i]); }
            worst = std::max({worst, std::fabs(hv - tv) / std::fabs(tv), std::fabs(ha - ta) / std::max(1e-9, std::fabs(ta) + 1e-3 * std::fabs(tv)),
                              std::fabs(hg - tg) / std::max(1e-9, std::fabs(tg) + 1e-3 * std::fabs(tv))});
        }
        std::printf("        worst relative mismatch of vega/vanna/volga: %.2e\n", worst);
        check(worst < 1e-3, "the three-option hedge carries the target's vega, vanna and volga");
    }

    // 4. The second-order closed form agrees with the vol the VV price implies.
    {
        double worst = 0.0;
        for (double K = 23'600; K <= 26'300; K += 100) {
            const bool call = K >= F;
            const double iv = implied(call, F, K, T, r, vv::price(call, F, K, T, r, p));
            worst = std::max(worst, std::fabs(iv - vv::vol(F, K, T, p)));
        }
        std::printf("        between the pillars, closed form vs price-implied: worst %.3f vol points\n", worst * 100);
        check(worst < 0.0010, "between the pillars the closed-form vol is within 0.1 vol points of the price's");
    }

    // 5. On a SABR smile, three pillars recover the rest of it.
    {
        altair::SabrParams sp;
        sp.alpha = 0.125 * std::pow(F, 0.5);
        sp.beta = 0.5;
        sp.rho = -0.45;
        sp.nu = 1.1;
        std::vector<vv::SmilePoint> smile;
        for (double K = 22'500; K <= 27'500; K += 100) {
            const auto s = altair::sabr_vol(F, K, T, sp);
            if (s) smile.push_back({K, *s});
        }
        const auto got = vv::pillars(F, T, smile);
        check(got.has_value() && got->k[0] < F && got->k[1] == 25'000.0 && got->k[2] > F, "pillars: a 25-delta put, the ATM, a 25-delta call");
        double worst = 0.0, worst_flat = 0.0;
        if (got)
            for (const auto& s : smile)
                if (s.k >= got->k[0] && s.k <= got->k[2]) {
                    worst = std::max(worst, std::fabs(vv::vol(F, s.k, T, *got) - s.vol));
                    worst_flat = std::max(worst_flat, std::fabs(got->vol[1] - s.vol));
                }
        std::printf("        SABR smile, pillars %.0f / %.0f / %.0f; inside them VV is off by %.4f vol points at worst, "
                    "one flat vol by %.2f\n", got ? got->k[0] : 0.0, got ? got->k[1] : 0.0, got ? got->k[2] : 0.0, worst * 100,
                    worst_flat * 100);
        check(got && worst < 0.004 && worst < 0.25 * worst_flat,
              "inside the pillars the VV smile is within 0.4 vol points of SABR, under a quarter of a flat vol's error");
    }

    // 6. Too little smile: no pillars.
    check(!vv::pillars(F, T, {{24'000, 0.15}, {25'000, 0.13}}), "fewer than three usable strikes: no pillars");

    std::printf("Vanna-volga smile: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
