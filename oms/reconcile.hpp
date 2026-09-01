// oms/reconcile.hpp -- what the broker thinks, versus what Altair thinks.
//
// P4-08. Run at startup and periodically through the session.
//
// THE TWO DISAGREEMENTS ARE NOT SYMMETRIC.
//
//   ORPHAN  the broker has a position Altair does not know about.
//   GHOST   Altair believes in a position the broker does not have.
//
// An orphan is worse, and by a lot. Nothing in Altair is managing it: it has
// no stop, no square-off clock, no place in the greek aggregate, and no size
// in any limit check. It is a live position with no owner, and it got there
// either because Altair restarted after placing an order, or because a fill
// arrived while the process was down. It sits there accruing P&L until a human
// notices.
//
// A ghost is a position Altair will try to manage and cannot -- every exit
// order it sends will be rejected. Bad, loud, and self-announcing.
//
// So an orphan TRIPS THE KILL SWITCH and a ghost raises a flag. Both stop new
// orders; only one means something is on the book with nobody watching it.
//
// QUANTITY IS COMPARED EXACTLY. Integer units, no tolerance. Two systems
// counting the same shares must agree to the share, and a tolerance here would
// be a tolerance on how wrong the position is allowed to be.
//
// AVERAGE PRICE IS NOT. Brokers compute it with their own rounding and their
// own treatment of charges, so a paise or two of difference is normal and
// blocking on it would block every session. A LARGE price difference is a
// different matter -- it means the two systems disagree about which fills
// happened -- so it is reported with a threshold rather than ignored.
//
// RULE 9 THROUGHOUT: a discrepancy blocks the affected symbol and raises a
// flag. It never picks whichever number looks more plausible.

#pragma once

#include <core/types/units.hpp>
#include <risk/limits.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

/// One side's view of a position.
struct PositionView {
    /// Stable identity. Both sides must key on the same thing -- a broker
    /// token and an internal id are different number spaces and comparing
    /// them produces an all-orphan reconciliation.
    std::uint64_t key = 0;
    /// SIGNED net position, units. Zero means flat, which is NOT the same as
    /// absent: a flat line the broker reports and Altair does not is a
    /// bookkeeping difference, not an orphan.
    Qty qty{0};
    /// Broker's or Altair's average price. UNIT: paise.
    Price avg_price{0};
    /// True when this side actually carries a line for this key.
    bool present = false;
};

enum class Discrepancy : std::uint8_t {
    None = 0,
    /// The broker has it, Altair does not. TRIPS THE KILL SWITCH.
    Orphan,
    /// Altair has it, the broker does not.
    Ghost,
    /// Both have it, quantities differ. Fatal -- see the header.
    QuantityMismatch,
    /// Both agree on quantity but the average prices differ by more than the
    /// tolerance. Reported, not fatal.
    PriceDivergence
};

struct ReconLine {
    std::uint64_t key = 0;
    Discrepancy what = Discrepancy::None;
    Qty broker_qty{0};
    Qty local_qty{0};
    /// broker minus local, in paise. Signed.
    std::int64_t price_gap = 0;
};

inline constexpr int kMaxReconLines = 256;

struct ReconResult {
    int matched = 0;
    int orphans = 0;
    int ghosts = 0;
    int qty_mismatches = 0;
    int price_divergences = 0;
    /// The discrepancies, in the order found. Truncated at kMaxReconLines,
    /// and `truncated` says so rather than silently dropping the rest.
    ReconLine lines[kMaxReconLines] = {};
    int line_count = 0;
    bool truncated = false;

    /// True when nothing needs a human.
    [[nodiscard]] bool clean() const noexcept {
        return orphans == 0 && ghosts == 0 && qty_mismatches == 0;
    }
    /// True when something is on the book that Altair is not managing.
    [[nodiscard]] bool has_unmanaged() const noexcept { return orphans > 0; }
};

enum class ReconError : std::uint8_t {
    /// One of the inputs was not sorted by key, so the merge cannot run.
    Unsorted,
    /// A duplicate key on one side. Two lines for one instrument is a
    /// bookkeeping bug that would otherwise silently double a position.
    DuplicateKey
};

/// Default price tolerance: 5 paise, one NIFTY option tick.
///
/// Not zero, because brokers round average prices their own way. Not large,
/// because a real disagreement about which fills happened shows up as rupees.
inline constexpr std::int64_t kReconPriceTolerancePaise = 5;

/// Compare two sorted views.
///
/// Both arrays must be sorted ascending by `key` and free of duplicates. That
/// is checked rather than assumed: an unsorted input silently produces a
/// reconciliation full of phantom orphans, which is the most alarming possible
/// output and the least true.
[[nodiscard]] inline std::expected<ReconResult, ReconError>
reconcile(const PositionView* broker, std::size_t nb,
          const PositionView* local, std::size_t nl,
          std::int64_t price_tolerance = kReconPriceTolerancePaise) noexcept {
    for (std::size_t i = 1; i < nb; ++i) {
        if (broker[i].key < broker[i - 1].key) {
            return std::unexpected(ReconError::Unsorted);
        }
        if (broker[i].key == broker[i - 1].key) {
            return std::unexpected(ReconError::DuplicateKey);
        }
    }
    for (std::size_t i = 1; i < nl; ++i) {
        if (local[i].key < local[i - 1].key) {
            return std::unexpected(ReconError::Unsorted);
        }
        if (local[i].key == local[i - 1].key) {
            return std::unexpected(ReconError::DuplicateKey);
        }
    }

    ReconResult r{};
    auto add = [&](std::uint64_t key, Discrepancy what, Qty bq, Qty lq,
                   std::int64_t gap) {
        if (r.line_count >= kMaxReconLines) { r.truncated = true; return; }
        r.lines[r.line_count++] = ReconLine{key, what, bq, lq, gap};
    };

    std::size_t i = 0, j = 0;
    while (i < nb || j < nl) {
        const bool have_b = i < nb;
        const bool have_l = j < nl;

        if (have_b && (!have_l || broker[i].key < local[j].key)) {
            // Broker only. A flat line is a bookkeeping difference, not an
            // orphan -- there is nothing on the book to manage.
            if (broker[i].qty.raw() != 0) {
                ++r.orphans;
                add(broker[i].key, Discrepancy::Orphan, broker[i].qty, Qty{0},
                    0);
            }
            ++i;
            continue;
        }
        if (have_l && (!have_b || local[j].key < broker[i].key)) {
            if (local[j].qty.raw() != 0) {
                ++r.ghosts;
                add(local[j].key, Discrepancy::Ghost, Qty{0}, local[j].qty, 0);
            }
            ++j;
            continue;
        }

        // Both sides carry this key.
        const std::int64_t bq = broker[i].qty.raw();
        const std::int64_t lq = local[j].qty.raw();
        if (bq != lq) {
            ++r.qty_mismatches;
            add(broker[i].key, Discrepancy::QuantityMismatch, broker[i].qty,
                local[j].qty, 0);
        } else {
            // Only compare prices where the quantity agrees: a price gap on a
            // line whose quantity is already wrong is a consequence, not a
            // second finding, and reporting both would double the noise.
            const std::int64_t gap = broker[i].avg_price.raw()
                                   - local[j].avg_price.raw();
            const std::int64_t mag = gap < 0 ? -gap : gap;
            if (bq != 0 && mag > price_tolerance) {
                ++r.price_divergences;
                add(broker[i].key, Discrepancy::PriceDivergence,
                    broker[i].qty, local[j].qty, gap);
            } else {
                ++r.matched;
            }
        }
        ++i;
        ++j;
    }
    return r;
}

/// Apply a reconciliation result to the kill switch.
///
/// An ORPHAN trips it. Nothing else does automatically -- a ghost and a
/// quantity mismatch are loud and block the affected symbol, but they do not
/// mean something is trading unwatched. Tripping on everything would make the
/// switch routine, and a switch that trips routinely gets reset routinely.
///
/// Returns the number of orphans, so a caller logs the count rather than
/// re-deriving it.
[[nodiscard]] inline int
enforce_reconciliation(const ReconResult& r, KillSwitch& kill,
                       Timestamp now) noexcept {
    if (r.orphans > 0) {
        kill.trip(KillReason::ReconciliationMismatch, now);
    }
    return r.orphans;
}

/// A one-line severity for logging and alerting.
[[nodiscard]] constexpr const char* describe(Discrepancy d) noexcept {
    switch (d) {
    case Discrepancy::None:             return "matched";
    case Discrepancy::Orphan:
        return "ORPHAN: broker holds a position Altair is not managing";
    case Discrepancy::Ghost:
        return "GHOST: Altair believes in a position the broker does not have";
    case Discrepancy::QuantityMismatch:
        return "QUANTITY MISMATCH: the two systems disagree on size";
    case Discrepancy::PriceDivergence:
        return "price divergence beyond tolerance";
    }
    return "unknown";
}

} // namespace altair
