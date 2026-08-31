// feed/failover.hpp — the stall watchdog and primary switch.
//
// P2-05. Decides WHEN to change the normaliser's active source. It does not
// decode, does not publish, and reads no clock: `now` is a parameter, so a
// replay of the same arrival pattern produces the same switches (rule 6).
//
// IT KEYS ON recv_ts, NEVER exchange_ts.
//
// That is the direct consequence of P2-02's finding: Kite's exchange timestamp
// is ONE-SECOND resolution, and two of its five modes carry none at all. A
// staleness threshold of 250 ms measured against a clock that only moves once
// a second would fire constantly on a perfectly healthy feed. `recv_ts` comes
// off the TSC and is locally monotonic, which is exactly what "have we heard
// anything lately" needs.
//
// Decisions D1..D6 are fixed in prompts/P2-05_failover.md.

#pragma once

#include <feed/normaliser.hpp>

#include <cstddef>
#include <cstdint>

namespace altair {

enum class FeedHealth : std::uint8_t {
    Unknown,   // nothing heard yet this session — NOT the same as healthy
    Live,      // heard within staleness_ms
    Stale      // silent for longer than staleness_ms
};

/// What the watchdog decided on this poll.
enum class FailoverAction : std::uint8_t {
    None,        // nothing changed
    SwitchTo,    // the primary moved — see `active`
    NoneHealthy  // BOTH sources are stale; nothing to switch to
};

struct FailoverDecision {
    FailoverAction action = FailoverAction::None;
    FeedSource     active = FeedSource::Kite;
    FeedHealth     kite = FeedHealth::Unknown;
    FeedHealth     xts = FeedHealth::Unknown;
};

class FailoverWatchdog {
public:
    struct Config {
        /// Silence longer than this marks a source Stale. UNIT: ms.
        std::int64_t staleness_ms = 250;
        /// A recovered source must stay Live this long before it can be
        /// switched BACK to. UNIT: ms. Without it a feed flapping either side
        /// of the threshold drags the primary with it, and every switch costs
        /// a subscription round trip.
        std::int64_t failback_stable_s = 60;
        /// When false, the primary never moves. The watchdog still reports
        /// health, so an operator can see what it WOULD have done.
        bool enabled = true;
        /// The source to prefer whenever both are healthy.
        FeedSource preferred = FeedSource::Kite;
    };

    struct Stats {
        std::uint64_t switches = 0;
        std::uint64_t failbacks = 0;      // switches back to `preferred`
        std::uint64_t none_healthy = 0;   // polls where BOTH were stale
        std::uint64_t suppressed = 0;     // switches the config forbade
    };

    explicit FailoverWatchdog(Config cfg) noexcept : cfg_(cfg), active_(cfg.preferred) {}

    /// Record that a struct arrived from `src` at `recv_ts`. ALTAIR_HOT.
    /// Called for BOTH sources, including the standby — that is the only way
    /// to know the standby is alive enough to fail over to.
    ALTAIR_HOT void on_data(FeedSource src, Timestamp recv_ts) noexcept {
        const std::size_t i = static_cast<std::size_t>(src);
        if (i >= kFeedSourceCount) {
            return;
        }
        // A clock that went backwards must not lower the high-water mark, or a
        // single out-of-order arrival would make the source look stale.
        if (!heard_[i] || last_[i] < recv_ts) {
            last_[i] = recv_ts;
        }
        heard_[i] = true;
    }

    /// Poll. `now` is a parameter — nothing here reads a clock.
    [[nodiscard]] FailoverDecision poll(Timestamp now) noexcept {
        FailoverDecision d{};
        d.kite = health_of(FeedSource::Kite, now);
        d.xts = health_of(FeedSource::Xts, now);
        d.active = active_;

        const FeedHealth act = (active_ == FeedSource::Kite) ? d.kite : d.xts;
        const FeedSource other = (active_ == FeedSource::Kite) ? FeedSource::Xts
                                                              : FeedSource::Kite;
        const FeedHealth oth = (other == FeedSource::Kite) ? d.kite : d.xts;

        // D5: neither source is usable. Reported, not silently tolerated —
        // switching to an equally dead feed would look like a fix.
        if (act != FeedHealth::Live && oth != FeedHealth::Live) {
            ++stats_.none_healthy;
            d.action = FailoverAction::NoneHealthy;
            return d;
        }

        if (act == FeedHealth::Live) {
            // Healthy. The only remaining question is failback: if the
            // preferred source is back and has been stable long enough, return
            // to it. D4.
            if (active_ != cfg_.preferred && oth == FeedHealth::Live
                && stable_for(other, now)) {
                if (!cfg_.enabled) {
                    ++stats_.suppressed;
                    return d;
                }
                active_ = other;
                ++stats_.switches;
                ++stats_.failbacks;
                d.action = FailoverAction::SwitchTo;
                d.active = active_;
            }
            return d;
        }

        // The active source is stale and the other is Live. Switch — with no
        // stability wait, because staying on a dead feed costs every tick.
        if (!cfg_.enabled) {
            ++stats_.suppressed;
            return d;
        }
        active_ = other;
        ++stats_.switches;
        d.action = FailoverAction::SwitchTo;
        d.active = active_;
        return d;
    }

    [[nodiscard]] FeedSource active() const noexcept { return active_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    /// Apply the decision to a Normaliser. Separate so the watchdog stays
    /// testable without one, and so the caller sees the switch happen.
    void apply(const FailoverDecision& d, Normaliser& n) const noexcept {
        if (d.action == FailoverAction::SwitchTo) {
            n.set_active(d.active);
        }
    }

    void reset_session() noexcept {
        stats_ = Stats{};
        active_ = cfg_.preferred;
        for (std::size_t i = 0; i < kFeedSourceCount; ++i) {
            heard_[i] = false;
            last_[i] = Timestamp{};
            live_since_[i] = Timestamp{};
        }
    }

private:
    /// D2: a source never heard from is Unknown, NOT Live and NOT Stale.
    ///
    /// Treating silence-so-far as healthy would let the watchdog fail over to
    /// a feed that has never delivered a single byte. Treating it as stale
    /// would trip `NoneHealthy` before the session has begun.
    [[nodiscard]] FeedHealth health_of(FeedSource src, Timestamp now) noexcept {
        const std::size_t i = static_cast<std::size_t>(src);
        if (i >= kFeedSourceCount || !heard_[i]) {
            return FeedHealth::Unknown;
        }
        const std::int64_t gap_ns = now.ns_since_epoch() - last_[i].ns_since_epoch();
        const bool live = gap_ns <= cfg_.staleness_ms * 1'000'000LL;
        if (live) {
            if (live_since_[i].is_epoch()) {
                live_since_[i] = now;      // start of this healthy stretch
            }
        } else {
            live_since_[i] = Timestamp{};  // the stretch is over
        }
        return live ? FeedHealth::Live : FeedHealth::Stale;
    }

    [[nodiscard]] bool stable_for(FeedSource src, Timestamp now) const noexcept {
        const std::size_t i = static_cast<std::size_t>(src);
        if (i >= kFeedSourceCount || live_since_[i].is_epoch()) {
            return false;
        }
        const std::int64_t held = now.ns_since_epoch()
                                - live_since_[i].ns_since_epoch();
        return held >= cfg_.failback_stable_s * 1'000'000'000LL;
    }

    Config     cfg_;
    Stats      stats_{};
    FeedSource active_;
    Timestamp  last_[kFeedSourceCount]{};
    Timestamp  live_since_[kFeedSourceCount]{};
    bool       heard_[kFeedSourceCount]{};
};

} // namespace altair
