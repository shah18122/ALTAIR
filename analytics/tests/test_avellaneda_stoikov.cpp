// Tests for analytics/avellaneda_stoikov.hpp -- the closed-form quotes and the
// sell-only ask.

#include <analytics/avellaneda_stoikov.hpp>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

using namespace altair;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
    else { std::printf("  ok  : %s\n", what); }
}
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

} // namespace

int main() {
    std::printf("Avellaneda-Stoikov quotes\n");
    const double s = 25'000.0, sigma = 8.0, gamma = 1e-4, k = 0.3, tau = 300.0;

    // ---- 1. flat book: symmetric about the mid -----------------------------------
    {
        const auto o = as_quote(s, 0.0, sigma, gamma, k, tau);
        check(o.has_value(), "a valid quote");
        check(near(o->reservation, s, 1e-9), "flat inventory: reservation is the mid");
        check(near(o->ask - s, s - o->bid, 1e-9), "flat inventory: bid and ask symmetric");
        const double want = gamma * sigma * sigma * tau + 2.0 / gamma * std::log(1.0 + gamma / k);
        check(near(o->spread, want, 1e-9), "spread = gamma sigma^2 tau + (2/gamma) ln(1 + gamma/k)");
    }
    // ---- 2. inventory leans the quotes back to flat --------------------------------
    {
        const auto lo = as_quote(s, 2.0, sigma, gamma, k, tau);
        const auto sh = as_quote(s, -2.0, sigma, gamma, k, tau);
        check(lo->reservation < s && sh->reservation > s, "long pulls the reservation down, short pushes it up");
        check(near(lo->reservation, s - 2.0 * gamma * sigma * sigma * tau, 1e-9), "r = s - q gamma sigma^2 tau");
        check(lo->ask - s < s - lo->bid, "long: the ask sits nearer the mid than the bid");
        check(near(lo->spread, sh->spread, 1e-9), "the spread does not depend on inventory");
    }
    // ---- 3. the session running out ----------------------------------------------
    {
        const auto end = as_quote(s, 3.0, sigma, gamma, k, 0.0);
        check(near(end->reservation, s, 1e-9), "tau 0: inventory risk is gone, reservation is the mid");
        check(near(end->spread, 2.0 / gamma * std::log1p(gamma / k), 1e-9), "tau 0: only the liquidity spread is left");
        const auto tiny = as_quote(s, 0.0, sigma, 1e-12, k, 0.0);
        check(near(tiny->spread, 2.0 / k, 1e-6), "gamma to 0: the spread tends to 2/k");
    }
    // ---- 4. sell-only for options ---------------------------------------------------
    {
        check(!as_sell_only_ask(100.0, 1.0, 2.0, 1e-3, 0.5, 60.0).has_value(), "sell-only refuses a long inventory");
        const auto a0 = as_sell_only_ask(100.0, 0.0, 2.0, 1e-3, 0.5, 60.0);
        const auto a3 = as_sell_only_ask(100.0, -3.0, 2.0, 1e-3, 0.5, 60.0);
        check(a0.has_value() && std::isnan(a0->bid), "sell-only never returns a bid");
        check(a0->ask > 100.0, "the ask is above the mid");
        check(a3->ask > a0->ask, "already short: each extra lot asks for more premium");
    }
    // ---- 5. bad inputs ---------------------------------------------------------------
    check(!as_quote(s, 0.0, sigma, 0.0, k, tau).has_value(), "gamma 0 refused");
    check(!as_quote(s, 0.0, sigma, gamma, -1.0, tau).has_value(), "negative k refused");
    check(!as_quote(s, 0.0, sigma, gamma, k, -1.0).has_value(), "negative tau refused");
    check(!as_quote(std::nan(""), 0.0, sigma, gamma, k, tau).has_value(), "NaN mid refused");
    // ---- 6. k recovered from exponential excursions ----------------------------------
    {
        std::mt19937 g(11);
        std::exponential_distribution<double> e(0.4);
        std::vector<double> x;
        for (int i = 0; i < 20000; ++i) { x.push_back(i % 3 == 0 ? -1.0 : e(g)); }
        const double kh = as_fit_k(x.begin(), x.end());
        std::printf("    k fitted %.4f against 0.4\n", kh);
        check(near(kh, 0.4, 0.015), "as_fit_k recovers the decay rate");
        std::vector<double> few(10, 1.0);
        check(std::isnan(as_fit_k(few.begin(), few.end())), "too few excursions: NaN, not a guess");
    }
    std::printf(failures == 0 ? "all passed\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
