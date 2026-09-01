// oms/throttle.hpp -- per-broker rate limits, and which venue gets the order.
//
// P4-07.
//
// THREE WINDOWS, AND THE TIGHTEST ONE BINDS.
//
// Kite publishes limits per SECOND, per MINUTE and per DAY, and they are not
// redundant: a strategy can sit comfortably under 10 orders a second all
// morning and still exhaust 3,000 in a day. Checking only the fastest window
// is the mistake that gets an account rate-limited at 14:30 with positions
// open and no way to close them.
//
// So every window is checked, all of them are consumed together, and the
// decision says WHICH one refused. "Throttled" is not actionable; "you have
// used your daily order budget" is.
//
// A REFUSED ORDER IS REFUSED, NOT QUEUED.
//
// This throttle does not hold orders back for later. A queue would mean an
// order arriving at the exchange at a price that stopped being current while
// it waited, which is worse than not sending it -- and it hides the fact that
// the strategy is asking for more than the venue allows. The caller is told
// how long until the next token, and decides.
//
// TIME COMES OFF THE TICK (rule 7). Every window is measured against the
// timestamp handed in, never a wall clock, so a replay throttles at exactly
// the ticks the live path did (rule 6).
//
// THE DAILY WINDOW DOES NOT REFILL BY WAITING. A second or a minute passes on
// its own; a day does not, within a session. Hitting the daily cap is a hard
// stop until the session is reset, and `wait_for` reports that rather than
// returning a plausible-looking delay a caller might sleep on.

#pragma once

#include <core/time/timestamp.hpp>
#include <core/types/units.hpp>

#include <cstdint>
#include <expected>

namespace altair {

/// Which limit refused. Ordinal 0 is Allowed, so a zeroed decision reads as
/// permission rather than as an unexplained refusal.
enum class ThrottleVerdict : std::uint8_t {
    Allowed = 0,
    PerSecond,
    PerMinute,
    PerDay
};

/// Published limits for one venue. Zero means "no limit published", which is
/// NOT the same as zero allowed -- an unset limit must not block everything.
struct BrokerLimits {
    std::int32_t per_second = 0;
    std::int32_t per_minute = 0;
    std::int32_t per_day = 0;
};

/// Kite's documented order limits.
///
/// Named constants rather than literals at the call site, because these change
/// and a number buried in a router is a number nobody finds when they do.
inline constexpr BrokerLimits kKiteOrderLimits{10, 200, 3000};

struct ThrottleDecision {
    ThrottleVerdict verdict = ThrottleVerdict::Allowed;
    /// How long until this would be allowed. Zero when allowed. For a daily
    /// refusal this is `Duration::max()`-shaped -- see `never_refills`.
    Duration wait_for{0};
    /// True when waiting cannot help, because the window is the day.
    bool never_refills = false;
};

/// A fixed-window counter over one interval.
///
/// FIXED window, not sliding, and that is a deliberate simplification with a
/// stated cost: at a window boundary a caller can send up to 2x the limit
/// across two adjacent windows. A sliding window avoids that and needs a
/// timestamp per event; a fixed one needs two integers. Kite's own limiter is
/// not documented to be sliding either, so a stricter local model would refuse
/// orders the venue would have accepted.
class FixedWindow {
public:
    FixedWindow() = default;
    FixedWindow(std::int32_t limit, Duration span) noexcept
        : limit_(limit), span_(span) {}

    /// Would one more fit? Does not consume.
    [[nodiscard]] bool would_fit(Timestamp now) const noexcept {
        if (limit_ <= 0) { return true; }        // no limit published
        if (rolled(now)) { return true; }
        return used_ < limit_;
    }

    /// Consume one. PRECONDITION: `would_fit` was true.
    void consume(Timestamp now) noexcept {
        if (limit_ <= 0) { return; }
        if (rolled(now)) {
            window_start_ = now;
            used_ = 0;
        }
        ++used_;
    }

    /// Time until the window rolls. Zero when it already has room.
    [[nodiscard]] Duration wait_for(Timestamp now) const noexcept {
        if (would_fit(now)) { return Duration{0}; }
        const std::int64_t elapsed = (now - window_start_).raw();
        const std::int64_t left = span_.raw() - elapsed;
        return Duration{left > 0 ? left : 0};
    }

    [[nodiscard]] std::int32_t used() const noexcept { return used_; }
    [[nodiscard]] std::int32_t limit() const noexcept { return limit_; }

    void reset(Timestamp now) noexcept {
        window_start_ = now;
        used_ = 0;
    }

private:
    [[nodiscard]] bool rolled(Timestamp now) const noexcept {
        return (now - window_start_).raw() >= span_.raw();
    }

    std::int32_t limit_ = 0;
    Duration span_{0};
    Timestamp window_start_{};
    std::int32_t used_ = 0;
};

/// All three windows for one venue.
class BrokerThrottle {
public:
    BrokerThrottle() = default;
    explicit BrokerThrottle(BrokerLimits lim) noexcept
        : sec_(lim.per_second, duration::seconds(1)),
          min_(lim.per_minute, duration::seconds(60)),
          day_(lim.per_day, duration::seconds(24 * 3600)) {}

    /// Ask permission WITHOUT consuming. Reports the tightest refusal.
    ///
    /// Checked coarsest-first -- day, then minute, then second -- so the
    /// reported reason is the one that matters most. A caller told "per
    /// second" would retry in a moment; told "per day" it stops.
    [[nodiscard]] ThrottleDecision check(Timestamp now) const noexcept {
        ThrottleDecision d{};
        if (!day_.would_fit(now)) {
            d.verdict = ThrottleVerdict::PerDay;
            d.wait_for = day_.wait_for(now);
            // A day does not pass within a session. Saying so stops a caller
            // sleeping on a number that will not help.
            d.never_refills = true;
            return d;
        }
        if (!min_.would_fit(now)) {
            d.verdict = ThrottleVerdict::PerMinute;
            d.wait_for = min_.wait_for(now);
            return d;
        }
        if (!sec_.would_fit(now)) {
            d.verdict = ThrottleVerdict::PerSecond;
            d.wait_for = sec_.wait_for(now);
            return d;
        }
        return d;
    }

    /// Check and consume. Consumes ALL THREE windows or none -- consuming a
    /// second-budget token for an order the daily budget refused would leak
    /// budget on every rejection.
    [[nodiscard]] ThrottleDecision acquire(Timestamp now) noexcept {
        const ThrottleDecision d = check(now);
        if (d.verdict != ThrottleVerdict::Allowed) {
            ++refused_;
            return d;
        }
        sec_.consume(now);
        min_.consume(now);
        day_.consume(now);
        ++granted_;
        return d;
    }

    [[nodiscard]] std::int32_t used_today() const noexcept {
        return day_.used();
    }
    [[nodiscard]] std::uint64_t granted() const noexcept { return granted_; }
    [[nodiscard]] std::uint64_t refused() const noexcept { return refused_; }

    /// Start a new session. The daily window is the only one a human ever
    /// needs to reset, and it is deliberately explicit.
    void new_session(Timestamp now) noexcept {
        sec_.reset(now);
        min_.reset(now);
        day_.reset(now);
    }

private:
    FixedWindow sec_;
    FixedWindow min_;
    FixedWindow day_;
    std::uint64_t granted_ = 0;
    std::uint64_t refused_ = 0;
};

/// Which venues exist. Kite is the only one live; XTS is declared so the
/// router has somewhere to route when P4-06 lands, and is REFUSED until then
/// rather than silently falling through to Kite.
enum class Venue : std::uint8_t { None = 0, Kite, Xts };

enum class RouteError : std::uint8_t {
    /// Every venue that could take this order is throttled.
    AllThrottled,
    /// The requested venue is not available in this build.
    VenueUnavailable,
    /// No venue was configured at all.
    NoVenue
};

struct RouteDecision {
    Venue venue = Venue::None;
    ThrottleDecision throttle{};
};

/// Pick a venue and take its token.
///
/// With one live venue this is a throttle check with a name on it. The shape
/// is here so P4-06 adds a venue rather than rewriting the caller -- but it
/// REFUSES an unavailable venue rather than falling back, because a silent
/// fallback sends an order to a broker the caller did not choose, on an
/// account they may not have meant to use.
[[nodiscard]] inline std::expected<RouteDecision, RouteError>
route(Venue preferred, BrokerThrottle& kite, Timestamp now) noexcept {
    if (preferred == Venue::None) {
        return std::unexpected(RouteError::NoVenue);
    }
    if (preferred == Venue::Xts) {
        // P4-06 is not built. Refusing beats routing to Kite behind the
        // caller's back.
        return std::unexpected(RouteError::VenueUnavailable);
    }
    RouteDecision d{};
    d.venue = Venue::Kite;
    d.throttle = kite.acquire(now);
    if (d.throttle.verdict != ThrottleVerdict::Allowed) {
        return std::unexpected(RouteError::AllThrottled);
    }
    return d;
}

[[nodiscard]] constexpr const char* describe(ThrottleVerdict v) noexcept {
    switch (v) {
    case ThrottleVerdict::Allowed:   return "allowed";
    case ThrottleVerdict::PerSecond: return "per-second limit";
    case ThrottleVerdict::PerMinute: return "per-minute limit";
    case ThrottleVerdict::PerDay:
        return "DAILY order budget exhausted -- waiting will not help";
    }
    return "unknown";
}

} // namespace altair
