// book/microstructure.hpp — imbalance and microprice on a BookState.
//
// SINCE 2026-10-29 A THIN ADAPTER. The arithmetic lives once, in book/flow.hpp
// (the live OBI engine's kernels: flow::imbalance, flow::weighted_imbalance,
// flow::microprice); this file only reads a BookState (the 5-level Kite-era
// book) into them, so the Quant page and the feature builder keep their API
// and there is no second copy of the math. The live engine, at 5 or 50
// levels, is flow::FlowEngine.
//
// P2-09a. The stateless third of P2-09: pure functions of a BookState, no
// history, no clock. VPIN and Kyle's lambda need rolling windows (P2-09b);
// queue position needs live order state (P2-09c, really Phase 4).
//
// THE HANDOFF FROM P2-08: every signal here returns empty when the book is not
// tradable. P2-08 stores crossed books rather than rejecting them, because the
// pre-open auction legitimately crosses -- and that decision only pays off if
// the consumers honour it. An imbalance or a microprice computed from a
// crossed book is arithmetic on a state that CANNOT BE TRADED, and it would be
// exactly the "free money" signal Phase 5 exists to disprove.
//
// An empty result is a real answer -- "there is no microprice right now" --
// and not an error to paper over with a default.
//
// Decisions D1..D6 are fixed in prompts/P2-09a_microstructure.md.

#pragma once

#include <book/flow.hpp>
#include <book/l2_book.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace altair {

/// Aggregate size on one side, over the top `levels` POPULATED entries.
struct SideDepth {
    Qty          qty;          // UNIT: units
    std::uint8_t levels_used;  // how many were actually summed (D5)
};

namespace detail {

/// Sum one side, clamped to what is really there.
///
/// D5: depth beyond the populated levels is not imagined. A request for five
/// levels on a book holding two sums two — it does not treat the zeroed slots
/// as zero-size liquidity at price zero, and it does not scale up to pretend.
[[nodiscard]] ALTAIR_HOT inline std::optional<SideDepth>
sum_side(const DepthLevel* lv, std::uint8_t populated,
         std::uint8_t levels) noexcept {
    if (levels == 0 || populated == 0
        || static_cast<std::size_t>(populated) > kDepthLevels) {
        return std::nullopt;
    }
    // RULE 11: this clamp is the safety, not a shortcut past it. Kite
    // always sends five depth slots and zero-pads the unused ones (D6), so
    // summing past `populated` counts zeroed slots as resting liquidity --
    // a thin book rendered as a deep one. Taking the smaller of what the
    // caller asked for and what is REAL is the whole point of the argument.
    const std::uint8_t n = levels < populated ? levels : populated;
    std::int64_t total = 0;
    for (std::uint8_t k = 0; k < n; ++k) {
        total += lv[k].qty.raw();
    }
    return SideDepth{Qty{total}, n};
}

} // namespace detail

[[nodiscard]] ALTAIR_HOT inline std::optional<SideDepth>
bid_depth(const BookState& b, std::uint8_t levels) noexcept {
    if (!is_tradable(b)) {
        return std::nullopt;
    }
    return detail::sum_side(b.bid, b.bid_levels, levels);
}

[[nodiscard]] ALTAIR_HOT inline std::optional<SideDepth>
ask_depth(const BookState& b, std::uint8_t levels) noexcept {
    if (!is_tradable(b)) {
        return std::nullopt;
    }
    return detail::sum_side(b.ask, b.ask_levels, levels);
}

/// Order book imbalance over the top `levels`.
/// UNIT: dimensionless, in [-1, +1]. Positive means more resting size on the
/// bid.
///
/// D3: a `double`, deliberately. OBI is a ratio and there is no integer
/// representation of it that is not a lie about precision. CLAUDE.md rule 3
/// permits doubles in analytics and bans them in the ledger, and this is
/// squarely analytics — **no ledger value is ever derived from an OBI without
/// going back through integer paise.** The numerator and denominator are exact
/// integers, so the only rounding is the final division.
[[nodiscard]] ALTAIR_HOT inline std::optional<double>
obi(const BookState& b, std::uint8_t levels = 1) noexcept {
    const auto bd = bid_depth(b, levels);
    const auto ad = ask_depth(b, levels);
    if (!bd || !ad) {
        return std::nullopt;
    }
    // D6: no size at all is "no information", not "balanced" -- flow::imbalance is empty then.
    return flow::imbalance(bd->qty.raw(), ad->qty.raw());
}

/// As `obi`, with harmonic weights w(k) = 1/(1+k).
///
/// D4: level 0 dominates, which matches how queues behave — the touch is where
/// the next trade happens. Weighting by PRICE DISTANCE instead is defensible
/// and sometimes better, but level spacing varies by instrument, which makes it
/// a feature-registry decision (Phase 5, versioned) rather than a book
/// primitive. This is the simple robust one.
[[nodiscard]] ALTAIR_HOT inline std::optional<double>
weighted_obi(const BookState& b,
             std::uint8_t levels = static_cast<std::uint8_t>(kDepthLevels)) noexcept {
    if (!is_tradable(b) || levels == 0) {
        return std::nullopt;
    }
    const std::uint8_t nb = levels < b.bid_levels ? levels : b.bid_levels;
    const std::uint8_t na = levels < b.ask_levels ? levels : b.ask_levels;
    if (nb == 0 || na == 0) {
        return std::nullopt;
    }

    static const flow::LevelWeights harmonic = flow::LevelWeights::harmonic();
    return flow::weighted_imbalance([&](std::size_t k) { return b.bid[k].qty.raw(); }, nb,
                                    [&](std::size_t k) { return b.ask[k].qty.raw(); }, na, harmonic);
}

/// Size-weighted fair price at the touch. UNIT: paise.
///
///     microprice = (bid_px * ask_qty + ask_px * bid_qty) / (bid_qty + ask_qty)
///
/// D1 — THE WEIGHTING IS CROSSED, and getting it backwards is invisible.
/// Bid *price* is weighted by ask *quantity*. Heavy size resting on the bid
/// means the next trade is more likely to lift the offer, so fair value sits
/// nearer the ask — and it is `bid_qty` that multiplies `ask_px`. Swap them and
/// the microprice leans the wrong way while STILL SITTING BETWEEN BID AND ASK,
/// so it looks entirely plausible and every model built on it learns the
/// inverse of the truth.
///
/// D2 — exact integer arithmetic, no double. This is a fair value a limit
/// price gets rounded from, and a drift of one unit is an order at the wrong
/// tick. Overflow returns empty rather than a wrapped number.
[[nodiscard]] ALTAIR_HOT inline std::optional<Price>
microprice(const BookState& b) noexcept {
    if (!is_tradable(b)) {
        return std::nullopt;
    }
    // D2: exact integers; overflow (and no size, D6) is empty, never a wrapped number.
    const auto r = flow::microprice(b.bid[0].px.raw(), b.bid[0].qty.raw(), b.ask[0].px.raw(), b.ask[0].qty.raw());
    if (!r) {
        return std::nullopt;
    }
    return Price{r->truncated()};
}

} // namespace altair
