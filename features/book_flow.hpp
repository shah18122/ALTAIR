// features/book_flow.hpp -- the book and flow families.
//
// P5-03. Builders over analytics the book already computes: order-book
// imbalance, microprice, spread, depth, VPIN and Kyle's lambda.
//
// EVERY FEATURE HERE IS MICRO OR FAST BAND, AND THAT IS ENFORCED UPSTREAM.
//
// CLAUDE.md's Nyquist argument is the reason: order-book imbalance decays in
// 10-200 ms. It carries real information about the next few hundred
// milliseconds and none at all about tomorrow. P5-01 refuses to serve these to
// a swing model, and this file simply does not offer a slow variant -- there
// is no "daily average imbalance" here, because averaging a 100 ms signal over
// a day does not make a daily signal, it makes noise with a small standard
// error.
//
// A BOOK WITH NO LIQUIDITY IS NOT A BOOK WITH ZERO IMBALANCE.
//
// The trap this family exists around. A zeroed `BookState` reads as "balanced,
// tight, no pressure" -- three confident, tradable-looking numbers -- when what
// actually happened is that no depth arrived. P2-08 already distinguishes
// them; this file carries that distinction into the vector as ABSENCE rather
// than collapsing it back into zeros.
//
// A CROSSED BOOK IS DATA, NOT AN ERROR. P2-08 stores and marks a crossed book
// rather than discarding it, because a crossed book is a real and informative
// state -- it happens at the open, during a circuit move, and when a feed is
// lagging. But the imbalance and microprice computed on one are not comparable
// to normal ones, so they are left ABSENT and the crossed flag is offered as
// its own feature instead.

#pragma once

#include <book/flow.hpp>
#include <book/l2_book.hpp>
#include <book/microstructure.hpp>
#include <features/vector.hpp>

#include <cmath>
#include <cstdint>
#include <expected>

namespace altair {

/// Slots for the book family. `kSkip` means the caller did not register it.
struct BookSlots {
    /// Order-book imbalance at the touch, in [-1, 1]. Positive is bid-heavy.
    FeatureIndex imbalance = kSkip;
    /// Depth-weighted imbalance across the visible book.
    FeatureIndex weighted_imbalance = kSkip;
    /// Microprice minus mid, in paise. The size-weighted fair value's
    /// departure from the naive midpoint.
    FeatureIndex microprice_offset = kSkip;
    /// Spread in paise.
    FeatureIndex spread = kSkip;
    /// Spread in basis points of the mid, which is what makes two instruments
    /// at different prices comparable.
    FeatureIndex spread_bps = kSkip;
    /// Total visible depth on each side, in units.
    FeatureIndex bid_depth = kSkip;
    FeatureIndex ask_depth = kSkip;
    /// 1.0 when the book is crossed, 0.0 when it is not. A FEATURE, not a
    /// reason to discard the tick -- see the header.
    FeatureIndex crossed = kSkip;

    /// How many book levels the depth features sum. NOT defaulted to the full
    /// book: a five-level depth and a two-level depth are different features,
    /// and a caller has to say which one it registered.
    std::uint8_t depth_levels = 0;
};

enum class BookFeatureError : std::uint8_t {
    /// No depth arrived. NOT the same as a balanced book.
    NoLiquidity,
    /// The book is crossed; the level features are not comparable.
    Crossed,
    /// A mid could not be formed, so nothing scaled by it can be.
    NoMid
};

/// Write the book family.
///
/// Returns how many slots were filled. A book with no liquidity fills NONE and
/// says NoLiquidity -- the caller gets an incomplete vector, which is the
/// truth, rather than a complete one full of zeros.
[[nodiscard]] inline std::expected<int, BookFeatureError>
build_book(const BookState& b, const BookSlots& s, FeatureVector& out) noexcept
{
    const DepthLevel* bb = best_bid(b);
    const DepthLevel* ba = best_ask(b);
    if (bb == nullptr || ba == nullptr) {
        return std::unexpected(BookFeatureError::NoLiquidity);
    }

    int written = 0;
    auto put = [&](FeatureIndex i, double v) {
        if (i != kSkip && out.set(i, v)) { ++written; }
    };

    // The crossed flag is written FIRST and unconditionally, because it is the
    // one thing that is still meaningful on a crossed book -- and a model that
    // sees the level features absent needs to know why.
    const bool is_crossed = !is_tradable(b);
    put(s.crossed, is_crossed ? 1.0 : 0.0);
    if (is_crossed) {
        return std::unexpected(BookFeatureError::Crossed);
    }

    if (const auto v = obi(b))          { put(s.imbalance, *v); }
    if (const auto v = weighted_obi(b)) { put(s.weighted_imbalance, *v); }
    // Depth is summed over an EXPLICIT number of levels. P2-08 requires the
    // count because "total depth" is not a fact about the book -- it is a
    // fact about how far down you chose to look, and two features summing
    // different depths are two different features (P5-01 hashes the lookback
    // for the same reason).
    if (const auto d = bid_depth(b, s.depth_levels)) {
        put(s.bid_depth, static_cast<double>(d->qty.raw()));
    }
    if (const auto d = ask_depth(b, s.depth_levels)) {
        put(s.ask_depth, static_cast<double>(d->qty.raw()));
    }

    const auto m = mid(b);
    const auto sp = spread(b);
    if (sp) { put(s.spread, static_cast<double>(sp->raw())); }
    if (m && sp && m->raw() > 0) {
        // Basis points of the mid. This is the form that is comparable across
        // instruments: 5 paise is a tight spread on NIFTY and an enormous one
        // on a Rs 20 stock.
        put(s.spread_bps,
            static_cast<double>(sp->raw()) * 10'000.0
            / static_cast<double>(m->raw()));
    }
    if (const auto mp = microprice(b)) {
        if (m) {
            // The OFFSET, not the level. A microprice of 24,080 tells a model
            // what the price is, which it already knows from the mid; the
            // departure from the mid is the information the book adds.
            put(s.microprice_offset,
                static_cast<double>(mp->raw() - m->raw()));
        }
    }
    return written;
}

/// Slots for the flow family.
struct FlowSlots {
    /// VPIN: the fraction of volume that is toxic (informed) flow.
    FeatureIndex vpin = kSkip;
    /// Kyle's lambda: expected price move per unit of signed volume.
    FeatureIndex kyle_lambda = kSkip;
    /// Lambda's own goodness of fit. Carried for the same reason Hurst's
    /// standard error is: a lambda fitted to eight trades is not a lambda,
    /// and P2-09b refuses to report one without its sample size.
    FeatureIndex kyle_r2 = kSkip;
    /// How many trades the lambda was fitted to.
    FeatureIndex kyle_samples = kSkip;
    /// How many completed volume buckets VPIN averages.
    FeatureIndex vpin_buckets = kSkip;
};

/// Write the flow family from the P2-09b estimators.
///
/// Each estimator is asked for its own reading and its slot is filled ONLY if
/// it has one. VPIN needs a completed volume bucket and lambda needs a
/// regression; neither has a meaningful value before then, and neither returns
/// a zero to say so.
template <class VpinT, class KyleT>
[[nodiscard]] inline int
build_flow(const VpinT& v, const KyleT& k, const FlowSlots& s,
           FeatureVector& out) noexcept {
    int written = 0;
    auto put = [&](FeatureIndex i, double x) {
        if (i != kSkip && out.set(i, x)) { ++written; }
    };
    if (const auto r = v.value()) {
        put(s.vpin, r->value);
        put(s.vpin_buckets, static_cast<double>(r->buckets));
    }
    if (const auto r = k.value()) {
        put(s.kyle_lambda, r->lambda);
        put(s.kyle_r2, r->r2);
        put(s.kyle_samples, static_cast<double>(r->samples));
    }
    return written;
}

} // namespace altair
