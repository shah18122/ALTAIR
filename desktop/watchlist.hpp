// desktop/watchlist.hpp -- adding an instrument by hand.
//
// P11Q-05d.
//
// WATCHING AN INSTRUMENT AND TRADING IT NEED DIFFERENT THINGS.
//
// Smit asked to be able to type an instrument in and watch its price. The
// reflex objection is rule 1 -- no lot size, tick size or expiry as a literal,
// everything from the point-in-time spec store -- and applying it flatly would
// make the feature impossible until the spec store is loaded, which is
// unhelpful and also wrong.
//
// Watching a price needs a token and a name. Nothing else. Lot size decides
// what a POSITION is worth, tick size decides what an ORDER may be priced at,
// and neither is needed to display a number arriving off a feed.
//
// So a row can be added with just an identifier, and it is marked
// `SpecState::Unresolved` -- watchable, and NOT tradeable. Resolution against
// the spec store promotes it. The split is explicit because the alternative is
// one of two bad things: refusing to show a price until a spec exists, or
// quietly letting an unresolved row look exactly like a resolved one and
// discovering the difference at order time.
//
// AN UNRESOLVED ROW IS NEVER SILENTLY PROMOTED.
//
// `resolve()` takes a spec and returns whether it applied. There is no code
// path that fills in a lot size from a default, a guess, or a similar
// instrument. A row whose spec lookup fails stays Unresolved and says why --
// rule 9, at the point where somebody is about to add something they intend to
// trade.
//
// AND A DUPLICATE IS REFUSED BY IDENTITY, NOT BY NAME.
//
// Two rows for the same token would double-count in any total the panel
// computes, and the same instrument can be typed with different spacing or
// case. The check is on the token.

#pragma once

#include <QString>

#include <cstdint>
#include <vector>

namespace altair::ui {

enum class SpecState : std::uint8_t {
    /// Ordinal zero. Watchable; NOT tradeable. No lot, no tick, no expiry.
    Unresolved = 0,
    /// Resolved against the point-in-time spec store.
    Resolved,
    /// The spec store answered, and the answer BLOCKS this instrument --
    /// sources disagreed, or the contract is ambiguous. Rule 9: it is shown,
    /// loudly, rather than dropped.
    Blocked
};

[[nodiscard]] inline QString spec_state_label(SpecState s) {
    switch (s) {
    case SpecState::Resolved:  return QStringLiteral("resolved");
    case SpecState::Blocked:   return QStringLiteral("BLOCKED");
    case SpecState::Unresolved:
    default:                   return QStringLiteral("watch only");
    }
}

struct WatchRow {
    std::uint32_t token = 0;
    QString symbol;
    SpecState spec = SpecState::Unresolved;
    /// Zero while unresolved. Never defaulted to 1, 25, 75 or anything else:
    /// a wrong lot size is the bug that silently scaled every P&L number in
    /// the predecessor.
    std::int64_t lot_size = 0;
    std::int64_t tick_size_paise = 0;
    /// Why it is not resolved, in words, for the row's tooltip.
    QString note;

    [[nodiscard]] bool tradeable() const noexcept {
        return spec == SpecState::Resolved && lot_size > 0
            && tick_size_paise > 0;
    }
};

enum class AddResult : std::uint8_t {
    Added = 0,
    /// Same token already present.
    Duplicate,
    /// No token, or a token of zero.
    BadIdentifier,
    /// Empty name.
    NoSymbol,
    Full
};

[[nodiscard]] inline QString add_result_label(AddResult r) {
    switch (r) {
    case AddResult::Added:         return QStringLiteral("added");
    case AddResult::Duplicate:     return QStringLiteral("already watching that instrument");
    case AddResult::BadIdentifier: return QStringLiteral("a token is required, and it cannot be zero");
    case AddResult::NoSymbol:      return QStringLiteral("a name is required");
    case AddResult::Full:          return QStringLiteral("watchlist is full");
    default:                       return QString();
    }
}

inline constexpr int kMaxWatchRows = 500;

class Watchlist {
public:
    /// Add by identifier. The row is WATCH-ONLY until a spec resolves it.
    AddResult add(std::uint32_t token, const QString& symbol) {
        if (token == 0) {
            return AddResult::BadIdentifier;
        }
        if (symbol.trimmed().isEmpty()) {
            return AddResult::NoSymbol;
        }
        if (rows_.size() >= kMaxWatchRows) {
            return AddResult::Full;
        }
        for (const WatchRow& r : rows_) {
            // By TOKEN. The same instrument typed with different case or
            // spacing is the same instrument, and two rows for it would
            // double-count in every total the panel computes.
            if (r.token == token) {
                return AddResult::Duplicate;
            }
        }
        WatchRow r;
        r.token = token;
        r.symbol = symbol.trimmed();
        r.spec = SpecState::Unresolved;
        r.note = QStringLiteral(
            "no contract spec loaded — price will display, but lot and tick "
            "are unknown so this cannot be sized or ordered");
        rows_.push_back(r);
        return AddResult::Added;
    }

    bool remove(std::uint32_t token) {
        for (auto it = rows_.begin(); it != rows_.end(); ++it) {
            if (it->token == token) {
                rows_.erase(it);
                return true;
            }
        }
        return false;
    }

    /// Apply a spec-store answer.
    ///
    /// Returns false when the lot or tick is not usable, and leaves the row
    /// UNRESOLVED rather than storing a zero that later reads as a number.
    /// There is deliberately no overload that supplies a default.
    bool resolve(std::uint32_t token, std::int64_t lot,
                 std::int64_t tick_paise) {
        for (WatchRow& r : rows_) {
            if (r.token != token) {
                continue;
            }
            if (lot <= 0 || tick_paise <= 0) {
                r.spec = SpecState::Unresolved;
                r.note = QStringLiteral(
                    "the spec store returned a lot or tick of zero — refused "
                    "rather than defaulted (rule 1)");
                return false;
            }
            r.lot_size = lot;
            r.tick_size_paise = tick_paise;
            r.spec = SpecState::Resolved;
            r.note.clear();
            return true;
        }
        return false;
    }

    /// The spec store answered and BLOCKED the instrument. Kept in the list,
    /// visibly blocked -- dropping it would look like an unknown symbol and
    /// get lost.
    bool block(std::uint32_t token, const QString& why) {
        for (WatchRow& r : rows_) {
            if (r.token == token) {
                r.spec = SpecState::Blocked;
                r.lot_size = 0;
                r.tick_size_paise = 0;
                r.note = why;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] const std::vector<WatchRow>& rows() const noexcept {
        return rows_;
    }
    [[nodiscard]] int size() const noexcept {
        return static_cast<int>(rows_.size());
    }

    /// How many rows could actually carry an order. Reported so a watchlist
    /// of forty names does not imply forty tradeable instruments.
    [[nodiscard]] int tradeable_count() const noexcept {
        int n = 0;
        for (const WatchRow& r : rows_) {
            if (r.tradeable()) ++n;
        }
        return n;
    }

private:
    std::vector<WatchRow> rows_;
};

} // namespace altair::ui
