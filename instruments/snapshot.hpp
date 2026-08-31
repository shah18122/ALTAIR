// instruments/snapshot.hpp — snapshot age, expiry crossing, and the
// download-failure policy.
//
// P1-08a. Pure logic: this file opens nothing, reads no clock, and knows
// nothing about HTTP. It answers one question — given a snapshot on disk and
// a fetch that may have failed, may the session use it? — and it answers it
// before 08:15 rather than during it.
//
// The decision that shapes everything here: the policy keys on snapshot age in
// TRADING days AND on whether an F&O expiry has been crossed. Age alone gets it
// exactly backwards on the one day a month that matters. A three-day-old
// snapshot mid-cycle is harmless — lot sizes do not change on a Tuesday. A
// ONE-day-old snapshot straddling a rollover is dangerous, because rollover is
// precisely when NSE revises lot sizes and the next cycle's strikes appear.
//
// The design decisions are fixed in prompts/P1-08a_snapshot_policy.md.

#pragma once

#include <instruments/contract_spec.hpp>

#include <cstddef>
#include <cstdint>

namespace altair {

// Sizing guidance for CALLERS building a MarketCalendar. Nothing here enforces
// them and nothing here stores a calendar — this header only reads the arrays
// it is handed. They are named so a caller sizing a fixed buffer has a number
// to use, not so a reader believes a bound is being checked.
inline constexpr std::size_t kSuggestedHolidayCap = 64;    // per year, generous
inline constexpr std::size_t kSuggestedExpiryCap = 512;

enum class SnapshotVerdict : std::uint8_t {
    Fresh,          // the fetch succeeded
    StaleUsable,    // <= 1 trading day old, no expiry crossed
    StaleAging,     // older, still within max_stale_days, no expiry crossed
    ExpiryCrossed,  // an F&O expiry fell between the snapshot and now
    TooOld,         // beyond max_stale_days
    NoSnapshot      // nothing on disk — the session must not start
};

/// True when this verdict means the source is WITHHELD from the Reconciler.
///
/// Withholding needs no new blocking machinery, and that is the point: a
/// source that is simply not `add`ed leaves P1-06 seeing a contract with no
/// primary (MissingPrimary) or no broker (NoBroker), and `verdict_blocks`
/// blocks it. A second blocking path would be a second thing that can disagree
/// with the first about whether a symbol is tradable.
[[nodiscard]] constexpr bool verdict_withholds(SnapshotVerdict v) noexcept {
    return v == SnapshotVerdict::ExpiryCrossed || v == SnapshotVerdict::TooOld;
}

/// True when the session must not begin at all.
///
/// Distinct from withholding. "Block every symbol" and "do not trade today"
/// are the same outcome, but only one of them says so out loud (rule 9).
[[nodiscard]] constexpr bool verdict_halts(SnapshotVerdict v) noexcept {
    return v == SnapshotVerdict::NoSnapshot;
}

/// What a parser stamps on every spec built from this snapshot.
[[nodiscard]] constexpr bool stale_flag_for(SnapshotVerdict v) noexcept {
    return v != SnapshotVerdict::Fresh;
}

/// The market calendar, SUPPLIED — never invented.
///
/// Altair has no holiday calendar yet and this file does not guess one:
/// Muhurat trading alone guarantees a guessed Indian calendar is wrong, and no
/// test here would catch it. Both arrays hold IST midnights.
struct MarketCalendar {
    const Timestamp* holidays = nullptr;   // full trading holidays
    std::size_t holiday_count = 0;
    const Timestamp* expiries = nullptr;   // F&O expiry dates, all series
    std::size_t expiry_count = 0;
};

struct SnapshotPolicy {
    /// UNIT: TRADING days. 0 means only today's snapshot is acceptable —
    /// not "unlimited".
    std::int32_t max_stale_days = 1;
};

struct SnapshotState {
    Timestamp taken_at{};   // IST midnight of the snapshot's trading date
    bool present = false;   // false => NoSnapshot
    bool fetch_ok = false;  // did today's fetch succeed?
};

namespace detail {

/// Longest span this will walk day by day before saturating. A gap larger than
/// this is decisively too old whatever the exact count, and the cap stops a
/// broken timestamp from spinning the loop for a geological age.
inline constexpr std::int64_t kMaxSpanDays = 4096;

/// Days since the IST midnight of 1970-01-01. UNIT: days.
/// Built on P0-02's ist_days_between rather than re-derived — the IST day
/// boundary is 18:30 UTC the previous day and getting that wrong by hand is
/// the classic off-by-one nobody can reproduce.
[[nodiscard]] constexpr std::int64_t ist_day_number(Timestamp t) noexcept {
    return ist_days_between(Timestamp::epoch(), t);
}

/// 0 = Monday .. 6 = Sunday, in IST.
/// 1970-01-01 was a Thursday, so day number 0 must map to 3.
[[nodiscard]] constexpr int ist_weekday(std::int64_t day_number) noexcept {
    // Floor-mod, not the built-in %: for dates before 1970 the day number is
    // negative and C++ truncates toward zero, which would yield a negative
    // weekday and index nothing sensible.
    std::int64_t w = (day_number + 3) % 7;
    if (w < 0) {
        w += 7;
    }
    return static_cast<int>(w);
}

[[nodiscard]] constexpr bool is_weekend(std::int64_t day_number) noexcept {
    const int w = ist_weekday(day_number);
    return w == 5 || w == 6;      // Saturday, Sunday
}

/// Linear scan, deliberately. The contract says the arrays are sorted, but a
/// binary search would be silently wrong on an unsorted one — and an unsorted
/// calendar is a caller bug that must not become a wrong trading decision.
[[nodiscard]] inline bool is_holiday(std::int64_t day_number,
                                     const MarketCalendar& cal) noexcept {
    for (std::size_t i = 0; i < cal.holiday_count; ++i) {
        if (ist_day_number(cal.holidays[i]) == day_number) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool is_trading_day(std::int64_t day_number,
                                         const MarketCalendar& cal) noexcept {
    return !is_weekend(day_number) && !is_holiday(day_number, cal);
}

} // namespace detail

/// Trading days in (from, to] — exclusive of `from`, inclusive of `to`.
/// UNIT: days. Weekends and listed holidays excluded.
///
/// Negative when `to` precedes `from`. A clock that went backwards is a real
/// event and returning 0 would hide it.
[[nodiscard]] inline std::int32_t
trading_days_between(Timestamp from, Timestamp to,
                     const MarketCalendar& cal) noexcept {
    const std::int64_t a = detail::ist_day_number(from);
    const std::int64_t b = detail::ist_day_number(to);
    if (a == b) {
        return 0;
    }
    const bool reversed = b < a;
    const std::int64_t lo = reversed ? b : a;
    const std::int64_t hi = reversed ? a : b;
    std::int64_t span = hi - lo;
    if (span > detail::kMaxSpanDays) {
        span = detail::kMaxSpanDays;
    }
    std::int32_t n = 0;
    for (std::int64_t d = lo + 1; d <= lo + span; ++d) {
        if (detail::is_trading_day(d, cal)) {
            ++n;
        }
    }
    return reversed ? -n : n;
}

/// True when `to` is strictly after `from` AND some listed expiry falls in the
/// CLOSED interval [from, to].
///
/// The closed lower bound is deliberate, and it corrects the half-open form
/// this card originally specified. A master snapshot stamped with trading date
/// D is fetched at 08:15 on D — BEFORE that day's 15:30 expiry. So a snapshot
/// dated on an expiry day is a PRE-rollover file, and by D+1 the rollover has
/// happened: expiring series are gone and any lot-size revision effective from
/// the new cycle now applies. Excluding `e == from` would hand that file to
/// D+1's session as usable and size every derivative from the previous cycle's
/// lot table — rule 1's failure mode, arriving through the back door.
///
/// We cannot tell from the outside whether a given file is pre- or post-
/// rollover, and rule 9 says ambiguity blocks rather than guesses.
///
/// The `to > from` guard keeps the same day safe: this morning's own snapshot
/// is fine on the day it was taken, expiry or not.
[[nodiscard]] inline bool expiry_crossed(Timestamp from, Timestamp to,
                                         const MarketCalendar& cal) noexcept {
    const std::int64_t a = detail::ist_day_number(from);
    const std::int64_t b = detail::ist_day_number(to);
    if (b <= a) {
        return false;
    }
    for (std::size_t i = 0; i < cal.expiry_count; ++i) {
        const std::int64_t e = detail::ist_day_number(cal.expiries[i]);
        if (e >= a && e <= b) {
            return true;
        }
    }
    return false;
}

/// The decision. UNIT: none.
///
/// `now` is a PARAMETER. Nothing here reads a wall clock — rule 7, and it is
/// also the only way a test can construct a Monday.
///
/// NOTE on Fresh: a successful fetch is trusted to have stamped `taken_at`
/// with today's trading date. P1-08b requirement 3 makes that recoverable from
/// the snapshot filename alone, so the invariant is enforced where the file is
/// written rather than second-guessed here.
[[nodiscard]] inline SnapshotVerdict
judge_snapshot(const SnapshotState& s, Timestamp now,
               const MarketCalendar& cal, const SnapshotPolicy& p) noexcept {
    // The ladder, in exactly this order. ExpiryCrossed outranks TooOld and both
    // outrank any age-only verdict: a one-day-old snapshot that straddles a
    // rollover must never be reported StaleUsable.
    if (!s.present) {
        return SnapshotVerdict::NoSnapshot;
    }
    if (s.fetch_ok) {
        return SnapshotVerdict::Fresh;
    }

    const std::int32_t age = trading_days_between(s.taken_at, now, cal);
    if (age < 0) {
        // A snapshot dated after `now` is a broken clock or a mislabelled
        // file. Either way it is not trustworthy, and the naive signed reading
        // would have called it fresh.
        return SnapshotVerdict::TooOld;
    }
    if (expiry_crossed(s.taken_at, now, cal)) {
        return SnapshotVerdict::ExpiryCrossed;
    }
    if (age > p.max_stale_days) {
        return SnapshotVerdict::TooOld;
    }
    if (age <= 1) {
        return SnapshotVerdict::StaleUsable;
    }
    return SnapshotVerdict::StaleAging;
}

} // namespace altair
