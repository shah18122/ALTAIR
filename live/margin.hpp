// live/margin.hpp -- the margin a paper book would block at the exchange,
// estimated on the safe side.
//
// NOT SPAN. The exchange's figure comes from its SPAN risk-parameter files
// (scan ranges, volatility shifts, inter-month and inter-commodity credits)
// plus the exposure margin; this tree does not have those files. What is
// here is a deliberately conservative stand-in, so a limit on it can only
// refuse too early, never too late:
//   * a future: (scan + exposure) x notional, either side;
//   * a short option: max(scan x underlying notional - out-of-the-money
//     amount, minimum x underlying notional) + exposure x underlying
//     notional -- NSE's short-option shape, with no credit for the premium
//     received (SPAN's net option value) and none for hedges: a short
//     strangle is charged both legs, where SPAN charges about one;
//   * a long option: the premium paid;
//   * cash equity: the whole value held overnight, the intraday fraction
//     otherwise (a short is intraday by law).
// Index underlyings get the index rates, everything else the stock rates.
// The rates are inputs (LiveMarginRates); they are typical published levels,
// UNVERIFIED against a contract note or a SPAN file.

#pragma once

#include <live/universe.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace altair::live {

struct LiveMarginRates {
    double index_scan = 0.10;          ///< of notional: SPAN's worst scenario, index futures and options
    double index_exposure = 0.02;      ///< exposure (extreme-loss) margin, index derivatives
    double index_short_min = 0.03;     ///< the short-option minimum, index
    double stock_scan = 0.20;
    double stock_exposure = 0.035;
    double stock_short_min = 0.075;
    double equity_intraday = 0.20;     ///< cash equity held intraday (MIS)
};

/// One leg to price. `price` is the instrument's own (the premium for an
/// option), `underlying` the underlying's level; both rupees.
struct LiveMarginLeg {
    LiveKind kind = LiveKind::Future;
    bool index = true;                 ///< an index underlying (index rates)
    int side = 0;                      ///< +1 long, -1 short
    std::int64_t qty = 0;              ///< units
    double price = 0.0;
    double underlying = 0.0;
    double strike = 0.0;
    bool carry = false;
};

/// The estimate in rupees; NaN when a price it needs is missing (a caller
/// must treat that as unknown, never as zero).
[[nodiscard]] inline double live_margin(const LiveMarginLeg& l, const LiveMarginRates& r = {}) noexcept {
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    const double q = static_cast<double>(l.qty < 0 ? -l.qty : l.qty);
    if (q == 0.0) return 0.0;
    const double scan = l.index ? r.index_scan : r.stock_scan;
    const double expo = l.index ? r.index_exposure : r.stock_exposure;
    switch (l.kind) {
    case LiveKind::Index: return 0.0;   // not tradable
    case LiveKind::Future:
        if (!(l.price > 0.0)) return kNaN;
        return (scan + expo) * l.price * q;
    case LiveKind::Equity:
        if (!(l.price > 0.0)) return kNaN;
        return (l.side > 0 && l.carry ? 1.0 : r.equity_intraday) * l.price * q;
    case LiveKind::Call:
    case LiveKind::Put: {
        if (l.side > 0) return l.price > 0.0 ? l.price * q : kNaN;   // the premium is all a buyer can lose
        if (!(l.underlying > 0.0) || !(l.strike > 0.0)) return kNaN;
        const double u = l.underlying * q;
        const double otm = std::max(0.0, l.kind == LiveKind::Call ? l.strike - l.underlying : l.underlying - l.strike) * q;
        const double floor_rate = l.index ? r.index_short_min : r.stock_short_min;
        return std::max(scan * u - otm, floor_rate * u) + expo * u;
    }
    }
    return kNaN;
}

/// Underlyings traded as indices on NSE and BSE (the index rates apply).
[[nodiscard]] inline bool live_index_underlying(const std::string& u) noexcept {
    return u == "NIFTY" || u == "BANKNIFTY" || u == "FINNIFTY" || u == "MIDCPNIFTY" || u == "NIFTYNXT50" || u == "SENSEX"
        || u == "BANKEX";
}

} // namespace altair::live
