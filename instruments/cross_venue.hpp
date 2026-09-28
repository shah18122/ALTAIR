// instruments/cross_venue.hpp -- the eligible cross-venue universe.
//
// P7-01. Which securities trade on BOTH NSE and BSE, so the cross-venue
// scanner (strategies/basis.hpp) has a defined set of pairs to watch. This
// builds that set; it does not price anything and includes no strategy header.
//
// THE JOIN KEY IS ISIN, NOT THE SYMBOL.
//
// NSE and BSE give the same company different trading symbols (RELIANCE vs
// RELIANCE, but 20MICRONS vs 538607-style codes on BSE), so matching on the
// symbol pairs the wrong scrips. ISIN is exchange-independent and is the only
// safe key. The FYERS symbol master carries it (instruments/fyers_master.hpp);
// so does the NSE/BSE equity master. A leg with no ISIN cannot be paired and
// is rejected, never guessed onto a symbol.
//
// AMBIGUITY IS EXCLUDED VISIBLY, NOT RESOLVED (rule 9).
//
// Three things can go wrong and each is COUNTED with its reason rather than
// silently dropped:
//   OneSided   the ISIN appears on one exchange only -- not cross-listed.
//   Collision  the same ISIN appears twice on the SAME exchange (a series
//              split, a stale row) -- which of the two is "the" leg is a guess,
//              so the pair is refused.
//   Full       the universe is at capacity -- refused, never truncated (r11).
// A caller reads stats() to see exactly how many of each, so "few pairs today"
// can never be mistaken for "the market is aligned".
//
// CASH ONLY. Cross-venue equity arbitrage is a cash-vs-cash trade; futures and
// options are single-venue here. A non-cash leg is rejected as NotCash.

#pragma once

#include "contract_spec.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>

namespace altair::instruments {

inline constexpr std::size_t kMaxCrossVenuePairs = kMaxInstruments;
inline constexpr std::size_t kIsinChars = 12;

enum class CrossVenueReject : std::uint8_t {
    NoIsin,     ///< the leg has no ISIN
    NotCash,    ///< the leg is not a cash-segment equity
    BadExchange,///< exchange is neither NSE nor BSE
    Collision,  ///< a second leg for this ISIN on the same exchange
    Full        ///< the universe is at capacity
};

/// One exchange listing of a security, as fed into the universe builder.
struct CrossVenueLeg {
    char isin[kIsinChars + 1]{};
    Exchange exchange{Exchange::NSE};
    Segment segment{Segment::Cash};
    InstrumentId id{InstrumentId::Invalid};
};

/// A security listed on both venues. `nse` and `bse` are the two canonical ids.
struct CrossVenuePair {
    char isin[kIsinChars + 1]{};
    InstrumentId nse{InstrumentId::Invalid};
    InstrumentId bse{InstrumentId::Invalid};
};

/// Builds and holds the cross-venue universe. Cold path: filled pre-open, read
/// during the session. Not thread-safe for concurrent add().
class CrossVenueUniverse {
public:
    struct Stats {
        std::uint32_t legs_added = 0;
        std::uint32_t rejected_no_isin = 0;
        std::uint32_t rejected_not_cash = 0;
        std::uint32_t rejected_bad_exchange = 0;
        std::uint32_t collisions = 0;
        /// ISINs seen on exactly one exchange. Computed by pairs()/one_sided().
        [[nodiscard]] std::uint32_t rejected() const noexcept {
            return rejected_no_isin + rejected_not_cash
                + rejected_bad_exchange + collisions;
        }
    };

    /// Admit one leg. A well-formed cash leg with an ISIN is recorded against
    /// that ISIN's slot; the second exchange fills the same slot. UNIT: none.
    [[nodiscard]] std::expected<void, CrossVenueReject>
    add(const CrossVenueLeg& leg) noexcept {
        if (leg.isin[0] == '\0') {
            ++stats_.rejected_no_isin;
            return std::unexpected(CrossVenueReject::NoIsin);
        }
        if (leg.segment != Segment::Cash) {
            ++stats_.rejected_not_cash;
            return std::unexpected(CrossVenueReject::NotCash);
        }
        if (leg.exchange != Exchange::NSE && leg.exchange != Exchange::BSE) {
            ++stats_.rejected_bad_exchange;
            return std::unexpected(CrossVenueReject::BadExchange);
        }

        Slot* slot = find(leg.isin);
        if (slot == nullptr) {
            if (count_ >= kMaxCrossVenuePairs) {
                return std::unexpected(CrossVenueReject::Full);
            }
            slot = &slots_[count_++];
            std::memcpy(slot->isin, leg.isin, kIsinChars + 1);
            slot->nse = InstrumentId::Invalid;
            slot->bse = InstrumentId::Invalid;
        }
        InstrumentId& side = (leg.exchange == Exchange::NSE) ? slot->nse
                                                             : slot->bse;
        if (side != InstrumentId::Invalid) {
            ++stats_.collisions;                 // second leg, same exchange
            return std::unexpected(CrossVenueReject::Collision);
        }
        side = leg.id;
        ++stats_.legs_added;
        return {};
    }

    /// Copy every cross-listed pair (both sides present, no collision) into
    /// `out`, up to `cap`. Returns the number written. A pair with a collision
    /// on either side is NOT emitted -- the ambiguity is in stats().collisions.
    [[nodiscard]] std::size_t pairs(CrossVenuePair* out, std::size_t cap) const noexcept {
        std::size_t n = 0;
        for (std::size_t i = 0; i < count_ && n < cap; ++i) {
            const Slot& s = slots_[i];
            if (s.nse != InstrumentId::Invalid && s.bse != InstrumentId::Invalid) {
                std::memcpy(out[n].isin, s.isin, kIsinChars + 1);
                out[n].nse = s.nse;
                out[n].bse = s.bse;
                ++n;
            }
        }
        return n;
    }

    /// Count of securities listed on both venues.
    [[nodiscard]] std::size_t matched() const noexcept {
        std::size_t n = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            if (slots_[i].nse != InstrumentId::Invalid
                && slots_[i].bse != InstrumentId::Invalid) { ++n; }
        }
        return n;
    }

    /// Count of ISINs seen on exactly one exchange -- not cross-listed.
    [[nodiscard]] std::size_t one_sided() const noexcept {
        std::size_t n = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            const bool nse = slots_[i].nse != InstrumentId::Invalid;
            const bool bse = slots_[i].bse != InstrumentId::Invalid;
            if (nse != bse) { ++n; }
        }
        return n;
    }

    [[nodiscard]] std::size_t distinct_isins() const noexcept { return count_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    struct Slot {
        char isin[kIsinChars + 1];
        InstrumentId nse;
        InstrumentId bse;
    };

    [[nodiscard]] Slot* find(const char* isin) noexcept {
        for (std::size_t i = 0; i < count_; ++i) {
            if (std::strcmp(slots_[i].isin, isin) == 0) { return &slots_[i]; }
        }
        return nullptr;
    }

    Slot slots_[kMaxCrossVenuePairs]{};
    std::size_t count_ = 0;
    Stats stats_{};
};

} // namespace altair::instruments
