// strategies/band_option_fade.hpp -- sell the option the range band says
// will not pay, and buy it back before the close.
//
// THE RULE. Each band model (models/band_curriculum.hpp) forecasts, at 09:20,
// an 80 % band for the session's close: anchor x exp(+-half). When a bar's
// high first reaches the upper edge, the move is faded with a short call at
// the first strike at or above the edge; when a bar's low first reaches the
// lower edge, with a short put at the first strike at or below it. One lot,
// sold at the close of the touching bar, bought back at the close of the
// square-off bar (15:20). At most one call and one put per model per day.
//
// THE PRICES ARE SYNTHETIC, AND THAT IS STATED EVERYWHERE THEY APPEAR. The
// dataset holds no option-chain history, so a premium is Black-76
// (analytics/greeks.hpp) on the forward S x exp(rT), with the bar's implied
// volatility -- INDIA VIX, NIFTY's own 30-day implied volatility, scaled by
// the caller for another underlying -- flat across strikes (no skew). A fill
// gives up `slippage_pts` of premium each way: sells receive less, buys pay
// more. What this measures is the band's edge against a fairly priced option,
// not against the book a trader would have met.

#pragma once

#include <analytics/greeks.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <vector>

namespace altair {

/// One bar of the underlying, and the implied volatility to price options at
/// its close. `t` is the bar's START, IST seconds; it closes `bar_seconds` later.
struct FadeBar {
    std::int64_t t = 0;
    double o = 0.0, h = 0.0, l = 0.0, c = 0.0;
    double iv = 0.0;   ///< annualised, e.g. 0.14
};

struct FadeDay {
    std::vector<FadeBar> bars;     ///< one session, in order
    std::size_t decide_bar = 0;    ///< the bar whose close the band was set at (09:15 bar -> 09:20)
    std::size_t exit_bar = 0;      ///< the bar whose close is the square-off (15:15 bar -> 15:20)
    std::int64_t expiry_ts = 0;    ///< the sold option's expiry, IST seconds
    std::int64_t bar_seconds = 300;
};

struct FadePolicy {
    double strike_step = 0.0;      ///< from the instrument master, never a literal
    double lot_size = 0.0;         ///< from the spec store, never a literal
    int lots = 1;
    double rate = 0.065;           ///< continuously compounded, for the forward and discounting
    double slippage_pts = 0.5;     ///< premium points given up on each fill
    double min_premium = 0.05;     ///< a sale that would receive less is not made
};

enum class FadeSide : std::uint8_t { Call, Put };

struct FadeTrade {
    std::size_t model = 0;
    FadeSide side = FadeSide::Call;
    double anchor = 0.0, half = 0.0, band_edge = 0.0, strike = 0.0;
    std::int64_t entry_t = 0, exit_t = 0, expiry_ts = 0;   ///< bar closes, IST seconds
    double entry_spot = 0.0, exit_spot = 0.0;
    double entry_iv = 0.0, exit_iv = 0.0;
    double entry_premium = 0.0, exit_premium = 0.0;        ///< per unit, rupees, after slippage
    double qty = 0.0;                                       ///< lots x lot size
    double gross = 0.0;                                     ///< (entry - exit) x qty, rupees
};

enum class FadeError : std::uint8_t { BadPolicy, BadDay, BadBand };

[[nodiscard]] inline const char* fade_error_text(FadeError e) noexcept {
    switch (e) {
    case FadeError::BadPolicy: return "strike step, lot size, lots and slippage must be positive";
    case FadeError::BadDay: return "the day's bars, decision, square-off or expiry are inconsistent";
    case FadeError::BadBand: return "the band's anchor and half-width must be positive and finite";
    }
    return "?";
}

namespace fade_detail {

inline constexpr double kYearSeconds = 365.0 * 86'400.0;

/// Black-76 premium per unit, in rupees, or NaN when it cannot be priced.
[[nodiscard]] inline double premium(FadeSide side, double spot, double strike, std::int64_t now,
                                    std::int64_t expiry, double iv, double rate) noexcept {
    const double T = static_cast<double>(expiry - now) / kYearSeconds;
    if (!(T > 0.0) || !(iv > 0.0) || !(spot > 0.0) || !(strike > 0.0)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double fwd = spot * std::exp(rate * T);
    const auto g = black76(side == FadeSide::Call ? OptionRight::Call : OptionRight::Put,
                           Price{static_cast<std::int64_t>(std::llround(fwd * 100.0))},
                           Price{static_cast<std::int64_t>(std::llround(strike * 100.0))},
                           Years{T}, Vol{iv}, rate);
    return g ? g->price / 100.0 : std::numeric_limits<double>::quiet_NaN();
}

} // namespace fade_detail

/// The trades the fade rule places on one day for one band.
[[nodiscard]] inline std::expected<std::vector<FadeTrade>, FadeError>
fade_day(const FadeDay& d, double anchor, double half, std::size_t model, const FadePolicy& p) {
    if (!(p.strike_step > 0.0) || !(p.lot_size > 0.0) || p.lots < 1 || !(p.slippage_pts >= 0.0)
        || !std::isfinite(p.rate) || !(p.min_premium > 0.0)) {
        return std::unexpected(FadeError::BadPolicy);
    }
    if (d.bars.empty() || d.exit_bar >= d.bars.size() || d.decide_bar >= d.exit_bar || d.bar_seconds <= 0) {
        return std::unexpected(FadeError::BadDay);
    }
    if (!(anchor > 0.0) || !(half > 0.0) || !std::isfinite(anchor) || !std::isfinite(half)) {
        return std::unexpected(FadeError::BadBand);
    }
    const FadeBar& out = d.bars[d.exit_bar];
    const std::int64_t exit_t = out.t + d.bar_seconds;
    if (d.expiry_ts <= exit_t) { return std::unexpected(FadeError::BadDay); }

    std::vector<FadeTrade> trades;
    const double upper = anchor * std::exp(half), lower = anchor * std::exp(-half);
    bool call_done = false, put_done = false;
    // A touch on the square-off bar itself leaves nothing to fade.
    for (std::size_t k = d.decide_bar + 1; k < d.exit_bar && !(call_done && put_done); ++k) {
        const FadeBar& b = d.bars[k];
        for (const FadeSide side : {FadeSide::Call, FadeSide::Put}) {
            const bool touched = side == FadeSide::Call ? b.h >= upper : b.l <= lower;
            bool& done = side == FadeSide::Call ? call_done : put_done;
            if (done || !touched) { continue; }
            done = true;
            FadeTrade t;
            t.model = model;
            t.side = side;
            t.anchor = anchor;
            t.half = half;
            t.band_edge = side == FadeSide::Call ? upper : lower;
            t.strike = side == FadeSide::Call ? std::ceil(t.band_edge / p.strike_step) * p.strike_step
                                              : std::floor(t.band_edge / p.strike_step) * p.strike_step;
            t.entry_t = b.t + d.bar_seconds;
            t.exit_t = exit_t;
            t.expiry_ts = d.expiry_ts;
            t.entry_spot = b.c;
            t.exit_spot = out.c;
            t.entry_iv = b.iv;
            t.exit_iv = out.iv;
            const double in = fade_detail::premium(side, b.c, t.strike, t.entry_t, d.expiry_ts, b.iv, p.rate);
            const double back = fade_detail::premium(side, out.c, t.strike, exit_t, d.expiry_ts, out.iv, p.rate);
            if (!std::isfinite(in) || !std::isfinite(back)) { continue; }
            t.entry_premium = in - p.slippage_pts;
            if (t.entry_premium < p.min_premium) { continue; }   // too cheap to be worth selling
            t.exit_premium = back + p.slippage_pts;
            t.qty = static_cast<double>(p.lots) * p.lot_size;
            t.gross = (t.entry_premium - t.exit_premium) * t.qty;
            trades.push_back(t);
        }
    }
    return trades;
}

} // namespace altair
