// book/l2_book.hpp — the L2 order book.
//
// P2-08. Kite's full mode and XTS 1502 both deliver a five-level SNAPSHOT, not
// deltas, so applying an update is a replace and O(1) is trivial at this depth.
// A price-keyed map would be slower, would allocate, and would model a problem
// Altair does not have. If a tick-by-tick order-level feed ever arrives, that
// is a different card, not an extension of this one.
//
// Two decisions carry the weight, and the first REVERSES what the skeleton of
// this file originally said:
//
//   * A crossed book is STORED AND MARKED, never rejected. During NSE's
//     pre-open call auction, orders are collected WITHOUT MATCHING, so the book
//     legitimately crosses while the equilibrium price is found. Rejecting
//     would leave the previous continuous-session book in place and present it
//     as current -- stale data masquerading as live, which is strictly worse
//     than crossed data correctly labelled. Flag, do not drop: the same rule
//     P2-04 follows, where only a rule-7 look-ahead violation drops.
//
//   * A book that has never been updated is NotFound, not empty. A default
//     BookState reads as "no liquidity" when the truth is "no data", and those
//     are different claims -- the first one is a lie a strategy would act on.
//
// Decisions D1..D6 are fixed in prompts/P2-08_l2_book.md.

#pragma once

#include <feed/tick.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class BookError : std::uint8_t {
    NotFound,        // never updated — NOT the same as empty (D1)
    BadInstrument,   // id outside the addressable range
    StaleSequence,   // seq not greater than the stored one (D3)
    BadDepthCount,   // a side claims more levels than its fixed array holds
    StaleTick        // the normaliser marked exchange time implausible
};

/// One instrument's book. Fixed-size and trivially copyable for snapshot
/// publication to analytics; it never owns dynamic storage.
struct BookState {
    InstrumentId  id;
    std::uint32_t seq;
    Timestamp     exchange_ts;
    Timestamp     recv_ts;
    DepthLevel    bid[kDepthLevels];
    DepthLevel    ask[kDepthLevels];
    std::uint8_t  bid_levels;
    std::uint8_t  ask_levels;

    /// D2: bid >= ask with both sides present. Stored, marked, not tradable.
    /// Legitimate during the pre-open auction; a fault at any other time.
    bool          crossed;
    std::uint8_t  reserved;

    /// D4: rejections since the last ACCEPTED update. Resets on success, so a
    /// non-zero value means "right now", not "ever". Per instrument, because a
    /// single symbol with a broken feed is the realistic failure and a global
    /// counter would show a slow climb and name nothing.
    std::uint16_t consecutive_rejects;
    std::uint16_t flags;
};

static_assert(std::is_trivially_copyable_v<BookState>);

/// The single question a strategy should ask before acting on a book.
///
/// Both sides must have liquidity, the book must not be crossed, and its
/// exchange timestamp must not be marked stale. An empty-sided book is not
/// tradable even though it is not crossed — there is simply nothing there to
/// trade against.
[[nodiscard]] constexpr bool has_valid_depth_counts(const BookState& b) noexcept {
    return b.bid_levels <= kDepthLevels && b.ask_levels <= kDepthLevels;
}

[[nodiscard]] constexpr bool is_tradable(const BookState& b) noexcept {
    return has_valid_depth_counts(b) && b.bid_levels > 0
        && b.ask_levels > 0 && !b.crossed
        && !has_flag(b.flags, TickFlag::Stale);
}

/// UNIT: paise. Empty unless both sides have a level. Mirrors the DepthUpdate
/// accessors in feed/tick.hpp so a consumer reads a book the same way whether
/// it holds an update or stored state.
[[nodiscard]] constexpr const DepthLevel* best_bid(const BookState& b) noexcept {
    return has_valid_depth_counts(b) && b.bid_levels > 0 ? &b.bid[0] : nullptr;
}

[[nodiscard]] constexpr const DepthLevel* best_ask(const BookState& b) noexcept {
    return has_valid_depth_counts(b) && b.ask_levels > 0 ? &b.ask[0] : nullptr;
}

[[nodiscard]] constexpr std::optional<Price> mid(const BookState& b) noexcept {
    const DepthLevel* lo = best_bid(b);
    const DepthLevel* hi = best_ask(b);
    if (lo == nullptr || hi == nullptr) {
        return std::nullopt;
    }
    // bid + (ask - bid) / 2, never (bid + ask) / 2 — see feed/tick.hpp.
    return Price{lo->px.raw() + (hi->px.raw() - lo->px.raw()) / 2};
}

[[nodiscard]] constexpr std::optional<Price> spread(const BookState& b) noexcept {
    const DepthLevel* lo = best_bid(b);
    const DepthLevel* hi = best_ask(b);
    if (lo == nullptr || hi == nullptr) {
        return std::nullopt;
    }
    return Price{hi->px.raw() - lo->px.raw()};
}

/// Addressable instruments. `InstrumentId` is dense from 0 (P1-01 assigns it
/// that way), so it indexes the array directly — no hash, no probe.
inline constexpr std::size_t kMaxBookInstruments = kMaxInstruments;

// ─────────────────────────────────────────────────────────────────────────
// L2Book
//
// 2.13 MB measured (BookState is 272 bytes x 8192, plus the seen bitmap).
// Not a stack object; hold it as a member, like SpecStore.
// ─────────────────────────────────────────────────────────────────────────
class L2Book {
public:
    struct Stats {
        std::uint64_t applied = 0;
        std::uint64_t crossed = 0;          // stored, but marked
        std::uint64_t stale_sequence = 0;   // rejected
        std::uint64_t bad_instrument = 0;   // rejected
        std::uint64_t malformed_depth = 0;  // a level count exceeds fixed capacity
        std::uint64_t instruments = 0;      // distinct ids seen
        std::uint64_t stale_tick = 0;       // rejected: TickFlag::Stale
    };

    L2Book() noexcept { clear(); }
    L2Book(const L2Book&) = delete;
    L2Book& operator=(const L2Book&) = delete;

    /// Apply one depth snapshot. UNIT: none. ALTAIR_HOT.
    ///
    /// Replaces rather than merges: five slots always, with the ones past
    /// `*_levels` zeroed, so old levels cannot show through beneath a shorter
    /// new book and invent liquidity that is not there.
    [[nodiscard]] ALTAIR_HOT std::expected<void, BookError>
    apply(const DepthUpdate& d) noexcept {
        const std::uint32_t i = static_cast<std::uint32_t>(d.id);
        if (i >= kMaxBookInstruments) {
            ++stats_.bad_instrument;
            return std::unexpected(BookError::BadInstrument);
        }

        BookState& b = books_[i];

        // TickFlag::Stale is a normaliser diagnostic, but storing its marked
        // price as the current book would still expose stale market state to
        // downstream consumers. Refuse it before changing any book field.
        if (has_flag(d.flags, TickFlag::Stale)) {
            ++stats_.stale_tick;
            if (seen_[i] && b.consecutive_rejects < 0xFFFFu) {
                ++b.consecutive_rejects;
            }
            return std::unexpected(BookError::StaleTick);
        }

        // The wire-facing count is an untrusted uint8_t, not proof that the
        // corresponding fixed array contains that many elements. Refuse the
        // entire snapshot before it can become a BookState consumed by
        // analytics. As with a stale sequence, record the rejection against
        // an already-observed instrument without mutating its market state.
        if (d.bid_levels > kDepthLevels || d.ask_levels > kDepthLevels) {
            ++stats_.malformed_depth;
            if (seen_[i] && b.consecutive_rejects < 0xFFFFu) {
                ++b.consecutive_rejects;
            }
            return std::unexpected(BookError::BadDepthCount);
        }

        // D3: an out-of-order update carries no information — it is a strictly
        // worse view of a moment already superseded, and applying it would move
        // the book backwards. `seq` comes from P2-04 and is monotonic across a
        // failover by construction, so this test holds through a primary
        // switch.
        if (seen_[i] && d.seq <= b.seq) {
            ++stats_.stale_sequence;
            if (b.consecutive_rejects < 0xFFFFu) {
                ++b.consecutive_rejects;
            }
            return std::unexpected(BookError::StaleSequence);
        }

        b.id = d.id;
        b.seq = d.seq;
        b.exchange_ts = d.exchange_ts;
        b.recv_ts = d.recv_ts;
        b.flags = d.flags;
        b.bid_levels = d.bid_levels;
        b.ask_levels = d.ask_levels;
        b.consecutive_rejects = 0;              // D4: resets on acceptance

        for (std::size_t k = 0; k < kDepthLevels; ++k) {
            b.bid[k] = (k < d.bid_levels) ? d.bid[k] : DepthLevel{};
            b.ask[k] = (k < d.ask_levels) ? d.ask[k] : DepthLevel{};
        }

        // D2: stored and marked, not rejected. Locked counts — a zero spread
        // is not a free trade, it is a book that cannot be traded.
        b.crossed = (d.bid_levels > 0 && d.ask_levels > 0
                     && d.bid[0].px.raw() >= d.ask[0].px.raw());
        if (b.crossed) {
            ++stats_.crossed;
        }

        if (!seen_[i]) {
            seen_[i] = true;
            ++stats_.instruments;
        }
        ++stats_.applied;
        return {};
    }

    /// The current book. UNIT: none.
    ///
    /// `NotFound` when this instrument has never been updated — which is NOT
    /// the same as an empty book, and a caller must be able to tell. An
    /// instrument updated with zero levels on both sides returns a state: that
    /// is a real observation of an empty book, and distinct from no
    /// observation at all.
    [[nodiscard]] ALTAIR_HOT std::expected<const BookState*, BookError>
    at(InstrumentId id) const noexcept {
        const std::uint32_t i = static_cast<std::uint32_t>(id);
        if (i >= kMaxBookInstruments) {
            return std::unexpected(BookError::BadInstrument);
        }
        if (!seen_[i]) {
            return std::unexpected(BookError::NotFound);
        }
        return &books_[i];
    }

    [[nodiscard]] bool has(InstrumentId id) const noexcept {
        const std::uint32_t i = static_cast<std::uint32_t>(id);
        return i < kMaxBookInstruments && seen_[i];
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return static_cast<std::size_t>(stats_.instruments);
    }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    void clear() noexcept {
        stats_ = Stats{};
        for (std::size_t i = 0; i < kMaxBookInstruments; ++i) {
            seen_[i] = false;
            books_[i] = BookState{};
        }
    }

private:
    BookState books_[kMaxBookInstruments]{};
    bool      seen_[kMaxBookInstruments]{};
    Stats     stats_{};
};

} // namespace altair
