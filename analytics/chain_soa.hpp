// analytics/chain_soa.hpp -- a whole option chain priced in one cache-resident
// sweep.
//
// Smit asked for SoA and AoS, padding, and cache-line alignment for L1, L2 and
// L3. Those are answers to one question -- which bytes does the hot loop
// touch, and do they arrive together -- and the honest way to apply them is to
// a loop where the answer matters. This is that loop.
//
// WHY STRUCTURE-OF-ARRAYS HERE.
//
// Pricing a chain is a sweep: for every strike, compute d1, d2 and the same
// eight outputs. The sweep reads ONE input column (strike) and writes each
// output column in order. Laid out as structure-of-arrays, every cache line
// the loop pulls in is 8 consecutive strikes of the SAME field -- all of it
// used, none of it wasted -- and the loop body is the same arithmetic on
// adjacent doubles, which is what a compiler turns into SIMD.
//
// Laid out as array-of-structures (one struct per strike, all fields
// together), the same sweep drags every field of every strike through the
// cache even when it needs one. AoS is not wrong in general: it is right when
// you read ALL of one record at once, which is exactly what the chain GRID
// does when it paints a row. So both exist below, and bench/bench_chain.cpp
// measures them against each other at L1-, L2- and L3-sized working sets
// rather than asserting which wins.
//
// WHY THESE SIZES.
//
// kChainCap is 256 strikes. Each column is 256 x 8 = 2,048 bytes -- exactly
// 32 cache lines -- and there are nine, so one expiry's entire chain is about
// 18 KB and fits in a 32 or 48 KB L1D with room to spare. NIFTY lists well
// under 256 strikes per expiry; the terminal subscribes to ATM +/- 10.
//
// RULE 11: a chain wider than kChainCap is REFUSED with TooManyStrikes, never
// truncated. A truncated chain prices the strikes it kept and silently omits
// the rest, and a max-pain or PCR computed over it would be wrong with no way
// to tell.
//
// ALIGNMENT, PADDING AND FALSE SHARING.
//
// Every column is alignas(64) and a whole number of lines long, so each one
// starts on its own line and no two columns ever share a line. Today one
// thread prices the chain; the day two threads write different columns (calls
// on one core, puts on another) that layout is what stops them invalidating
// each other's lines on every store -- false sharing, the cost that does not
// show up in a single-threaded profile at all.
//
// NO HEAP, NO EXCEPTIONS, NO std::expected PER ELEMENT (rule 4). Inputs are
// validated ONCE at the top and the whole chain is refused if any is bad;
// the loop itself has no branches that can fail.
//
// CORRECTNESS IS NOT THE KERNEL'S OWN CLAIM. The arithmetic below is a copy of
// analytics/greeks.hpp's black76 and it would be easy for the two to drift.
// analytics/tests/test_chain_soa.cpp prices every strike both ways and
// requires agreement, and checks put-call parity on every row.
//
// MODEL, NOT MARKET. Until option quotes arrive these are Black-76 values at
// ONE volatility across the chain. Real options carry a smile; a flat-vol
// chain misprices the wings. Every consumer must say which it is showing.

#pragma once

#include <analytics/greeks.hpp>
#include <core/cache_topology.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

inline constexpr std::size_t kChainCap = 256;

enum class ChainError : std::uint8_t {
    /// More strikes than kChainCap. Refused, not truncated -- rule 11.
    TooManyStrikes,
    /// Non-positive forward, strike, time or volatility, or a non-finite rate.
    BadInput
};

/// Structure of arrays. One cache-line-aligned column per field.
struct ChainSoA {
    std::size_t n = 0;

    alignas(kCacheLineBytes) double strike[kChainCap]{};     ///< paise
    alignas(kCacheLineBytes) double call_px[kChainCap]{};    ///< paise
    alignas(kCacheLineBytes) double put_px[kChainCap]{};     ///< paise
    alignas(kCacheLineBytes) double call_delta[kChainCap]{}; ///< vs forward
    alignas(kCacheLineBytes) double put_delta[kChainCap]{};  ///< vs forward
    alignas(kCacheLineBytes) double gamma[kChainCap]{};      ///< 1/paise
    alignas(kCacheLineBytes) double vega[kChainCap]{};       ///< paise per 1.0 vol
    alignas(kCacheLineBytes) double call_theta[kChainCap]{}; ///< paise per year
    alignas(kCacheLineBytes) double put_theta[kChainCap]{};  ///< paise per year
};

// The layout claims above, as compile-time facts rather than comments.
static_assert(sizeof(double) * kChainCap % kCacheLineBytes == 0,
              "each column must be a whole number of cache lines");
static_assert(alignof(ChainSoA) == kCacheLineBytes,
              "the chain must start on a cache line");
static_assert(sizeof(ChainSoA) <= 20u * 1024u,
              "one expiry must fit in L1D alongside the working set");

/// Array of structures: the same fields, one record per strike.
///
/// Padded to exactly two cache lines per record, so a record never straddles
/// three lines and records never share a line with a neighbour -- the fair
/// way to lay out AoS. An unpadded 72-byte record would make AoS look worse
/// than it is, and the benchmark would then be measuring bad padding rather
/// than the layout.
struct alignas(kCacheLineBytes) ChainRowAoS {
    double strike = 0.0;
    double call_px = 0.0;
    double put_px = 0.0;
    double call_delta = 0.0;
    double put_delta = 0.0;
    double gamma = 0.0;
    double vega = 0.0;
    double call_theta = 0.0;
    double put_theta = 0.0;
    double pad_[7]{};             ///< 9 + 7 = 16 doubles = 128 bytes = 2 lines
};
static_assert(sizeof(ChainRowAoS) == 2 * kCacheLineBytes,
              "an AoS record is exactly two cache lines");

/// Common inputs for a chain sweep, validated once.
struct ChainInputs {
    double forward = 0.0;   ///< paise
    double years = 0.0;
    double vol = 0.0;       ///< annualised, 0.12 = 12%
    double rate = 0.0;      ///< continuously compounded

    [[nodiscard]] bool valid() const noexcept {
        return forward > 0.0 && years > 0.0 && vol > 0.0
               && std::isfinite(forward) && std::isfinite(years)
               && std::isfinite(vol) && std::isfinite(rate);
    }
};

namespace detail {

/// Everything that is the same for every strike, computed once.
struct ChainConst {
    double F, T, s, r, sqrtT, vsqrtT, df, half_s2T, theta_head;
};

[[nodiscard]] inline ChainConst chain_const(const ChainInputs& in) noexcept {
    ChainConst c{};
    c.F = in.forward;
    c.T = in.years;
    c.s = in.vol;
    c.r = in.rate;
    c.sqrtT = std::sqrt(c.T);
    c.vsqrtT = c.s * c.sqrtT;
    c.df = std::exp(-c.r * c.T);
    c.half_s2T = 0.5 * c.s * c.s * c.T;
    // -(F * df * s) / (2 sqrtT), the part of theta that multiplies pdf(d1).
    c.theta_head = -(c.F * c.df * c.s) / (2.0 * c.sqrtT);
    return c;
}

}  // namespace detail

/// Price every strike in `c` (c.n and c.strike[] must be set). SoA sweep.
///
/// ALTAIR_HOT: no allocation, no exceptions, one validation up front.
[[nodiscard]] ALTAIR_HOT inline std::expected<void, ChainError>
price_chain(ChainSoA& c, const ChainInputs& in) noexcept {
    if (c.n > kChainCap) { return std::unexpected(ChainError::TooManyStrikes); }
    if (!in.valid()) { return std::unexpected(ChainError::BadInput); }
    for (std::size_t i = 0; i < c.n; ++i) {
        if (!(c.strike[i] > 0.0)) { return std::unexpected(ChainError::BadInput); }
    }

    const detail::ChainConst k = detail::chain_const(in);
    const std::size_t n = c.n;
    for (std::size_t i = 0; i < n; ++i) {
        const double K = c.strike[i];
        const double d1 = (std::log(k.F / K) + k.half_s2T) / k.vsqrtT;
        const double d2 = d1 - k.vsqrtT;
        const double nd1 = detail::norm_cdf(d1);
        const double nd2 = detail::norm_cdf(d2);
        const double pdf1 = detail::norm_pdf(d1);
        // norm_cdf(-d), NOT 1 - norm_cdf(d). In the wings N(d1) is within an
        // ulp of 1 and the subtraction cancels to noise, so a deep OTM put
        // would be priced from rounding error. erfc stays accurate in the
        // tail -- and it is what black76 does, which the test holds us to.
        const double nmd1 = detail::norm_cdf(-d1);
        const double nmd2 = detail::norm_cdf(-d2);

        const double call = k.df * (k.F * nd1 - K * nd2);
        const double put = k.df * (K * nmd2 - k.F * nmd1);
        c.call_px[i] = call;
        c.put_px[i] = put;
        c.call_delta[i] = k.df * nd1;
        c.put_delta[i] = -k.df * nmd1;
        c.gamma[i] = k.df * pdf1 / (k.F * k.vsqrtT);
        c.vega[i] = k.df * k.F * pdf1 * k.sqrtT;
        c.call_theta[i] = k.theta_head * pdf1 + k.r * call;
        c.put_theta[i] = k.theta_head * pdf1 + k.r * put;
    }
    return {};
}

/// The same sweep over an array of structures. Exists for the comparison.
[[nodiscard]] ALTAIR_HOT inline std::expected<void, ChainError>
price_chain_aos(ChainRowAoS* rows, std::size_t n,
                const ChainInputs& in) noexcept {
    if (n > kChainCap) { return std::unexpected(ChainError::TooManyStrikes); }
    if (!in.valid() || rows == nullptr) {
        return std::unexpected(ChainError::BadInput);
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!(rows[i].strike > 0.0)) { return std::unexpected(ChainError::BadInput); }
    }
    const detail::ChainConst k = detail::chain_const(in);
    for (std::size_t i = 0; i < n; ++i) {
        ChainRowAoS& r = rows[i];
        const double K = r.strike;
        const double d1 = (std::log(k.F / K) + k.half_s2T) / k.vsqrtT;
        const double d2 = d1 - k.vsqrtT;
        const double nd1 = detail::norm_cdf(d1);
        const double nd2 = detail::norm_cdf(d2);
        const double pdf1 = detail::norm_pdf(d1);
        // norm_cdf(-d), NOT 1 - norm_cdf(d). In the wings N(d1) is within an
        // ulp of 1 and the subtraction cancels to noise, so a deep OTM put
        // would be priced from rounding error. erfc stays accurate in the
        // tail -- and it is what black76 does, which the test holds us to.
        const double nmd1 = detail::norm_cdf(-d1);
        const double nmd2 = detail::norm_cdf(-d2);
        const double call = k.df * (k.F * nd1 - K * nd2);
        const double put = k.df * (K * nmd2 - k.F * nmd1);
        r.call_px = call;
        r.put_px = put;
        r.call_delta = k.df * nd1;
        r.put_delta = -k.df * nmd1;
        r.gamma = k.df * pdf1 / (k.F * k.vsqrtT);
        r.vega = k.df * k.F * pdf1 * k.sqrtT;
        r.call_theta = k.theta_head * pdf1 + k.r * call;
        r.put_theta = k.theta_head * pdf1 + k.r * put;
    }
    return {};
}

}  // namespace altair
