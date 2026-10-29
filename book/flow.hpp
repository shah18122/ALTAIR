// book/flow.hpp -- the live order-book imbalance engine: OBI, microprice, OFI,
// executed vs cancelled liquidity, persistence, absorption and withdrawal.
//
// Fed tick by tick: every book update (5 levels from the FYERS data socket, or
// up to 50 from the FYERS TBT socket, whichever the price service publishes
// for the instrument) and every trade print. One FlowEngine per instrument.
//
// WHAT IT MEASURES
//   * OBI over the touch, the top `short_levels` (5) and the top `deep_levels`
//     (up to 50) -- unweighted (exact integer sums, one rounding at the final
//     division) and weighted, w_i = exp(-decay * (i - 1)).
//   * Microprice (A1*Pbid + B1*Pask) / (B1 + A1), an exact integer rational in
//     paise, and its offset from the mid in ticks. Heavy bid size pulls it
//     toward the ASK: bid 999.95 x 8,000, ask 1,000.00 x 2,000 gives 999.99.
//   * OFI (Cont, Kukanov & Stoikov 2014) between consecutive books, at the
//     touch and summed over the top `short_levels` by level index, and its sum
//     over a rolling time window, normalised to [-1, +1] by sum |e|.
//   * Where displayed size went. A drop in a level's size is EXECUTED when a
//     trade print of that side explains it, CANCELLED when none does within
//     `settle_ns`. The two arrive on different FYERS sockets in either order,
//     so each waits up to `settle_ns` for the other; a print that never meets
//     a visible drop is HIDDEN execution (iceberg or reserve size). Trade size
//     is the change in the day's volume, which is exact even across dropped
//     frames; the side is the price against the book before it (at or through
//     the ask a buy, at or through the bid a sell, else above / below the mid,
//     exactly at the mid unknown).
//   * Persistence (consecutive updates the short OBI has held beyond
//     +/- threshold, and for how long), a time-decayed EWMA of it, momentum
//     (mid change over the window, ticks), the short-term high and low of the
//     mid (breakout window, excluding this update), and the composite
//     S = w_obi * OBI_deep_weighted + w_ofi * OFI_norm + w_mom * tanh(M / scale).
//   * Absorption (heavy selling into bids that do not fall and are refilled,
//     visibly or by hidden size;
//     the mirror for buying) and liquidity withdrawal (ask size cancelled while
//     buyers lift successive asks with real prints; the mirror on the bid).
//
// THE WEIGHTS AND THRESHOLDS ARE PLACEHOLDERS. S is a model structure, not a
// fitted one; nothing here has been calibrated out of sample.
//
// LIMITS OF THE DATA, said once. The book on the bus is a snapshot per frame;
// when frames are coalesced for a slow reader, OFI and the depletion split are
// computed between the books actually seen, so adds and removals inside the
// lost frames net out. Depletion is only measured over the price range BOTH
// books show, so a level falling off the bottom of a 5-level book is never
// read as a cancel.
//
// THREADING: one writer. A FlowEngine is driven by the single thread that owns
// it (the live engine's feed thread) and holds no locks and no atomics; it is
// never shared. Readers get a FlowSignals by value. Nothing on the update path
// allocates, throws or blocks: every buffer is sized in the constructor.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace altair::flow {

inline constexpr std::size_t kMaxLevels = 50;

/// One price level. UNIT: paise, units.
struct Level {
    std::int64_t px = 0;
    std::int64_t qty = 0;
};

// ---------------------------------------------------------------------------
// Pure kernels (also behind book/microstructure.hpp)
// ---------------------------------------------------------------------------

/// (b - a) / (b + a) from exact integer sums. Empty when there is no size at
/// all: 0.0 would claim "balanced" where the truth is "no information".
[[nodiscard]] inline std::optional<double> imbalance(std::int64_t b, std::int64_t a) noexcept {
    if (b < 0 || a < 0 || b > std::numeric_limits<std::int64_t>::max() - a) return std::nullopt;
    const std::int64_t t = b + a;
    if (t <= 0) return std::nullopt;
    return static_cast<double>(b - a) / static_cast<double>(t);
}

/// An exact rational in paise.
struct Ratio {
    std::int64_t num = 0, den = 1;
    [[nodiscard]] double value() const noexcept { return static_cast<double>(num) / static_cast<double>(den); }
    /// Rounded toward zero, as integer paise.
    [[nodiscard]] std::int64_t truncated() const noexcept { return num / den; }
};

namespace detail {
[[nodiscard]] constexpr bool mul_overflows(std::int64_t a, std::int64_t b) noexcept {
    if (a == 0 || b == 0) return false;
    if (a < 0 || b < 0) return true;   // prices and sizes are never negative here
    return a > std::numeric_limits<std::int64_t>::max() / b;
}
} // namespace detail

/// Microprice (bid_px * ask_qty + ask_px * bid_qty) / (bid_qty + ask_qty).
/// THE WEIGHTING IS CROSSED: bid price by ask size. Empty for a book that
/// cannot be traded (no bid, no ask, locked or crossed), no size, or overflow.
[[nodiscard]] inline std::optional<Ratio> microprice(std::int64_t bp, std::int64_t bq, std::int64_t ap,
                                                     std::int64_t aq) noexcept {
    if (bp <= 0 || ap <= bp || bq < 0 || aq < 0) return std::nullopt;
    if (bq > std::numeric_limits<std::int64_t>::max() - aq) return std::nullopt;
    const std::int64_t den = bq + aq;
    if (den <= 0) return std::nullopt;
    if (detail::mul_overflows(bp, aq) || detail::mul_overflows(ap, bq)) return std::nullopt;
    const std::int64_t l = bp * aq, r = ap * bq;
    if (l > std::numeric_limits<std::int64_t>::max() - r) return std::nullopt;
    return Ratio{l + r, den};
}

/// Level weights. Index 0 is the touch.
struct LevelWeights {
    std::array<double, kMaxLevels> w{};
    /// w_i = exp(-decay * i), i from 0: the touch weighs 1.
    [[nodiscard]] static LevelWeights exponential(double decay) noexcept {
        LevelWeights x;
        const double d = std::isfinite(decay) && decay > 0.0 ? decay : 0.0;
        for (std::size_t i = 0; i < kMaxLevels; ++i) x.w[i] = std::exp(-d * static_cast<double>(i));
        return x;
    }
    /// w_i = 1 / (1 + i).
    [[nodiscard]] static LevelWeights harmonic() noexcept {
        LevelWeights x;
        for (std::size_t i = 0; i < kMaxLevels; ++i) x.w[i] = 1.0 / static_cast<double>(1 + i);
        return x;
    }
};

/// Sum of the first `n` sizes. `qty_at(k)` returns level k's size.
template <class QtyAt>
[[nodiscard]] inline std::int64_t side_sum(QtyAt&& qty_at, std::size_t n) noexcept {
    std::int64_t s = 0;
    for (std::size_t k = 0; k < n; ++k) s += qty_at(k);
    return s;
}

/// Weighted imbalance over `nb` bid and `na` ask levels (each clamped to 50).
template <class BidQty, class AskQty>
[[nodiscard]] inline std::optional<double> weighted_imbalance(BidQty&& bq, std::size_t nb, AskQty&& aq, std::size_t na,
                                                              const LevelWeights& w) noexcept {
    if (nb == 0 || na == 0) return std::nullopt;
    // RULE 11: proven -- no book here holds more than kMaxLevels a side (the
    // bus carries at most 50, server/price_payload.hpp), and the weight table
    // has exactly that many; this only keeps the index inside it.
    nb = nb < kMaxLevels ? nb : kMaxLevels;
    na = na < kMaxLevels ? na : kMaxLevels;
    double b = 0.0, a = 0.0;
    for (std::size_t k = 0; k < nb; ++k) b += w.w[k] * static_cast<double>(bq(k));
    for (std::size_t k = 0; k < na; ++k) a += w.w[k] * static_cast<double>(aq(k));
    const double t = b + a;
    if (!(t > 0.0)) return std::nullopt;
    return (b - a) / t;
}

/// Cont-Kukanov-Stoikov order flow imbalance between two quotes of one level.
/// Exact integers: bids added or lifted count +, asks added or hit count -.
[[nodiscard]] constexpr std::int64_t ofi_level(std::int64_t pb0, std::int64_t qb0, std::int64_t pa0, std::int64_t qa0,
                                               std::int64_t pb1, std::int64_t qb1, std::int64_t pa1,
                                               std::int64_t qa1) noexcept {
    std::int64_t e = 0;
    if (pb1 >= pb0) e += qb1;
    if (pb1 <= pb0) e -= qb0;
    if (pa1 <= pa0) e -= qa1;
    if (pa1 >= pa0) e += qa0;
    return e;
}

/// Which side initiated a trade, from the book before it.
enum class Aggressor : std::uint8_t { Unknown, Buy, Sell };

[[nodiscard]] constexpr Aggressor aggressor(std::int64_t px, std::int64_t bid, std::int64_t ask) noexcept {
    if (bid <= 0 || ask <= bid || px <= 0) return Aggressor::Unknown;
    if (px >= ask) return Aggressor::Buy;
    if (px <= bid) return Aggressor::Sell;
    const std::int64_t twice = 2 * px, mid2 = bid + ask;   // inside the spread: against the mid, exactly
    if (twice > mid2) return Aggressor::Buy;
    if (twice < mid2) return Aggressor::Sell;
    return Aggressor::Unknown;
}

// ---------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------

struct FlowConfig {
    std::size_t short_levels = 5;                  ///< the "five-level OBI"
    std::size_t deep_levels = 50;                  ///< clamped to what the book shows
    double decay = 0.25;                           ///< exponential level-weight decay
    std::int64_t tick = 5;                         ///< paise (NSE F&O: 5)
    std::int64_t window_ns = 5'000'000'000;        ///< OFI, flow, momentum, absorption window
    std::int64_t breakout_ns = 30'000'000'000;     ///< the short-term high / low
    std::int64_t settle_ns = 500'000'000;          ///< how long a print and a drop wait for each other
    std::int64_t ewma_ns = 2'000'000'000;          ///< OBI EWMA time constant
    double threshold = 0.30;                       ///< |OBI_short| that counts toward persistence
    double w_obi = 0.4, w_ofi = 0.4, w_mom = 0.2;  ///< S weights (placeholders)
    double mom_scale_ticks = 4.0;                  ///< momentum normalisation
    double absorb_ratio = 2.0;                     ///< heavy side's prints over the other's
    double absorb_refill = 0.5;                    ///< refills over the prints absorbed
    double withdraw_ratio = 1.0;                   ///< cancelled size over the mean touch size
    std::int64_t withdraw_ticks = 2;               ///< successive levels lifted / hit
    std::size_t capacity = 1024;                   ///< events per window (rounded up to a power of two)
};

/// What the session has seen since the last reset(), never expired: a
/// screen diffs two snapshots to list each event (desktop/order_flow_page.hpp).
struct FlowTotals {
    std::int64_t buy = 0, sell = 0, unknown = 0;                   ///< printed, units
    std::int64_t bid_exec_drop = 0, ask_exec_drop = 0;
    std::int64_t bid_cancel = 0, ask_cancel = 0, bid_add = 0, ask_add = 0;
    std::int64_t hidden_buy = 0, hidden_sell = 0;
};

/// Everything the engine knows after the last event. Plain data.
struct FlowSignals {
    std::int64_t ns = 0;
    bool valid = false;                 ///< a tradable book has been seen and is current
    std::uint16_t bid_levels = 0, ask_levels = 0;
    std::int64_t bid = 0, ask = 0, bid_qty = 0, ask_qty = 0;   ///< touch, paise / units
    double mid = 0.0;                   ///< paise
    double spread_ticks = 0.0;
    Ratio microprice{};
    double micro_offset_ticks = 0.0;    ///< microprice - mid
    double obi1 = 0.0, obi_short = 0.0, obi_deep = 0.0, obi_deep_w = 0.0;
    std::uint16_t short_used = 0, deep_used = 0;   ///< levels actually summed (min of both sides)
    std::int64_t ofi_last = 0, ofi_multi_last = 0;
    std::int64_t ofi_window = 0, ofi_multi_window = 0;
    double ofi_norm = 0.0;              ///< sum e / sum |e| over the window, [-1, +1]
    std::int64_t buy_exec = 0, sell_exec = 0, unknown_exec = 0;   ///< prints in the window, units
    std::int64_t ask_exec_drop = 0, bid_exec_drop = 0;            ///< displayed size taken by prints
    std::int64_t ask_cancel = 0, bid_cancel = 0;                  ///< displayed size withdrawn
    std::int64_t ask_add = 0, bid_add = 0;                        ///< displayed size added
    std::int64_t hidden_buy = 0, hidden_sell = 0;                 ///< prints no visible drop explained
    double trade_imbalance = 0.0;       ///< (buy - sell) / (buy + sell), [-1, +1]
    double momentum_ticks = 0.0, mom_norm = 0.0;
    double score = 0.0;                 ///< S
    int persist = 0;                    ///< updates beyond +threshold (> 0) or -threshold (< 0)
    std::int64_t persist_ns = 0;        ///< since the run began
    double obi_ewma = 0.0;
    double prior_high = 0.0, prior_low = 0.0;   ///< mid over the breakout window before this update (paise; 0 none)
    std::int64_t bid_window_low = 0, ask_window_high = 0, bid_window_start = 0, ask_window_start = 0;
    double mean_bid_qty = 0.0, mean_ask_qty = 0.0;   ///< touch size over the window
    int absorption = 0;                 ///< +1 buyers absorb selling, -1 sellers absorb buying
    int withdrawal = 0;                 ///< +1 ask liquidity pulled while lifted, -1 bid side
    std::uint64_t books = 0, trades = 0, malformed = 0, evicted = 0, untradable = 0;
    FlowTotals total{};                 ///< since reset(), not windowed
};

namespace detail {

/// Prints or drops waiting to be explained, oldest first: a fixed ring.
/// A matched item drops to zero size and is skipped, then popped once it
/// reaches the front; nothing is ever shifted.
class Pending {
public:
    struct Item { std::int64_t ns = 0, px = 0, qty = 0; };
    static constexpr std::size_t kCap = 256;   // a power of two
    [[nodiscard]] std::size_t size() const noexcept { return t_ - h_; }
    [[nodiscard]] Item& at(std::size_t i) noexcept { return a_[(h_ + i) & (kCap - 1)]; }
    [[nodiscard]] const Item& at(std::size_t i) const noexcept { return a_[(h_ + i) & (kCap - 1)]; }
    void pop() noexcept { ++h_; }
    /// Pops spent items off the front.
    void trim() noexcept { while (h_ != t_ && a_[h_ & (kCap - 1)].qty <= 0) ++h_; }
    /// Appends; when full the oldest is removed first and handed back.
    std::optional<Item> push(Item x) noexcept {
        std::optional<Item> out;
        trim();
        if (size() == kCap) { out = at(0); pop(); }
        a_[t_ & (kCap - 1)] = x;
        ++t_;
        return out;
    }
    void clear() noexcept { h_ = t_ = 0; }

private:
    std::array<Item, kCap> a_{};
    std::size_t h_ = 0, t_ = 0;
};

/// Sliding-window extreme of (ns, value) by time: monotonic deque, fixed size.
template <bool Max>
class WindowExtreme {
public:
    explicit WindowExtreme(std::size_t cap) : a_(cap), mask_(cap - 1) {}
    void clear() noexcept { h_ = t_ = 0; }
    void push(std::int64_t ns, double v) noexcept {
        while (t_ > h_ && (Max ? a_[(t_ - 1) & mask_].v <= v : a_[(t_ - 1) & mask_].v >= v)) --t_;
        if (t_ - h_ == a_.size()) ++h_;   // full: the oldest goes early
        a_[t_ & mask_] = {ns, v};
        ++t_;
    }
    void expire(std::int64_t before_ns) noexcept {
        while (t_ > h_ && a_[h_ & mask_].ns < before_ns) ++h_;
    }
    [[nodiscard]] bool empty() const noexcept { return t_ == h_; }
    [[nodiscard]] double value() const noexcept { return a_[h_ & mask_].v; }

private:
    struct E { std::int64_t ns = 0; double v = 0.0; };
    std::vector<E> a_;
    std::size_t mask_;
    std::size_t h_ = 0, t_ = 0;
};

[[nodiscard]] inline std::size_t pow2(std::size_t n) noexcept {
    std::size_t p = 64;
    while (p < n && p < (std::size_t{1} << 20)) p <<= 1;
    return p;
}

} // namespace detail

class FlowEngine {
public:
    explicit FlowEngine(FlowConfig c = {})
        : cfg_(sane(c)), w_(LevelWeights::exponential(cfg_.decay)), ring_(detail::pow2(cfg_.capacity)), mask_(ring_.size() - 1),
          hi_(ring_.size()), lo_(ring_.size()), bid_lo_(ring_.size()), ask_hi_(ring_.size()) {}

    [[nodiscard]] const FlowConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] const FlowSignals& signals() const noexcept { return s_; }

    /// A new session: everything goes.
    void reset() noexcept {
        books_[0] = books_[1] = Book{};
        cur_ix_ = 0;
        have_book_ = false;
        h_ = t_ = 0;
        sum_ = Sums{};
        hi_.clear(); lo_.clear(); bid_lo_.clear(); ask_hi_.clear();
        bid_drop_.clear(); ask_drop_.clear(); buy_wait_.clear(); sell_wait_.clear();
        last_volume_ = -1;
        persist_ = 0; persist_since_ = 0; ewma_ = 0.0; ewma_ns_ = 0;
        s_ = FlowSignals{};
    }

    /// A book: `nb` bids best first, `na` asks best first (any type with
    /// .px and .qty, paise and units), at feed time `ns`. Levels from the
    /// first non-positive price or size, or the first out of order, are not
    /// used.
    template <class L>
    void on_book(const L* bids, std::size_t nb, const L* asks, std::size_t na, std::int64_t ns) noexcept {
        // Double-buffered: the new book is read into the spare slot, and the
        // slots swap -- no copy, no clearing (only the first nb / na are read).
        Book& b = books_[cur_ix_ ^ 1u];
        const Book& p = books_[cur_ix_];
        Diff db, da;
        b.nb = load(bids, nb, b.bid, b.bid_cum, b.bid_wcum, true, p.bid, p.nb, db);
        b.na = load(asks, na, b.ask, b.ask_cum, b.ask_wcum, false, p.ask, p.na, da);
        ++s_.books;
        if (b.nb == 0 || b.na == 0 || b.ask[0].px <= b.bid[0].px) {
            // Untradable (one-sided, locked, crossed): no OBI, and no OFI or
            // depletion measured against it on either side.
            ++s_.untradable;
            have_book_ = false;
            cur_ix_ ^= 1u;
            settle(ns);
            s_.valid = false;
            s_.ns = ns;
            return;
        }
        Ev e;
        e.ns = ns;
        e.book = true;
        touch_of(b, e);
        if (have_book_) {
            e.ofi = ofi_level(p.bid[0].px, p.bid[0].qty, p.ask[0].px, p.ask[0].qty, b.bid[0].px, b.bid[0].qty,
                              b.ask[0].px, b.ask[0].qty);
            const std::size_t n = std::min({cfg_.short_levels, p.nb, p.na, b.nb, b.na});
            for (std::size_t k = 0; k < n; ++k)
                e.ofi_multi += ofi_level(p.bid[k].px, p.bid[k].qty, p.ask[k].px, p.ask[k].qty, b.bid[k].px,
                                         b.bid[k].qty, b.ask[k].px, b.ask[k].qty);
            // A drop at a price waits for prints that reach it; prints already
            // waiting are matched first, best price first.
            const auto on_bid = [&](std::int64_t px, std::int64_t q) { e.bid_exec_drop += drop(px, q, ns, true); };
            const auto on_ask = [&](std::int64_t px, std::int64_t q) { e.ask_exec_drop += drop(px, q, ns, false); };
            // The usual diff -- the same prices, some sizes changed -- was
            // measured while the book was read; anything else is merged by price.
            if (db.same) { e.bid_add += db.add; for (std::size_t i = 0; i < db.ndrop; ++i) on_bid(p.bid[db.drop_at[i]].px, db.drop_qty[i]); }
            else changes(p.bid, p.nb, b.bid, b.nb, true, e.bid_add, on_bid);
            if (da.same) { e.ask_add += da.add; for (std::size_t i = 0; i < da.ndrop; ++i) on_ask(p.ask[da.drop_at[i]].px, da.drop_qty[i]); }
            else changes(p.ask, p.na, b.ask, b.na, false, e.ask_add, on_ask);
        }
        cur_ix_ ^= 1u;
        have_book_ = true;
        // The breakout high / low is the window BEFORE this update.
        const double mid = 0.5 * static_cast<double>(e.bid + e.ask);
        hi_.expire(ns - cfg_.breakout_ns);
        lo_.expire(ns - cfg_.breakout_ns);
        s_.prior_high = hi_.empty() ? 0.0 : hi_.value();
        s_.prior_low = lo_.empty() ? 0.0 : lo_.value();
        hi_.push(ns, mid);
        lo_.push(ns, mid);
        bid_lo_.push(ns, static_cast<double>(e.bid));
        ask_hi_.push(ns, static_cast<double>(e.ask));
        push(e);
        settle(ns);
        snapshot(ns, &e);
    }

    /// A trade print at `px` paise. `volume` is the day's cumulative volume
    /// (exact; < 0 when the feed did not send it); `last_qty`, the print's own
    /// size, is used only without volume.
    void on_trade(std::int64_t px, std::int64_t volume, std::int64_t last_qty, std::int64_t ns) noexcept {
        std::int64_t q = 0;
        if (volume >= 0) {
            if (last_volume_ >= 0 && volume > last_volume_) q = volume - last_volume_;
            // RULE 11: a volume that went backwards (a new session, a feed
            // restart) re-bases; no size is invented for it.
            last_volume_ = volume;
        } else if (last_qty > 0) {
            q = last_qty;
        }
        ++s_.trades;
        if (q > 0) {
            Ev e;
            e.ns = ns;
            touch_of(books_[cur_ix_], e);
            switch (aggressor(px, have_book_ ? e.bid : 0, have_book_ ? e.ask : 0)) {
            case Aggressor::Buy: e.buy = q; e.ask_exec_drop = print(px, q, ns, true); break;
            case Aggressor::Sell: e.sell = q; e.bid_exec_drop = print(px, q, ns, false); break;
            case Aggressor::Unknown: e.unknown = q; break;
            }
            push(e);
        }
        settle(ns);
        snapshot(ns, nullptr);
    }

private:
    struct Book {
        std::size_t nb = 0, na = 0;
        std::array<Level, kMaxLevels> bid{}, ask{};
        /// Running sums from the touch: size, and weighted size.
        std::array<std::int64_t, kMaxLevels> bid_cum{}, ask_cum{};
        std::array<double, kMaxLevels> bid_wcum{}, ask_wcum{};
    };
    /// One side against the same side of the book before, at equal indices.
    struct Diff {
        bool same = false;            ///< same levels at the same prices
        std::int64_t add = 0;         ///< size added, summed
        std::size_t ndrop = 0;        ///< levels whose size fell
        std::array<std::uint8_t, kMaxLevels> drop_at;     // written before read: not cleared per book
        std::array<std::int64_t, kMaxLevels> drop_qty;
    };
    /// One event in the window. Every event carries the touch as it stood.
    struct Ev {
        std::int64_t ns = 0;
        std::int64_t bid = 0, ask = 0, bid_qty = 0, ask_qty = 0;
        std::int64_t ofi = 0, ofi_multi = 0;
        std::int64_t buy = 0, sell = 0, unknown = 0;
        std::int64_t bid_add = 0, ask_add = 0, bid_exec_drop = 0, ask_exec_drop = 0;
        std::int64_t bid_cancel = 0, ask_cancel = 0, hidden_buy = 0, hidden_sell = 0;
        bool book = false;
    };
    struct Sums {
        std::int64_t ofi = 0, ofi_abs = 0, ofi_multi = 0;
        std::int64_t buy = 0, sell = 0, unknown = 0;
        std::int64_t bid_add = 0, ask_add = 0, bid_exec_drop = 0, ask_exec_drop = 0;
        std::int64_t bid_cancel = 0, ask_cancel = 0, hidden_buy = 0, hidden_sell = 0;
        std::int64_t bid_qty = 0, ask_qty = 0, books = 0;
    };

    static FlowConfig sane(FlowConfig c) noexcept {
        if (c.short_levels == 0) c.short_levels = 1;
        if (c.short_levels > kMaxLevels) c.short_levels = kMaxLevels;
        if (c.deep_levels == 0) c.deep_levels = 1;
        if (c.deep_levels > kMaxLevels) c.deep_levels = kMaxLevels;
        if (c.tick <= 0) c.tick = 1;
        if (c.window_ns <= 0) c.window_ns = 1;
        if (c.breakout_ns <= 0) c.breakout_ns = c.window_ns;
        if (c.settle_ns < 0) c.settle_ns = 0;
        if (c.ewma_ns <= 0) c.ewma_ns = 1;
        if (!(c.mom_scale_ticks > 0.0)) c.mom_scale_ticks = 1.0;
        return c;
    }

    void touch_of(const Book& b, Ev& e) const noexcept {
        e.bid = b.nb ? b.bid[0].px : 0;
        e.ask = b.na ? b.ask[0].px : 0;
        e.bid_qty = b.nb ? b.bid[0].qty : 0;
        e.ask_qty = b.na ? b.ask[0].qty : 0;
    }

    /// Copies the usable prefix of one side -- positive prices and sizes, each
    /// level strictly worse than the one before -- and, in the same pass, its
    /// running sums and its diff against the same side of the book before.
    /// Returns how many levels.
    template <class L>
    std::size_t load(const L* in, std::size_t n, std::array<Level, kMaxLevels>& out, std::array<std::int64_t, kMaxLevels>& cum,
                     std::array<double, kMaxLevels>& wcum, bool bid, const std::array<Level, kMaxLevels>& old,
                     std::size_t old_n, Diff& d) noexcept {
        if (in == nullptr) return 0;
        // RULE 11: proven -- the bus carries at most 50 levels a side
        // (server/price_payload.hpp kMaxDepthLevels == kMaxLevels).
        n = n < kMaxLevels ? n : kMaxLevels;
        std::int64_t run = 0, pxdiff = 0;
        double w0 = 0.0, w1 = 0.0;   // two lanes: half the dependency chain
        std::size_t k = 0;
        for (; k < n; ++k) {
            const auto px = static_cast<std::int64_t>(in[k].px), q = static_cast<std::int64_t>(in[k].qty);
            if (px <= 0 || q <= 0) break;   // padding: the end of what is real
            if (k > 0 && (bid ? px >= out[k - 1].px : px <= out[k - 1].px)) { ++s_.malformed; break; }
            out[k] = Level{px, q};
            run += q;
            cum[k] = run;
            const double wq = w_.w[k] * static_cast<double>(q);
            if (k & 1u) w1 += wq; else w0 += wq;
            wcum[k] = w0 + w1;
            // Against the old book at the same index: always initialised
            // memory, and only trusted when every index matched.
            pxdiff |= px ^ old[k].px;
            const std::int64_t dq = q - old[k].qty;
            if (dq < 0) { d.drop_at[d.ndrop] = static_cast<std::uint8_t>(k); d.drop_qty[d.ndrop] = -dq; ++d.ndrop; }
            else d.add += dq;
        }
        d.same = have_book_ && k == old_n && pxdiff == 0;
        return k;
    }

    /// Size removed from and added to one side between two books, price by
    /// price, over the price range BOTH books show (a level past it is out of
    /// sight, not gone). Removals go to `on_drop(px, qty)`, best price first.
    template <class OnDrop>
    static void changes(const std::array<Level, kMaxLevels>& o, std::size_t no, const std::array<Level, kMaxLevels>& n,
                        std::size_t nn, bool bid, std::int64_t& add, OnDrop&& on_drop) noexcept {
        if (no == 0 || nn == 0) return;
        const std::int64_t edge = bid ? std::max(o[no - 1].px, n[nn - 1].px) : std::min(o[no - 1].px, n[nn - 1].px);
        const auto better = [bid](std::int64_t a, std::int64_t b) { return bid ? a > b : a < b; };
        const auto inside = [&](std::int64_t px) { return bid ? px >= edge : px <= edge; };
        std::size_t i = 0, j = 0;
        for (;;) {
            const bool hi = i < no && inside(o[i].px), hj = j < nn && inside(n[j].px);
            if (!hi && !hj) break;
            if (hi && (!hj || better(o[i].px, n[j].px))) { on_drop(o[i].px, o[i].qty); ++i; }   // level gone
            else if (hj && (!hi || better(n[j].px, o[i].px))) { add += n[j].qty; ++j; }        // level new
            else {                                                                            // same price
                const std::int64_t d = n[j].qty - o[i].qty;
                if (d < 0) on_drop(o[i].px, -d);
                else add += d;
                ++i; ++j;
            }
        }
    }

    /// A print reaches a level when it trades at or through it: a buy at
    /// `px` takes asks priced <= px, a sell takes bids priced >= px.
    [[nodiscard]] static bool reaches(bool buy, std::int64_t print_px, std::int64_t level_px) noexcept {
        return buy ? level_px <= print_px : level_px >= print_px;
    }

    /// A drop of `q` at `px` on the bid (`bid`) or ask side: matched against
    /// waiting prints, oldest first; the rest waits. Returns the size matched.
    std::int64_t drop(std::int64_t px, std::int64_t q, std::int64_t ns, bool bid) noexcept {
        detail::Pending& prints = bid ? sell_wait_ : buy_wait_;
        std::int64_t got = 0;
        for (std::size_t k = 0; k < prints.size() && q > 0; ++k) {
            auto& p = prints.at(k);
            if (p.qty <= 0 || !reaches(!bid, p.px, px)) continue;
            // RULE 11: not a capacity clamp -- matching arithmetic; what is
            // left of either side carries on (and waits), nothing is dropped.
            const std::int64_t m = p.qty < q ? p.qty : q;
            p.qty -= m; q -= m; got += m;
        }
        prints.trim();
        if (q > 0) wait(bid ? bid_drop_ : ask_drop_, {ns, px, q}, ns);
        return got;
    }

    /// A print of `q` at `px`: matched against waiting drops it reaches,
    /// best price first, oldest first within a price; the rest waits.
    std::int64_t print(std::int64_t px, std::int64_t q, std::int64_t ns, bool buy) noexcept {
        detail::Pending& drops = buy ? ask_drop_ : bid_drop_;
        std::int64_t got = 0;
        while (q > 0) {
            std::size_t best = drops.size();
            for (std::size_t k = 0; k < drops.size(); ++k) {
                const auto& d = drops.at(k);
                if (d.qty <= 0 || !reaches(buy, px, d.px)) continue;
                if (best == drops.size() || (buy ? d.px < drops.at(best).px : d.px > drops.at(best).px)) best = k;
            }
            if (best == drops.size()) break;
            auto& d = drops.at(best);
            // RULE 11: not a capacity clamp -- matching arithmetic; the rest carries on.
            const std::int64_t m = d.qty < q ? d.qty : q;
            d.qty -= m; q -= m; got += m;
        }
        drops.trim();
        if (q > 0) wait(buy ? buy_wait_ : sell_wait_, {ns, px, q}, ns);
        return got;
    }

    void wait(detail::Pending& list, detail::Pending::Item it, std::int64_t ns) noexcept {
        if (const auto old = list.push(it)) expire_one(list, *old, ns);   // full: the oldest settles early
    }

    /// An item that waited too long: a drop no print explained is a cancel;
    /// a print no drop explained is hidden execution.
    void expire_one(const detail::Pending& list, const detail::Pending::Item& it, std::int64_t ns) noexcept {
        Ev e;
        e.ns = ns;
        touch_of(books_[cur_ix_], e);
        if (&list == &bid_drop_) e.bid_cancel = it.qty;
        else if (&list == &ask_drop_) e.ask_cancel = it.qty;
        else if (&list == &buy_wait_) e.hidden_buy = it.qty;
        else e.hidden_sell = it.qty;
        push(e);
    }

    /// Settles everything that has waited longer than settle_ns, as one event.
    void settle(std::int64_t ns) noexcept {
        Ev e;
        bool any = false;
        const auto run = [&](detail::Pending& l, std::int64_t& into) {
            for (l.trim(); l.size() > 0 && ns - l.at(0).ns > cfg_.settle_ns; l.trim()) {
                into += l.at(0).qty;
                l.pop();
                any = true;
            }
        };
        run(bid_drop_, e.bid_cancel);
        run(ask_drop_, e.ask_cancel);
        run(buy_wait_, e.hidden_buy);
        run(sell_wait_, e.hidden_sell);
        if (!any) return;
        e.ns = ns;
        touch_of(books_[cur_ix_], e);
        push(e);
    }

    void add(const Ev& e, std::int64_t sign) noexcept {
        sum_.ofi += sign * e.ofi;
        sum_.ofi_abs += sign * (e.ofi < 0 ? -e.ofi : e.ofi);
        sum_.ofi_multi += sign * e.ofi_multi;
        sum_.buy += sign * e.buy; sum_.sell += sign * e.sell; sum_.unknown += sign * e.unknown;
        sum_.bid_add += sign * e.bid_add; sum_.ask_add += sign * e.ask_add;
        sum_.bid_exec_drop += sign * e.bid_exec_drop; sum_.ask_exec_drop += sign * e.ask_exec_drop;
        sum_.bid_cancel += sign * e.bid_cancel; sum_.ask_cancel += sign * e.ask_cancel;
        sum_.hidden_buy += sign * e.hidden_buy; sum_.hidden_sell += sign * e.hidden_sell;
        if (e.book) { sum_.bid_qty += sign * e.bid_qty; sum_.ask_qty += sign * e.ask_qty; sum_.books += sign; }
    }

    void expire_window(std::int64_t ns) noexcept {
        while (t_ > h_ && ring_[h_ & mask_].ns < ns - cfg_.window_ns) { add(ring_[h_ & mask_], -1); ++h_; }
    }

    void push(const Ev& e) noexcept {
        // A window holding more events than the ring evicts the oldest early,
        // and counts it: never silently.
        expire_window(e.ns);
        if (t_ - h_ == ring_.size()) { add(ring_[h_ & mask_], -1); ++h_; ++s_.evicted; }
        ring_[t_ & mask_] = e;
        ++t_;
        add(e, +1);
        FlowTotals& d = s_.total;
        d.buy += e.buy; d.sell += e.sell; d.unknown += e.unknown;
        d.bid_exec_drop += e.bid_exec_drop; d.ask_exec_drop += e.ask_exec_drop;
        d.bid_cancel += e.bid_cancel; d.ask_cancel += e.ask_cancel; d.bid_add += e.bid_add; d.ask_add += e.ask_add;
        d.hidden_buy += e.hidden_buy; d.hidden_sell += e.hidden_sell;
    }

    /// The signals after an event; `book` is the book update just applied, if one was.
    void snapshot(std::int64_t ns, const Ev* book) noexcept {
        expire_window(ns);
        bid_lo_.expire(ns - cfg_.window_ns);
        ask_hi_.expire(ns - cfg_.window_ns);
        FlowSignals& s = s_;
        s.ns = ns;
        s.valid = have_book_;
        if (!s.valid) return;
        const Book& b = books_[cur_ix_];
        s.bid_levels = static_cast<std::uint16_t>(b.nb);
        s.ask_levels = static_cast<std::uint16_t>(b.na);
        s.bid = b.bid[0].px; s.ask = b.ask[0].px; s.bid_qty = b.bid[0].qty; s.ask_qty = b.ask[0].qty;
        const double tick = static_cast<double>(cfg_.tick);
        s.mid = 0.5 * static_cast<double>(s.bid + s.ask);
        s.spread_ticks = static_cast<double>(s.ask - s.bid) / tick;
        if (const auto mp = microprice(s.bid, s.bid_qty, s.ask, s.ask_qty)) {
            s.microprice = *mp;
            s.micro_offset_ticks = (mp->value() - s.mid) / tick;
        }
        if (book != nullptr) {   // the book-only readings change only when the book does
            // The same number of levels on both sides, so a side the feed shows
            // fewer levels of is not read as a weaker side.
            const std::size_t n_short = std::min({cfg_.short_levels, b.nb, b.na});
            const std::size_t n_deep = std::min({cfg_.deep_levels, b.nb, b.na});
            s.short_used = static_cast<std::uint16_t>(n_short);
            s.deep_used = static_cast<std::uint16_t>(n_deep);
            s.obi1 = imbalance(b.bid[0].qty, b.ask[0].qty).value_or(0.0);
            s.obi_short = imbalance(b.bid_cum[n_short - 1], b.ask_cum[n_short - 1]).value_or(0.0);
            s.obi_deep = imbalance(b.bid_cum[n_deep - 1], b.ask_cum[n_deep - 1]).value_or(0.0);
            // The same arithmetic as weighted_imbalance, summed as the book was read.
            const double wb = b.bid_wcum[n_deep - 1], wa = b.ask_wcum[n_deep - 1];
            s.obi_deep_w = wb + wa > 0.0 ? (wb - wa) / (wb + wa) : 0.0;
            s.ofi_last = book->ofi;
            s.ofi_multi_last = book->ofi_multi;
            const int dir = s.obi_short >= cfg_.threshold ? 1 : (s.obi_short <= -cfg_.threshold ? -1 : 0);
            if (dir == 0) persist_ = 0;
            else if (persist_ == 0 || (dir > 0) != (persist_ > 0)) { persist_ = dir; persist_since_ = ns; }
            else persist_ += dir;
            if (ewma_ns_ == 0) ewma_ = s.obi_short;
            else {
                const double a = 1.0 - std::exp(-static_cast<double>(ns - ewma_ns_) / static_cast<double>(cfg_.ewma_ns));
                ewma_ += a * (s.obi_short - ewma_);
            }
            ewma_ns_ = ns;
        }
        s.persist = persist_;
        s.persist_ns = persist_ == 0 ? 0 : ns - persist_since_;
        s.obi_ewma = ewma_;

        s.ofi_window = sum_.ofi;
        s.ofi_multi_window = sum_.ofi_multi;
        s.ofi_norm = sum_.ofi_abs > 0 ? static_cast<double>(sum_.ofi) / static_cast<double>(sum_.ofi_abs) : 0.0;
        s.buy_exec = sum_.buy; s.sell_exec = sum_.sell; s.unknown_exec = sum_.unknown;
        s.ask_exec_drop = sum_.ask_exec_drop; s.bid_exec_drop = sum_.bid_exec_drop;
        s.ask_cancel = sum_.ask_cancel; s.bid_cancel = sum_.bid_cancel;
        s.ask_add = sum_.ask_add; s.bid_add = sum_.bid_add;
        s.hidden_buy = sum_.hidden_buy; s.hidden_sell = sum_.hidden_sell;
        s.trade_imbalance = imbalance(sum_.buy, sum_.sell).value_or(0.0);

        // The window's oldest event holds the touch as the window began.
        const Ev& first = ring_[h_ & mask_];
        s.bid_window_start = first.bid > 0 ? first.bid : s.bid;
        s.ask_window_start = first.ask > 0 ? first.ask : s.ask;
        s.bid_window_low = bid_lo_.empty() ? s.bid : static_cast<std::int64_t>(bid_lo_.value());
        s.ask_window_high = ask_hi_.empty() ? s.ask : static_cast<std::int64_t>(ask_hi_.value());
        const double mid0 = s.ask_window_start > s.bid_window_start
                                ? 0.5 * static_cast<double>(s.bid_window_start + s.ask_window_start) : s.mid;
        s.momentum_ticks = (s.mid - mid0) / tick;
        s.mom_norm = std::tanh(s.momentum_ticks / cfg_.mom_scale_ticks);
        s.score = cfg_.w_obi * s.obi_deep_w + cfg_.w_ofi * s.ofi_norm + cfg_.w_mom * s.mom_norm;
        s.mean_bid_qty = sum_.books > 0 ? static_cast<double>(sum_.bid_qty) / static_cast<double>(sum_.books) : 0.0;
        s.mean_ask_qty = sum_.books > 0 ? static_cast<double>(sum_.ask_qty) / static_cast<double>(sum_.books) : 0.0;

        // C. Absorption: one side prints heavily into the other, whose price
        // holds and whose size is refilled -- visibly (adds) or invisibly
        // (prints no displayed drop explained: an iceberg reloading).
        const auto heavy = [&](std::int64_t mine, std::int64_t theirs) {
            return mine > 0 && static_cast<double>(mine) >= cfg_.absorb_ratio * static_cast<double>(theirs);
        };
        s.absorption = 0;
        if (heavy(sum_.sell, sum_.buy) && s.bid_window_low >= s.bid_window_start
            && static_cast<double>(sum_.bid_add + sum_.hidden_sell) >= cfg_.absorb_refill * static_cast<double>(sum_.sell))
            s.absorption = 1;
        else if (heavy(sum_.buy, sum_.sell) && s.ask_window_high <= s.ask_window_start
                 && static_cast<double>(sum_.ask_add + sum_.hidden_buy) >= cfg_.absorb_refill * static_cast<double>(sum_.buy))
            s.absorption = -1;
        // D. Withdrawal: size pulled from one side while real prints lift
        // (hit) successive levels of it.
        const std::int64_t lift = cfg_.withdraw_ticks * cfg_.tick;
        s.withdrawal = 0;
        if (s.mean_ask_qty > 0.0 && static_cast<double>(sum_.ask_cancel) >= cfg_.withdraw_ratio * s.mean_ask_qty
            && sum_.buy > sum_.sell && s.ask - s.ask_window_start >= lift)
            s.withdrawal = 1;
        else if (s.mean_bid_qty > 0.0 && static_cast<double>(sum_.bid_cancel) >= cfg_.withdraw_ratio * s.mean_bid_qty
                 && sum_.sell > sum_.buy && s.bid_window_start - s.bid >= lift)
            s.withdrawal = -1;
    }

    FlowConfig cfg_;
    LevelWeights w_;
    Book books_[2]{};
    unsigned cur_ix_ = 0;
    bool have_book_ = false;
    std::vector<Ev> ring_;
    std::size_t mask_;
    std::size_t h_ = 0, t_ = 0;
    Sums sum_{};
    detail::WindowExtreme<true> hi_;
    detail::WindowExtreme<false> lo_;
    detail::WindowExtreme<false> bid_lo_;
    detail::WindowExtreme<true> ask_hi_;
    detail::Pending bid_drop_, ask_drop_, buy_wait_, sell_wait_;
    std::int64_t last_volume_ = -1;
    int persist_ = 0;
    std::int64_t persist_since_ = 0;
    double ewma_ = 0.0;
    std::int64_t ewma_ns_ = 0;
    FlowSignals s_{};
};

} // namespace altair::flow
