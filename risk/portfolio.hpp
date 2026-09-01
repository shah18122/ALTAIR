// risk/portfolio.hpp -- book-level greeks, in money.
//
// P4-03. Aggregates the per-contract greeks from P3-01 and P3-02 across a
// whole position book.
//
// RAW GREEKS DO NOT ADD UP. THIS IS THE WHOLE CARD.
//
// A delta of 0.5 on a NIFTY option and a delta of 0.5 on a RELIANCE option are
// not the same thing and their sum is not a quantity. One is 0.5 x 75 units x
// Rs 24,080; the other is 0.5 x 250 units x Rs 2,900. Adding the 0.5s gives
// 1.0, which is not an exposure, is not in any unit, and predicts nothing.
//
// So everything here converts to MONEY first and adds afterwards:
//
//     cash delta   change in book value per 1% move in the underlying
//     cash gamma   change in cash delta per further 1% move
//     cash vega    change in book value per 1 VOL POINT
//     cash theta   change in book value per CALENDAR DAY
//     cash rho     change in book value per 1% move in rates
//
// Every one is paise. `tests/test_portfolio.cpp` builds a two-underlying book,
// moves both by 1%, reprices it exactly, and checks the cash greeks predicted
// the move -- while the raw sum of deltas does not even have the right sign.
//
// VEGA MUST BE BUCKETED BY EXPIRY, AND SUMMING IT IS A REAL MISTAKE.
//
// A one-vol move in the front month and a one-vol move six months out are not
// the same event and almost never happen together. A calendar spread -- long
// back-month vega, short front-month vega -- can be FLAT in total vega and be
// one of the largest risks on a book. Total vega alone says zero. So the
// aggregate carries per-bucket vega as well, and a caller that only reads the
// total has been given the means not to.
//
// SIGN COMES FROM THE POSITION, NOT FROM THE GREEK. A short call has negative
// delta and POSITIVE theta. Multiplying by signed lots is the only place a
// sign is introduced, so there is one place to get it wrong rather than one
// per greek.
//
// MONEY LEAVES AS INTEGER PAISE (rule 3). The greeks arrive as doubles, which
// rule 3 allows in analytics; the aggregate is an exposure that risk limits
// compare against, so it is rounded to paise at this boundary and not carried
// as a double any further.

#pragma once

#include <analytics/greeks.hpp>
#include <analytics/greeks2.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class PortfolioError : std::uint8_t {
    /// A position carried a non-positive lot size or underlying price.
    BadPosition,
    /// More expiry buckets than the aggregate can hold.
    TooManyBuckets,
    /// A greek was not finite.
    NotFinite,
    /// An exposure overflowed int64 paise.
    Overflow
};

/// One line of the book.
struct PositionLine {
    /// SIGNED lots. Negative is short. The only place a sign enters.
    Lots lots{0};
    /// Units per contract, from the spec store (rule 1).
    LotSize lot_size{0};
    /// Price of the UNDERLYING, in paise. Not the option price -- cash delta
    /// and cash gamma are exposures to the underlying moving.
    Price underlying{0};
    /// Per-unit greeks from `black76` / `black_scholes`.
    Greeks greeks{};
    /// Per-unit higher greeks from `black76_higher`. Optional; a zeroed value
    /// contributes nothing, which is correct for a linear instrument.
    HigherGreeks higher{};
    /// Which underlying this is. Positions sharing it share a spot move.
    std::uint16_t underlying_id = 0;
    /// Which expiry bucket. Vega is reported per bucket as well as in total.
    std::uint8_t expiry_bucket = 0;
    /// Sector, for concentration. 0 means unclassified, which is REPORTED
    /// rather than folded into a real sector.
    std::uint16_t sector_id = 0;
};

inline constexpr int kMaxExpiryBuckets = 8;
inline constexpr int kMaxSectors = 24;

/// Book-level exposure. Every field is paise unless stated.
struct PortfolioGreeks {
    /// Change in book value per 1% move in every underlying. UNIT: paise.
    std::int64_t cash_delta = 0;
    /// Change in cash_delta per a further 1% move. UNIT: paise.
    std::int64_t cash_gamma = 0;
    /// Change in book value per 1 vol point (0.01 of sigma). UNIT: paise.
    std::int64_t cash_vega = 0;
    /// Change in book value per calendar day. Negative for a long book.
    std::int64_t cash_theta = 0;
    /// Change in book value per 1% move in rates. UNIT: paise.
    std::int64_t cash_rho = 0;

    /// Second order, in money. Present because a book that is delta and vega
    /// flat can still be badly exposed through these.
    /// Change in cash_delta per 1 vol point. UNIT: paise.
    std::int64_t cash_vanna = 0;
    /// Change in cash_vega per 1 vol point. UNIT: paise.
    std::int64_t cash_volga = 0;
    /// Change in cash_delta per calendar day. UNIT: paise.
    std::int64_t cash_charm = 0;

    /// Vega per expiry bucket. A total of zero across a calendar spread is
    /// not a flat book, and this is what says so.
    std::int64_t vega_by_expiry[kMaxExpiryBuckets] = {};
    /// Absolute cash delta per sector, for concentration limits.
    std::int64_t abs_delta_by_sector[kMaxSectors] = {};

    /// Gross and net notional. UNIT: paise.
    std::int64_t gross_notional = 0;
    std::int64_t net_notional = 0;

    /// Lines that entered, and lines skipped for an unclassified sector.
    int positions = 0;
    int unclassified = 0;
};

namespace detail {

/// Round a double exposure to paise, away from zero.
///
/// Away from zero, not toward: an exposure rounded toward zero understates
/// risk by up to a paisa per line, and the direction of a rounding rule on a
/// risk number should be the conservative one.
[[nodiscard]] inline std::expected<std::int64_t, PortfolioError>
to_paise(double v) noexcept {
    if (!std::isfinite(v)) { return std::unexpected(PortfolioError::NotFinite); }
    if (v > 9.0e18 || v < -9.0e18) {
        return std::unexpected(PortfolioError::Overflow);
    }
    return static_cast<std::int64_t>(v >= 0.0 ? std::ceil(v - 0.5)
                                              : std::floor(v + 0.5));
}

} // namespace detail

/// One line's contribution, in money, before summation.
///
/// Exposed because a caller staring at a surprising book total wants to know
/// which line produced it, and recomputing that by hand is how the wrong line
/// gets blamed.
struct LineExposure {
    double cash_delta = 0.0;
    double cash_gamma = 0.0;
    double cash_vega = 0.0;
    double cash_theta = 0.0;
    double cash_rho = 0.0;
    double cash_vanna = 0.0;
    double cash_volga = 0.0;
    double cash_charm = 0.0;
    double notional = 0.0;
};

/// Convert one line's per-unit greeks into money.
///
/// The conversions, each stated once:
///
///   qty      = lots * lot_size            SIGNED, so shorts flip everything
///   notional = qty * underlying
///   delta    per 1% move  = delta * qty * S * 0.01
///   gamma    per 1% move  = gamma * qty * S^2 * 0.01^2
///   vega     per vol pt   = vega  * qty * 0.01
///   theta    per day      = theta * qty / 365
///   rho      per 1% rate  = rho   * qty * 0.01
///
/// The 0.01 on delta and the 0.0001 on gamma are what make two underlyings
/// comparable: a 1% move is the same EVENT on both, where a one-rupee move is
/// not. Gamma carries the square because it is a second derivative -- using
/// one factor of 0.01 there is the single most common error in a risk report,
/// and it overstates cash gamma by a hundred times.
[[nodiscard]] inline std::expected<LineExposure, PortfolioError>
line_exposure(const PositionLine& p) noexcept {
    if (p.lot_size.raw() <= 0 || p.underlying.raw() <= 0) {
        return std::unexpected(PortfolioError::BadPosition);
    }
    const double qty = static_cast<double>(p.lots.raw())
                     * static_cast<double>(p.lot_size.raw());
    const double S = static_cast<double>(p.underlying.raw());

    LineExposure e{};
    e.notional = qty * S;
    e.cash_delta = p.greeks.delta * qty * S * 0.01;
    e.cash_gamma = p.greeks.gamma * qty * S * S * 0.01 * 0.01;
    e.cash_vega = p.greeks.vega * qty * 0.01;
    e.cash_theta = theta_per_day(p.greeks.theta) * qty;
    e.cash_rho = p.greeks.rho * qty * 0.01;

    // Second order. vanna is d(delta)/d(sigma), so in money it is a cash
    // delta per vol point: one factor of S*0.01 for the delta side and one
    // 0.01 for the vol side.
    e.cash_vanna = p.higher.vanna * qty * S * 0.01 * 0.01;
    e.cash_volga = p.higher.volga * qty * 0.01 * 0.01;
    e.cash_charm = per_day(p.higher.charm) * qty * S * 0.01;

    if (!std::isfinite(e.cash_delta) || !std::isfinite(e.cash_gamma)
        || !std::isfinite(e.cash_vega) || !std::isfinite(e.cash_theta)
        || !std::isfinite(e.cash_rho) || !std::isfinite(e.notional)) {
        return std::unexpected(PortfolioError::NotFinite);
    }
    return e;
}

/// Aggregate a book.
[[nodiscard]] inline std::expected<PortfolioGreeks, PortfolioError>
aggregate(const PositionLine* lines, std::size_t n) noexcept {
    double d = 0, g = 0, v = 0, t = 0, r = 0, vn = 0, vg = 0, ch = 0;
    double gross = 0, net = 0;
    double vega_bucket[kMaxExpiryBuckets] = {};
    double sector[kMaxSectors] = {};
    PortfolioGreeks out{};

    for (std::size_t i = 0; i < n; ++i) {
        const PositionLine& p = lines[i];
        if (p.expiry_bucket >= kMaxExpiryBuckets
            || p.sector_id >= kMaxSectors) {
            return std::unexpected(PortfolioError::TooManyBuckets);
        }
        const auto e = line_exposure(p);
        if (!e) { return std::unexpected(e.error()); }

        d += e->cash_delta;  g += e->cash_gamma;  v += e->cash_vega;
        t += e->cash_theta;  r += e->cash_rho;
        vn += e->cash_vanna; vg += e->cash_volga; ch += e->cash_charm;
        net += e->notional;
        gross += std::fabs(e->notional);
        vega_bucket[p.expiry_bucket] += e->cash_vega;
        // Concentration is about SIZE, not direction: a long and a short in
        // one sector do not cancel as an operational exposure, whatever they
        // do to the delta.
        sector[p.sector_id] += std::fabs(e->cash_delta);
        ++out.positions;
        if (p.sector_id == 0) { ++out.unclassified; }
    }

    struct Field { double src; std::int64_t* dst; };
    const Field fields[] = {
        {d, &out.cash_delta},   {g, &out.cash_gamma},  {v, &out.cash_vega},
        {t, &out.cash_theta},   {r, &out.cash_rho},    {vn, &out.cash_vanna},
        {vg, &out.cash_volga},  {ch, &out.cash_charm},
        {gross, &out.gross_notional}, {net, &out.net_notional},
    };
    for (const Field& f : fields) {
        const auto p = detail::to_paise(f.src);
        if (!p) { return std::unexpected(p.error()); }
        *f.dst = *p;
    }
    for (int i = 0; i < kMaxExpiryBuckets; ++i) {
        const auto p = detail::to_paise(vega_bucket[i]);
        if (!p) { return std::unexpected(p.error()); }
        out.vega_by_expiry[i] = *p;
    }
    for (int i = 0; i < kMaxSectors; ++i) {
        const auto p = detail::to_paise(sector[i]);
        if (!p) { return std::unexpected(p.error()); }
        out.abs_delta_by_sector[i] = *p;
    }
    return out;
}

/// Predicted change in book value for a move, to second order.
///
///     dV = cash_delta*pct + 0.5*cash_gamma*pct^2 + cash_vega*vol_pts
///          + cash_theta*days
///
/// `pct` is the underlying move in PERCENT (1.0 means 1%), `vol_pts` in vol
/// points, `days` in calendar days. The half on gamma is the Taylor
/// coefficient and dropping it is the second most common error in a risk
/// report -- it doubles the convexity term.
[[nodiscard]] constexpr double
predicted_pnl(const PortfolioGreeks& g, double pct, double vol_pts,
              double days) noexcept {
    return static_cast<double>(g.cash_delta) * pct
         + 0.5 * static_cast<double>(g.cash_gamma) * pct * pct
         + static_cast<double>(g.cash_vega) * vol_pts
         + static_cast<double>(g.cash_theta) * days;
}

/// Margin utilisation as a fraction of available capital.
///
/// The margin NUMBER comes from the broker -- SPAN plus exposure is an
/// exchange calculation that changes without notice, and computing it locally
/// would produce a number that disagrees with the one that actually blocks an
/// order. This only ratios what the broker reported.
[[nodiscard]] inline std::expected<double, PortfolioError>
margin_utilisation(Notional margin_used, Notional margin_available) noexcept {
    if (margin_available.raw() <= 0) {
        return std::unexpected(PortfolioError::BadPosition);
    }
    return static_cast<double>(margin_used.raw())
         / static_cast<double>(margin_available.raw());
}

/// Largest absolute sector exposure, and which sector it was.
///
/// Sector 0 is "unclassified" and is EXCLUDED from the maximum: it is not a
/// concentration, it is a gap in the reference data, and letting it win the
/// comparison would report a data problem as a risk problem.
struct SectorPeak {
    std::int64_t exposure = 0;
    int sector = 0;
};

[[nodiscard]] inline SectorPeak
largest_sector(const PortfolioGreeks& g) noexcept {
    SectorPeak peak{};
    for (int i = 1; i < kMaxSectors; ++i) {
        if (g.abs_delta_by_sector[i] > peak.exposure) {
            peak.exposure = g.abs_delta_by_sector[i];
            peak.sector = i;
        }
    }
    return peak;
}

} // namespace altair
