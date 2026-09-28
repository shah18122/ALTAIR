// analytics/advanced_pricers.hpp -- M07 and M09-M11 numerical references.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <vector>

namespace altair {

enum class AdvancedAnalyticsError : std::uint8_t {
    BadParameter,
    NonFinite,
    UnstableGrid,
    IntegrationFailed
};

struct EgarchParams {
    double omega = 0.0;
    double alpha = 0.0;
    double gamma = 0.0;
    double beta = 0.0;
};

struct EgarchFit {
    EgarchParams params{};
    double initial_variance = 0.0;
    double negative_log_likelihood = 0.0;
};

namespace detail {
[[nodiscard]] inline double egarch_nll(std::span<const double> returns,
                                       EgarchParams p,
                                       double initial_variance) noexcept {
    if (!(initial_variance > 0.0) || p.alpha < 0.0
        || !(p.beta > -0.98 && p.beta < 0.995))
        return std::numeric_limits<double>::infinity();
    constexpr double eabs = 0.79788456080286535588;
    double log_h = std::log(initial_variance);
    double nll = 0.0;
    for (const double value : returns) {
        if (!std::isfinite(value) || log_h < -40.0 || log_h > 20.0)
            return std::numeric_limits<double>::infinity();
        const double h = std::exp(log_h);
        const double z = value / std::sqrt(h);
        nll += 0.5 * (log_h + z * z); // constant omitted
        log_h = p.omega + p.beta * log_h
              + p.alpha * (std::fabs(z) - eabs) + p.gamma * z;
    }
    return std::isfinite(nll) ? nll
                              : std::numeric_limits<double>::infinity();
}
} // namespace detail

/// Bounded deterministic Gaussian quasi-MLE for EGARCH(1,1).
/// Parameter box: omega [-20,5], alpha [0,2], gamma [-2,2],
/// beta (-0.98,0.995). Coordinate search is deliberately deterministic so
/// identical research folds and checkpoints produce identical estimates.
[[nodiscard]] inline std::expected<EgarchFit, AdvancedAnalyticsError>
fit_egarch(std::span<const double> returns) {
    if (returns.size() < 64)
        return std::unexpected(AdvancedAnalyticsError::BadParameter);
    double mean = 0.0;
    for (const double x : returns) {
        if (!std::isfinite(x))
            return std::unexpected(AdvancedAnalyticsError::NonFinite);
        mean += x;
    }
    mean /= static_cast<double>(returns.size());
    double variance = 0.0;
    for (const double x : returns) variance += (x - mean) * (x - mean);
    variance /= static_cast<double>(returns.size() - 1);
    if (!(variance > 1e-16))
        return std::unexpected(AdvancedAnalyticsError::BadParameter);

    EgarchFit best{};
    best.initial_variance = variance;
    best.negative_log_likelihood = std::numeric_limits<double>::infinity();
    for (const double initial_beta : {0.50, 0.80, 0.94}) {
        EgarchParams p{std::log(variance) * (1.0 - initial_beta),
                       0.10, -0.05, initial_beta};
        double value = detail::egarch_nll(returns, p, variance);
        std::array<double, 4> step{0.5, 0.10, 0.10, 0.04};
        for (int iteration = 0; iteration < 160; ++iteration) {
            bool improved = false;
            for (std::size_t coordinate = 0; coordinate < 4; ++coordinate) {
                for (const double direction : {-1.0, 1.0}) {
                    EgarchParams trial = p;
                    double* parameter[] = {&trial.omega, &trial.alpha,
                                           &trial.gamma, &trial.beta};
                    *parameter[coordinate] += direction * step[coordinate];
                    trial.omega = std::clamp(trial.omega, -20.0, 5.0);
                    trial.alpha = std::clamp(trial.alpha, 0.0, 2.0);
                    trial.gamma = std::clamp(trial.gamma, -2.0, 2.0);
                    trial.beta = std::clamp(trial.beta, -0.979, 0.994);
                    const double candidate = detail::egarch_nll(
                        returns, trial, variance);
                    if (candidate + 1e-12 < value) {
                        p = trial; value = candidate; improved = true;
                    }
                }
            }
            if (!improved) {
                for (double& s : step) s *= 0.5;
                if (*std::max_element(step.begin(), step.end()) < 1e-6) break;
            }
        }
        if (value < best.negative_log_likelihood) {
            best.params = p;
            best.negative_log_likelihood = value;
        }
    }
    if (!std::isfinite(best.negative_log_likelihood))
        return std::unexpected(AdvancedAnalyticsError::IntegrationFailed);
    return best;
}

/// EGARCH(1,1): log(h_t)=omega+beta*log(h_{t-1})
/// +alpha*(|z|-sqrt(2/pi))+gamma*z. Negative gamma is equity leverage.
[[nodiscard]] inline std::expected<std::vector<double>, AdvancedAnalyticsError>
egarch_variance_path(std::span<const double> returns, EgarchParams p,
                     double initial_variance) {
    if (returns.empty() || !(initial_variance > 0.0)
        || !(std::fabs(p.beta) < 1.0) || p.alpha < 0.0
        || !std::isfinite(p.omega) || !std::isfinite(p.alpha)
        || !std::isfinite(p.gamma) || !std::isfinite(p.beta))
        return std::unexpected(AdvancedAnalyticsError::BadParameter);
    constexpr double expected_abs_normal = 0.79788456080286535588;
    std::vector<double> out(returns.size());
    double log_variance = std::log(initial_variance);
    for (std::size_t i = 0; i < returns.size(); ++i) {
        if (!std::isfinite(returns[i]))
            return std::unexpected(AdvancedAnalyticsError::NonFinite);
        const double variance = std::exp(log_variance);
        if (!(variance > 0.0) || !std::isfinite(variance))
            return std::unexpected(AdvancedAnalyticsError::NonFinite);
        const double z = returns[i] / std::sqrt(variance);
        log_variance = p.omega + p.beta * log_variance
                     + p.alpha * (std::fabs(z) - expected_abs_normal)
                     + p.gamma * z;
        out[i] = std::exp(log_variance);
    }
    return out;
}

enum class VanillaRight : std::uint8_t { Call, Put };

[[nodiscard]] inline double normal_cdf(double x) noexcept {
    return 0.5 * std::erfc(-x / std::sqrt(2.0));
}

[[nodiscard]] inline std::expected<double, AdvancedAnalyticsError>
black_scholes_price(VanillaRight right, double spot, double strike,
                    double maturity, double rate, double dividend,
                    double volatility) noexcept {
    if (!(spot > 0.0) || !(strike > 0.0) || !(maturity > 0.0)
        || !(volatility > 0.0) || !std::isfinite(rate)
        || !std::isfinite(dividend))
        return std::unexpected(AdvancedAnalyticsError::BadParameter);
    const double root_t = std::sqrt(maturity);
    const double d1 = (std::log(spot / strike)
                      + (rate - dividend + 0.5 * volatility * volatility)
                        * maturity) / (volatility * root_t);
    const double d2 = d1 - volatility * root_t;
    const double call = spot * std::exp(-dividend * maturity) * normal_cdf(d1)
                      - strike * std::exp(-rate * maturity) * normal_cdf(d2);
    return right == VanillaRight::Call
        ? call
        : call - spot * std::exp(-dividend * maturity)
               + strike * std::exp(-rate * maturity);
}

struct HestonOptionParams {
    double v0 = 0.04;
    double kappa = 1.0;
    double theta = 0.04;
    double vol_of_variance = 0.3;
    double rho = -0.5;
};

namespace advanced_detail {

using Complex = std::complex<double>;
inline constexpr Complex imaginary{0.0, 1.0};

[[nodiscard]] inline Complex heston_cf(Complex u, double log_spot,
                                       double maturity, double rate,
                                       double dividend,
                                       const HestonOptionParams& p) noexcept {
    const double sigma2 = p.vol_of_variance * p.vol_of_variance;
    const Complex a = p.kappa - p.rho * p.vol_of_variance * imaginary * u;
    Complex d = std::sqrt(a * a + sigma2 * (u * u + imaginary * u));
    // Little-Heston-trap branch: Re(d)>=0 keeps exp(-dT) bounded.
    if (d.real() < 0.0) d = -d;
    const Complex g = (a - d) / (a + d);
    const Complex exp_dt = std::exp(-d * maturity);
    const Complex c = imaginary * u
                          * (log_spot + (rate - dividend) * maturity)
                    + (p.kappa * p.theta / sigma2)
                      * ((a - d) * maturity
                         - 2.0 * std::log((1.0 - g * exp_dt) / (1.0 - g)));
    const Complex dcoef = ((a - d) / sigma2)
                        * ((1.0 - exp_dt) / (1.0 - g * exp_dt));
    return std::exp(c + dcoef * p.v0);
}

[[nodiscard]] inline std::expected<double, AdvancedAnalyticsError>
simpson_integral(const auto& function, double upper, std::size_t panels) {
    if (panels < 2 || panels % 2 != 0 || !(upper > 0.0))
        return std::unexpected(AdvancedAnalyticsError::BadParameter);
    const double h = upper / static_cast<double>(panels);
    double sum = function(0.0) + function(upper);
    if (!std::isfinite(sum))
        return std::unexpected(AdvancedAnalyticsError::IntegrationFailed);
    for (std::size_t i = 1; i < panels; ++i) {
        const double value = function(h * static_cast<double>(i));
        if (!std::isfinite(value))
            return std::unexpected(AdvancedAnalyticsError::IntegrationFailed);
        sum += (i % 2 == 0 ? 2.0 : 4.0) * value;
    }
    return sum * h / 3.0;
}

} // namespace advanced_detail

/// Heston call/put via characteristic-function probabilities P1/P2.
[[nodiscard]] inline std::expected<double, AdvancedAnalyticsError>
heston_price(VanillaRight right, double spot, double strike, double maturity,
             double rate, double dividend, HestonOptionParams p,
             double integration_upper = 120.0, std::size_t panels = 4096) {
    if (!(spot > 0.0) || !(strike > 0.0) || !(maturity > 0.0)
        || !(p.v0 > 0.0) || !(p.kappa > 0.0) || !(p.theta > 0.0)
        || !(p.vol_of_variance > 0.0) || !(std::fabs(p.rho) < 1.0)
        || panels > 1'000'000)
        return std::unexpected(AdvancedAnalyticsError::BadParameter);
    using namespace advanced_detail;
    const double log_spot = std::log(spot), log_strike = std::log(strike);
    const Complex phi_minus_i = heston_cf(-imaginary, log_spot, maturity,
                                          rate, dividend, p);
    const auto p1_integrand = [&](double u) noexcept {
        if (u == 0.0) u = 1e-8;
        const Complex z{u, 0.0};
        const Complex numerator = std::exp(-imaginary * z * log_strike)
                                * heston_cf(z - imaginary, log_spot, maturity,
                                            rate, dividend, p);
        return std::real(numerator / (imaginary * z * phi_minus_i));
    };
    const auto p2_integrand = [&](double u) noexcept {
        if (u == 0.0) u = 1e-8;
        const Complex z{u, 0.0};
        return std::real(std::exp(-imaginary * z * log_strike)
                         * heston_cf(z, log_spot, maturity, rate, dividend, p)
                         / (imaginary * z));
    };
    const auto i1 = simpson_integral(p1_integrand, integration_upper, panels);
    const auto i2 = simpson_integral(p2_integrand, integration_upper, panels);
    if (!i1 || !i2) return std::unexpected(AdvancedAnalyticsError::IntegrationFailed);
    const double p1 = 0.5 + *i1 / 3.14159265358979323846;
    const double p2 = 0.5 + *i2 / 3.14159265358979323846;
    const double call = spot * std::exp(-dividend * maturity) * p1
                      - strike * std::exp(-rate * maturity) * p2;
    const double value = right == VanillaRight::Call
        ? call
        : call - spot * std::exp(-dividend * maturity)
               + strike * std::exp(-rate * maturity);
    return std::isfinite(value) && value >= 0.0
        ? std::expected<double, AdvancedAnalyticsError>{value}
        : std::unexpected(AdvancedAnalyticsError::IntegrationFailed);
}

/// Recombining Boyle trinomial tree. Drift-adjusted probabilities are refused
/// if a requested step count makes any probability negative.
[[nodiscard]] inline std::expected<double, AdvancedAnalyticsError>
trinomial_price(VanillaRight right, bool american, double spot, double strike,
                double maturity, double rate, double dividend,
                double volatility, std::size_t steps) {
    if (!(spot > 0.0) || !(strike > 0.0) || !(maturity > 0.0)
        || !(volatility > 0.0) || steps == 0 || steps > 20'000)
        return std::unexpected(AdvancedAnalyticsError::BadParameter);
    const double dt = maturity / static_cast<double>(steps);
    const double dx = volatility * std::sqrt(3.0 * dt);
    const double drift = rate - dividend - 0.5 * volatility * volatility;
    const double pu = 1.0 / 6.0 + drift * std::sqrt(dt)
                                  / (2.0 * volatility * std::sqrt(3.0));
    const double pm = 2.0 / 3.0;
    const double pd = 1.0 - pu - pm;
    if (pu < 0.0 || pm < 0.0 || pd < 0.0)
        return std::unexpected(AdvancedAnalyticsError::UnstableGrid);
    std::vector<double> value(2 * steps + 1);
    const auto payoff = [&](double underlying) noexcept {
        return right == VanillaRight::Call ? std::max(underlying - strike, 0.0)
                                           : std::max(strike - underlying, 0.0);
    };
    for (std::size_t j = 0; j < value.size(); ++j) {
        const auto offset = static_cast<std::ptrdiff_t>(j)
                          - static_cast<std::ptrdiff_t>(steps);
        value[j] = payoff(spot * std::exp(dx * static_cast<double>(offset)));
    }
    const double discount = std::exp(-rate * dt);
    for (std::size_t level = steps; level-- > 0;) {
        const std::size_t width = 2 * level + 1;
        for (std::size_t j = 0; j < width; ++j) {
            value[j] = discount * (pd * value[j] + pm * value[j + 1]
                                   + pu * value[j + 2]);
            if (american) {
                const auto offset = static_cast<std::ptrdiff_t>(j)
                                  - static_cast<std::ptrdiff_t>(level);
                value[j] = std::max(value[j], payoff(
                    spot * std::exp(dx * static_cast<double>(offset))));
            }
        }
    }
    return value[0];
}

/// Explicit log-space Black-Scholes PDE. The caller supplies the price grid;
/// the function increases time steps until all stencil weights are nonnegative.
[[nodiscard]] inline std::expected<double, AdvancedAnalyticsError>
pde_european_price(VanillaRight right, double spot, double strike,
                   double maturity, double rate, double dividend,
                   double volatility, std::size_t space_steps,
                   std::size_t requested_time_steps = 1) {
    if (!(spot > 0.0) || !(strike > 0.0) || !(maturity > 0.0)
        || !(volatility > 0.0) || space_steps < 40 || space_steps > 20'000
        || requested_time_steps == 0)
        return std::unexpected(AdvancedAnalyticsError::BadParameter);
    const double width = std::max(5.0 * volatility * std::sqrt(maturity), 1.0);
    const double x_min = std::log(spot) - width;
    const double x_max = std::log(spot) + width;
    const double dx = (x_max - x_min) / static_cast<double>(space_steps);
    const double drift = rate - dividend - 0.5 * volatility * volatility;
    std::size_t time_steps = requested_time_steps;
    const double stability_rate = volatility * volatility / (dx * dx) + rate;
    time_steps = std::max(time_steps, static_cast<std::size_t>(
        std::ceil(maturity * stability_rate * 1.05)));
    if (time_steps > 5'000'000)
        return std::unexpected(AdvancedAnalyticsError::UnstableGrid);
    const double dt = maturity / static_cast<double>(time_steps);
    const double a = dt * (0.5 * volatility * volatility / (dx * dx)
                         - drift / (2.0 * dx));
    const double b = 1.0 - dt * (volatility * volatility / (dx * dx) + rate);
    const double c = dt * (0.5 * volatility * volatility / (dx * dx)
                         + drift / (2.0 * dx));
    if (a < 0.0 || b < 0.0 || c < 0.0)
        return std::unexpected(AdvancedAnalyticsError::UnstableGrid);
    std::vector<double> current(space_steps + 1), next(space_steps + 1);
    const auto payoff = [&](double underlying) noexcept {
        return right == VanillaRight::Call ? std::max(underlying - strike, 0.0)
                                           : std::max(strike - underlying, 0.0);
    };
    for (std::size_t j = 0; j <= space_steps; ++j)
        current[j] = payoff(std::exp(x_min + dx * static_cast<double>(j)));
    for (std::size_t n = 0; n < time_steps; ++n) {
        const double tau = dt * static_cast<double>(n + 1);
        if (right == VanillaRight::Call) {
            next[0] = 0.0;
            next[space_steps] = std::exp(x_max) * std::exp(-dividend * tau)
                              - strike * std::exp(-rate * tau);
        } else {
            next[0] = strike * std::exp(-rate * tau)
                    - std::exp(x_min) * std::exp(-dividend * tau);
            next[space_steps] = 0.0;
        }
        for (std::size_t j = 1; j < space_steps; ++j)
            next[j] = a * current[j - 1] + b * current[j] + c * current[j + 1];
        current.swap(next);
    }
    const double coordinate = (std::log(spot) - x_min) / dx;
    const std::size_t left = std::min(space_steps - 1,
        static_cast<std::size_t>(coordinate));
    const double fraction = coordinate - static_cast<double>(left);
    return current[left] * (1.0 - fraction) + current[left + 1] * fraction;
}

} // namespace altair
