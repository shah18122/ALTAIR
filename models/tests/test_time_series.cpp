#include <models/time_series.hpp>

#include <cmath>
#include <cstdio>
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
    std::uint64_t rng_state = 0x8E6D5A4C3B2A1907ull;
    auto normal = [&rng_state]() {
        auto uniform = [&rng_state]() {
            rng_state ^= rng_state >> 12;
            rng_state ^= rng_state << 25;
            rng_state ^= rng_state >> 27;
            const std::uint64_t bits = rng_state * 2685821657736338717ull;
            return (static_cast<double>(bits >> 11) + 1.0)
                 / (static_cast<double>(1ull << 53) + 2.0);
        };
        const double u1 = uniform(), u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1))
             * std::cos(2.0 * 3.14159265358979323846 * u2);
    };

    // M04: recover a deterministic ARMA(1,1) conditional recursion.
    {
        std::vector<double> x(800, 0.0);
        double previous_noise = 0.0;
        for (std::size_t i = 1; i < x.size(); ++i) {
            const double noise = 0.08 * normal();
            x[i] = 0.2 + 0.55 * x[i - 1] + noise + 0.25 * previous_noise;
            previous_noise = noise;
        }
        const auto model = fit_arma(x, 1, 1);
        if (model) std::printf("ARMA phi=%.6f theta=%.6f var=%.8f\n",
                               model->ar[0], model->ma[0], model->residual_variance);
        else std::printf("ARMA error=%u\n", static_cast<unsigned>(model.error()));
        check(model && std::fabs(model->ar[0] - 0.55) < 0.12
                    && std::fabs(model->ma[0] - 0.25) < 0.16,
              "ARMA(1,1) recovers seeded stable/invertible coefficients");
        check(model && model->stationary && model->invertible
                    && model->forecast(x).has_value(),
              "ARMA exposes diagnostics and a finite one-step forecast");
    }

    // M05: integrated forecast is returned on the original level scale.
    {
        std::vector<double> level(300, 100.0);
        for (std::size_t i = 1; i < level.size(); ++i)
            level[i] = level[i - 1] + 0.5 + 0.1 * std::sin(static_cast<double>(i));
        const auto arima = fit_arima(level, 1, 1, 0);
        const auto next = arima ? arima->forecast_one()
                                : std::expected<double, TimeSeriesError>{
                                      std::unexpected(TimeSeriesError::BadParameter)};
        check(next && *next > level.back() + 0.25 && *next < level.back() + 0.75,
              "ARIMA inverse-differences its forecast back to the price level");
    }

    // M06: seasonal differencing removes a repeating 12-step level pattern.
    {
        constexpr std::size_t season = 12;
        std::vector<double> x(1200, 50.0), w(x.size(), 0.0);
        for (std::size_t i = season; i < x.size(); ++i) {
            w[i] = 0.30 * w[i - 1] + 0.25 * w[i - season] + 0.05 * normal();
            x[i] = x[i - season] + w[i];
        }
        const auto sarima = fit_sarima(x, 1, 0, 0, 1, 1, 0, season);
        const auto next = sarima ? sarima->forecast_one()
                                 : std::expected<double, TimeSeriesError>{
                                       std::unexpected(TimeSeriesError::BadParameter)};
        const double truth = x[x.size() - season]
                           + 0.30 * w.back() + 0.25 * w[w.size() - season];
        if (next) std::printf("SARIMA next=%.6f truth=%.6f\n", *next, truth);
        else std::printf("SARIMA error=%u\n", static_cast<unsigned>(next.error()));
        check(next && std::fabs(*next - truth) < 0.12,
              "SARIMA restores a seasonal-differenced forecast to level space");
        check(!fit_sarima(x, 1, 0, 0, 1, 1, 0, 1),
              "SARIMA refuses a meaningless season of one");
    }

    // M08: exact-discretisation OU estimator and transition variance.
    {
        constexpr double theta = 0.35, mean = 2.5, sigma = 0.4, dt = 0.1;
        const double decay = std::exp(-theta * dt);
        const double innovation = sigma * std::sqrt((1.0 - decay * decay)
                                                   / (2.0 * theta));
        std::vector<double> x(5000, mean);
        for (std::size_t i = 1; i < x.size(); ++i) {
            x[i] = mean + decay * (x[i - 1] - mean) + innovation * normal();
        }
        const auto ou = fit_ou(x, dt);
        if (ou) std::printf("OU theta=%.6f mean=%.6f sigma=%.6f\n",
                            ou->theta, ou->mean, ou->sigma);
        else std::printf("OU error=%u\n", static_cast<unsigned>(ou.error()));
        check(ou && std::fabs(ou->theta - theta) < 0.08
                 && std::fabs(ou->mean - mean) < 0.12
                 && std::fabs(ou->sigma - sigma) < 0.08,
              "OU exact-discretisation fit recovers theta, mean and sigma");
        check(ou && ou->half_life() > 0.0 && ou->theta_se > 0.0
                 && std::isfinite(ou->step(mean, 0.0)),
              "OU reports uncertainty, half-life and exact transition step");
    }

    std::printf("Time-series models: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
