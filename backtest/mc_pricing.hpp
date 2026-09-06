// backtest/mc_pricing.hpp -- Monte Carlo option pricing.
//
// P17-01.
//
// AN MC PRICE WITHOUT ITS STANDARD ERROR IS A NUMBER PRETENDING TO BE EXACT.
//
// This is the card. Every function here returns a `McPrice` carrying the
// estimate AND its standard error, and there is deliberately no accessor that
// hands back the price alone. A closed-form price is a fact about the model; a
// Monte Carlo price is a MEASUREMENT of it, and ROADMAP section 3 says
// measurements carry error and the error propagates. Sizing on an MC price
// whose 95% interval is +/- 40 paise, quoted to the paisa, is the same failure
// as sizing on a point estimate of edge.
//
// CONVERGENCE IS 1/sqrt(n), AND THAT IS SLOW.
//
// Quadrupling the paths halves the error. Getting one more decimal place costs
// a hundred times the work. Anyone reaching for MC because it is "more
// accurate" than a closed form has it backwards -- it is more GENERAL and less
// accurate, and it is the right tool only where no closed form exists.
//
// THE CONTROL VARIATE IS WHY THIS IS USABLE AT ALL.
//
// For a payoff that correlates with something we can price exactly, the exact
// price corrects the simulated one:
//
//     estimate = mc(payoff) - beta * (mc(control) - exact(control))
//
// The bracket is a MEASURED ERROR on a known answer, so subtracting it removes
// that much of the same error from the payoff we care about. On an Asian
// option against a European control this is routinely a 10x variance
// reduction -- a hundredfold saving in paths -- and it costs one extra
// evaluation per path.
//
// ANTITHETIC VARIATES ARE NOT ALWAYS FREE.
//
// Pairing z with -z halves variance for a payoff MONOTONE in z, which a
// vanilla call is. For a non-monotone payoff -- a straddle, a barrier that can
// be knocked out on either side -- the pairing can leave variance UNCHANGED or
// slightly worse, because the two paths no longer offset. So it is a parameter
// rather than a default, and the test measures the reduction instead of
// assuming it.
//
// RISK-NEUTRAL DRIFT, NOT THE REAL-WORLD ONE.
//
// Paths here evolve under the forward measure with zero drift on the forward:
// `analytics/greeks.hpp` uses Black-76, where the forward is already a
// martingale. Simulating with a real-world mu and discounting at r is the
// classic error that produces a price nobody can arbitrage against but
// everybody can lose money on.

#pragma once

#include <analytics/greeks.hpp>
#include <backtest/montecarlo.hpp>
#include <core/types/units.hpp>

#include <cmath>
#include <cstdint>
#include <expected>
#include <vector>

namespace altair {

/// A price and how well it is known. There is no way to get one without the
/// other, on purpose.
struct McPrice {
    /// UNIT: paise, same as the forward and strike it came from.
    double price = 0.0;
    /// Standard error of the mean. UNIT: paise.
    double std_error = 0.0;
    std::size_t paths = 0;

    /// The 95% interval. Two standard errors, stated as such rather than
    /// hidden behind a "confidence" the caller has to guess the level of.
    [[nodiscard]] double lo95() const noexcept { return price - 2.0 * std_error; }
    [[nodiscard]] double hi95() const noexcept { return price + 2.0 * std_error; }

    /// Does `other` sit inside this estimate's interval? The question an MC
    /// price is actually for: not "is it equal" -- it never is -- but "is the
    /// difference inside the noise".
    [[nodiscard]] bool agrees_with(double other) const noexcept {
        return other >= lo95() && other <= hi95();
    }
};

/// What to simulate.
struct McSpec {
    /// Forward price, paise. Black-76: the forward, not the spot.
    double forward = 0.0;
    double strike = 0.0;
    /// Years to expiry.
    double t = 0.0;
    /// Annualised volatility, a plain fraction.
    double vol = 0.0;
    /// Continuously-compounded risk-free rate, a plain fraction. Used ONLY to
    /// discount; the forward carries no drift under this measure.
    double rate = 0.0;
    OptionRight right = OptionRight::Call;

    /// Steps per path. One is exact for a payoff that depends only on the
    /// terminal value; a path-dependent payoff needs many, and using one for
    /// an Asian is a silent modelling error rather than an approximation.
    std::size_t steps = 1;

    [[nodiscard]] bool valid() const noexcept {
        return forward > 0.0 && strike > 0.0 && t > 0.0 && vol > 0.0
            && steps >= 1;
    }
};

namespace detail {

/// Terminal forward after one exact log step. Exact in distribution, so a
/// terminal-only payoff needs no discretisation and carries no step bias.
[[nodiscard]] inline double terminal_forward(double f0, double vol, double t,
                                             double z) noexcept {
    return f0 * std::exp(-0.5 * vol * vol * t + vol * std::sqrt(t) * z);
}

[[nodiscard]] inline double vanilla_payoff(OptionRight r, double f,
                                           double k) noexcept {
    return r == OptionRight::Call ? (f > k ? f - k : 0.0)
                                  : (k > f ? k - f : 0.0);
}

} // namespace detail

/// European option by Monte Carlo.
///
/// Exists to be CHECKED, not used: `black76` prices this exactly and faster.
/// Its job is to prove the simulation machinery is right on the one payoff
/// where the answer is known, so the Asian below can be trusted where it is
/// not.
[[nodiscard]] inline std::expected<McPrice, McError>
mc_european(const McSpec& s, std::size_t paths, std::uint64_t seed,
            bool antithetic = true) {
    if (!s.valid()) { return std::unexpected(McError::BadParameter); }
    if (paths < 2) { return std::unexpected(McError::TooFewSamples); }

    Rng rng(seed);
    const double df = std::exp(-s.rate * s.t);
    double sum = 0.0, sum2 = 0.0;
    std::size_t n = 0;

    const std::size_t draws = antithetic ? (paths + 1) / 2 : paths;
    for (std::size_t i = 0; i < draws; ++i) {
        const double z = rng.normal();
        // The antithetic PAIR is averaged into ONE observation, not counted as
        // two. Treating them as independent would report a standard error
        // sqrt(2) too small -- the variance reduction would be double-counted
        // and then claimed again in the error bar.
        double v = detail::vanilla_payoff(
            s.right, detail::terminal_forward(s.forward, s.vol, s.t, z),
            s.strike);
        if (antithetic) {
            v = 0.5 * (v + detail::vanilla_payoff(
                               s.right,
                               detail::terminal_forward(s.forward, s.vol, s.t,
                                                        -z),
                               s.strike));
        }
        sum += v;
        sum2 += v * v;
        ++n;
    }
    const double dn = static_cast<double>(n);
    const double mean = sum / dn;
    const double var = (sum2 / dn - mean * mean) * dn / (dn - 1.0);
    McPrice out;
    out.price = df * mean;
    out.std_error = df * std::sqrt(var > 0.0 ? var / dn : 0.0);
    out.paths = n;
    return out;
}

/// Arithmetic-average Asian call/put -- a payoff with NO closed form, which is
/// the reason this file exists.
///
/// `control` uses the European price on the same paths as a control variate.
/// The geometric Asian would be a tighter control (it has a closed form and
/// correlates more strongly), and it is not used here because the European one
/// is already in the tree and correctness beats cleverness on a first cut;
/// the header records the better option rather than pretending this is it.
[[nodiscard]] inline std::expected<McPrice, McError>
mc_asian(const McSpec& s, std::size_t paths, std::uint64_t seed,
         bool control = true) {
    if (!s.valid() || s.steps < 2) { return std::unexpected(McError::BadParameter); }
    if (paths < 2) { return std::unexpected(McError::TooFewSamples); }

    const auto exact = black76(s.right, Price{static_cast<std::int64_t>(s.forward)},
                               Price{static_cast<std::int64_t>(s.strike)},
                               Years{s.t}, Vol{s.vol}, s.rate);
    if (control && !exact) { return std::unexpected(McError::BadParameter); }
    const double exact_euro = control ? exact->price : 0.0;

    Rng rng(seed);
    const double df = std::exp(-s.rate * s.t);
    const double dt = s.t / static_cast<double>(s.steps);
    const double drift = -0.5 * s.vol * s.vol * dt;
    const double diff = s.vol * std::sqrt(dt);

    std::vector<double> a(paths, 0.0), e(paths, 0.0);
    for (std::size_t p = 0; p < paths; ++p) {
        double f = s.forward;
        double acc = 0.0;
        for (std::size_t k = 0; k < s.steps; ++k) {
            f *= std::exp(drift + diff * rng.normal());
            acc += f;
        }
        a[p] = detail::vanilla_payoff(s.right,
                                      acc / static_cast<double>(s.steps),
                                      s.strike);
        e[p] = detail::vanilla_payoff(s.right, f, s.strike);
    }

    const double dn = static_cast<double>(paths);
    double ma = 0.0, me = 0.0;
    for (std::size_t p = 0; p < paths; ++p) { ma += a[p]; me += e[p]; }
    ma /= dn;
    me /= dn;

    double beta = 0.0;
    if (control) {
        // beta = Cov(asian, euro) / Var(euro). The REGRESSION coefficient, not
        // 1.0: using 1.0 assumes the two move one-for-one and can make the
        // variance worse when they do not.
        double cov = 0.0, vare = 0.0;
        for (std::size_t p = 0; p < paths; ++p) {
            cov += (a[p] - ma) * (e[p] - me);
            vare += (e[p] - me) * (e[p] - me);
        }
        if (vare > 0.0) { beta = cov / vare; }
    }

    // The controlled observation, per path, so its variance is the variance of
    // what is actually being averaged.
    double sum = 0.0, sum2 = 0.0;
    for (std::size_t p = 0; p < paths; ++p) {
        const double v = a[p] - beta * (e[p] - exact_euro / df);
        sum += v;
        sum2 += v * v;
    }
    const double mean = sum / dn;
    const double var = (sum2 / dn - mean * mean) * dn / (dn - 1.0);

    McPrice out;
    out.price = df * mean;
    out.std_error = df * std::sqrt(var > 0.0 ? var / dn : 0.0);
    out.paths = paths;
    return out;
}

} // namespace altair
