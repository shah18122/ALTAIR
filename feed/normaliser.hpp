// feed/normaliser.hpp — gating, ordering, and source routing.
//
// P2-04. The decoder is a pure function; this is the stateful half. It owns
// per-instrument history, applies the look-ahead guard, decides what is
// publishable, and counts everything it refuses.
//
// The trap this file is built around: PlausibilityGate holds ONE `last_`, so
// it is a per-SOURCE object. Setting require_monotonic on a gate shared across
// instruments would reject a large fraction of perfectly good ticks — NIFTY at
// 10:00:05 followed by BANKNIFTY at 10:00:04 is completely ordinary — and it
// would look exactly like a broken feed. The gate does floor and ceiling;
// per-instrument ordering is tracked here.
//
// Decisions D1..D7 are fixed in prompts/P2-04_normaliser.md.

#pragma once

#include <core/time/exchange_ts.hpp>
#include <feed/tick.hpp>

#include <cstddef>
#include <cstdint>

namespace altair {

/// What the caller should do with the struct it just submitted.
enum class Verdictum : std::uint8_t {
    Publish,       // normalised; push it
    DropFuture,    // AfterCeiling — the look-ahead guard fired (D5)
    DropInactive   // decoded from the standby source (D3)
};

class Normaliser {
public:
    struct Config {
        /// `require_monotonic` is forced OFF by the constructor. See the file
        /// header — on a shared gate it cannot be correct.
        PlausibilityGate::Config gate{};
        FeedSource active = FeedSource::Kite;
    };

    struct Stats {
        std::uint64_t submitted = 0;
        std::uint64_t published = 0;
        std::uint64_t dropped_future = 0;     // rule 7 — never enters the pipeline
        std::uint64_t dropped_inactive = 0;
        std::uint64_t flagged_stale = 0;      // floor breach, flagged not dropped
        std::uint64_t out_of_order = 0;       // per-instrument regression
        std::uint64_t dropped_ring_full = 0;  // reported by the caller
    };

    explicit Normaliser(Config cfg) noexcept
        : cfg_(force_no_monotonic(cfg)),
          gate_(cfg_.gate) {
        clear_history();
    }

    Normaliser(const Normaliser&) = delete;
    Normaliser& operator=(const Normaliser&) = delete;

    /// Normalise a tick in place. UNIT: none. ALTAIR_HOT.
    ///
    /// Assigns `seq`, sets flags, and returns the verdict. `local_now` is a
    /// parameter — nothing here reads a clock, so a replay of the same inputs
    /// produces the same decisions (rule 6, rule 10).
    [[nodiscard]] ALTAIR_HOT Verdictum
    submit(Tick& t, Timestamp local_now) noexcept {
        ++stats_.submitted;
        if (t.source != cfg_.active) {
            // D3: the standby feed is gated and tracked so its staleness is
            // observable, but it must not reach the pipeline and must not
            // consume a sequence number.
            ++stats_.dropped_inactive;
            return Verdictum::DropInactive;
        }
        std::uint16_t flags = t.flags;
        const Verdictum v = judge(t.id, t.exchange_ts, local_now, flags);
        if (v != Verdictum::Publish) {
            return v;
        }
        t.flags = flags;
        t.seq = seq_++;
        ++stats_.published;
        return Verdictum::Publish;
    }

    /// The same path for a depth update — same gate, same sequence counter.
    /// A separate counter would make the two streams impossible to interleave
    /// deterministically on replay.
    [[nodiscard]] ALTAIR_HOT Verdictum
    submit(DepthUpdate& d, Timestamp local_now) noexcept {
        ++stats_.submitted;
        if (d.source != cfg_.active) {
            ++stats_.dropped_inactive;
            return Verdictum::DropInactive;
        }
        std::uint16_t flags = d.flags;
        const Verdictum v = judge(d.id, d.exchange_ts, local_now, flags);
        if (v != Verdictum::Publish) {
            return v;
        }
        d.flags = flags;
        d.seq = seq_++;
        ++stats_.published;
        return Verdictum::Publish;
    }

    /// The caller's ring was full. UNIT: none.
    /// Backpressure is information, not noise: it means a consumer is too slow.
    void note_dropped_full() noexcept { ++stats_.dropped_ring_full; }

    /// Switch the publishing source. P2-05 calls this.
    /// Per-instrument history is deliberately NOT reset: the instrument's
    /// timeline is continuous even when the feed reporting it changes.
    void set_active(FeedSource s) noexcept { cfg_.active = s; }
    [[nodiscard]] FeedSource active() const noexcept { return cfg_.active; }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] const PlausibilityGate& gate() const noexcept { return gate_; }
    [[nodiscard]] std::uint32_t next_seq() const noexcept { return seq_; }

    void reset_session() noexcept {
        seq_ = 0;
        stats_ = Stats{};
        gate_ = PlausibilityGate{cfg_.gate};
        clear_history();
    }

private:
    static Config force_no_monotonic(Config c) noexcept {
        // Requirement 1: this is not a preference to respect. A shared gate
        // with monotonic on rejects ordinary interleaved ticks, and silently
        // honouring the caller would produce a feed that looks broken.
        c.gate.require_monotonic = false;
        return c;
    }

    /// The shared decision path. Returns the verdict and may set flags.
    [[nodiscard]] ALTAIR_HOT Verdictum
    judge(InstrumentId id, Timestamp exchange_ts, Timestamp local_now,
          std::uint16_t& flags) noexcept {
        const auto admitted = gate_.admit(exchange_ts, local_now);
        if (!admitted) {
            if (admitted.error() == TsReject::AfterCeiling) {
                // D5: the tick claims to be from the future. Rule 7 is
                // absolute — this one never enters the pipeline.
                ++stats_.dropped_future;
                return Verdictum::DropFuture;
            }
            // Everything else is SUSPICIOUS, not impossible. Losing a real
            // trade to a clock problem is the worse failure, and a hole in the
            // volume series would hide the timestamp problem rather than
            // reveal it.
            flags = set_flag(flags, TickFlag::Stale);
            ++stats_.flagged_stale;
        }

        // D2: per-instrument ordering. EQUAL is normal — Kite's exchange_ts is
        // one-second resolution (P2-02), so many ticks share a second. Only a
        // strictly decreasing timestamp is anomalous.
        const std::uint32_t i = static_cast<std::uint32_t>(id);
        if (i < kMaxInstruments) {
            if (seen_[i] && exchange_ts < last_ts_[i]) {
                flags = set_flag(flags, TickFlag::Stale);
                ++stats_.out_of_order;
            }
            if (!seen_[i] || last_ts_[i] < exchange_ts) {
                last_ts_[i] = exchange_ts;
            }
            seen_[i] = true;
        }
        return Verdictum::Publish;
    }

    void clear_history() noexcept {
        for (std::size_t i = 0; i < kMaxInstruments; ++i) {
            seen_[i] = false;
            last_ts_[i] = Timestamp{};
        }
    }

    Config cfg_;
    PlausibilityGate gate_;
    Stats stats_{};
    std::uint32_t seq_ = 0;

    // ~128 KB. Not a stack object.
    Timestamp last_ts_[kMaxInstruments]{};
    bool seen_[kMaxInstruments]{};
};

} // namespace altair
