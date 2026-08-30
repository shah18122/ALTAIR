#pragma once

// P0-04 — per-source epoch normalisation and the plausibility gate.
//
// Every feed states time differently: seconds since the Unix epoch,
// milliseconds, or seconds since a vendor epoch. Some encode IST wall-clock as
// though it were UTC — a 5h30m error that looks entirely plausible, because the
// number IS a valid timestamp. Just for the wrong instant, in the future, which
// is the one direction that silently injects look-ahead.
//
// CLAUDE.md rule 7: strategies read time off the tick, never off a wall clock.
// That makes this file the single place a bad clock can poison everything
// downstream. Rule 9: an implausible timestamp is rejected and counted — never
// clamped, never guessed.
//
// No clock is read here. `local_now` is always a parameter, so replay and live
// take the identical path (rule 6) and every decision is testable.

#include <time/timestamp.hpp>
#include <types/units.hpp>

#include <cstdint>
#include <expected>
#include <limits>

namespace altair {

// ─────────────────────────────────────────────────────────────────────────
// The unit a source states its raw integer timestamp in.
// ─────────────────────────────────────────────────────────────────────────
enum class TimeUnit : std::uint8_t { Seconds, Millis, Micros, Nanos };

// ─────────────────────────────────────────────────────────────────────────
// Why a raw timestamp was not believed. Never silently corrected.
// ─────────────────────────────────────────────────────────────────────────
enum class TsReject : std::uint8_t {
    Unconvertible,    // scaling to int64 nanoseconds would overflow
    BeforeFloor,      // earlier than the configured plausible floor
    AfterCeiling,     // later than local_now + max_future_skew (LOOK-AHEAD guard)
    Regressed,        // earlier than the last admitted timestamp from this source
    OutsideSession    // a valid instant, but not inside the trading window
};

// ─────────────────────────────────────────────────────────────────────────
// How one feed states time. LOADED FROM CONFIG, never hardcoded per broker —
// an epoch or unit guessed wrong shifts every tick from that source by a
// constant, which looks like a working feed until it is reconciled.
// ─────────────────────────────────────────────────────────────────────────
struct SourceTimeSpec {
    /// The unit of the raw integer.
    TimeUnit unit;

    /// (source epoch) - (Unix epoch). UNIT: nanoseconds.
    /// Zero for a Unix-epoch source. Positive for a later epoch: a source
    /// counting from 2010-01-01 has +1'262'304'000'000'000'000 ns.
    Duration epoch_offset;

    /// True iff the raw value encodes IST wall-clock as though it were UTC.
    /// Such a value is kIstOffset (5 h 30 m) LARGER than the truth, i.e. it
    /// stamps every tick 19800 s into the future. Corrected by subtraction.
    bool ist_naive;
};

// ─────────────────────────────────────────────────────────────────────────
// Internal helpers. Named namespace, never anonymous — this is a header.
// ─────────────────────────────────────────────────────────────────────────
namespace detail {

/// True iff a + b overflows int64. Distinct name from P0-01's mul_overflows,
/// which is a different function with a different signature.
/// UNIT: none. PRECONDITION: none.
[[nodiscard]] constexpr bool add_overflows_i64(std::int64_t a, std::int64_t b) noexcept {
    if (b > 0) {
        return a > std::numeric_limits<std::int64_t>::max() - b;
    }
    if (b < 0) {
        return a < std::numeric_limits<std::int64_t>::min() - b;
    }
    return false;
}

} // namespace detail

/// Nanoseconds in one unit of `u`. UNIT: ns/unit. PRECONDITION: none.
[[nodiscard]] constexpr std::int64_t ns_per_unit(TimeUnit u) noexcept {
    switch (u) {
        case TimeUnit::Seconds: return 1'000'000'000LL;
        case TimeUnit::Millis:  return 1'000'000LL;
        case TimeUnit::Micros:  return 1'000LL;
        case TimeUnit::Nanos:   break;
    }
    return 1LL;
}

/// Largest |raw| that can be scaled to int64 nanoseconds without overflow,
/// for unit `u`. UNIT: units of `u`. PRECONDITION: none.
[[nodiscard]] constexpr std::int64_t max_raw_for(TimeUnit u) noexcept {
    return std::numeric_limits<std::int64_t>::max() / ns_per_unit(u);
}

/// Convert one raw broker integer to a UTC instant.
/// UNIT: raw is in spec.unit since spec's epoch; result is ns since the Unix
/// epoch, UTC. Total: no input of any int64 value invokes UB.
/// PRECONDITION: none. Returns Unconvertible rather than wrapping.
[[nodiscard]] ALTAIR_HOT constexpr std::expected<Timestamp, TsReject>
to_utc(std::int64_t raw, SourceTimeSpec spec) noexcept {
    const std::int64_t limit = max_raw_for(spec.unit);

    // Bound the magnitude BEFORE negating anything: -INT64_MIN is UB, so the
    // check is written to never form it.
    if (raw > limit || raw < -limit) {
        return std::unexpected(TsReject::Unconvertible);
    }

    const std::int64_t scaled = raw * ns_per_unit(spec.unit);

    const std::int64_t off = spec.epoch_offset.raw();
    if (detail::add_overflows_i64(scaled, off)) {
        return std::unexpected(TsReject::Unconvertible);
    }
    std::int64_t ns = scaled + off;

    if (spec.ist_naive) {
        // SUBTRACT. A naive encoder writes the integer for IST wall-clock read
        // as UTC, which is kIstOffset LARGER than the truth. Adding would
        // double the error to 11 h and push the tick further into the future.
        const std::int64_t ist = kIstOffset.raw();
        if (detail::add_overflows_i64(ns, -ist)) {
            return std::unexpected(TsReject::Unconvertible);
        }
        ns -= ist;
    }

    return Timestamp{ns};
}

// ─────────────────────────────────────────────────────────────────────────
// A trading window, as offsets from IST midnight.
//
// NO CONSTANT IS SHIPPED FOR NSE OR BSE. Session times change — muhurat
// sessions, special sessions, exchange notices — and CLAUDE.md rule 1 bans
// baked-in market parameters. The caller fills this from config.
// ─────────────────────────────────────────────────────────────────────────
struct SessionWindow {
    /// Inclusive start. UNIT: ns since IST midnight, [0, 86'400'000'000'000).
    std::int64_t open_ns;
    /// Inclusive end. UNIT: ns since IST midnight, [0, 86'400'000'000'000).
    std::int64_t close_ns;
};

/// True iff `t` falls inside `w` on its own IST day, bounds inclusive.
/// UNIT: none. PRECONDITION: w.open_ns <= w.close_ns, both in range.
[[nodiscard]] constexpr bool is_in_session(Timestamp t, SessionWindow w) noexcept {
    const std::int64_t sod = ist_ns_since_midnight(t);
    return sod >= w.open_ns && sod <= w.close_ns;
}

// ─────────────────────────────────────────────────────────────────────────
// PlausibilityGate — one per feed source. NOT thread-safe: it carries the
// last-admitted watermark, so it belongs to exactly one decoder thread.
// ─────────────────────────────────────────────────────────────────────────
class PlausibilityGate {
public:
    struct Config {
        /// Reject anything earlier than this. UNIT: ns since the Unix epoch.
        Timestamp floor;

        /// Tolerance for a source clock running ahead of ours.
        /// UNIT: nanoseconds. Anything beyond local_now + this is rejected —
        /// this is the look-ahead guard, not a nicety.
        Duration max_future_skew;

        /// Reject a timestamp earlier than the last admitted one.
        bool require_monotonic;

        /// Also require the timestamp to fall inside `session`.
        /// Default OFF: a pre-open tick is a valid instant.
        bool enforce_session;

        /// Only consulted when enforce_session is true.
        SessionWindow session;
    };

    struct Stats {
        std::uint64_t admitted;
        std::uint64_t before_floor;
        std::uint64_t after_ceiling;
        std::uint64_t regressed;
        std::uint64_t outside_session;
    };

    constexpr explicit PlausibilityGate(Config cfg) noexcept : cfg_(cfg) {}

    /// Decide whether to believe `exchange_ts`, given our own clock reading.
    /// UNIT: both arguments are ns since the Unix epoch, UTC.
    /// Checks run in a fixed order — floor, ceiling, monotonic, session — and
    /// the FIRST failure is reported. Exactly one counter moves per call.
    /// PRECONDITION: local_now comes from a clock this process trusts.
    [[nodiscard]] ALTAIR_HOT constexpr std::expected<Timestamp, TsReject>
    admit(Timestamp exchange_ts, Timestamp local_now) noexcept {
        if (exchange_ts < cfg_.floor) {
            ++stats_.before_floor;
            return std::unexpected(TsReject::BeforeFloor);
        }

        // The look-ahead guard. A source clock ahead of ours by more than the
        // budget is stamping ticks in the future; believing it would let a
        // strategy act on information it cannot yet have.
        if (exchange_ts > local_now + cfg_.max_future_skew) {
            ++stats_.after_ceiling;
            return std::unexpected(TsReject::AfterCeiling);
        }

        // An exact repeat is admitted: two trades can share a millisecond.
        if (cfg_.require_monotonic && exchange_ts < last_) {
            ++stats_.regressed;
            return std::unexpected(TsReject::Regressed);
        }

        if (cfg_.enforce_session && !is_in_session(exchange_ts, cfg_.session)) {
            ++stats_.outside_session;
            return std::unexpected(TsReject::OutsideSession);
        }

        ++stats_.admitted;
        last_ = exchange_ts;   // advanced ONLY on success
        return exchange_ts;
    }

    /// Running tally. Never reset implicitly — a feed's reject rate is a
    /// health signal that P9 drift detection consumes.
    [[nodiscard]] constexpr const Stats& stats() const noexcept { return stats_; }

    [[nodiscard]] constexpr const Config& config() const noexcept { return cfg_; }

    /// The last admitted timestamp, or the epoch if nothing has been admitted.
    [[nodiscard]] constexpr Timestamp last_admitted() const noexcept { return last_; }

    /// Clear the watermark and the counters. For session rollover and tests.
    constexpr void reset() noexcept {
        stats_ = Stats{};
        last_  = Timestamp::epoch();
    }

private:
    Config cfg_{};
    Stats stats_{};
    Timestamp last_{};
};

} // namespace altair
