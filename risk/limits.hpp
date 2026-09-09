// risk/limits.hpp -- pre-trade checks, and the kill switch.
//
// P4-02. The last thing between a signal and the exchange.
//
// EVERY CHECK RUNS. THE ORDER IS NOT REJECTED ON THE FIRST ONE.
//
// A fail-fast checker tells you an order was too big. Running all of them
// tells you it was too big AND outside the band AND off the tick, which is a
// different diagnosis: one violation is a sizing bug, three is a broken
// upstream. The result is a bitmask, so the cost of completeness is a few
// branches on a path that runs per ORDER, not per tick.
//
// THE KILL SWITCH IS STICKY, AND THAT IS THE ENTIRE DESIGN.
//
// Once tripped it refuses everything until someone explicitly resets it. It
// does not clear when the condition goes away, it does not clear at the next
// tick, and it does not clear on a new session. A kill switch that
// re-arms itself is a warning light, and CLAUDE.md rule 9 asks for the other
// thing: failing loud beats trading wrong.
//
// It is also checked FIRST and short-circuits: while tripped, no other check
// even runs, because there is no order it would approve.
//
// CONSERVATION IS CHECKED IN INTEGER PAISE, WITH NO TOLERANCE.
//
//     sum(fills) + sum(costs) + cash_delta == 0
//
// Exactly zero. Not "within a paisa", not "within epsilon". This is the
// invariant CLAUDE.md's physics section asks for, and it is checkable exactly
// only because rule 3 keeps the ledger in integers. The test does the same
// arithmetic in double and shows a real breach vanishing into rounding -- at
// F&O notional scale a double cannot even represent the residual, so a
// floating-point conservation check is not a weaker version of this one, it is
// a different check that answers yes when the answer is no.
//
// SPECS COME FROM THE STORE AND MISSING IS NOT ZERO (rule 1, rule 9).
// A zero tick size or a zero lot size means the spec store was read before it
// was loaded, and that blocks the order rather than skipping the check. A
// price band of zero, on the other hand, genuinely means "the exchange
// publishes none" -- so those two zeros mean opposite things and are handled
// as such, which is exactly the phantom-default trap.

#pragma once

#include <core/types/units.hpp>
#include <core/time/timestamp.hpp>

#include <cmath>
#include <cstdint>
#include <expected>

namespace altair {

/// Why an order was refused. A BITMASK: an order can fail several ways at
/// once, and which combination it is carries information.
enum class Violation : std::uint32_t {
    None                 = 0u,
    /// The kill switch is tripped. When set, no other bit is meaningful --
    /// nothing else was evaluated.
    KillSwitchTripped    = 1u << 0,
    /// Order quantity is not a whole number of lots.
    NotWholeLots         = 1u << 1,
    /// Order quantity exceeds the exchange freeze quantity for one order.
    FreezeQuantity       = 1u << 2,
    /// Order quantity exceeds the configured per-order cap.
    MaxOrderQty          = 1u << 3,
    /// Resulting position would exceed the per-instrument quantity cap.
    MaxPositionQty       = 1u << 4,
    /// Resulting position would exceed the per-instrument notional cap.
    MaxPositionNotional  = 1u << 5,
    /// Resulting gross book would exceed the portfolio notional cap.
    MaxGrossNotional     = 1u << 6,
    /// Limit price is outside the exchange daily price band.
    OutsidePriceBand     = 1u << 7,
    /// Limit price is not a multiple of the tick size.
    NotOnTick            = 1u << 8,
    /// Realised plus unrealised loss today is at or past the daily stop.
    DailyLossLimit       = 1u << 9,
    /// Too many orders already live.
    MaxOpenOrders        = 1u << 10,
    /// The quote this order is priced against is older than the staleness
    /// budget. Trading on a stale book is trading on a guess.
    StaleQuote           = 1u << 11,
    /// The contract spec was missing or unloaded -- a zero lot or tick size.
    /// Blocks rather than defaults (rule 9).
    SpecUnavailable      = 1u << 12,
    /// Non-positive price or quantity on the order itself.
    MalformedOrder       = 1u << 13
};

[[nodiscard]] constexpr Violation operator|(Violation a, Violation b) noexcept {
    return static_cast<Violation>(static_cast<std::uint32_t>(a)
                                | static_cast<std::uint32_t>(b));
}
constexpr Violation& operator|=(Violation& a, Violation b) noexcept {
    a = a | b;
    return a;
}
[[nodiscard]] constexpr bool has(Violation set, Violation bit) noexcept {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(bit))
           != 0u;
}
[[nodiscard]] constexpr bool clean(Violation set) noexcept {
    return set == Violation::None;
}
/// How many distinct checks the order failed.
[[nodiscard]] constexpr int violation_count(Violation set) noexcept {
    std::uint32_t v = static_cast<std::uint32_t>(set);
    int n = 0;
    while (v != 0u) { n += static_cast<int>(v & 1u); v >>= 1; }
    return n;
}

/// Why the kill switch tripped. Ordinal 0 is NotTripped, so a zeroed struct
/// does not read as a real trip reason.
enum class KillReason : std::uint8_t {
    NotTripped = 0,
    /// sum(fills) + sum(costs) + cash_delta was not exactly zero.
    ConservationBreach,
    /// The daily loss limit was breached.
    DailyLoss,
    /// The feed stopped or went unhealthy.
    FeedLoss,
    /// Broker reconciliation found a position Altair does not know about.
    ReconciliationMismatch,
    /// A human pulled it.
    Manual
};

/// The kill switch. Sticky by construction.
class KillSwitch {
public:
    /// Trip it. Idempotent, and the FIRST reason is the one kept -- later
    /// trips are consequences of the first and overwriting would lose the
    /// cause in favour of a symptom.
    void trip(KillReason why, Timestamp when) noexcept {
        ++trips_;
        if (reason_ != KillReason::NotTripped) { return; }
        reason_ = why;
        tripped_at_ = when;
    }

    [[nodiscard]] bool tripped() const noexcept {
        return reason_ != KillReason::NotTripped;
    }
    [[nodiscard]] KillReason reason() const noexcept { return reason_; }
    [[nodiscard]] Timestamp tripped_at() const noexcept { return tripped_at_; }
    /// How many times a trip was attempted, including while already tripped.
    [[nodiscard]] std::uint64_t trips() const noexcept { return trips_; }

    /// Clear it. Takes an acknowledgement token that a caller has to write
    /// out, because `reset()` is too easy to reach for in a retry loop. There
    /// is no timeout, no auto-clear and no clear-on-session-start.
    void reset(std::uint64_t acknowledgement, Timestamp when) noexcept {
        if (acknowledgement != kResetAcknowledgement) { return; }
        reason_ = KillReason::NotTripped;
        tripped_at_ = when;
        ++resets_;
    }
    [[nodiscard]] std::uint64_t resets() const noexcept { return resets_; }

    /// The token `reset` requires. Deliberately not 1, not 0, and not
    /// derivable from anything the program already holds.
    static constexpr std::uint64_t kResetAcknowledgement = 0xA17A12'DEAD'BEEFull;

private:
    KillReason reason_ = KillReason::NotTripped;
    Timestamp tripped_at_{};
    std::uint64_t trips_ = 0;
    std::uint64_t resets_ = 0;
};

/// The conservation identity, in exact integer paise.
///
/// `fills` is the signed cash effect of every fill (negative for a buy,
/// positive for a sell), `costs` every charge (always negative), and
/// `cash_delta` the change the ledger recorded. They must sum to EXACTLY zero.
///
/// Returns the residual. Zero is the only acceptable answer; the caller trips
/// the switch on anything else. Returning the number rather than a bool is
/// deliberate -- the size and sign of a breach is the first thing anyone will
/// want, and recomputing it after the fact is how it gets lost.
[[nodiscard]] ALTAIR_HOT constexpr std::int64_t
conservation_residual(Notional fills, Notional costs,
                      Notional cash_delta) noexcept {
    return fills.raw() + costs.raw() + cash_delta.raw();
}

/// Per-instrument and portfolio caps. All from config, none defaulted to
/// something permissive.
struct RiskLimits {
    /// Per-order quantity cap, in units. Zero means no configured cap (the
    /// exchange freeze quantity still applies).
    Qty max_order_qty{0};
    /// Per-instrument absolute position cap, in units.
    Qty max_position_qty{0};
    /// Per-instrument absolute position cap, in paise of notional.
    Notional max_position_notional{0};
    /// Portfolio gross notional cap, in paise.
    Notional max_gross_notional{0};
    /// Daily loss limit as a POSITIVE number of paise. Breaching it is a
    /// kill-switch condition, not merely a rejected order.
    Notional max_daily_loss{0};
    /// Maximum simultaneously live orders.
    std::int32_t max_open_orders = 0;
    /// How old the quote backing an order may be.
    Duration max_quote_age{0};
};

/// What the engine currently holds and knows.
struct AccountState {
    /// Signed current position in this instrument, units.
    Qty position{0};
    /// Gross notional across the whole book, paise.
    Notional gross_notional{0};
    /// Realised plus unrealised P&L today, signed. Negative is a loss.
    Notional day_pnl{0};
    std::int32_t open_orders = 0;
};

/// The order being proposed, plus the spec fields it must respect.
struct ProposedOrder {
    /// Signed quantity: positive to buy, negative to sell. UNIT: units.
    Qty qty{0};
    /// Limit price. UNIT: paise.
    Price limit_price{0};
    /// Timestamp of the quote this was priced against. Compared against
    /// `now` -- both come off the tick, never a wall clock (rule 7).
    Timestamp quote_ts{};

    // ── from the spec store, never literals (rule 1) ──
    LotSize lot_size{0};
    Price tick_size{0};
    /// Exchange freeze quantity. Zero means none published.
    Qty freeze_qty{0};
    /// Daily price band. Both zero means the exchange publishes none.
    Price band_lower{0};
    Price band_upper{0};
};

/// Run every pre-trade check. Returns the full set of violations.
///
/// `now` comes off the tick (rule 7). Nothing here reads a clock, so a replay
/// produces the same decisions as the live path (rule 6).
[[nodiscard]] ALTAIR_HOT inline Violation
check_order(const ProposedOrder& o, const AccountState& acct,
            const RiskLimits& lim, const KillSwitch& kill,
            Timestamp now) noexcept {
    // Checked first and short-circuits. There is no order it would approve,
    // so evaluating the rest would only produce misleading detail.
    if (kill.tripped()) { return Violation::KillSwitchTripped; }

    Violation v = Violation::None;

    // A zero lot or tick size is the spec store answering before it was
    // loaded. That BLOCKS -- it is not a licence to skip the check.
    if (o.lot_size.raw() <= 0 || o.tick_size.raw() <= 0) {
        v |= Violation::SpecUnavailable;
    }
    if (o.qty.raw() == 0 || o.limit_price.raw() <= 0) {
        v |= Violation::MalformedOrder;
    }

    const std::int64_t abs_qty = o.qty.raw() < 0 ? -o.qty.raw() : o.qty.raw();

    if (o.lot_size.raw() > 0 && abs_qty % o.lot_size.raw() != 0) {
        v |= Violation::NotWholeLots;
    }
    if (o.tick_size.raw() > 0 && o.limit_price.raw() > 0
        && o.limit_price.raw() % o.tick_size.raw() != 0) {
        v |= Violation::NotOnTick;
    }
    if (o.freeze_qty.raw() > 0 && abs_qty > o.freeze_qty.raw()) {
        v |= Violation::FreezeQuantity;
    }
    if (lim.max_order_qty.raw() > 0 && abs_qty > lim.max_order_qty.raw()) {
        v |= Violation::MaxOrderQty;
    }

    // A price band of zero means the exchange publishes none -- the OPPOSITE
    // of what a zero lot size means. Two zeros, two meanings, and conflating
    // them is the phantom-default defect.
    if (o.band_lower.raw() > 0 && o.limit_price.raw() < o.band_lower.raw()) {
        v |= Violation::OutsidePriceBand;
    }
    if (o.band_upper.raw() > 0 && o.limit_price.raw() > o.band_upper.raw()) {
        v |= Violation::OutsidePriceBand;
    }

    // Resulting position, not current. A cap checked against the position you
    // already have approves the order that breaches it.
    const std::int64_t after = acct.position.raw() + o.qty.raw();
    const std::int64_t abs_after = after < 0 ? -after : after;
    if (lim.max_position_qty.raw() > 0
        && abs_after > lim.max_position_qty.raw()) {
        v |= Violation::MaxPositionQty;
    }
    if (lim.max_position_notional.raw() > 0 && o.limit_price.raw() > 0) {
        const auto n = notional_of(o.limit_price, Qty{abs_after});
        if (!n || n->raw() > lim.max_position_notional.raw()) {
            v |= Violation::MaxPositionNotional;
        }
    }
    if (lim.max_gross_notional.raw() > 0 && o.limit_price.raw() > 0) {
        const auto add = notional_of(o.limit_price, Qty{abs_qty});
        if (!add
            || acct.gross_notional.raw() > lim.max_gross_notional.raw() - add->raw()) {
            v |= Violation::MaxGrossNotional;
        }
    }

    // day_pnl is signed; the limit is a positive magnitude.
    if (lim.max_daily_loss.raw() > 0
        && acct.day_pnl.raw() <= -lim.max_daily_loss.raw()) {
        v |= Violation::DailyLossLimit;
    }
    if (lim.max_open_orders > 0 && acct.open_orders >= lim.max_open_orders) {
        v |= Violation::MaxOpenOrders;
    }
    if (lim.max_quote_age.raw() > 0) {
        const std::int64_t age = (now - o.quote_ts).raw();
        // A quote from the FUTURE is as broken as one too old, and silently
        // accepting it would be look-ahead (rule 7).
        if (age < 0 || age > lim.max_quote_age.raw()) {
            v |= Violation::StaleQuote;
        }
    }
    return v;
}

/// Fold one tick's ledger movement through the conservation check, tripping
/// the switch on any non-zero residual.
///
/// Returns the residual so a caller can log it. The switch is tripped as a
/// side effect because a breach that is merely returned is a breach someone
/// can forget to act on.
[[nodiscard]] ALTAIR_HOT inline std::int64_t
enforce_conservation(KillSwitch& kill, Notional fills, Notional costs,
                     Notional cash_delta, Timestamp now) noexcept {
    const std::int64_t r = conservation_residual(fills, costs, cash_delta);
    if (r != 0) { kill.trip(KillReason::ConservationBreach, now); }
    return r;
}

} // namespace altair
