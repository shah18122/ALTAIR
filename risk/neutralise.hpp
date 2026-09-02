// risk/neutralise.hpp -- sector and beta neutralisation, at the PORTFOLIO
// layer.
//
// P10-06. ROADMAP section 7.5 states the requirement in those words: "beta and
// sector neutralisation enforced at the **portfolio** layer, not per pair."
//
// PER-PAIR HEDGING TRADES THE EXPOSURES THAT WOULD HAVE CANCELLED.
//
// This is the card, and it is a cost argument rather than a correctness one --
// which is why it survives review so easily. Hedging each pair to neutrality
// is not WRONG. Beta is linear, so a book of individually neutral pairs is a
// neutral book, and every number on the risk report reads correctly.
//
// It is simply far more trading than the same neutrality needs. Measured on a
// book of four pairs carrying residual betas of +0.30, -0.25, +0.18 and -0.20,
// Rs 50 crore a leg:
//
//     net beta exposure         Rs 1.50 crore   (book beta +0.0037)
//     hedged AT THE PORTFOLIO   Rs 1.44 crore traded
//     hedged PAIR BY PAIR       Rs 46.26 crore traded
//
// 32.1x, and the difference is bought and sold against itself. P3-09 charges
// for every leg of it.
//
// The same holds sector by sector, and worse: a long-IT leg in one pair and a
// short-IT leg in another are exactly offsetting, and a per-pair sector hedge
// pays the spread on both.
//
// So `neutralise` takes the WHOLE BOOK and returns one hedge per instrument,
// and there is deliberately no per-pair overload -- a function that hedged one
// pair could be called in a loop, which is the thing being avoided.
//
// A BETA IS AN ESTIMATE AND THE HEDGE INHERITS ITS ERROR.
//
// The second thing. Each leg's beta was measured on a window, with a standard
// error P5-04 already insists on carrying. A hedge computed from point
// estimates is neutral only if every estimate is right. `HedgePlan` therefore
// reports the residual beta's own standard error alongside the point estimate,
// so "|beta| < 0.1" can be read as the claim it is -- about a measured
// quantity -- rather than as an exact statement.
//
// THE PHASE EXIT, MEASURED. A book that starts at beta +0.1062 +- 0.0212 is
// hedged with -236 lots of the index future to +0.0001 +- 0.0212. Rounding to
// whole lots goes TOWARD zero, so the hedge never overshoots: over-hedging
// flips the sign of the residual and costs a round trip to discover, while
// under-hedging leaves a smaller version of what was already there.

#pragma once

#include <core/types/units.hpp>
#include <risk/cost.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

inline constexpr std::size_t kMaxSectors = 24;
inline constexpr std::size_t kMaxLegs = 256;

enum class NeutraliseError : std::uint8_t {
    /// The book is empty.
    NoLegs,
    /// A leg had no sector, or a sector index out of range.
    BadSector,
    /// The index future's own beta was not supplied. It is not 1.0 by
    /// assumption -- see `HedgeInstrument`.
    NoHedgeBeta,
    /// The hedge instrument's lot size or price is missing.
    NoHedgeInstrument,
    /// Too many legs.
    Full
};

/// One position in the book.
struct Leg {
    std::uint32_t instrument = 0;
    std::uint16_t sector = 0;
    /// Signed: negative is short.
    Qty qty{0};
    Price price{0};
    /// Beta to the hedge index, MEASURED, with its own standard error.
    double beta = 0.0;
    double beta_std_error = 0.0;

    /// Signed notional in paise.
    [[nodiscard]] std::int64_t notional() const noexcept {
        return qty.raw() * price.raw();
    }
    /// Beta-weighted notional -- the quantity that actually adds up.
    [[nodiscard]] double beta_notional() const noexcept {
        return beta * static_cast<double>(notional());
    }
};

/// What the book is exposed to, before any hedge.
struct BookExposure {
    /// Sum of beta * notional, in paise. The number a hedge must cancel.
    double net_beta_notional = 0.0;
    /// Sum of |notional|, for expressing beta as a ratio.
    double gross_notional = 0.0;
    /// Per-sector signed notional.
    double sector_notional[kMaxSectors] = {};
    /// Standard error of the net beta exposure, from the legs' own beta
    /// errors added in quadrature.
    double net_beta_std_error = 0.0;
    std::size_t legs = 0;

    /// Portfolio beta as a ratio of gross. THE number the phase exit is
    /// stated in: |beta| < 0.1.
    [[nodiscard]] double beta() const noexcept {
        return gross_notional > 0.0 ? net_beta_notional / gross_notional : 0.0;
    }
    /// Its standard error, in the same units.
    [[nodiscard]] double beta_error() const noexcept {
        return gross_notional > 0.0 ? net_beta_std_error / gross_notional : 0.0;
    }
    /// Is the book neutral to `tol`, allowing for the estimate's own error?
    ///
    /// The LOWER bound of |beta| must clear the tolerance, not the point
    /// estimate -- the same discipline as P6-04's `fires()`, applied to a risk
    /// number instead of a signal.
    [[nodiscard]] bool neutral(double tol, double k) const noexcept {
        return std::fabs(beta()) + k * beta_error() < tol;
    }
};

/// Measure the book. Nothing is hedged here; this is the observation.
[[nodiscard]] inline std::expected<BookExposure, NeutraliseError>
measure(const Leg* legs, std::size_t n) noexcept {
    if (n == 0) { return std::unexpected(NeutraliseError::NoLegs); }
    if (n > kMaxLegs) { return std::unexpected(NeutraliseError::Full); }
    BookExposure e{};
    e.legs = n;
    double var = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (legs[i].sector >= kMaxSectors) {
            return std::unexpected(NeutraliseError::BadSector);
        }
        const double notional = static_cast<double>(legs[i].notional());
        e.net_beta_notional += legs[i].beta * notional;
        e.gross_notional += std::fabs(notional);
        e.sector_notional[legs[i].sector] += notional;
        // Errors add in QUADRATURE, not linearly: the legs' beta estimates
        // come from separate regressions and are not the same mistake made
        // twice. Adding them linearly would overstate the uncertainty by
        // sqrt(n).
        const double c = legs[i].beta_std_error * notional;
        var += c * c;
    }
    e.net_beta_std_error = std::sqrt(var);
    return e;
}

/// The instrument the hedge is put on.
struct HedgeInstrument {
    Price price{0};
    /// Lot size, from the spec store. Never a literal (rule 1).
    Qty lot{0};
    /// The hedge instrument's OWN beta to the index the legs were measured
    /// against. Not 1.0 by assumption: a NIFTY future has a beta of 1 to
    /// NIFTY and something else to BANKNIFTY, and hedging a bank-heavy book
    /// with the wrong one leaves a residual nobody is looking for.
    double beta = 0.0;

    [[nodiscard]] bool valid() const noexcept {
        return price.raw() > 0 && lot.raw() > 0 && std::fabs(beta) > 1e-9;
    }
};

struct HedgePlan {
    /// Lots of the hedge instrument. Signed; negative is short.
    std::int64_t lots = 0;
    /// Beta-weighted notional still uncovered after rounding to whole lots.
    double residual_beta_notional = 0.0;
    /// Residual as a ratio of gross, with its error.
    double residual_beta = 0.0;
    double residual_beta_error = 0.0;
    /// Notional actually traded to put the hedge on, in paise.
    std::int64_t hedge_notional = 0;
};

/// Hedge the WHOLE BOOK to beta neutrality with one instrument.
///
/// There is no per-pair overload. A function that hedged one pair could be
/// called in a loop, which is exactly the pattern this card exists to avoid.
[[nodiscard]] inline std::expected<HedgePlan, NeutraliseError>
neutralise(const Leg* legs, std::size_t n,
           const HedgeInstrument& h) noexcept {
    if (!h.valid()) {
        return std::unexpected(h.price.raw() > 0 && h.lot.raw() > 0
                                   ? NeutraliseError::NoHedgeBeta
                                   : NeutraliseError::NoHedgeInstrument);
    }
    const auto e = measure(legs, n);
    if (!e) { return std::unexpected(e.error()); }

    const double per_lot_beta_notional =
        h.beta * static_cast<double>(h.lot.raw() * h.price.raw());
    // Round to whole lots TOWARD ZERO. Over-hedging flips the sign of the
    // residual, which turns a small long exposure into a small short one and
    // costs a round trip to discover; under-hedging leaves the exposure it
    // started with, smaller.
    const double want = -e->net_beta_notional / per_lot_beta_notional;
    HedgePlan p{};
    p.lots = static_cast<std::int64_t>(want);       // truncates toward zero
    p.residual_beta_notional = e->net_beta_notional
                             + static_cast<double>(p.lots)
                                   * per_lot_beta_notional;
    p.residual_beta = e->gross_notional > 0.0
                    ? p.residual_beta_notional / e->gross_notional : 0.0;
    p.residual_beta_error = e->beta_error();
    p.hedge_notional = (p.lots < 0 ? -p.lots : p.lots) * h.lot.raw()
                     * h.price.raw();
    return p;
}

/// What hedging PAIR BY PAIR would have traded, for the same neutrality.
///
/// `pair_of` maps each leg to a pair id. Exists only so the difference can be
/// measured -- there is no code path that actually does this.
[[nodiscard]] inline std::expected<std::int64_t, NeutraliseError>
per_pair_hedge_notional(const Leg* legs, const std::uint32_t* pair_of,
                        std::size_t n, const HedgeInstrument& h) noexcept {
    if (!h.valid()) { return std::unexpected(NeutraliseError::NoHedgeBeta); }
    if (n == 0) { return std::unexpected(NeutraliseError::NoLegs); }
    const double per_lot = h.beta
        * static_cast<double>(h.lot.raw() * h.price.raw());

    std::int64_t total = 0;
    // One pass per distinct pair id. n is small and this runs at rebalance,
    // not on a tick.
    for (std::size_t i = 0; i < n; ++i) {
        bool seen = false;
        for (std::size_t j = 0; j < i; ++j) {
            if (pair_of[j] == pair_of[i]) { seen = true; break; }
        }
        if (seen) { continue; }
        double bn = 0.0;
        for (std::size_t j = 0; j < n; ++j) {
            if (pair_of[j] == pair_of[i]) { bn += legs[j].beta_notional(); }
        }
        const std::int64_t lots =
            static_cast<std::int64_t>(-bn / per_lot);
        total += (lots < 0 ? -lots : lots) * h.lot.raw() * h.price.raw();
    }
    return total;
}

/// Sector exposure as a fraction of gross, largest first in magnitude.
struct SectorBreach {
    std::uint16_t sector = 0;
    double share = 0.0;
};

/// The worst sector concentration in the book.
///
/// Reported rather than hedged: a sector hedge needs a sector instrument, and
/// India has liquid futures on very few of them. Naming the breach and letting
/// the trade handler size around it is honest; silently hedging a sector with
/// an index future is not, because the index is not the sector.
[[nodiscard]] inline SectorBreach worst_sector(const BookExposure& e) noexcept {
    SectorBreach b{};
    if (!(e.gross_notional > 0.0)) { return b; }
    for (std::size_t s = 0; s < kMaxSectors; ++s) {
        const double share = e.sector_notional[s] / e.gross_notional;
        if (std::fabs(share) > std::fabs(b.share)) {
            b.share = share;
            b.sector = static_cast<std::uint16_t>(s);
        }
    }
    return b;
}

} // namespace altair
