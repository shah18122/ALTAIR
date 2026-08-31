// book/l2_book.hpp — the L2 order book.
//
// P2-08. ***SKELETON. `apply` IS NOT IMPLEMENTED.***
//
// The structure is decided; the update path is not written. `apply` returns
// `NotImplemented` so a caller that wires this up finds out immediately rather
// than trading against an empty book that reports a bid of zero.
//
// Two things about the shape are already settled and should not drift:
//
//   * Kite's full mode delivers a five-level SNAPSHOT, not deltas, so applying
//     an update is a replace rather than an incremental edit. XTS 1502 is also
//     a snapshot. That makes O(1) update trivial for this depth and is why
//     kDepthLevels is fixed — a price-keyed map would be slower and would
//     allocate.
//   * A crossed or locked book is REJECTED, not stored. Rule 9: a book where
//     the bid is at or above the ask is a decoder bug or a genuinely broken
//     feed, and either way it is not a trading opportunity. The predecessor's
//     class of "free money" signal comes from exactly this state being taken
//     at face value.

#pragma once

#include <feed/tick.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

enum class BookError : std::uint8_t {
    NotFound,        // no book for that instrument
    Full,            // more instruments than kMaxBookInstruments
    Crossed,         // bid >= ask; refused, not stored
    StaleSequence,   // an update older than the one already applied
    NotImplemented   // P2-08 is a skeleton; `apply` does not work yet
};

/// One instrument's book. Fixed depth, no allocation, trivially copyable so it
/// can ride a seqlock out to the analytics thread.
struct BookState {
    InstrumentId  id;
    std::uint32_t seq;            // the seq of the update that produced this
    Timestamp     exchange_ts;
    DepthLevel    bid[kDepthLevels];
    DepthLevel    ask[kDepthLevels];
    std::uint8_t  bid_levels;
    std::uint8_t  ask_levels;
    std::uint8_t  reserved[6];
};

static_assert(std::is_trivially_copyable_v<BookState>);

inline constexpr std::size_t kMaxBookInstruments = 4096;

class L2Book {
public:
    L2Book() noexcept = default;
    L2Book(const L2Book&) = delete;
    L2Book& operator=(const L2Book&) = delete;

    /// Apply one depth update. UNIT: none.
    ///
    /// NOT IMPLEMENTED. When it is: reject a crossed or locked book with
    /// `Crossed` and leave the previous state intact, reject an out-of-order
    /// `seq` with `StaleSequence`, and otherwise replace the five levels.
    [[nodiscard]] std::expected<void, BookError>
    apply(const DepthUpdate& d) noexcept {
        (void)d;
        return std::unexpected(BookError::NotImplemented);
    }

    /// The current book for an instrument. UNIT: none.
    [[nodiscard]] std::expected<const BookState*, BookError>
    at(InstrumentId id) const noexcept {
        (void)id;
        return std::unexpected(BookError::NotImplemented);
    }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    void clear() noexcept { count_ = 0; }

private:
    std::size_t count_ = 0;
};

} // namespace altair
