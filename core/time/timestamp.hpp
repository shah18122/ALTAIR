#pragma once

// P0-02 — affine time algebra and IST.
//
// Timestamps live in an affine space; durations live in the associated vector
// space. Instant - instant = duration. Instant + duration = instant.
// Instant + instant does not exist, exactly as adding two positions in space
// does not exist. This is the same dimensional discipline that makes
// `Price * Lots` ill-formed in core/types.
//
// All instants are nanoseconds since the Unix epoch, UTC. IST is UTC+05:30 with
// no daylight saving, so the conversion is a constant offset — but session
// boundaries are IST, so IST helpers live here.
//
// This header computes; it never observes. Reading the TSC is P0-03;
// normalising broker timestamps is P0-04.

#include <types/units.hpp>

#include <cstdint>
#include <compare>
#include <limits>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// Duration — a signed span of time in nanoseconds. A vector quantity:
// durations add, subtract, negate, and scale freely.
// ─────────────────────────────────────────────────────────────────────────

/// Dimension tag for Duration. Incomplete by design; never instantiated.
struct DurationTag;

/// A signed span of time. UNIT: nanoseconds. Range +/- 292 years.
using Duration = StrongInt<DurationTag, std::int64_t>;

// ─────────────────────────────────────────────────────────────────────────
// Timestamp — an instant, nanoseconds since the Unix epoch, UTC.
// An AFFINE point. Timestamp + Timestamp does not exist, by design.
// Range is +/- 292 years around 1970, which covers every use in this system.
// ─────────────────────────────────────────────────────────────────────────
class Timestamp {
public:
    /// The Unix epoch, 1970-01-01T00:00:00Z. UNIT: nanoseconds since that epoch.
    constexpr Timestamp() noexcept = default;

    /// PRECONDITION: ns is nanoseconds since 1970-01-01T00:00:00Z.
    /// Explicit: a bare integer is not an instant until someone says which epoch.
    constexpr explicit Timestamp(std::int64_t ns_since_unix_epoch) noexcept
        : ns_(ns_since_unix_epoch) {}

    /// UNIT: nanoseconds since 1970-01-01T00:00:00Z, UTC.
    [[nodiscard]] constexpr std::int64_t ns_since_epoch() const noexcept { return ns_; }

    /// 1970-01-01T00:00:00Z. UNIT: nanoseconds. Equals Timestamp{0}.
    [[nodiscard]] static constexpr Timestamp epoch() noexcept { return Timestamp{0}; }

    /// Smallest representable instant. UNIT: nanoseconds. Roughly 1677-09-21.
    [[nodiscard]] static constexpr Timestamp min() noexcept {
        return Timestamp{std::numeric_limits<std::int64_t>::min()};
    }

    /// Largest representable instant. UNIT: nanoseconds. Roughly 2262-04-11.
    [[nodiscard]] static constexpr Timestamp max() noexcept {
        return Timestamp{std::numeric_limits<std::int64_t>::max()};
    }

    /// True iff this instant is exactly the Unix epoch. A default-constructed
    /// Timestamp is the epoch, which is how "unset" is detected upstream.
    [[nodiscard]] constexpr bool is_epoch() const noexcept { return ns_ == 0; }

    friend constexpr auto operator<=>(Timestamp, Timestamp) noexcept = default;
    friend constexpr bool operator==(Timestamp, Timestamp) noexcept = default;

    // ── Affine algebra. Note what is absent. ──

    /// Instant + span = instant. UNIT: nanoseconds.
    /// PRECONDITION: the sum fits in int64 nanoseconds. Unchecked.
    friend constexpr Timestamp operator+(Timestamp t, Duration d) noexcept {
        return Timestamp{t.ns_ + d.raw()};
    }

    /// Span + instant = instant. Commutative form of the above. UNIT: nanoseconds.
    /// PRECONDITION: the sum fits in int64 nanoseconds. Unchecked.
    friend constexpr Timestamp operator+(Duration d, Timestamp t) noexcept {
        return Timestamp{t.ns_ + d.raw()};
    }

    /// Instant - span = instant. UNIT: nanoseconds.
    /// PRECONDITION: the difference fits in int64 nanoseconds. Unchecked.
    friend constexpr Timestamp operator-(Timestamp t, Duration d) noexcept {
        return Timestamp{t.ns_ - d.raw()};
    }

    /// Instant - instant = span. The only subtraction that changes dimension.
    /// UNIT: nanoseconds.
    /// PRECONDITION: the difference fits in int64 nanoseconds. Unchecked.
    friend constexpr Duration operator-(Timestamp a, Timestamp b) noexcept {
        return Duration{a.ns_ - b.ns_};
    }

    /// Advance by a span. UNIT: nanoseconds.
    /// PRECONDITION: the sum fits in int64 nanoseconds. Unchecked.
    constexpr Timestamp& operator+=(Duration d) noexcept {
        ns_ += d.raw();
        return *this;
    }

    /// Rewind by a span. UNIT: nanoseconds.
    /// PRECONDITION: the difference fits in int64 nanoseconds. Unchecked.
    constexpr Timestamp& operator-=(Duration d) noexcept {
        ns_ -= d.raw();
        return *this;
    }

private:
    std::int64_t ns_{};
};

// ─────────────────────────────────────────────────────────────────────────
// Duration constructors.
//
// UNCHECKED by design: int64 nanoseconds spans +/- 292 years, and every span
// this system reasons about (session windows, model horizons, cooldowns,
// staleness thresholds) is under a day. Overflow requires an argument
// physically impossible in this domain.
// PRECONDITION on every one: the result must fit in int64 nanoseconds.
// The kMax* constants below give the exact bound for each.
// ─────────────────────────────────────────────────────────────────────────
namespace duration {

/// UNIT: nanoseconds in, nanoseconds out. PRECONDITION: none; always exact.
[[nodiscard]] constexpr Duration nanos(std::int64_t n) noexcept {
    return Duration{n};
}

/// UNIT: microseconds in, nanoseconds out. PRECONDITION: |n| <= kMaxMicros.
[[nodiscard]] constexpr Duration micros(std::int64_t n) noexcept {
    return Duration{n * 1'000LL};
}

/// UNIT: milliseconds in, nanoseconds out. PRECONDITION: |n| <= kMaxMillis.
[[nodiscard]] constexpr Duration millis(std::int64_t n) noexcept {
    return Duration{n * 1'000'000LL};
}

/// UNIT: seconds in, nanoseconds out. PRECONDITION: |n| <= kMaxSeconds.
[[nodiscard]] constexpr Duration seconds(std::int64_t n) noexcept {
    return Duration{n * 1'000'000'000LL};
}

/// UNIT: minutes in, nanoseconds out. PRECONDITION: |n| <= kMaxMinutes.
[[nodiscard]] constexpr Duration minutes(std::int64_t n) noexcept {
    return Duration{n * 60'000'000'000LL};
}

/// UNIT: hours in, nanoseconds out. PRECONDITION: |n| <= kMaxHours.
[[nodiscard]] constexpr Duration hours(std::int64_t n) noexcept {
    return Duration{n * 3'600'000'000'000LL};
}

/// UNIT: days in, nanoseconds out. A "day" here is exactly 86400 s; Unix time
/// excludes leap seconds, so this is exact. PRECONDITION: |n| <= kMaxDays.
[[nodiscard]] constexpr Duration days(std::int64_t n) noexcept {
    return Duration{n * 86'400'000'000'000LL};
}

/// Largest |n| accepted by each constructor without overflowing int64 ns.
inline constexpr std::int64_t kMaxMicros  = 9223372036854775LL;
inline constexpr std::int64_t kMaxMillis  = 9223372036854LL;
inline constexpr std::int64_t kMaxSeconds = 9223372036LL;
inline constexpr std::int64_t kMaxMinutes = 153722867LL;
inline constexpr std::int64_t kMaxHours   = 2562047LL;
inline constexpr std::int64_t kMaxDays    = 106751LL;

} // namespace duration

// ── Duration accessors. Truncate toward zero. ──

/// UNIT: nanoseconds. Exact. PRECONDITION: none.
[[nodiscard]] constexpr std::int64_t to_nanos(Duration d) noexcept {
    return d.raw();
}

/// UNIT: microseconds. Truncates toward zero, so -1500 ns -> -1. PRECONDITION: none.
[[nodiscard]] constexpr std::int64_t to_micros(Duration d) noexcept {
    return d.raw() / 1'000LL;
}

/// UNIT: milliseconds. Truncates toward zero. PRECONDITION: none.
[[nodiscard]] constexpr std::int64_t to_millis(Duration d) noexcept {
    return d.raw() / 1'000'000LL;
}

/// UNIT: seconds. Truncates toward zero, NOT floor: millis(-5) -> 0, not -1.
/// PRECONDITION: none.
[[nodiscard]] constexpr std::int64_t to_seconds(Duration d) noexcept {
    return d.raw() / 1'000'000'000LL;
}

/// UNIT: seconds, fractional. LOSSY — double has 53 bits of mantissa and a
/// Duration has 64, so spans beyond ~104 days lose nanosecond resolution.
/// For logging and analytics ONLY. Never use for ordering or comparison;
/// compare Durations directly, which is exact. PRECONDITION: none.
[[nodiscard]] constexpr double to_seconds_f(Duration d) noexcept {
    return static_cast<double>(d.raw()) / 1'000'000'000.0;
}

/// UNIT: milliseconds, fractional. LOSSY — see to_seconds_f.
/// For logging and analytics ONLY. Never use for ordering or comparison.
/// PRECONDITION: none.
[[nodiscard]] constexpr double to_millis_f(Duration d) noexcept {
    return static_cast<double>(d.raw()) / 1'000'000.0;
}

/// Magnitude of a span. UNIT: nanoseconds.
/// PRECONDITION: none. Duration{INT64_MIN} has no representable negation, so
/// that single input returns Duration{INT64_MAX} rather than invoking UB.
/// The result is off by one nanosecond in that case, deliberately and only there.
[[nodiscard]] constexpr Duration abs_duration(Duration d) noexcept {
    if (d.raw() == std::numeric_limits<std::int64_t>::min()) {
        return Duration{std::numeric_limits<std::int64_t>::max()};
    }
    return d.raw() < 0 ? Duration{-d.raw()} : d;
}

// ─────────────────────────────────────────────────────────────────────────
// Internal helpers. Named namespace, NEVER anonymous: this header is included
// everywhere, and an anonymous namespace in a header gives internal-linkage
// entities to inline functions with external linkage, which is an ODR
// violation. (That was defect 1 of P0-01.)
// ─────────────────────────────────────────────────────────────────────────
namespace detail {

/// Nanoseconds in one 86400-second day. UNIT: nanoseconds.
inline constexpr std::int64_t kNsPerDay = 86'400'000'000'000LL;

/// a mod kNsPerDay with FLOOR semantics — result always in [0, kNsPerDay).
/// C++ `%` truncates toward zero, so -1 % kNsPerDay is -1; this returns
/// kNsPerDay - 1 instead. UNIT: nanoseconds. PRECONDITION: none.
[[nodiscard]] constexpr std::int64_t floor_mod_day(std::int64_t a) noexcept {
    const std::int64_t r = a % kNsPerDay;
    return r < 0 ? r + kNsPerDay : r;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────
// IST — Indian Standard Time. UTC+05:30, no daylight saving, ever.
// India has observed no DST since 1945; this is a constant, not a lookup.
// ─────────────────────────────────────────────────────────────────────────

/// +05:30 expressed in nanoseconds. UNIT: nanoseconds. Exactly 19800 seconds.
inline constexpr Duration kIstOffset = Duration{19'800'000'000'000LL};

/// Nanoseconds elapsed since the most recent IST midnight at or before `t`.
/// UNIT: nanoseconds. Always in [0, 86'400'000'000'000). Uses floor semantics,
/// so it is correct for instants before the Unix epoch.
/// PRECONDITION: t.ns_since_epoch() < INT64_MAX - 19'800'000'000'000 — i.e. `t`
/// is not within 5h30m of Timestamp::max(). The IST offset is added before the
/// modulo, so instants at the very top of the range would overflow. Real market
/// instants are ~1.8e18, nowhere near the 9.2e18 ceiling.
[[nodiscard]] constexpr std::int64_t ist_ns_since_midnight(Timestamp t) noexcept {
    return detail::floor_mod_day(t.ns_since_epoch() + kIstOffset.raw());
}

/// The IST midnight at or before `t`, as a UTC instant.
/// UNIT: nanoseconds since the Unix epoch, UTC.
/// PRECONDITION: as ist_ns_since_midnight.
[[nodiscard]] constexpr Timestamp ist_midnight_of(Timestamp t) noexcept {
    return t - Duration{ist_ns_since_midnight(t)};
}

/// The instant of `hh:mm:ss` plus `ns` IST, on the IST day containing `t`.
/// UNIT: hh/mm/ss are whole hours/minutes/seconds; ns is nanoseconds.
/// Returns nanoseconds since the Unix epoch, UTC.
/// PRECONDITION: 0 <= hh < 24, 0 <= mm < 60, 0 <= ss < 60, 0 <= ns < 1e9.
/// No validation is performed; the caller guarantees the range.
[[nodiscard]] constexpr Timestamp ist_time_of_day(Timestamp t,
                                                  int hh, int mm, int ss,
                                                  std::int64_t ns = 0) noexcept {
    // Widen to 64 bits BEFORE the multiply. hh/mm/ss are int and the literal
    // 1'000'000'000 is also int, so computing this in int would overflow:
    // 09:15 alone is 33'300'000'000'000, which is 15'506x INT_MAX. That is
    // signed overflow — UB, not a wrapped value.
    const std::int64_t sod = static_cast<std::int64_t>(hh) * 3600
                           + static_cast<std::int64_t>(mm) * 60
                           + static_cast<std::int64_t>(ss);
    return ist_midnight_of(t) + Duration{sod * 1'000'000'000LL + ns};
}

/// Whole IST days between the IST midnights of `a` and `b`. Sign follows b - a.
/// UNIT: days. Exact — both operands are IST midnights, so the division never
/// has a remainder to round.
/// PRECONDITION: as ist_ns_since_midnight, for both arguments.
[[nodiscard]] constexpr std::int64_t ist_days_between(Timestamp a, Timestamp b) noexcept {
    // .raw() is required, not stylistic: StrongInt deliberately defines no
    // operator/, because dividing a dimensioned quantity by a bare scalar is
    // exactly the silent unit loss core/types exists to prevent.
    return (ist_midnight_of(b) - ist_midnight_of(a)).raw() / detail::kNsPerDay;
}

// ─────────────────────────────────────────────────────────────────────────
// Literals — for tests and configuration only.
// Every one yields a Duration in nanoseconds.
// PRECONDITION on each: the result must fit in int64 nanoseconds.
// ─────────────────────────────────────────────────────────────────────────
namespace literals {

/// UNIT: nanoseconds. PRECONDITION: v <= INT64_MAX.
[[nodiscard]] constexpr Duration operator""_ns(unsigned long long v) noexcept {
    return Duration{static_cast<std::int64_t>(v)};
}

/// UNIT: microseconds -> nanoseconds. PRECONDITION: v <= duration::kMaxMicros.
[[nodiscard]] constexpr Duration operator""_us(unsigned long long v) noexcept {
    return duration::micros(static_cast<std::int64_t>(v));
}

/// UNIT: milliseconds -> nanoseconds. PRECONDITION: v <= duration::kMaxMillis.
[[nodiscard]] constexpr Duration operator""_ms(unsigned long long v) noexcept {
    return duration::millis(static_cast<std::int64_t>(v));
}

/// UNIT: seconds -> nanoseconds. PRECONDITION: v <= duration::kMaxSeconds.
[[nodiscard]] constexpr Duration operator""_s(unsigned long long v) noexcept {
    return duration::seconds(static_cast<std::int64_t>(v));
}

/// UNIT: minutes -> nanoseconds. PRECONDITION: v <= duration::kMaxMinutes.
[[nodiscard]] constexpr Duration operator""_min(unsigned long long v) noexcept {
    return duration::minutes(static_cast<std::int64_t>(v));
}

/// UNIT: hours -> nanoseconds. PRECONDITION: v <= duration::kMaxHours.
[[nodiscard]] constexpr Duration operator""_h(unsigned long long v) noexcept {
    return duration::hours(static_cast<std::int64_t>(v));
}

} // namespace literals
} // namespace altair
