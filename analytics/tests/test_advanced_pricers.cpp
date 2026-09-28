#include <analytics/advanced_pricers.hpp>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* text) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    if (!ok) ++failures;
}
}

int main() {
    using namespace altair;

    // M07: every term is checked against the recursion written above it.
    {
        const EgarchParams p{-0.12, 0.18, -0.10, 0.92};
        const std::vector<double> r{0.01, -0.02, 0.005};
        const auto path = egarch_variance_path(r, p, 0.0004);
        double log_h = std::log(0.0004);
        const double z = r[0] / std::sqrt(0.0004);
        log_h = p.omega + p.beta * log_h
              + p.alpha * (std::fabs(z) - 0.79788456080286535588)
              + p.gamma * z;
        check(path && std::fabs((*path)[0] - std::exp(log_h)) < 1e-15,
              "EGARCH recursion matches a hand-computed first step");
        const auto positive = egarch_variance_path(
            std::vector<double>{0.02}, p, 0.0004);
        const auto negative = egarch_variance_path(
            std::vector<double>{-0.02}, p, 0.0004);
        check(positive && negative && (*negative)[0] > (*positive)[0],
              "negative EGARCH leverage raises variance more after a loss");

        std::mt19937_64 generator(0xE6A2C4ULL);
        std::normal_distribution<double> normal;
        const EgarchParams truth{-0.35, 0.16, -0.12, 0.96};
        std::vector<double> seeded(3000);
        double seeded_log_h = truth.omega / (1.0 - truth.beta);
        for (double& value : seeded) {
            const double innovation = normal(generator);
            value = std::sqrt(std::exp(seeded_log_h)) * innovation;
            seeded_log_h = truth.omega + truth.beta * seeded_log_h
                + truth.alpha * (std::fabs(innovation) - 0.79788456080286535588)
                + truth.gamma * innovation;
        }
        const auto fit = fit_egarch(seeded);
        check(fit && std::fabs(fit->params.beta - truth.beta) < 0.06
                  && std::fabs(fit->params.gamma - truth.gamma) < 0.08
                  && fit->params.alpha > 0.04,
              "bounded EGARCH QMLE recovers persistence and leverage on a seeded process");
    }

    constexpr double s = 100.0, k = 100.0, t = 1.0, r = 0.05, q = 0.01;
    constexpr double vol = 0.20;
    const auto analytic = black_scholes_price(VanillaRight::Call, s, k, t, r, q, vol);

    // M09: as vol-of-variance tends to zero Heston converges to BS with sqrt(v0).
    {
        HestonOptionParams p; p.v0 = vol * vol; p.theta = p.v0; p.kappa = 4.0;
        p.vol_of_variance = 0.01; p.rho = -0.4;
        const auto heston = heston_price(VanillaRight::Call, s, k, t, r, q, p);
        check(heston && analytic && std::fabs(*heston - *analytic) < 0.08,
              "Heston CF pricer converges to Black-Scholes at low vol-of-variance");
        const auto put = heston_price(VanillaRight::Put, s, k, t, r, q, p);
        const double parity = s * std::exp(-q * t) - k * std::exp(-r * t);
        check(heston && put && std::fabs((*heston - *put) - parity) < 1e-7,
              "Heston call and put obey discounted put-call parity");
    }

    // M10: recombining trinomial convergence and American lower bound.
    {
        const auto tri = trinomial_price(VanillaRight::Call, false,
            s, k, t, r, q, vol, 800);
        check(tri && analytic && std::fabs(*tri - *analytic) < 0.03,
              "trinomial European call converges to Black-Scholes");
        const auto euro_put = trinomial_price(VanillaRight::Put, false,
            s, k, t, r, q, vol, 800);
        const auto american_put = trinomial_price(VanillaRight::Put, true,
            s, k, t, r, q, vol, 800);
        check(euro_put && american_put && *american_put >= *euro_put,
              "American trinomial put is never worth less than European");
    }

    // M11: explicit PDE uses a proved nonnegative stencil and converges.
    {
        const auto pde = pde_european_price(VanillaRight::Call,
            s, k, t, r, q, vol, 600);
        check(pde && analytic && std::fabs(*pde - *analytic) < 0.04,
              "finite-difference PDE converges to the analytic European call");
        const auto pde_put = pde_european_price(VanillaRight::Put,
            s, k, t, r, q, vol, 600);
        const double parity = s * std::exp(-q * t) - k * std::exp(-r * t);
        check(pde && pde_put && std::fabs((*pde - *pde_put) - parity) < 0.05,
              "PDE call and put preserve discounted parity within grid error");
    }

    std::printf("Advanced analytics: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
