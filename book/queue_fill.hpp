// book/queue_fill.hpp -- where a resting limit order stands in an L2 queue,
// and when it fills: the probability queue model (hftbacktest's
// ProbQueueModel; Rigtorp, "Estimating order queue position", 2013).
//
// WHY. book/queue_position.hpp replays a queue exactly, but it needs every
// order (L3). FYERS sends aggregated levels (5 from the data socket, 50 from
// TBT) and trade prints, so a resting order's place can only be estimated.
// The estimate is the classic one:
//   * on arrival the order joins the BACK: everything shown at its price is
//     ahead of it (front = level quantity);
//   * a trade AT its price eats the front first (front -= traded);
//   * the level shrinking by more than what traded there is CANCELS, and a
//     cancel is from behind with probability p = f(back) / (f(back) + f(front)),
//     f(x) = x^n (power) or log(1 + x); so the front moves up by (1 - p) of it;
//   * the level growing adds behind (front never grows).
// A FILL happens only the way a resting order can be filled:
//   * a trade at its price once the front is used up (the part of the trade
//     that reached it);
//   * a trade THROUGH its price (an aggressor swept past it): up to the traded
//     quantity -- never more than printed, which is conservative;
//   * the opposite touch reaching its price (a crossing quote): the whole rest.
// The level vanishing from the book with no trade at it is read as cancels:
// the order is at the head but unfilled until someone trades with it.
//
// n is the knob: hftbacktest's 2023 study used n = 2 and moved to 3 in 2025
// for a harder queue. Default 3 here (pessimistic). Nothing is fitted yet: the
// recorded tapes are the place to fit it (fills of real resting orders).
//
// Pure arithmetic on quantities, no I/O, no allocation. Prices in paise,
// quantities in units.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace altair::queue {

enum class ProbFn : std::uint8_t { Power, Log };

struct ProbQueueConfig {
    ProbFn fn = ProbFn::Power;
    double n = 3.0;   ///< the power (ProbFn::Power)
};

/// The chance a cancel of `front` ahead / `back` behind came from behind.
[[nodiscard]] inline double prob_behind(const ProbQueueConfig& c, double front, double back) noexcept {
    front = std::max(0.0, front);
    back = std::max(0.0, back);
    const auto f = [&](double x) { return c.fn == ProbFn::Log ? std::log1p(x) : std::pow(x, c.n); };
    const double fb = f(back), ff = f(front);
    if (!(fb + ff > 0.0)) return 1.0;   // nothing either side: whatever left was not ahead of us
    return fb / (fb + ff);
}

/// One resting order's place in its level.
struct QueueOrder {
    int side = 0;                 ///< +1 a resting buy (at the bid), -1 a resting sell (at the ask)
    std::int64_t px = 0;          ///< paise
    std::int64_t remaining = 0;   ///< units still to fill
    double front = 0.0;           ///< units estimated ahead
    double level = 0.0;           ///< the level's shown quantity at the last update (ours not included)
    double traded_since = 0.0;    ///< traded at our price since the last depth update
    bool placed = false;
};

/// Join the back of the level at `px`, which shows `level_qty` now.
inline void place(QueueOrder& o, int side, std::int64_t px, std::int64_t qty, std::int64_t level_qty) noexcept {
    o.side = side;
    o.px = px;
    o.remaining = qty;
    o.front = static_cast<double>(std::max<std::int64_t>(0, level_qty));
    o.level = o.front;
    o.traded_since = 0.0;
    o.placed = true;
}

/// True when a trade at `trade_px` is on the side that hits our order: a
/// sell rests at the ask and is lifted by buyers at its price or above.
[[nodiscard]] inline bool reaches(const QueueOrder& o, std::int64_t trade_px) noexcept {
    return o.side < 0 ? trade_px >= o.px : trade_px <= o.px;
}

/// A trade of `qty` at `trade_px`. Returns the units of OUR order it filled.
[[nodiscard]] inline std::int64_t on_trade(QueueOrder& o, std::int64_t trade_px, std::int64_t qty) noexcept {
    if (!o.placed || o.remaining <= 0 || qty <= 0 || !reaches(o, trade_px)) return 0;
    if (trade_px != o.px) {   // through our price: it swept past us
        const std::int64_t got = std::min(o.remaining, qty);
        o.remaining -= got;
        o.front = 0.0;
        return got;
    }
    o.traded_since += static_cast<double>(qty);
    const double left = static_cast<double>(qty) - o.front;   // what reached us after the front
    o.front = std::max(0.0, o.front - static_cast<double>(qty));
    if (left < 1.0) return 0;
    const std::int64_t got = std::min(o.remaining, static_cast<std::int64_t>(std::floor(left)));
    o.remaining -= got;
    return got;
}

/// The level at our price now shows `level_qty` (0 when it is gone).
inline void on_depth(QueueOrder& o, const ProbQueueConfig& c, std::int64_t level_qty) noexcept {
    if (!o.placed) return;
    const double now = static_cast<double>(std::max<std::int64_t>(0, level_qty));
    double chg = o.level - now - o.traded_since;   // the shrink that trades do not explain
    o.traded_since = 0.0;
    if (chg <= 0.0) {                              // it grew (or trades explain it): new orders went behind
        o.front = std::min(o.front, now);
        o.level = now;
        return;
    }
    const double front = std::min(o.front, o.level);
    const double back = std::max(0.0, o.level - front);
    const double p = prob_behind(c, front, back);
    const double est = front - (1.0 - p) * chg + std::min(0.0, back - p * chg);
    o.front = std::clamp(est, 0.0, now);
    o.level = now;
}

/// The opposite touch is `opp_px` (0: none). Returns the units filled when it
/// crossed our price (a buyer bidding our offer or more takes us at once).
[[nodiscard]] inline std::int64_t on_cross(QueueOrder& o, std::int64_t opp_px) noexcept {
    if (!o.placed || o.remaining <= 0 || opp_px <= 0) return 0;
    const bool crossed = o.side < 0 ? opp_px >= o.px : opp_px <= o.px;
    if (!crossed) return 0;
    const std::int64_t got = o.remaining;
    o.remaining = 0;
    o.front = 0.0;
    return got;
}

} // namespace altair::queue
