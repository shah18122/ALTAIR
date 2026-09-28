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
// An orphan or an exact quantity mismatch TRIPS THE KILL SWITCH; a ghost raises
// a flag. A sign flip is a quantity mismatch too. A quantity tolerance would
// be a tolerance on how wrong the live position may be.
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
#include <limits>

namespace altair {

/// Broker product is part of instrument identity: one token can name separate
/// MIS and NRML positions. Unknown is refused rather than merged by accident.
enum class ReconProduct : std::uint8_t { Unknown = 0, Mis, Nrml, Cnc };

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
    /// Composite identity component. Must be known for reconciliation.
    ReconProduct product = ReconProduct::Unknown;
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
    ReconProduct product = ReconProduct::Unknown;
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
    DuplicateKey,
    /// Product is missing or is not a supported reconciliation product.
    InvalidProduct,
    /// Price tolerance must be nonnegative.
    InvalidTolerance,
    /// A nonzero count requires a non-null input pointer.
    NullInput,
    /// Every row within the supplied count must be marked present.
    InvalidPresence,
    /// Result counters use int and cannot represent a larger input side.
    TooManyRows
};

/// Default price tolerance: 5 paise, one NIFTY option tick.
///
/// Not zero, because brokers round average prices their own way. Not large,
/// because a real disagreement about which fills happened shows up as rupees.
inline constexpr std::int64_t kReconPriceTolerancePaise = 5;

/// Compare two sorted views.
///
/// Both arrays must be sorted ascending by `(product, key)` and free of duplicates. That
/// is checked rather than assumed: an unsorted input silently produces a
/// reconciliation full of phantom orphans, which is the most alarming possible
/// output and the least true.
[[nodiscard]] inline std::expected<ReconResult, ReconError>
reconcile(const PositionView* broker, std::size_t nb,
           const PositionView* local, std::size_t nl,
           std::int64_t price_tolerance = kReconPriceTolerancePaise) noexcept {
    if ((broker == nullptr && nb != 0) || (local == nullptr && nl != 0)) {
        return std::unexpected(ReconError::NullInput);
    }
    constexpr auto kMaxCount =
        static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (nb > kMaxCount || nl > kMaxCount) {
        return std::unexpected(ReconError::TooManyRows);
    }
    if (price_tolerance < 0) {
        return std::unexpected(ReconError::InvalidTolerance);
    }
    const auto valid_product = [](ReconProduct p) noexcept {
        return p == ReconProduct::Mis || p == ReconProduct::Nrml
            || p == ReconProduct::Cnc;
    };
    const auto less_identity = [](const PositionView& a,
                                  const PositionView& b) noexcept {
        if (a.product != b.product) {
            return static_cast<std::uint8_t>(a.product)
                 < static_cast<std::uint8_t>(b.product);
        }
        return a.key < b.key;
    };
    for (std::size_t i = 0; i < nb; ++i) {
        if (!broker[i].present) {
            return std::unexpected(ReconError::InvalidPresence);
        }
        if (!valid_product(broker[i].product)) {
            return std::unexpected(ReconError::InvalidProduct);
        }
    }
    for (std::size_t i = 0; i < nl; ++i) {
        if (!local[i].present) {
            return std::unexpected(ReconError::InvalidPresence);
        }
        if (!valid_product(local[i].product)) {
            return std::unexpected(ReconError::InvalidProduct);
        }
    }
    for (std::size_t i = 1; i < nb; ++i) {
        if (less_identity(broker[i], broker[i - 1])) {
            return std::unexpected(ReconError::Unsorted);
        }
        if (!less_identity(broker[i - 1], broker[i])
            && !less_identity(broker[i], broker[i - 1])) {
            return std::unexpected(ReconError::DuplicateKey);
        }
    }
    for (std::size_t i = 1; i < nl; ++i) {
        if (less_identity(local[i], local[i - 1])) {
            return std::unexpected(ReconError::Unsorted);
        }
        if (!less_identity(local[i - 1], local[i])
            && !less_identity(local[i], local[i - 1])) {
            return std::unexpected(ReconError::DuplicateKey);
        }
    }

    ReconResult r{};
    auto add = [&](ReconProduct product, std::uint64_t key, Discrepancy what,
                   Qty bq, Qty lq, std::int64_t gap) {
        if (r.line_count >= kMaxReconLines) { r.truncated = true; return; }
        r.lines[r.line_count++] = ReconLine{key, what, bq, lq, gap, product};
    };

    std::size_t i = 0, j = 0;
    while (i < nb || j < nl) {
        const bool have_b = i < nb;
        const bool have_l = j < nl;

        if (have_b && (!have_l || less_identity(broker[i], local[j]))) {
            // Broker only. A flat line is a bookkeeping difference, not an
            // orphan -- there is nothing on the book to manage.
            if (broker[i].qty.raw() != 0) {
                ++r.orphans;
                add(broker[i].product, broker[i].key, Discrepancy::Orphan,
                    broker[i].qty, Qty{0}, 0);
            }
            ++i;
            continue;
        }
        if (have_l && (!have_b || less_identity(local[j], broker[i]))) {
            if (local[j].qty.raw() != 0) {
                ++r.ghosts;
                add(local[j].product, local[j].key, Discrepancy::Ghost, Qty{0},
                    local[j].qty, 0);
            }
            ++j;
            continue;
        }

        // Both sides carry this key.
        const std::int64_t bq = broker[i].qty.raw();
        const std::int64_t lq = local[j].qty.raw();
        if (bq != lq) {
            ++r.qty_mismatches;
            add(broker[i].product, broker[i].key, Discrepancy::QuantityMismatch,
                broker[i].qty, local[j].qty, 0);
        } else {
            // Only compare prices where the quantity agrees: a price gap on a
            // line whose quantity is already wrong is a consequence, not a
            // second finding, and reporting both would double the noise.
            const std::int64_t broker_price = broker[i].avg_price.raw();
            const std::int64_t local_price = local[j].avg_price.raw();
            std::uint64_t magnitude = 0;
            std::int64_t gap = 0;
            if (broker_price >= local_price) {
                // Unsigned subtraction represents the full nonnegative
                // difference, including INT64_MAX - INT64_MIN.
                magnitude = static_cast<std::uint64_t>(broker_price)
                          - static_cast<std::uint64_t>(local_price);
                gap = magnitude > static_cast<std::uint64_t>(
                                       std::numeric_limits<std::int64_t>::max())
                    ? std::numeric_limits<std::int64_t>::max()
                    : static_cast<std::int64_t>(magnitude);
            } else {
                magnitude = static_cast<std::uint64_t>(local_price)
                          - static_cast<std::uint64_t>(broker_price);
                constexpr std::uint64_t kInt64MinMagnitude =
                    std::uint64_t{1} << 63;
                gap = magnitude >= kInt64MinMagnitude
                    ? std::numeric_limits<std::int64_t>::min()
                    : -static_cast<std::int64_t>(magnitude);
            }
            if (bq != 0
                && magnitude > static_cast<std::uint64_t>(price_tolerance)) {
                ++r.price_divergences;
                add(broker[i].product, broker[i].key,
                    Discrepancy::PriceDivergence, broker[i].qty,
                    local[j].qty, gap);
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
/// An ORPHAN or any exact quantity mismatch trips it. In particular, opposite
/// non-zero signs are an explicit sign flip, not a tolerable difference. A
/// ghost alone remains a flag: it is not a broker-held position Altair cannot
/// manage.
///
/// Returns the number of orphans, preserving the original reporting contract.
[[nodiscard]] inline int
enforce_reconciliation(const ReconResult& r, KillSwitch& kill,
                       Timestamp now) noexcept {
    bool sign_flip = false;
    for (int i = 0; i < r.line_count; ++i) {
        const ReconLine& line = r.lines[i];
        if (line.what == Discrepancy::QuantityMismatch
            && ((line.broker_qty.raw() < 0 && line.local_qty.raw() > 0)
                || (line.broker_qty.raw() > 0 && line.local_qty.raw() < 0))) {
            sign_flip = true;
            break;
        }
    }
    if (r.orphans > 0 || r.qty_mismatches > 0 || sign_flip) {
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
