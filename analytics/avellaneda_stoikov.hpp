// analytics/avellaneda_stoikov.hpp -- the Avellaneda-Stoikov market maker's
// quotes, and the sell-only form ALTAIR is allowed to use on options.
//
// Avellaneda, M. & Stoikov, S. (2008). High-frequency trading in a limit
// order book. Quantitative Finance 8(3), 217-224.
//
// THE MODEL. A dealer quotes a bid and an ask around a mid that moves as
// Brownian motion with volatility sigma. A quote at distance delta from the
// mid is hit at Poisson rate A exp(-k delta). The dealer holds q units and
// maximises exponential utility of terminal wealth with risk aversion gamma
// over a horizon T. Its two answers, to first order in the inventory:
//
//   reservation price  r = s - q gamma sigma^2 (T - t)
//   total spread       delta_a + delta_b = gamma sigma^2 (T - t) + (2/gamma) ln(1 + gamma/k)
//   quotes             bid = r - spread/2,  ask = r + spread/2
//
// The reservation price is where the dealer is indifferent to holding one more
// unit: long inventory pulls it below the mid, so the ask gets more
// aggressive and the bid less, and the book leans back to flat. The spread
// has an inventory-risk part, shrinking as the session runs out, and a
// liquidity part 2/gamma ln(1 + gamma/k) that depends only on how fast fills
// decay with distance.
//
// UNITS. Prices in index points. sigma in points per sqrt(minute), tau in
// minutes, so sigma^2 tau is points^2. q is in lots and gamma is per point per
// lot. k is per point: 1/k is the mean excursion past a quote.
//
// SELL-ONLY. Smit's standing rule is never to buy an option. On options the
// dealer may only post the ask, and its inventory can only be short (q <= 0).
// `as_sell_only_ask` returns that ask: the least premium worth selling one
// more at, given what is already short. A short book raises the reservation
// price (q < 0 makes -q gamma sigma^2 tau positive), so each extra lot sold
// asks for more premium. It never returns a bid, and refuses a long inventory.
// Closing a short option is a buy; that is a stop-loss or expiry decision for
// the strategy that sold it, not a quote this model makes.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>

namespace altair {

enum class AsError : std::uint8_t {
    /// gamma, k or sigma not positive and finite, or tau negative.
    BadParameter,
    /// sell-only quoting asked with a long inventory.
    LongInventory,
};

struct AsQuote {
    double reservation = 0.0;   ///< r = s - q gamma sigma^2 tau
    double spread = 0.0;        ///< total, ask - bid
    double bid = 0.0;
    double ask = 0.0;
    double inventory_skew = 0.0;   ///< r - s
    double risk_half = 0.0;        ///< gamma sigma^2 tau / 2
    double liquidity_half = 0.0;   ///< (1/gamma) ln(1 + gamma/k)
};

[[nodiscard]] inline bool as_valid(double gamma, double k, double sigma, double tau) noexcept {
    return std::isfinite(gamma) && gamma > 0.0 && std::isfinite(k) && k > 0.0
        && std::isfinite(sigma) && sigma > 0.0 && std::isfinite(tau) && tau >= 0.0;
}

/// Both quotes. `mid` the mid, `q` inventory in lots (+ long), `tau` minutes left.
[[nodiscard]] inline std::expected<AsQuote, AsError>
as_quote(double mid, double q, double sigma, double gamma, double k, double tau) noexcept {
    if (!as_valid(gamma, k, sigma, tau) || !std::isfinite(mid) || !std::isfinite(q)) {
        return std::unexpected(AsError::BadParameter);
    }
    AsQuote o;
    const double var_tau = sigma * sigma * tau;
    o.inventory_skew = -q * gamma * var_tau;
    o.reservation = mid + o.inventory_skew;
    o.risk_half = 0.5 * gamma * var_tau;
    // log1p: gamma/k is often 1e-4 or smaller, where log(1 + x) loses digits.
    o.liquidity_half = std::log1p(gamma / k) / gamma;
    o.spread = 2.0 * (o.risk_half + o.liquidity_half);
    o.bid = o.reservation - 0.5 * o.spread;
    o.ask = o.reservation + 0.5 * o.spread;
    return o;
}

/// The ask only, for an instrument that may only be sold (options). `q_short`
/// is the inventory in lots and must be <= 0. The returned quote's bid is NaN.
[[nodiscard]] inline std::expected<AsQuote, AsError>
as_sell_only_ask(double mid, double q_short, double sigma, double gamma, double k, double tau) noexcept {
    if (q_short > 0.0) { return std::unexpected(AsError::LongInventory); }
    auto o = as_quote(mid, q_short, sigma, gamma, k, tau);
    if (o) { o->bid = std::numeric_limits<double>::quiet_NaN(); }
    return o;
}

/// k from excursions past the previous close, in points: with fills at rate
/// A exp(-k delta), the excursion beyond any level is exponential with rate k,
/// whose maximum-likelihood estimate is 1 / (mean positive excursion). NaN
/// when fewer than `min_n` positive excursions.
template <class It>
[[nodiscard]] double as_fit_k(It first, It last, std::size_t min_n = 50) noexcept {
    double sum = 0.0;
    std::size_t n = 0;
    for (; first != last; ++first) {
        const double x = *first;
        if (std::isfinite(x) && x > 0.0) { sum += x; ++n; }
    }
    if (n < min_n || !(sum > 0.0)) { return std::numeric_limits<double>::quiet_NaN(); }
    return static_cast<double>(n) / sum;
}

} // namespace altair
