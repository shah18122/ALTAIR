// desktop/market_clock.hpp -- what time it is, according to the tape.
//
// P11Q-03.
//
// THE MARKET CLOCK READS THE TICK. THE WALL CLOCK IS LABELLED AS SUCH.
//
// Rule 7, at the most visible widget on the screen. A dashboard that shows
// `QDateTime::currentDateTime()` next to a replayed session tells you the
// market is open at 21:40 on a Thursday, and every judgement made in front of
// it inherits that.
//
// So `MarketClock` takes the engine timestamp off the last tick applied and
// derives the session phase from it. The wall clock is displayed too, in a
// different colour and with the word "wall" on it, because the DIVERGENCE is
// the useful signal: in live trading the two track, and in a replay they are
// hours apart. A single clock cannot tell you which mode you are in.
//
// NO SESSION TIME IS BAKED IN.
//
// `core/time/exchange_ts.hpp` says it outright: "NO CONSTANT IS SHIPPED FOR
// NSE OR BSE. Session times change -- muhurat sessions, special sessions,
// exchange notices -- and CLAUDE.md rule 1 bans baked-in market parameters."
//
// So `SessionWindow` is passed IN. The values used by the demo below come from
// one place, are labelled as demo values, and are the thing P11Q-06 replaces
// with `config/altair.toml` when a real feed is wired. They are not defaults
// and nothing falls back to them.
//
// AND "CLOSED" IS NOT THE SAME AS "NO DATA".
//
// Phase `Unknown` exists and is the initial state. Before the first tick there
// is no engine time, so the honest answer is not "closed" -- which is a claim
// about the market -- but "we do not know yet", which is a claim about us.
// Absence is not zero here either.

#pragma once

#include <core/time/exchange_ts.hpp>
#include <core/time/timestamp.hpp>

#include <QColor>
#include <QString>

#include <cstdint>

namespace altair::ui {

enum class SessionPhase : std::uint8_t {
    /// No tick has arrived, so there is no engine time to judge by.
    Unknown = 0,
    PreOpen,
    Open,
    Closed
};

[[nodiscard]] inline QString phase_label(SessionPhase p) {
    switch (p) {
    case SessionPhase::PreOpen: return QStringLiteral("PRE-OPEN");
    case SessionPhase::Open:    return QStringLiteral("OPEN");
    case SessionPhase::Closed:  return QStringLiteral("CLOSED");
    case SessionPhase::Unknown:
    default:                    return QStringLiteral("NO DATA");
    }
}

[[nodiscard]] inline QColor phase_colour(SessionPhase p) {
    switch (p) {
    case SessionPhase::Open:    return QColor(0x1B, 0x8A, 0x4B);
    case SessionPhase::PreOpen: return QColor(0xB9, 0x77, 0x0B);
    case SessionPhase::Closed:  return QColor(0x7F, 0x8C, 0x8D);
    case SessionPhase::Unknown:
    default:                    return QColor(0x7F, 0x8C, 0x8D);
    }
}

/// Session windows for the demo replay.
///
/// DEMO VALUES, IN ONE PLACE, DELIBERATELY NOT DEFAULTS. P11Q-06 replaces this
/// with the effective-dated window from `config/altair.toml`; until then this
/// struct is the only thing that knows a time, and it is named so a grep for
/// "demo" finds it.
struct DemoSessionTimes {
    /// 09:00 IST, in ns since IST midnight.
    static constexpr std::int64_t kPreOpenNs = 9LL * 3600 * 1'000'000'000LL;
    /// 09:15 IST.
    static constexpr std::int64_t kOpenNs =
        (9LL * 3600 + 15 * 60) * 1'000'000'000LL;
    /// 15:30 IST.
    static constexpr std::int64_t kCloseNs =
        (15LL * 3600 + 30 * 60) * 1'000'000'000LL;

    [[nodiscard]] static constexpr SessionWindow trading() noexcept {
        return SessionWindow{kOpenNs, kCloseNs};
    }
    [[nodiscard]] static constexpr SessionWindow pre_open() noexcept {
        return SessionWindow{kPreOpenNs, kOpenNs - 1};
    }
};

/// The clock, driven by the tape.
class MarketClock {
public:
    explicit MarketClock(SessionWindow trading, SessionWindow pre_open) noexcept
        : trading_(trading), pre_open_(pre_open) {}

    /// Feed it the engine timestamp off the last tick applied. Never a wall
    /// clock -- there is deliberately no `tick()` taking no argument.
    void observe(Timestamp engine_time) noexcept {
        engine_ = engine_time;
        seen_ = true;
    }

    [[nodiscard]] bool has_engine_time() const noexcept { return seen_; }
    [[nodiscard]] Timestamp engine_time() const noexcept { return engine_; }

    [[nodiscard]] SessionPhase phase() const noexcept {
        if (!seen_) {
            // Not "Closed". We do not know, and saying "closed" would be a
            // claim about the market rather than about our data.
            return SessionPhase::Unknown;
        }
        if (is_in_session(engine_, trading_)) {
            return SessionPhase::Open;
        }
        if (is_in_session(engine_, pre_open_)) {
            return SessionPhase::PreOpen;
        }
        return SessionPhase::Closed;
    }

    /// Nanoseconds until the session opens, or since it closed. Negative
    /// before the open, positive after the close, and absent when unknown --
    /// so a caller cannot render "0" and imply the bell just rang.
    [[nodiscard]] bool time_to_open_ns(std::int64_t& out) const noexcept {
        if (!seen_) {
            return false;
        }
        const std::int64_t sod = ist_ns_since_midnight(engine_);
        out = sod - trading_.open_ns;
        return true;
    }

private:
    SessionWindow trading_{0, 0};
    SessionWindow pre_open_{0, 0};
    Timestamp engine_{};
    bool seen_ = false;
};

} // namespace altair::ui
