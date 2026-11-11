// live/tests/test_basket_alpha.cpp -- the basket-alpha grid and features match their definitions.
#include <live/basket_alpha.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}
bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol * (1.0 + std::fabs(b)); }
}  // namespace

int main() {
    using namespace altair::live;
    std::printf("basket alpha\n");
    const auto dir = std::filesystem::temp_directory_path() / "altair_basket_alpha_test";
    std::filesystem::create_directories(dir);
    const auto cfg = (dir / "basket.csv").string();
    {
        std::ofstream f(cfg);
        f << "# comment\nkey,value\new_half_s,1800\nbeta_A,0.5\nbeta_B,0.5\nc0_10,0.1\nc0_60,0.2\n";
        for (const char* h : {"10", "60"})
            for (const char* n : kBasketFeatureNames) f << "coef_" << h << "_" << n << ",1\n";
    }
    const auto p = read_basket_alpha(cfg);
    check(p.ok && p.betas.size() == 2 && p.betas[0].first == "A" && near(p.c0[1], 0.2) && near(p.coef[0][7], 1.0),
          "reads betas, intercepts and all 16 coefficients");
    {
        std::ofstream f(dir / "bad.csv");
        f << "beta_A,1\nc0_10,0\nc0_60,0\ncoef_10_rev10,1\n";
    }
    const auto bad = read_basket_alpha((dir / "bad.csv").string());
    check(!bad.ok && !bad.why.empty(), "a file missing coefficients is refused, with the reason");

    // Future up 1 bp a second, stocks up 2 bp a second; every print an uptick of 10 lots.
    BasketAlphaState st(2, {0.5, 0.5});
    const auto fut = [](int s) { return 100.0 * std::exp(1e-4 * s); };
    const auto stk = [](int s) { return 50.0 * std::exp(2e-4 * s); };
    std::array<double, kBasketFeatures> x{};
    for (int s = 0; s <= 400; ++s) {
        if (s >= 200 && s <= 204) continue;   // five silent seconds: prices carried, no volume
        st.on_stock(s, 0, stk(s));
        st.on_stock(s, 1, stk(s));
        st.on_future(s, fut(s), 10.0);
    }
    check(!BasketAlphaState(2, {0.5, 0.5}).features(x), "no features without history");
    st.advance(401);   // closes second 400
    check(st.last_closed_sec() == 400 && st.slots() == 401, "one slot a second, silent seconds included");
    check(st.features(x), "features once 300 s of history exist");
    check(near(x[0], 10.0) && near(x[1], 60.0), "rev10 / rev60: the future's own return in bp");
    check(near(x[3], 10.0) && near(x[4], 30.0), "leadgap: basket return minus future return");
    check(near(x[5], 1.0), "breadth: every stock up over 60 s");
    check(near(x[6], 1.0) && near(x[7], 1.0), "flow: every print an uptick");
    // gap = log F - B falls 1 bp a second; its EWMA lags, so the future looks cheap (misprice > 0)
    check(x[2] > 0.0, "misprice: future cheap against a basket running ahead");
    check(near(basket_forecast(p, 0, x), 0.1 + x[0] + x[1] + x[2] + x[3] + x[4] + x[5] + x[6] + x[7]), "forecast = c0 + coef . x");

    // A down-tick flips the sign; a zero-tick keeps it.
    BasketAlphaState t(1, {1.0});
    for (int s = 0; s <= 400; ++s) {
        t.on_stock(s, 0, 10.0);
        t.on_future(s, s < 380 ? 100.0 + 0.05 * s : 100.0 + 0.05 * 379 - 0.05 * (s - 379), 1.0);
    }
    t.on_future(400, 100.0 + 0.05 * 379 - 0.05 * 21, 1.0);   // zero tick in second 400
    t.advance(401);
    check(t.features(x), "features on the second series");
    // seconds 341..379 up (39 x 1), 380..400 down (21 x 1, plus the zero tick: 1 more down at 400)
    check(near(x[6], (39.0 - 22.0) / 61.0, 1e-9), "tick rule: down-ticks sell, a zero-tick keeps the last sign");

    std::printf(failures == 0 ? "all basket alpha checks passed\n" : "%d basket alpha check(s) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
