// core/types/broker_positions.hpp
// Read-side broker position payload, v1. No transport, no order capability.
#pragma once

#include <time/timestamp.hpp>
#include <types/broker_state.hpp>
#include <types/signed_sum.hpp>
#include <types/units.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

namespace altair::broker_view {

inline constexpr std::uint32_t kPositionSchemaVersion = 1;

/// Hard cap per account snapshot. RULE 11: a broker reporting more is REFUSED,
/// never truncated — a short position dropped off the end under-reports risk,
/// and every downstream total would agree with the truncated slice.
inline constexpr std::size_t kMaxPositionsPerAccount = 256;

/// The spec store's InstrumentId value, carried as an opaque key because
/// core/types must not depend on instruments/ (instruments/ depends on types/).
/// The producer sets it from InstrumentId; conversion lives at that boundary.
enum class InstrumentKey : std::uint32_t { None = 0xFFFF'FFFFu };

/// Provider product semantics are NOT unified. Unknown is a real value: it
/// means the provider sent something this build does not model, and a caller
/// must not assume Intraday.
enum class PositionProduct : std::uint8_t {
    Unknown, Intraday, Delivery, Margin, CarryForward
};

enum class PositionError : std::uint8_t {
    TooManyPositions,   ///< count exceeded kMaxPositionsPerAccount
    SchemaMismatch,
    SessionUnbound,     ///< account_session fails valid()
    StaleSnapshot,      ///< observed window not fresh at `now`
    UnknownInstrument,  ///< InstrumentKey::None
    NoMark,             ///< last_mark absent — P&L is unknown, not zero
    Overflow            ///< notional_of overflowed int64 paise
};

/// One position. ARRAY-OF-STRUCTURES, deliberately — see the card's §5.
struct Position {
    InstrumentKey instrument{InstrumentKey::None};
    PositionProduct product{PositionProduct::Unknown};
    /// Signed: negative is short. Units (lots x lot_size), never lots.
    Qty net_qty{};
    /// Volume-weighted average entry, integer paise.
    Price average_price{};
    /// Absent means the provider sent no mark. Absent is NOT zero.
    std::optional<Price> last_mark{};
    /// When this row's mark was observed. Per row: marks age independently.
    EvidenceWindow marked{};
};

/// One broker account's open positions at one instant.
/// NO CROSS-BROKER NETTING: this is per session, and no merge function exists.
struct PositionSnapshot {
    std::uint32_t schema_version{kPositionSchemaVersion};
    SessionKey account_session{};
    EvidenceWindow observed{};
    std::uint16_t count{};
    Position position[kMaxPositionsPerAccount]{};
};

/// Refuse a count that exceeds the cap. Returns the count on success.
///
/// UNIT: dimensionless count of positions; returned as std::uint16_t, which is
/// wide enough for the whole [0, kMaxPositionsPerAccount] range.
/// PRECONDITION: none. `reported` may be any std::size_t, including a hostile
/// or corrupted value.
/// POSTCONDITION: on success the value equals `reported` exactly. The function
/// never truncates, clamps or saturates (rule 11) — a refusal is an error, not
/// a smaller number.
[[nodiscard]] constexpr std::expected<std::uint16_t, PositionError>
checked_count(std::size_t reported) noexcept {
    if (reported > kMaxPositionsPerAccount) {
        return std::unexpected(PositionError::TooManyPositions);
    }
    return static_cast<std::uint16_t>(reported);
}

/// Structural validity of one row. Does not consult the clock.
///
/// PRECONDITION: none; any value the provider could have produced is accepted.
/// A row is valid iff it names a real instrument (not InstrumentKey::None) and
/// carries a non-negative average_price in integer paise. A zero net_qty is
/// valid — a flat row is a legitimate report — and PositionProduct::Unknown is
/// valid, because this build not modelling a product is not an error here.
[[nodiscard]] constexpr bool valid(const Position& p) noexcept {
    return p.instrument != InstrumentKey::None
        && !p.average_price.is_negative();
}

/// Snapshot is this schema, bound to a valid session, fresh at `now`, and
/// every row in [0, count) is valid(). UTC ns; no wall-clock read.
///
/// UNIT: `now` is nanoseconds since the Unix epoch, UTC, and arrives as a
/// parameter — this never reads a clock (final.md §13 rule 7).
/// PRECONDITION: `now` is a plausible UTC instant; any int64 is safe.
/// Rows at index >= count are NOT read, so a caller may leave them defaulted.
/// A count above the cap refuses rather than reading past the array.
[[nodiscard]] constexpr bool usable(const PositionSnapshot& s,
                                    Timestamp now) noexcept {
    if (s.schema_version != kPositionSchemaVersion) return false;
    if (!valid(s.account_session)) return false;
    if (!fresh(s.observed, now)) return false;
    if (static_cast<std::size_t>(s.count) > kMaxPositionsPerAccount) return false;
    for (std::size_t i = 0; i < static_cast<std::size_t>(s.count); ++i) {
        if (!valid(s.position[i])) return false;
    }
    return true;
}

/// Unrealised P&L in paise: (last_mark - average_price) * net_qty.
/// A PURE FUNCTION, never a stored field, so it cannot go stale against the
/// mark it was computed from. Signs: long + rising and short + falling both
/// yield a positive result.
///
/// UNIT: integer paise. Price and Qty are exact integers and the product goes
/// through notional_of, so there is no rounding and no floating point.
/// PRECONDITION: none. A row need not be valid() for its P&L to be computed.
/// ERROR: NoMark when last_mark is absent — zero and average_price are never
/// substituted, because unknown P&L and flat P&L are different facts.
/// Overflow when the CHECKED subtraction (mark - average_price) leaves int64
/// paise, and equally when the subsequent product does.
[[nodiscard]] constexpr std::expected<Notional, PositionError>
unrealised(const Position& p) noexcept {
    if (!p.last_mark.has_value()) {
        return std::unexpected(PositionError::NoMark);
    }
    // THE SUBTRACTION IS CHECKED, and it has to be. valid() constrains
    // average_price to be non-negative but places NO constraint on last_mark,
    // so this row is structurally valid and its subtraction still leaves int64:
    //     average_price = 9'223'372'036'854'775'807, last_mark = -2
    // `Price a - Price b` reads `static_cast<Rep>(a.raw() - b.raw())`, and the
    // overflow happens in `a.raw() - b.raw()` BEFORE the cast — undefined
    // behaviour, not a wrapped number, and no check after it can undo that.
    // ExactSignedSum keeps the terms in two separate limbs, so no intermediate
    // can overflow and try_value() reports representability exactly. Rule 11:
    // refused, not UB. (P0-02c1)
    ExactSignedSum delta;
    delta.add(p.last_mark->raw());
    delta.subtract(p.average_price.raw());
    std::int64_t delta_paise = 0;
    if (!delta.try_value(delta_paise)) {
        return std::unexpected(PositionError::Overflow);
    }
    // The checked cross-dimension multiply. The sign of net_qty does the
    // long/short work; nothing is special-cased.
    const auto value = notional_of(Price{delta_paise}, p.net_qty);
    if (!value.has_value()) {
        return std::unexpected(PositionError::Overflow);
    }
    return *value;
}

/// Sum of unrealised() over [0, count). Refuses on the FIRST row that cannot
/// be priced — a total that silently skips unmarked rows is a wrong total.
///
/// UNIT: integer paise, exact.
/// PRECONDITION: none. Rows at index >= count are not read. A count above
/// kMaxPositionsPerAccount is refused before any row is read, because [0,
/// count) would leave the array.
/// ERROR: the first error from unrealised(), so NoMark names the row that could
/// not be priced. Overflow is reported for a row's product and equally for the
/// RUNNING SUM leaving int64 paise — a total is not allowed to wrap.
/// TooManyPositions when count exceeds the cap.
[[nodiscard]] constexpr std::expected<Notional, PositionError>
unrealised_total(const PositionSnapshot& s) noexcept {
    // A count the array cannot hold is refused BEFORE any row is read: with
    // count above the cap this loop would walk off the end of `position`, and
    // reading past the array is undefined behaviour, not a wrong number. The
    // same refusal usable() applies, for the same reason — rule 11 refuses,
    // it never truncates. Precedent for the guard: round_to_tick, C21-001.
    if (static_cast<std::size_t>(s.count) > kMaxPositionsPerAccount) {
        return std::unexpected(PositionError::TooManyPositions);
    }

    // ONE OVERFLOW STRATEGY IN THIS HEADER. The running sum uses the same
    // ExactSignedSum as unrealised() rather than a second set of hand-rolled
    // bounds, so a row's product and the total cannot disagree about what
    // "leaves int64 paise" means. The check runs after EVERY addend, which is
    // the documented behaviour: a total that leaves int64 is refused at the
    // point it leaves, not merely at the end — a sum that wrapped and came
    // back would otherwise be reported as a small, true-looking number, which
    // is exactly the failure mode rule 11 exists to prevent. (P0-02c1)
    ExactSignedSum sum;
    std::int64_t running = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(s.count); ++i) {
        const auto row = unrealised(s.position[i]);
        if (!row.has_value()) {
            return std::unexpected(row.error());
        }
        sum.add(row->raw());
        if (!sum.try_value(running)) {
            return std::unexpected(PositionError::Overflow);
        }
    }
    return Notional{running};
}

/// True iff the row holds no exposure. Used to exclude flat rows from the
/// open-position table without deleting them from the snapshot.
///
/// UNIT: net_qty is signed units (negative is short); zero means exactly flat.
/// PRECONDITION: none — a defaulted row is flat, which is why a caller must
/// check valid() before treating a row as a real instrument.
[[nodiscard]] constexpr bool is_flat(const Position& p) noexcept {
    return p.net_qty == Qty{0};
}

} // namespace altair::broker_view

