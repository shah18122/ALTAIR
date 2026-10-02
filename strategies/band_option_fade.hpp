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
//
// WHY THE TOUCH RULE LOSES, AND WHAT strangle_day DOES INSTEAD. The band's
// 80 % is a statement made at 09:20 about the close. Once the price has
// reached an edge, whether it ends back inside is close to a coin flip -- for
// a random walk exactly one half (the reflection principle) -- so fade_day
// trades after the band's information is spent, and the option it sells is
// near the money: about half a lot of futures against the move, for a few
// hours of a multi-week option's time decay. strangle_day sells both edges AT
// 09:20, when the band's probability still applies.

#pragma once

#include <analytics/greeks.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <vector>

namespace altair {

/// One bar of the underlying, and the implied volatility to price options at
/// its close. `t` is the bar's START, IST seconds; it closes `bar_seconds` later.
struct FadeBar {
    std::int64_t t = 0;
    double o = 0.0, h = 0.0, l = 0.0, c = 0.0;
    double iv = 0.0;   ///< annualised, e.g. 0.14
};

/// How time to expiry is measured. CALENDAR: (expiry - now) / 365 days -- the
/// convention INDIA VIX is quoted in, under which a session from 09:20 to
/// 15:20 is a quarter of a day's decay. TRADING: (what is left of today's
/// session + full sessions to expiry) / 252, so decay and variance arrive only
/// while the market is open, as desks price intraday; a whole session is one
/// 252nd of a year, which matches VIX's 30 calendar days to within 2 %. The
/// two disagree most on expiry day: at 09:20 the trading clock gives the
/// option about 5.6x the variance.
///
/// Both are wrong for an intraday seller. Calendar time credits a quarter of
/// a day's decay for most of the day's risk; trading time credits a whole
/// day's decay, overnight gap risk included, to someone who is flat
/// overnight. A VARIANCE clock splits each day's unit of time between the
/// overnight gap and the session in proportion to where the variance happens:
/// `intraday_share` of it accrues through the session, the rest overnight.
/// With intraday_share = 1 it is the trading clock. Which one real NSE prices
/// follow intraday is a question for intraday option data, not this file.
struct FadeClock {
    bool trading = false;             ///< false: calendar
    std::int64_t session_close = 0;   ///< today's 15:30, IST seconds
    double sessions_after = 0.0;      ///< full days after today, through the expiry day
    double intraday_share = 1.0;      ///< of a day's variance that falls inside the session
};

struct FadeDay {
    std::vector<FadeBar> bars;     ///< one session, in order
    std::size_t decide_bar = 0;    ///< the bar whose close the band was set at (09:15 bar -> 09:20)
    std::size_t exit_bar = 0;      ///< the bar whose close is the square-off (15:15 bar -> 15:20)
    std::int64_t expiry_ts = 0;    ///< the sold option's expiry, IST seconds
    std::int64_t bar_seconds = 300;
    FadeClock clock;
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
    const char* exit_reason = "15:20";                      ///< or "stop"
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
inline constexpr double kSessionSeconds = 22'500.0;   // 09:15 to 15:30
inline constexpr double kSessionsPerYear = 252.0;

/// Years from `now` to `expiry` on `c`'s clock.
[[nodiscard]] inline double years(const FadeClock& c, std::int64_t now, std::int64_t expiry) noexcept {
    if (!c.trading) { return static_cast<double>(expiry - now) / kYearSeconds; }
    if (now >= expiry) { return 0.0; }
    // RULE 11: safe-side clamp -- after today's close nothing of today is left.
    const double today = std::max(0.0, static_cast<double>(c.session_close - now)) / kSessionSeconds;
    return (today * c.intraday_share + c.sessions_after) / kSessionsPerYear;
}

/// Black-76 premium per unit, in rupees, or NaN when it cannot be priced.
[[nodiscard]] inline double premium(FadeSide side, double spot, double strike, std::int64_t now,
                                    std::int64_t expiry, double iv, double rate, const FadeClock& clock = {}) noexcept {
    const double T = years(clock, now, expiry);
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

/// dV/dS per unit of the underlying (Black-76's dV/dF carried to spot), or NaN.
[[nodiscard]] inline double spot_delta(FadeSide side, double spot, double strike, std::int64_t now,
                                       std::int64_t expiry, double iv, double rate, const FadeClock& clock = {}) noexcept {
    const double T = years(clock, now, expiry);
    if (!(T > 0.0) || !(iv > 0.0) || !(spot > 0.0) || !(strike > 0.0)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double fwd = spot * std::exp(rate * T);
    const auto g = black76(side == FadeSide::Call ? OptionRight::Call : OptionRight::Put,
                           Price{static_cast<std::int64_t>(std::llround(fwd * 100.0))},
                           Price{static_cast<std::int64_t>(std::llround(strike * 100.0))},
                           Years{T}, Vol{iv}, rate);
    return g ? g->delta * std::exp(rate * T) : std::numeric_limits<double>::quiet_NaN();
}

[[nodiscard]] inline std::expected<void, FadeError> check(const FadeDay& d, double anchor, double half,
                                                          const FadePolicy& p) {
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
    if (d.expiry_ts <= d.bars[d.exit_bar].t + d.bar_seconds) { return std::unexpected(FadeError::BadDay); }
    return {};
}

} // namespace fade_detail

/// The trades the fade rule places on one day for one band.
[[nodiscard]] inline std::expected<std::vector<FadeTrade>, FadeError>
fade_day(const FadeDay& d, double anchor, double half, std::size_t model, const FadePolicy& p) {
    if (const auto ok = fade_detail::check(d, anchor, half, p); !ok) { return std::unexpected(ok.error()); }
    const FadeBar& out = d.bars[d.exit_bar];
    const std::int64_t exit_t = out.t + d.bar_seconds;

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
            const double in = fade_detail::premium(side, b.c, t.strike, t.entry_t, d.expiry_ts, b.iv, p.rate, d.clock);
            const double back = fade_detail::premium(side, out.c, t.strike, exit_t, d.expiry_ts, out.iv, p.rate, d.clock);
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

// ---------------------------------------------------------------------------
// SELL BOTH EDGES AT THE DECISION.
//
// At the decision bar's close: a call at the first strike at or above the
// upper edge and a put at the first at or below the lower, one lot each. Each
// leg is held to the square-off unless its stop fires: the leg is bought back
// at the first bar close where its premium has reached `stop_multiple` times
// what it was sold for. The stop is judged at bar closes only -- a spike that
// reverses inside a bar does not trigger it, which is slightly kind to the
// seller, and said so.
//
// THE HEDGE IS IN WHOLE FUTURES LOTS. At every bar close the futures position
// is set to the whole number of lots nearest the open legs' delta, so one lot
// of strangle is hedged only once its delta reaches half a lot. Futures are
// priced as the underlying (the basis barely moves inside a session). Every
// change of position is one fill, which the caller prices.

struct StranglePolicy {
    double stop_multiple = 0.0;   ///< buy a leg back once its premium reaches this x its sale; 0 = never
    bool hedge = false;           ///< hold whole futures lots against the open legs' delta
};

struct HedgeFill {
    std::int64_t t = 0;    ///< bar close, IST seconds
    long lots = 0;         ///< signed: positive buys futures
    double price = 0.0;
};

struct StrangleDay {
    FadeTrade call, put;
    double hedge_gross = 0.0;        ///< futures P&L, rupees
    std::vector<HedgeFill> fills;    ///< every futures order, the closing one included
    double gross = 0.0;              ///< both legs and the hedge, rupees
};

/// The strangle sold at the decision, or nullopt when either leg cannot be
/// priced or would receive less than `min_premium`.
[[nodiscard]] inline std::expected<std::optional<StrangleDay>, FadeError>
strangle_day(const FadeDay& d, double anchor, double half, std::size_t model, const FadePolicy& p,
             const StranglePolicy& s) {
    if (const auto ok = fade_detail::check(d, anchor, half, p); !ok) { return std::unexpected(ok.error()); }
    if (!(s.stop_multiple == 0.0 || s.stop_multiple > 1.0)) { return std::unexpected(FadeError::BadPolicy); }
    const FadeBar& in = d.bars[d.decide_bar];
    const FadeBar& out = d.bars[d.exit_bar];
    const std::int64_t t0 = in.t + d.bar_seconds;
    const std::int64_t exit_t = out.t + d.bar_seconds;
    const double upper = anchor * std::exp(half), lower = anchor * std::exp(-half);
    const double qty = static_cast<double>(p.lots) * p.lot_size;

    StrangleDay r;
    for (const FadeSide side : {FadeSide::Call, FadeSide::Put}) {
        FadeTrade& t = side == FadeSide::Call ? r.call : r.put;
        t.model = model;
        t.side = side;
        t.anchor = anchor;
        t.half = half;
        t.band_edge = side == FadeSide::Call ? upper : lower;
        t.strike = side == FadeSide::Call ? std::ceil(upper / p.strike_step) * p.strike_step
                                          : std::floor(lower / p.strike_step) * p.strike_step;
        t.entry_t = t0;
        t.expiry_ts = d.expiry_ts;
        t.entry_spot = in.c;
        t.entry_iv = in.iv;
        t.qty = qty;
        const double prem = fade_detail::premium(side, in.c, t.strike, t0, d.expiry_ts, in.iv, p.rate, d.clock);
        if (!std::isfinite(prem) || prem - p.slippage_pts < p.min_premium) { return std::optional<StrangleDay>{}; }
        t.entry_premium = prem - p.slippage_pts;
    }

    bool open[2] = {true, true};
    long lots = 0;   // futures held
    const auto rehedge = [&](const FadeBar& b, std::int64_t now) {
        if (!s.hedge) { return; }
        double units = 0.0;   // the short legs' delta is -qty x delta; the hedge holds +qty x delta
        for (int k = 0; k < 2; ++k) {
            if (!open[k]) { continue; }
            const FadeTrade& t = k == 0 ? r.call : r.put;
            const double dl = fade_detail::spot_delta(t.side, b.c, t.strike, now, d.expiry_ts, b.iv, p.rate, d.clock);
            if (std::isfinite(dl)) { units += qty * dl; }
        }
        const long want = std::lround(units / p.lot_size);
        if (want != lots) {
            r.fills.push_back({now, want - lots, b.c});
            lots = want;
        }
    };
    const auto close_leg = [&](int k, const FadeBar& b, std::int64_t now, double mid, const char* why) {
        FadeTrade& t = k == 0 ? r.call : r.put;
        t.exit_t = now;
        t.exit_spot = b.c;
        t.exit_iv = b.iv;
        t.exit_premium = mid + p.slippage_pts;
        t.gross = (t.entry_premium - t.exit_premium) * t.qty;
        t.exit_reason = why;
        open[k] = false;
    };

    rehedge(in, t0);
    double last = in.c;
    for (std::size_t k = d.decide_bar + 1; k <= d.exit_bar; ++k) {
        const FadeBar& b = d.bars[k];
        const std::int64_t now = b.t + d.bar_seconds;
        r.hedge_gross += static_cast<double>(lots) * p.lot_size * (b.c - last);
        last = b.c;
        for (int leg = 0; leg < 2; ++leg) {
            if (!open[leg]) { continue; }
            const FadeTrade& t = leg == 0 ? r.call : r.put;
            double mid = fade_detail::premium(t.side, b.c, t.strike, now, d.expiry_ts, b.iv, p.rate, d.clock);
            if (!std::isfinite(mid)) { mid = t.side == FadeSide::Call ? std::max(0.0, b.c - t.strike)
                                                                      : std::max(0.0, t.strike - b.c); }
            if (k == d.exit_bar) { close_leg(leg, b, now, mid, "15:20"); continue; }
            if (s.stop_multiple > 0.0 && mid + p.slippage_pts >= s.stop_multiple * t.entry_premium) {
                close_leg(leg, b, now, mid, "stop");
            }
        }
        if (k < d.exit_bar) { rehedge(b, now); }
    }
    if (lots != 0) {
        r.fills.push_back({exit_t, -lots, out.c});
        lots = 0;
    }
    r.gross = r.call.gross + r.put.gross + r.hedge_gross;
    return std::optional<StrangleDay>{std::move(r)};
}

} // namespace altair
