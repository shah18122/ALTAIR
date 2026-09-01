// analytics/hurst.hpp -- Hurst exponent by rescaled range, with its error bar.
//
// P3-06c.
//
// H > 0.5 is persistence (a move tends to be followed by a move the same way),
// H = 0.5 is a random walk, H < 0.5 is mean reversion. The number is easy to
// compute and easy to believe, and there are exactly two ways to get a
// confident wrong answer from it. Both are guarded here.
//
// TRAP 1 -- FEED IT RETURNS, NOT PRICES.
//
// Rescaled range is defined on the INCREMENTS of a process. Hand it a price
// series and it reports H near 1.0 every single time, because a price IS the
// running sum of its returns and a running sum is trivially persistent. The
// answer is not evidence about the market; it is a restatement of what
// integration does. `tests/test_hurst.cpp` runs one dataset both ways and
// measures 0.99 against 0.53.
//
// This one is not hypothetical. It is the standard way a backtest ends up
// claiming a strong trending regime that does not exist, and nothing in the
// output looks wrong -- 0.99 reads as an extremely clean signal.
//
// TRAP 2 -- THE UNCORRECTED ESTIMATOR IS BIASED HIGH ON SHORT SAMPLES.
//
// Raw R/S on independent noise does not return 0.5. It returns something
// closer to 0.6 on a few thousand points, and more on fewer, because E[R/S]
// for a finite random walk is not n^0.5. Anis and Lloyd give the finite-sample
// expectation exactly, and this implementation regresses against it rather
// than against n^0.5. Uncorrected, a pure random series looks persistent; and
// "the market is trending" is the conclusion people want, which is exactly why
// it needs to be the one that is hardest to reach by accident.
//
// THE ERROR BAR IS PART OF THE ANSWER (ROADMAP section 3).
//
// H is a measurement over a finite sample and its uncertainty is large. On
// 4096 points the standard error is a few hundredths; on 512 it is bigger than
// most of the effects anyone claims to find. So `HurstEstimate` carries the
// standard error, and `hurst_departs_from_random_walk` is the only sanctioned
// way to ask the question that matters -- is this DIFFERENT from 0.5, given
// how little data there is. An H of 0.58 with a standard error of 0.06 is a
// random walk, and reporting it as persistence is how a strategy gets built on
// nothing.

#pragma once

#include <core/types/units.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>

namespace altair {

enum class HurstError : std::uint8_t {
    /// Fewer than kHurstMinSamples observations. R/S needs several block
    /// sizes and several blocks at each; below that the regression has too
    /// few points to have a slope, let alone a meaningful one.
    TooShort,
    /// The series is constant, or constant within every block, so every
    /// rescaled range divides by a zero standard deviation.
    Degenerate,
    /// Fewer than three usable scales survived, so a slope and its standard
    /// error cannot both be formed.
    TooFewScales,
    NotFinite
};

/// A measured Hurst exponent.
struct HurstEstimate {
    /// The exponent. 0.5 is a random walk. UNIT: dimensionless.
    double h = 0.0;
    /// Standard error of `h`, from the log-log regression. UNIT: same as h.
    ///
    /// Present because this is the number that decides whether `h` means
    /// anything, and a bare double invites reading 0.58 as persistence.
    double std_error = 0.0;
    /// R-squared of the RAW log(R/S) against log(block size) regression.
    ///
    /// A diagnostic on whether a scaling law holds at all, separate from what
    /// its exponent is. A series with no self-similar structure can still
    /// produce a slope; a low r_squared says that slope describes nothing.
    double r_squared = 0.0;
    /// Block sizes that contributed.
    int scales = 0;
    /// Observations used.
    std::size_t n = 0;
};

/// Below this, `hurst_rs` refuses. Not a soft recommendation: 128 points give
/// four scales at best, and a standard error near 0.1 -- wider than any effect
/// worth trading.
inline constexpr std::size_t kHurstMinSamples = 128;
/// Smallest block. R/S on fewer than 16 points is dominated by its own
/// discreteness.
inline constexpr std::size_t kHurstMinBlock = 16;
/// Ceiling on the number of block sizes, so the regression workspace is a
/// fixed array and nothing allocates.
inline constexpr int kHurstMaxScales = 24;

namespace detail {

/// Anis-Lloyd expected value of R/S for INDEPENDENT data of length n.
///
/// This is the null hypothesis done properly. The textbook R/S estimator
/// implicitly compares against n^0.5, which is only the asymptotic answer;
/// the finite-sample expectation is larger, and regressing against the wrong
/// null is precisely what makes noise look persistent.
[[nodiscard]] inline double anis_lloyd_expected_rs(std::size_t n) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    if (n < 2) { return 0.0; }
    double sum = 0.0;
    for (std::size_t i = 1; i < n; ++i) {
        sum += std::sqrt(static_cast<double>(n - i) / static_cast<double>(i));
    }
    const double dn = static_cast<double>(n);
    // Gamma((n-1)/2) / (sqrt(pi) * Gamma(n/2)), for ALL n.
    //
    // Anis and Lloyd give this form for n <= 340 and an asymptotic
    // sqrt(2/(pi*n)) substitute above it, because in 1976 Gamma(n/2) overflowed
    // for n in the hundreds. `std::lgamma` removes that constraint, and the
    // split is worth removing with it: the asymptotic form is systematically
    // 0.22% BELOW the exact one right across the boundary (21.9931 against
    // 21.9445 at n = 340), so a series spanning block sizes on both sides sees
    // a step in its own null that is not in the data. Small, and in the
    // direction that manufactures persistence.
    //
    // lgamma does not lose the difference for large n: both terms are ~5e5 at
    // n = 1e5 with absolute error ~5e-11, and their difference is O(1).
    const double ratio = std::exp(std::lgamma(0.5 * (dn - 1.0))
                                  - std::lgamma(0.5 * dn))
                       / std::sqrt(kPi);
    return ratio * sum;
}

/// Rescaled range of one block, computed in two passes over the caller's
/// storage. No temporaries, so nothing allocates.
///
/// R is the range of the CUMULATIVE deviation from the block mean, and S is
/// the population standard deviation of the block. Using the sample standard
/// deviation here instead is a common variant; it changes the constant, not
/// the exponent, and would then disagree with the Anis-Lloyd expectation,
/// which is derived for the population form.
[[nodiscard]] inline bool block_rs(const double* x, std::size_t m,
                                   double& out) noexcept {
    double mean = 0.0;
    for (std::size_t i = 0; i < m; ++i) { mean += x[i]; }
    mean /= static_cast<double>(m);

    double cum = 0.0;
    double lo = 0.0;
    double hi = 0.0;
    double ss = 0.0;
    for (std::size_t i = 0; i < m; ++i) {
        const double d = x[i] - mean;
        cum += d;
        if (cum < lo) { lo = cum; }
        if (cum > hi) { hi = cum; }
        ss += d * d;
    }
    const double s = std::sqrt(ss / static_cast<double>(m));
    if (!(s > 0.0) || !std::isfinite(s)) { return false; }
    const double r = hi - lo;
    if (!std::isfinite(r)) { return false; }
    out = r / s;
    return true;
}

} // namespace detail

/// Hurst exponent of `x[0..n)` by rescaled range, Anis-Lloyd corrected.
///
/// `x` MUST be increments -- log returns, order-flow imbalance, a spread --
/// and must NOT be a price level. See the header note; a price series returns
/// ~1.0 and means nothing.
[[nodiscard]] inline std::expected<HurstEstimate, HurstError>
hurst_rs(const double* x, std::size_t n) noexcept {
    if (n < kHurstMinSamples) {
        return std::unexpected(HurstError::TooShort);
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(x[i])) { return std::unexpected(HurstError::NotFinite); }
    }

    // log(block size), log(mean R/S), and log(expected R/S) at each scale.
    double lx[kHurstMaxScales];
    double ly[kHurstMaxScales];
    double le[kHurstMaxScales];
    int k = 0;
    bool any_degenerate = false;

    for (std::size_t m = kHurstMinBlock; m <= n / 2 && k < kHurstMaxScales;
         m *= 2) {
        const std::size_t blocks = n / m;
        double acc = 0.0;
        std::size_t used = 0;
        for (std::size_t b = 0; b < blocks; ++b) {
            double rs = 0.0;
            if (detail::block_rs(x + b * m, m, rs)) {
                acc += rs;
                ++used;
            } else {
                any_degenerate = true;
            }
        }
        // Require a majority of blocks at this scale to be usable, so one
        // flat patch does not silently redefine what the scale measured.
        if (used * 2 <= blocks) { continue; }
        const double mean_rs = acc / static_cast<double>(used);
        if (!(mean_rs > 0.0)) { continue; }
        lx[k] = std::log(static_cast<double>(m));
        ly[k] = std::log(mean_rs);
        le[k] = std::log(detail::anis_lloyd_expected_rs(m));
        ++k;
    }

    if (k < 3) {
        return std::unexpected(any_degenerate ? HurstError::Degenerate
                                              : HurstError::TooFewScales);
    }

    const double dk = static_cast<double>(k);
    double sx = 0.0;
    for (int i = 0; i < k; ++i) { sx += lx[i]; }
    const double mx = sx / dk;
    double sxx = 0.0;
    for (int i = 0; i < k; ++i) { sxx += (lx[i] - mx) * (lx[i] - mx); }
    if (!(sxx > 0.0)) { return std::unexpected(HurstError::TooFewScales); }

    // The exponent comes from the CORRECTED regression: log(R/S observed)
    // minus log(R/S expected under independence), against log(block size).
    // Its slope is the departure from a random walk, so H is slope + 0.5.
    double sy = 0.0;
    for (int i = 0; i < k; ++i) { sy += ly[i] - le[i]; }
    const double my = sy / dk;
    double sxy = 0.0;
    for (int i = 0; i < k; ++i) {
        sxy += (lx[i] - mx) * ((ly[i] - le[i]) - my);
    }
    const double slope = sxy / sxx;

    double resid = 0.0;
    for (int i = 0; i < k; ++i) {
        const double fit = my + slope * (lx[i] - mx);
        const double e = ((ly[i] - le[i]) - my) - slope * (lx[i] - mx);
        (void)fit;
        resid += e * e;
    }

    HurstEstimate out{};
    out.h = slope + 0.5;

    // Standard error of an OLS slope: k-2 degrees of freedom, which is why
    // three scales is the hard minimum -- at two the denominator is zero and
    // the estimate would carry no uncertainty at all, the most dangerous
    // thing it could report.
    const double regression_se = std::sqrt(resid / (dk - 2.0) / sxx);

    // FLOORED AT 1/sqrt(n), and this is not conservatism for its own sake.
    //
    // The regression standard error measures only the SCATTER OF THE POINTS
    // ABOUT THE FITTED LINE. It says nothing about the sampling variability
    // of the line itself -- and R/S values at different block sizes are
    // computed from the same underlying data, so they move together. A few
    // scales can lie on a beautifully straight line that is displaced
    // wholesale by sampling noise, and the regression cheerfully reports a
    // tiny standard error for it.
    //
    // Caught by the tests rather than by reasoning: on 256 points of
    // INDEPENDENT noise the raw regression gave 0.387 +/- 0.047, which is
    // 2.4 sigma from 0.5 and would have been reported as significant mean
    // reversion in data that has none. The known sampling error of an R/S
    // Hurst estimate is ~1/sqrt(n), which at n = 256 is 0.0625 and correctly
    // makes that estimate insignificant.
    const double sampling_floor = 1.0 / std::sqrt(static_cast<double>(n));
    out.std_error = (regression_se > sampling_floor) ? regression_se
                                                     : sampling_floor;

    // R-squared of the RAW relationship, as a separate diagnostic: does a
    // power law describe this series at all, whatever its exponent is.
    double ry = 0.0;
    for (int i = 0; i < k; ++i) { ry += ly[i]; }
    const double rmy = ry / dk;
    double rsxy = 0.0;
    double rsyy = 0.0;
    for (int i = 0; i < k; ++i) {
        rsxy += (lx[i] - mx) * (ly[i] - rmy);
        rsyy += (ly[i] - rmy) * (ly[i] - rmy);
    }
    out.r_squared = (rsyy > 0.0) ? (rsxy * rsxy) / (sxx * rsyy) : 0.0;
    out.scales = k;
    out.n = n;

    if (!std::isfinite(out.h)) { return std::unexpected(HurstError::NotFinite); }
    return out;
}

/// Is this estimate distinguishable from a random walk?
///
/// The ONLY sanctioned way to act on a Hurst number. `sigmas` is how many
/// standard errors away from 0.5 the estimate must sit; 2.0 is the usual bar.
///
/// ROADMAP section 3: a signal whose error bar straddles zero is not a signal.
/// Here the null is 0.5 rather than zero, and the arithmetic is the same.
[[nodiscard]] inline bool
hurst_departs_from_random_walk(const HurstEstimate& e,
                               double sigmas = 2.0) noexcept {
    if (!(e.std_error > 0.0) || !std::isfinite(e.std_error)) { return false; }
    return std::fabs(e.h - 0.5) > sigmas * e.std_error;
}

} // namespace altair
