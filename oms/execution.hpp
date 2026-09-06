// oms/execution.hpp -- execution algorithms: TWAP, VWAP, POV.
//
// P13-01 / 02 / 03 / 04.
//
// WHY THESE LIVE IN `oms/` AND NOWHERE ELSE.
//
// They decide QUANTITY AND TIMING of orders, which is placing an order in
// every sense that matters. CLAUDE.md is unconditional: `oms/` is the only
// thing that can place an order, and if order-placing code appears elsewhere
// that is a review failure rather than a refactor opportunity. A scheduler in
// `strategies/` would be exactly that, wearing a different noun.
//
// THE INVARIANT: SUM OF CHILDREN EQUALS THE PARENT. EXACTLY.
//
// In integer quantity, checked, every slice. This is the conservation law of
// execution and it is violated the same way conservation of cash is: not by a
// big obvious error but by rounding, twelve times, in the same direction.
// 1000 shares over 7 slices is 142.857 each; seven lots of 142 is 994 and the
// parent is six short with every child reporting success. So the schedule
// carries the remainder explicitly and the last slice absorbs it, and
// `total()` is asserted against the parent rather than assumed.
//
// A SCHEDULE IS NOT A PROMISE ABOUT FILLS.
//
// Everything here produces INTENT: how much to work, and when. What actually
// fills is the broker's answer, and `oms/order_state.hpp` owns that. Keeping
// the two apart is what makes an implementation-shortfall attribution possible
// at all (P13-06) -- you cannot separate delay from execution if the thing
// that scheduled the order also recorded what came back.
//
// TIME COMES OFF THE TICK.
//
// Rule 7, and it bites harder here than almost anywhere: a TWAP that reads a
// wall clock in a replay slices against real time while the market moves at
// replay speed, and produces a schedule that is perfect and unreproducible.
// Every function takes the session clock as a parameter.

#pragma once

#include <core/time/timestamp.hpp>
#include <core/types/units.hpp>

#include <cstdint>
#include <expected>
#include <vector>

namespace altair {

enum class ExecError : std::uint8_t {
    /// Quantity is zero or negative. Direction belongs in `side`.
    BadQuantity,
    /// The window ends before it starts, or is zero-length.
    BadWindow,
    /// Fewer slices than one, or more than the schedule can hold.
    BadSliceCount,
    /// A volume curve that is empty, negative, or sums to zero -- so it
    /// cannot be normalised into weights.
    BadCurve,
    /// The participation rate is outside (0, 1].
    BadRate,
    /// The instrument reports no volume, so a volume-based algorithm has
    /// nothing to schedule against. See `vwap_schedule`.
    NoVolume
};

[[nodiscard]] inline const char* exec_error_text(ExecError e) noexcept {
    switch (e) {
    case ExecError::BadQuantity:   return "quantity must be positive";
    case ExecError::BadWindow:     return "window must be positive-length";
    case ExecError::BadSliceCount: return "slice count out of range";
    case ExecError::BadCurve:      return "volume curve is unusable";
    case ExecError::BadRate:       return "participation rate outside (0,1]";
    case ExecError::NoVolume:      return "instrument reports no volume";
    }
    return "unknown";
}

/// One child order: work `qty` at `at`.
struct Slice {
    Timestamp at{};
    Qty qty{0};
};

/// The parent's plan. Fixed capacity, so a schedule can be built on the hot
/// path without allocating (rule 4).
class Schedule {
public:
    static constexpr std::size_t kMaxSlices = 512;

    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    [[nodiscard]] const Slice& operator[](std::size_t i) const noexcept {
        return slice_[i < n_ ? i : 0];
    }

    /// Sum of every child. THE check: this must equal the parent exactly.
    [[nodiscard]] Qty total() const noexcept {
        std::int64_t t = 0;
        for (std::size_t i = 0; i < n_; ++i) { t += slice_[i].qty.raw(); }
        return Qty{t};
    }

    [[nodiscard]] bool push(Timestamp at, Qty q) noexcept {
        if (n_ >= kMaxSlices) { return false; }
        slice_[n_++] = Slice{at, q};
        return true;
    }

private:
    Slice slice_[kMaxSlices]{};
    std::size_t n_ = 0;
};

namespace detail {

/// Split `total` into `n` parts as evenly as integers permit, then hand the
/// remainder out ONE UNIT AT A TIME from the front.
///
/// Not "give the remainder to the last slice": that makes the final child up
/// to n-1 units larger than the others, which is the slice most likely to move
/// the market and the one a POV cap is most likely to reject. Spreading it
/// keeps every child within one unit of every other.
inline void even_split(std::int64_t total, std::size_t n,
                       std::int64_t* out) noexcept {
    const auto dn = static_cast<std::int64_t>(n);
    const std::int64_t base = total / dn;
    const std::int64_t rem = total - base * dn;
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = base + (static_cast<std::int64_t>(i) < rem ? 1 : 0);
    }
}

} // namespace detail

// ---------------------------------------------------------------------------
// P13-02 — TWAP
// ---------------------------------------------------------------------------

/// Uniform slices across the window.
///
/// The honest thing to say about TWAP is that it is a BENCHMARK first and a
/// strategy second. It assumes nothing about where volume is, which makes it
/// robust and also makes it wrong in exactly the predictable way: it works the
/// same quantity through the thin middle of the session as through the open,
/// so its slices are a larger share of volume precisely when the book is
/// thinnest.
[[nodiscard]] inline std::expected<Schedule, ExecError>
twap_schedule(Qty total, Timestamp start, Timestamp end,
              std::size_t slices) noexcept {
    if (total.raw() <= 0) { return std::unexpected(ExecError::BadQuantity); }
    if ((end - start).raw() <= 0) {
        return std::unexpected(ExecError::BadWindow);
    }
    if (slices == 0 || slices > Schedule::kMaxSlices
        || static_cast<std::int64_t>(slices) > total.raw()) {
        // More slices than units would produce zero-quantity children, which
        // are not orders. Refused rather than silently dropped.
        return std::unexpected(ExecError::BadSliceCount);
    }
    std::int64_t part[Schedule::kMaxSlices];
    detail::even_split(total.raw(), slices, part);

    const std::int64_t span = (end - start).raw();
    const auto dn = static_cast<std::int64_t>(slices);
    Schedule s;
    for (std::size_t i = 0; i < slices; ++i) {
        // Slice i works at the START of its own interval, not the midpoint:
        // the last child must land strictly before `end`, or the parent
        // finishes after its own deadline.
        const std::int64_t off =
            span * static_cast<std::int64_t>(i) / dn;
        if (!s.push(start + Duration{off}, Qty{part[i]})) {
            // Unreachable: slices was bounded against kMaxSlices above. Checked
            // anyway, because "it cannot happen" is how a capacity bound and
            // its guard drift apart.
            return std::unexpected(ExecError::BadSliceCount);
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
// P13-03 — VWAP
// ---------------------------------------------------------------------------

/// Slice in proportion to an expected volume CURVE.
///
/// `curve[i]` is the expected share of the window's volume in bucket i. It is
/// normalised here, so it may be raw volumes, percentages, or anything
/// positive -- what it may NOT be is negative or all-zero, and both are
/// refused rather than normalised into nonsense.
///
/// AN INDEX HAS NO VOLUME, AND THAT IS NOT A ZERO.
///
/// NIFTY 50 does not trade; it is computed from things that do. `reports_volume`
/// exists on the instrument for exactly this reason (P11Q-09), and a VWAP
/// schedule for an index is not "flat" or "equal-weighted" -- it is UNDEFINED,
/// and `NoVolume` says so instead of quietly degrading to TWAP. Degrading
/// quietly is how a benchmark nobody can compute gets reported as met.
[[nodiscard]] inline std::expected<Schedule, ExecError>
vwap_schedule(Qty total, Timestamp start, Timestamp end, const double* curve,
              std::size_t buckets, bool instrument_reports_volume) noexcept {
    if (total.raw() <= 0) { return std::unexpected(ExecError::BadQuantity); }
    if ((end - start).raw() <= 0) {
        return std::unexpected(ExecError::BadWindow);
    }
    if (!instrument_reports_volume) {
        return std::unexpected(ExecError::NoVolume);
    }
    if (curve == nullptr || buckets == 0 || buckets > Schedule::kMaxSlices
        || static_cast<std::int64_t>(buckets) > total.raw()) {
        return std::unexpected(ExecError::BadSliceCount);
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < buckets; ++i) {
        if (!(curve[i] >= 0.0)) { return std::unexpected(ExecError::BadCurve); }
        sum += curve[i];
    }
    if (!(sum > 0.0)) { return std::unexpected(ExecError::BadCurve); }

    // LARGEST-REMAINDER APPORTIONMENT, not round-and-hope.
    //
    // Rounding each share independently loses or gains units and the parent
    // no longer sums. So the floor is taken first and the leftover units go to
    // the buckets with the largest fractional parts -- the same method used
    // for apportioning seats, and for the same reason: the total is fixed and
    // must be honoured exactly.
    std::int64_t part[Schedule::kMaxSlices];
    double frac[Schedule::kMaxSlices];
    std::int64_t assigned = 0;
    for (std::size_t i = 0; i < buckets; ++i) {
        const double exact =
            static_cast<double>(total.raw()) * curve[i] / sum;
        const auto fl = static_cast<std::int64_t>(exact);
        part[i] = fl;
        frac[i] = exact - static_cast<double>(fl);
        assigned += fl;
    }
    std::int64_t left = total.raw() - assigned;
    while (left > 0) {
        std::size_t best = 0;
        double bf = -1.0;
        for (std::size_t i = 0; i < buckets; ++i) {
            if (frac[i] > bf) { bf = frac[i]; best = i; }
        }
        ++part[best];
        frac[best] = -1.0;          // used; cannot win twice
        --left;
    }

    const std::int64_t span = (end - start).raw();
    const auto db = static_cast<std::int64_t>(buckets);
    Schedule s;
    for (std::size_t i = 0; i < buckets; ++i) {
        if (part[i] == 0) { continue; }   // a bucket with no volume gets no child
        const std::int64_t off = span * static_cast<std::int64_t>(i) / db;
        if (!s.push(start + Duration{off}, Qty{part[i]})) {
            return std::unexpected(ExecError::BadSliceCount);
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
// P13-04 — POV (percentage of volume)
// ---------------------------------------------------------------------------

/// What a POV algorithm decides for ONE bucket, given the volume it just saw.
struct PovStep {
    Qty send{0};
    /// True when the cap bound the size rather than the remaining quantity.
    /// Reported because a POV that is permanently capped is not participating
    /// at the requested rate -- it is falling behind, and the caller needs to
    /// know that before the close, not after.
    bool capped = false;
};

/// POV is FEEDBACK, and feedback on a quantity that can collapse.
///
/// Participate at `rate` of observed volume. The failure mode is specific and
/// worth naming: if volume dries up, a POV algorithm sends less, which is
/// correct; but it then has the same quantity left with less time, and a naive
/// implementation compensates by raising its participation -- which pushes the
/// price, which moves other participants away, which reduces volume further.
/// That loop is why this returns `capped` instead of silently catching up, and
/// why the caller, not this function, decides whether to cross the spread.
[[nodiscard]] inline std::expected<PovStep, ExecError>
pov_step(Qty remaining, std::int64_t bucket_volume, double rate) noexcept {
    if (remaining.raw() < 0) { return std::unexpected(ExecError::BadQuantity); }
    if (!(rate > 0.0) || rate > 1.0) {
        return std::unexpected(ExecError::BadRate);
    }
    if (bucket_volume < 0) { return std::unexpected(ExecError::BadCurve); }
    PovStep out;
    if (remaining.raw() == 0 || bucket_volume == 0) { return out; }

    const auto cap = static_cast<std::int64_t>(
        static_cast<double>(bucket_volume) * rate);
    if (cap <= 0) {
        // Volume so thin that the rate rounds to nothing. Sending one unit
        // anyway would be participating at 100% of a one-lot bucket.
        out.capped = true;
        return out;
    }
    if (cap < remaining.raw()) {
        out.send = Qty{cap};
        out.capped = true;
    } else {
        out.send = remaining;
    }
    return out;
}

} // namespace altair
