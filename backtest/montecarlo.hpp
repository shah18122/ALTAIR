// backtest/montecarlo.hpp -- path generators, resampling, and metrics.
//
// P6-07. GBM, Heston, Merton jump-diffusion, IID and block bootstrap, and the
// performance statistics computed off them.
//
// AN IID BOOTSTRAP DESTROYS THE THING YOU ARE MEASURING.
//
// This is the card. Resampling returns one at a time is the obvious way to
// build a confidence interval around a backtest, and it silently assumes the
// returns are independent. Market returns are not: volatility clusters, so a
// bad day is followed by another bad day far more often than chance allows,
// and it is precisely those runs that produce the drawdowns capital dies in.
//
// Shuffle them and the clustering is gone. The mean survives, the variance
// survives, the Sharpe survives -- and the drawdown distribution collapses,
// because a drawdown is a property of the ORDER of returns and nothing else.
// An IID bootstrap therefore reports a comfortable tail for a strategy whose
// real tail killed it, and every summary statistic it quotes agrees with the
// truth.
//
// The block bootstrap resamples CONTIGUOUS RUNS, which preserves the
// clustering within a block. The block length is a parameter with no default:
// too short and it degenerates to IID, too long and every sample is the
// original series.
//
// Measured on 2,000 resamples of a clustered Heston source, 2,000 returns
// each, both drawing from the SAME random stream (common random numbers, so
// the comparison is between the estimators and not between two draws):
//
//                     mean sd    mean Sharpe   MDD p50   MDD p95   MDD worst
//   IID bootstrap     0.00311     -0.0212      20.21%    33.57%     44.76%
//   BLOCK bootstrap   0.00312     -0.0215      18.60%    31.82%     49.63%
//
// The three ORDER-FREE statistics agree to three decimals. The drawdown does
// not -- and the shape of the disagreement is worth stating precisely rather
// than rounding into a slogan.
//
// The block bootstrap's WORST path is 10.9% deeper: preserving runs is what
// strings bad days together, and the extreme tail is where that shows. Its
// median and 95th percentile sit LOWER, because 2,000 returns in blocks of 50
// is only 40 independent draws per path against 2,000, which widens the whole
// distribution. The two effects pull the middle and the extreme in opposite
// directions.
//
// What survives unambiguously is the thing that matters: shuffling changes
// only the statistic that depends on ORDER, and that statistic is the one a
// risk limit is set against.
//
// MAX DRAWDOWN IS NOT COMPARABLE ACROSS BACKTESTS OF DIFFERENT LENGTHS.
//
// The second thing. Expected max drawdown grows with the number of
// observations because a longer series has more chances to make a new low.
// Measured on the SAME driftless GBM at 0.3% per-step vol, 2,000 paths each:
//
//     250 steps   median MDD   5.00%
//    1000 steps   median MDD  10.13%
//    4000 steps   median MDD  19.90%
//
// A ratio of 3.98 across a 16x length change, against the 4.00 that sqrt(n)
// predicts. So a five-year backtest showing a deeper drawdown than a one-year
// backtest is telling you it is five years long, and quoting the two side by
// side is a units error dressed as a risk comparison.
//
// THE SQRT(252) IN AN ANNUALISED SHARPE ASSUMES INDEPENDENCE.
//
// The third. Lo (2002): with autocorrelated returns the correct scaling is
//
//     SR(q) = SR * q / sqrt(q + 2 * sum_{k=1..q-1} (q-k) * rho_k)
//
// which reduces to SR*sqrt(q) only when every rho is zero. Positive
// autocorrelation -- which any trend-following or slow-moving strategy has by
// construction -- makes sqrt(q) too generous, and the overstatement is tens of
// percent rather than a rounding term.
//
// GBM CANNOT PRODUCE THE RISK YOU ARE TRYING TO SIZE FOR.
//
// Geometric Brownian motion has constant volatility and no jumps, so it has no
// fat tails and no clustering. It is here as the null -- the thing to compare
// against -- and Heston and the jump-diffusion are here because the difference
// between them and GBM is the entire risk question.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class McError : std::uint8_t {
    /// A parameter was outside its admissible range.
    BadParameter,
    /// Fewer observations than the estimator needs.
    TooFewSamples,
    /// The block length was not specified, or does not divide the problem.
    NoBlockLength,
    /// The series had no variation.
    Degenerate
};

/// xorshift64*, seeded explicitly. Deterministic across platforms, which is
/// what rule 10 needs: a Monte Carlo result that cannot be reproduced from its
/// seed is an anecdote.
class Rng {
public:
    /// xorshift needs a non-zero state. Replace ONLY a zero seed -- an
    /// earlier `seed | 1` collapsed every even seed onto the odd one
    /// above it, so 42 and 43 gave bit-identical streams and half of
    /// any seed sweep duplicated the other half.
    explicit Rng(std::uint64_t seed) noexcept
        : s_(seed != 0 ? seed : 0x9E3779B97F4A7C15ull) {}

    [[nodiscard]] std::uint64_t next_u64() noexcept {
        s_ ^= s_ >> 12;
        s_ ^= s_ << 25;
        s_ ^= s_ >> 27;
        return s_ * 2685821657736338717ull;
    }
    [[nodiscard]] double uniform() noexcept {
        return static_cast<double>(next_u64() >> 11)
             / static_cast<double>(1ull << 53);
    }
    /// Box-Muller with a cached second variate -- two normals per pair of
    /// uniforms rather than one, which halves the generator cost.
    [[nodiscard]] double normal() noexcept {
        if (have_spare_) { have_spare_ = false; return spare_; }
        double u1 = uniform();
        if (u1 < 1e-300) { u1 = 1e-300; }
        const double u2 = uniform();
        const double r = std::sqrt(-2.0 * std::log(u1));
        const double th = 6.283185307179586 * u2;
        spare_ = r * std::sin(th);
        have_spare_ = true;
        return r * std::cos(th);
    }
    [[nodiscard]] std::size_t below(std::size_t n) noexcept {
        return n == 0 ? 0 : static_cast<std::size_t>(next_u64() % n);
    }

private:
    std::uint64_t s_;
    double spare_ = 0.0;
    bool have_spare_ = false;
};

// ---------------------------------------------------------------------------
// Path generators
// ---------------------------------------------------------------------------

struct GbmParams {
    double mu = 0.0;            // drift, per step
    double sigma = 0.0;         // volatility, per step
};

/// Geometric Brownian motion, as LOG RETURNS.
///
/// Returns rather than levels, because every statistic below wants returns and
/// converting back and forth loses precision for no reason. The Ito correction
/// is applied: the log drift is mu - sigma^2/2, so exp of the accumulated path
/// has expectation exp(mu*n). Omitting it is the classic error and makes every
/// simulated strategy look better the higher its volatility.
[[nodiscard]] inline std::expected<void, McError>
gbm_returns(double* out, std::size_t n, const GbmParams& p, Rng& rng) noexcept {
    if (!(p.sigma >= 0.0) || n == 0) {
        return std::unexpected(McError::BadParameter);
    }
    const double drift = p.mu - 0.5 * p.sigma * p.sigma;
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = drift + p.sigma * rng.normal();
    }
    return {};
}

struct HestonParams {
    double mu = 0.0;
    double v0 = 0.0;            // initial variance, per step
    double kappa = 0.0;         // mean-reversion speed of variance
    double theta = 0.0;         // long-run variance
    double xi = 0.0;            // vol of vol
    double rho = 0.0;           // correlation between the two Brownians
};

/// Heston stochastic volatility, full-truncation Euler.
///
/// Full truncation -- taking max(v, 0) inside the diffusion AND in the drift --
/// rather than reflection or absorption. Euler schemes for Heston can drive the
/// variance negative when the Feller condition 2*kappa*theta >= xi^2 fails,
/// which it routinely does on fitted equity parameters; full truncation is the
/// scheme with the least bias among the cheap fixes, and pretending the
/// condition holds is not one of the options.
[[nodiscard]] inline std::expected<void, McError>
heston_returns(double* out, std::size_t n, const HestonParams& p,
               Rng& rng) noexcept {
    if (n == 0 || !(p.v0 >= 0.0) || !(p.kappa >= 0.0) || !(p.theta >= 0.0)
        || !(p.xi >= 0.0) || !(p.rho > -1.0 && p.rho < 1.0)) {
        return std::unexpected(McError::BadParameter);
    }
    double v = p.v0;
    const double rc = std::sqrt(1.0 - p.rho * p.rho);
    for (std::size_t i = 0; i < n; ++i) {
        const double z1 = rng.normal();
        const double z2 = p.rho * z1 + rc * rng.normal();
        const double vp = v > 0.0 ? v : 0.0;
        const double sv = std::sqrt(vp);
        out[i] = p.mu - 0.5 * vp + sv * z1;
        v = v + p.kappa * (p.theta - vp) + p.xi * sv * z2;
    }
    return {};
}

/// The most arrivals one step may draw.
///
/// P33-03. The inversion loop has to be bounded -- this runs per step, per
/// path, and must not allocate or spin -- but the bound has to be far enough
/// out that hitting it is impossible rather than merely unlikely, and the
/// caller has to be refused if it is not.
inline constexpr std::size_t kMaxJumpsPerStep = 64;

struct JumpParams {
    double mu = 0.0;
    double sigma = 0.0;
    /// Jump intensity, PER STEP. Not per year.
    ///
    /// THE UNITS ERROR THIS REFUSES. A Merton model calibrated at, say, fifty
    /// jumps a year entered here without dividing by 252 gives lambda = 50 --
    /// and the first version of jump_returns accepted it, drew from an
    /// inversion loop capped at sixteen arrivals, and silently returned
    /// exactly sixteen jumps on essentially every step. Measured: at
    /// lambda 20, P(N <= 16) is 0.221, so 78 per cent of steps were truncated;
    /// at lambda 50 it is zero to six decimal places.
    ///
    /// Nothing reported anything. The path came back finite, the metrics
    /// computed, and the tail was whatever a hard-capped process produces.
    double lambda = 0.0;
    double jump_mean = 0.0;     // mean log jump size
    double jump_sd = 0.0;       // sd of log jump size
};

/// Merton jump-diffusion.
///
/// The compensator is applied: the diffusion drift is reduced by
/// lambda * (exp(m + s^2/2) - 1) so the total expected return is still mu.
/// Without it, adding jumps silently adds drift, and a "risk" simulation ends
/// up more profitable than the base case -- which is how a jump model gets
/// adopted as an alpha model.
[[nodiscard]] inline std::expected<void, McError>
jump_returns(double* out, std::size_t n, const JumpParams& p,
             Rng& rng) noexcept {
    if (n == 0 || !(p.sigma >= 0.0) || !(p.lambda >= 0.0)
        || !(p.jump_sd >= 0.0)) {
        return std::unexpected(McError::BadParameter);
    }
    // REFUSED, not truncated. Twelve arrivals per step against a cap of 64
    // leaves P(N > cap) below 1e-25, so the bound cannot be reached in any
    // run anybody will make -- and a lambda above it is not a jump process
    // being simulated, it is a units error. Rule 9.
    if (p.lambda > 12.0) {
        return std::unexpected(McError::BadParameter);
    }
    const double comp = p.lambda
        * (std::exp(p.jump_mean + 0.5 * p.jump_sd * p.jump_sd) - 1.0);
    const double drift = p.mu - 0.5 * p.sigma * p.sigma - comp;
    for (std::size_t i = 0; i < n; ++i) {
        double r = drift + p.sigma * rng.normal();
        // Poisson with small lambda: at most a handful of arrivals per step,
        // drawn by inversion. Exact rather than the "at most one jump"
        // approximation, which understates the tail exactly where it matters.
        double u = rng.uniform();
        double acc = std::exp(-p.lambda);
        double term = acc;
        std::size_t k = 0;
        while (u > acc && k < kMaxJumpsPerStep) {
            ++k;
            term *= p.lambda / static_cast<double>(k);
            acc += term;
        }
        for (std::size_t j = 0; j < k; ++j) {
            r += p.jump_mean + p.jump_sd * rng.normal();
        }
        out[i] = r;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Resampling
// ---------------------------------------------------------------------------

/// Resample returns one at a time. DESTROYS the ordering, and with it every
/// clustering effect. Present so the damage can be measured.
[[nodiscard]] inline std::expected<void, McError>
iid_bootstrap(double* out, std::size_t n, const double* src,
              std::size_t src_n, Rng& rng) noexcept {
    if (n == 0 || src_n == 0) {
        return std::unexpected(McError::TooFewSamples);
    }
    for (std::size_t i = 0; i < n; ++i) { out[i] = src[rng.below(src_n)]; }
    return {};
}

/// Resample CONTIGUOUS BLOCKS, preserving clustering within each.
///
/// `block` has no default. Too short and it degenerates to the IID case; too
/// long and every sample is the original series with its start rotated. The
/// right length is roughly the decorrelation time of whatever is being
/// preserved, which the caller knows and this does not.
[[nodiscard]] inline std::expected<void, McError>
block_bootstrap(double* out, std::size_t n, const double* src,
                std::size_t src_n, std::size_t block, Rng& rng) noexcept {
    if (n == 0 || src_n == 0) {
        return std::unexpected(McError::TooFewSamples);
    }
    if (block == 0 || block > src_n) {
        return std::unexpected(McError::NoBlockLength);
    }
    std::size_t i = 0;
    while (i < n) {
        // CIRCULAR blocks, so every observation has equal probability of
        // appearing. A non-circular version under-samples the tail of the
        // series, which is usually where the interesting regime is.
        const std::size_t start = rng.below(src_n);
        for (std::size_t k = 0; k < block && i < n; ++k, ++i) {
            out[i] = src[(start + k) % src_n];
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

struct Metrics {
    double mean = 0.0;
    double sd = 0.0;
    /// Per-period Sharpe. Dimensionless.
    double sharpe = 0.0;
    /// Downside deviation against the target, and Sortino.
    double downside_deviation = 0.0;
    double sortino = 0.0;
    /// Largest peak-to-trough decline in the cumulative LOG series, as a
    /// fraction. Positive.
    double max_drawdown = 0.0;
    /// Lag-1 autocorrelation of the returns. Carried because the annualised
    /// Sharpe is wrong without it.
    double autocorr1 = 0.0;
    std::size_t n = 0;
};

[[nodiscard]] inline std::expected<Metrics, McError>
metrics(const double* r, std::size_t n, double target) noexcept {
    if (n < 3) { return std::unexpected(McError::TooFewSamples); }
    Metrics m{};
    m.n = n;
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) { s += r[i]; }
    m.mean = s / static_cast<double>(n);

    double s2 = 0.0, down = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = r[i] - m.mean;
        s2 += d * d;
        const double e = r[i] - target;
        if (e < 0.0) { down += e * e; }
    }
    m.sd = std::sqrt(s2 / static_cast<double>(n - 1));
    if (!(m.sd > 0.0)) { return std::unexpected(McError::Degenerate); }
    m.sharpe = (m.mean - target) / m.sd;

    // THE DIVISOR IS n, NOT THE NUMBER OF DOWNSIDE OBSERVATIONS. Dividing by
    // the downside count turns a strategy that is rarely-but-badly negative
    // into one with a spectacular Sortino, which is exactly backwards.
    m.downside_deviation = std::sqrt(down / static_cast<double>(n));
    m.sortino = m.downside_deviation > 0.0
                  ? (m.mean - target) / m.downside_deviation
                  : 0.0;

    double peak = 0.0, cum = 0.0, worst = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        cum += r[i];
        if (cum > peak) { peak = cum; }
        const double dd = peak - cum;
        if (dd > worst) { worst = dd; }
    }
    // In LOG space the drawdown is a difference; convert once, at the end.
    m.max_drawdown = 1.0 - std::exp(-worst);

    double c = 0.0;
    for (std::size_t i = 1; i < n; ++i) {
        c += (r[i] - m.mean) * (r[i - 1] - m.mean);
    }
    m.autocorr1 = s2 > 0.0 ? c / s2 : 0.0;
    return m;
}

/// The naive annualisation everyone writes: SR * sqrt(q).
[[nodiscard]] inline double naive_annualised_sharpe(double sharpe,
                                                    std::size_t q) noexcept {
    return sharpe * std::sqrt(static_cast<double>(q));
}

/// Lo (2002): SR * q / sqrt(q + 2 * sum_{k=1..q-1} (q-k) * rho_k).
///
/// Approximated with an AR(1) autocorrelation structure, rho_k = rho^k, which
/// is what a slow-moving strategy actually looks like and is far closer than
/// assuming rho_k = 0 for all k. Reduces exactly to SR*sqrt(q) at rho = 0.
[[nodiscard]] inline std::expected<double, McError>
lo_annualised_sharpe(double sharpe, std::size_t q, double rho) noexcept {
    if (q == 0 || !(rho > -1.0 && rho < 1.0)) {
        return std::unexpected(McError::BadParameter);
    }
    const double qd = static_cast<double>(q);
    double acc = 0.0;
    double rk = rho;
    for (std::size_t k = 1; k < q; ++k) {
        acc += (qd - static_cast<double>(k)) * rk;
        rk *= rho;
        if (std::fabs(rk) < 1e-18) { break; }
    }
    const double denom = qd + 2.0 * acc;
    if (!(denom > 0.0)) { return std::unexpected(McError::Degenerate); }
    return sharpe * qd / std::sqrt(denom);
}

/// A distribution of one statistic over many simulated paths.
struct Distribution {
    double mean = 0.0;
    double p05 = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double worst = 0.0;
    std::size_t n = 0;
};

/// Summarise a sample. Sorts in place, which is why it takes a mutable span:
/// copying to sort would allocate, and this runs thousands of times.
[[nodiscard]] inline std::expected<Distribution, McError>
summarise(double* v, std::size_t n) noexcept {
    if (n < 3) { return std::unexpected(McError::TooFewSamples); }
    for (std::size_t i = 1; i < n; ++i) {          // insertion sort: n is small
        const double x = v[i];
        std::size_t j = i;
        while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; --j; }
        v[j] = x;
    }
    Distribution d{};
    d.n = n;
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) { s += v[i]; }
    d.mean = s / static_cast<double>(n);
    d.p05 = v[(n * 5) / 100];
    d.p50 = v[n / 2];
    d.p95 = v[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1];
    d.worst = v[n - 1];
    return d;
}

} // namespace altair
