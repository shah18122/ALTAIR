#pragma once

// P0-09a — the conservation ledger.
//
// CLAUDE.md's physics discipline: conservation laws become runtime invariants.
// ROADMAP §3.6, law 1:
//
//     Sum(fills) + Sum(costs) + cash_delta == 0     exactly, in paise
//
// Money is neither created nor destroyed. Every paisa leaving the cash balance
// is either the notional of a fill or a charge, and the three sum to zero at
// every instant. Not "within a tolerance" — EXACTLY, because rule 3 puts every
// money amount in integer paise precisely so this can be checked with `==`.
//
// Law 2 is the accounting identity behind equity:
//
//     Sum(position_i x price_i) + cash == equity
//
// The other two laws in §3.6 (net delta vs hedge, order-book depth
// monotonicity) need greeks (P3) and an L2 book (P2-08). They are NOT stubbed
// here: a check that always passes is worse than an absent one.
//
// A breach means the books are wrong, and a system whose books are wrong stops
// trading — rule 9. So a breach LATCHES. A transient breach that clears itself
// is a breach you never find.

#include <types/units.hpp>

#include <cstdint>
#include <expected>
#include <limits>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// Which conservation law was violated.
// ─────────────────────────────────────────────────────────────────────────
enum class Breach : std::uint8_t {
    CashConservation,   // fills + costs + cash_delta != 0
    Overflow,           // an accumulator would exceed int64 paise
    Latched             // a previous breach has not been cleared
};

namespace detail {

/// True iff a + b overflows int64. Self-contained: core/invariant deliberately
/// does not depend on core/time, so it does not reuse P0-04's copy.
/// UNIT: none. PRECONDITION: none.
[[nodiscard]] constexpr bool ledger_add_overflows(std::int64_t a,
                                                  std::int64_t b) noexcept {
    if (b > 0) {
        return a > std::numeric_limits<std::int64_t>::max() - b;
    }
    if (b < 0) {
        return a < std::numeric_limits<std::int64_t>::min() - b;
    }
    return false;
}

/// True iff a - b overflows int64. Written as its own check rather than
/// `ledger_add_overflows(a, -b)`, because negating INT64_MIN is UB.
/// UNIT: none. PRECONDITION: none.
[[nodiscard]] constexpr bool ledger_sub_overflows(std::int64_t a,
                                                  std::int64_t b) noexcept {
    if (b > 0) {
        return a < std::numeric_limits<std::int64_t>::min() + b;
    }
    if (b < 0) {
        return a > std::numeric_limits<std::int64_t>::max() + b;
    }
    return false;
}

/// UNIT: paise. PRECONDITION: none.
[[nodiscard]] constexpr bool notional_add_overflows(Notional a, Notional b) noexcept {
    return ledger_add_overflows(a.raw(), b.raw());
}

/// UNIT: paise. PRECONDITION: none.
[[nodiscard]] constexpr bool notional_sub_overflows(Notional a, Notional b) noexcept {
    return ledger_sub_overflows(a.raw(), b.raw());
}

/// UNIT: units. PRECONDITION: none.
[[nodiscard]] constexpr bool qty_add_overflows(Qty a, Qty b) noexcept {
    return ledger_add_overflows(a.raw(), b.raw());
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────
// ConservationLedger — the single choke point for money, and the proof that
// nothing went around it.
//
// SINGLE INSTRUMENT. Portfolio aggregation is P4-03; this is the per-symbol
// ledger it will be built from.
//
// SIGN CONVENTION, and it is the thing to get right:
//   qty > 0 is a BUY  — cash goes DOWN by (price x qty) and by the cost
//   qty < 0 is a SELL — cash goes UP   by (price x |qty|), and DOWN by the cost
// Costs are ALWAYS a debit, on both sides. A cost credited on a sell is the
// classic sign error, and it makes every short look profitable.
// ─────────────────────────────────────────────────────────────────────────
class ConservationLedger {
public:
    /// A ledger starting flat with the given cash. UNIT: paise.
    constexpr explicit ConservationLedger(Notional initial_cash) noexcept
        : initial_cash_(initial_cash), cash_(initial_cash) {}

    /// Record a fill. UNIT: qty in units, px in paise per unit, cost in paise.
    /// PRECONDITION: cost >= 0 — a cost is always a debit. A negative cost is
    /// a rebate and must be modelled as a cash adjustment, not a negative cost.
    /// Returns Latched if a breach is outstanding, Overflow if any accumulator
    /// would exceed int64 paise. On any error NOTHING is mutated.
    [[nodiscard]] ALTAIR_HOT std::expected<void, Breach>
    on_fill(Qty qty, Price px, Notional cost) noexcept {
        if (breached_) {
            return std::unexpected(Breach::Latched);
        }
        // A negative cost is a rebate. Accepting one here would keep the
        // identity arithmetically true while making it meaningless.
        if (cost.is_negative()) {
            return std::unexpected(Breach::CashConservation);
        }

        // notional_of, not a raw multiply — the overflow check is the point.
        const auto n = notional_of(px, qty);
        if (!n.has_value()) {
            return std::unexpected(Breach::Overflow);
        }
        const Notional notional = *n;

        // Check EVERY accumulation before applying any of it. A partially
        // applied fill is a guaranteed breach on the next check.
        if (detail::notional_add_overflows(fills_, notional)) {
            return std::unexpected(Breach::Overflow);
        }
        if (detail::notional_add_overflows(costs_, cost)) {
            return std::unexpected(Breach::Overflow);
        }
        if (detail::notional_add_overflows(notional, cost)) {
            return std::unexpected(Breach::Overflow);
        }
        const Notional outflow = notional + cost;
        // Checked as a SUBTRACTION, not as an add of `-outflow`: negating
        // INT64_MIN would be UB before the guard ever ran.
        if (detail::notional_sub_overflows(cash_, outflow)) {
            return std::unexpected(Breach::Overflow);
        }
        if (detail::qty_add_overflows(position_, qty)) {
            return std::unexpected(Breach::Overflow);
        }

        fills_ += notional;
        costs_ += cost;
        position_ += qty;
        // BOTH subtract. For a sell `notional` is negative, so cash RISES by
        // |notional| — and the cost still falls.
        cash_ -= outflow;
        ++fill_count_;
        return {};
    }

    /// Record a cash movement that is NOT a fill: a margin transfer, a
    /// dividend, a rebate. UNIT: paise, signed. Tracked separately so the
    /// conservation identity stays exact.
    /// Returns Latched or Overflow. On any error NOTHING is mutated.
    [[nodiscard]] std::expected<void, Breach>
    on_cash_adjustment(Notional amount) noexcept {
        if (breached_) {
            return std::unexpected(Breach::Latched);
        }
        if (detail::notional_add_overflows(adjustments_, amount)) {
            return std::unexpected(Breach::Overflow);
        }
        if (detail::notional_add_overflows(cash_, amount)) {
            return std::unexpected(Breach::Overflow);
        }
        adjustments_ += amount;
        cash_ += amount;
        return {};
    }

    /// Verify ROADMAP §3.6 law 1, EXACTLY:
    ///     fills + costs + (cash - initial_cash - adjustments) == 0
    /// UNIT: none. Returns CashConservation on a breach, Latched if one is
    /// already outstanding. Does NOT latch a new breach by itself.
    [[nodiscard]] ALTAIR_HOT std::expected<void, Breach> check() const noexcept {
        if (breached_) {
            return std::unexpected(Breach::Latched);
        }
        // Integer paise, so this is `==` and not an epsilon. A tolerance here
        // would hide exactly the class of bug the invariant exists to catch.
        const Notional delta = cash_ - initial_cash_ - adjustments_;
        if (!(fills_ + costs_ + delta).is_zero()) {
            return std::unexpected(Breach::CashConservation);
        }
        return {};
    }

    /// check(), and latch on failure. UNIT: none. This is what a tick loop
    /// calls; check() alone is for a reporting path that must not alter state.
    [[nodiscard]] ALTAIR_HOT std::expected<void, Breach> check_and_trip() noexcept {
        auto r = check();
        if (!r.has_value()) {
            trip(r.error());
        }
        return r;
    }

    /// ROADMAP §3.6 law 2: position x mark + cash.
    /// UNIT: paise. PRECONDITION: none. Returns Overflow if position x mark
    /// does not fit in int64 paise.
    [[nodiscard]] std::expected<Notional, Breach> equity(Price mark) const noexcept {
        const auto mv = notional_of(mark, position_);
        if (!mv.has_value()) {
            return std::unexpected(Breach::Overflow);
        }
        if (detail::notional_add_overflows(*mv, cash_)) {
            return std::unexpected(Breach::Overflow);
        }
        return *mv + cash_;
    }

    /// Realised cash movement since construction, i.e. cash - initial_cash.
    /// UNIT: paise. At a FLAT position this is realised P&L net of all costs.
    [[nodiscard]] constexpr Notional cash_delta() const noexcept {
        return cash_ - initial_cash_;
    }

    // ── State. ──
    [[nodiscard]] constexpr Notional cash() const noexcept { return cash_; }
    [[nodiscard]] constexpr Notional initial_cash() const noexcept { return initial_cash_; }
    [[nodiscard]] constexpr Notional fills_notional() const noexcept { return fills_; }
    [[nodiscard]] constexpr Notional total_costs() const noexcept { return costs_; }
    [[nodiscard]] constexpr Notional adjustments() const noexcept { return adjustments_; }
    [[nodiscard]] constexpr Qty position() const noexcept { return position_; }
    [[nodiscard]] constexpr std::uint64_t fill_count() const noexcept { return fill_count_; }

    /// True iff the position is exactly zero. UNIT: none.
    [[nodiscard]] constexpr bool is_flat() const noexcept { return position_.is_zero(); }

    // ── The kill switch. ──

    /// True iff a breach is latched. UNIT: none. While true, every on_fill and
    /// on_cash_adjustment returns Latched.
    [[nodiscard]] constexpr bool is_breached() const noexcept { return breached_; }

    /// The latched breach, or Breach::Overflow when none — callers must check
    /// is_breached() first. UNIT: none.
    [[nodiscard]] constexpr Breach latched_breach() const noexcept { return latched_; }

    /// Breaches latched since construction. UNIT: count. Never reset by
    /// clear_breach() — the history is the audit trail.
    [[nodiscard]] constexpr std::uint64_t breach_count() const noexcept {
        return breach_count_;
    }

    /// Latch a breach explicitly. UNIT: none. Idempotent in the sense that a
    /// second trip does not overwrite the first breach — the FIRST one is the
    /// diagnosis; a later one is usually its consequence.
    void trip(Breach b) noexcept {
        if (!breached_) {
            latched_ = b;
            breached_ = true;
        }
        ++breach_count_;
    }

    /// Clear the latch. UNIT: none. Deliberately NOT called "reset" — it does
    /// not touch the balances, only the kill switch, and a human decides.
    void clear_breach() noexcept { breached_ = false; }

private:
    Notional initial_cash_{};
    Notional cash_{};
    Notional fills_{};
    Notional costs_{};
    Notional adjustments_{};
    Qty position_{};
    std::uint64_t fill_count_ = 0;
    std::uint64_t breach_count_ = 0;
    Breach latched_ = Breach::Overflow;
    bool breached_ = false;
};

} // namespace altair
