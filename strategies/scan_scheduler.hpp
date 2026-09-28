// strategies/scan_scheduler.hpp -- the event-driven cross-venue scan scheduler.
//
// P7-02. A cross-venue scanner cannot re-price every pair on every tick and
// stay within budget, and it must not re-price a pair whose two legs were last
// seen seconds apart. This decides WHICH pairs are worth scanning right now:
// event-driven (a pair becomes a candidate only when one of its legs updates),
// freshness-gated (both legs recent), skew-gated (the two legs seen close in
// time), and bounded (at most N pairs handed out per drain).
//
// IT SCHEDULES. IT DOES NOT PRICE, AND IT DOES NOT TRADE.
//
// The output is a list of pair ids ready to scan; strategies/basis.hpp prices
// them and oms/ decides. This file includes no cost model and no oms/ header.
//
// A MISSING OR STALE LEG SUPPRESSES THE SIGNAL, AND THE SUPPRESSION IS COUNTED
// (rule 9). A pair whose BSE leg has not printed since the open is not "no
// opportunity" -- it is "not observable", and the two are different facts. Both
// are in stats(), so coverage is a number, never an assumption. "Scan
// everything" means the subscribed, both-legs-fresh universe, with the gap
// reported.
//
// TIME COMES OFF THE UPDATE, NEVER A WALL CLOCK (rule 7). on_update and drain
// both take the timestamp as a parameter.

#pragma once

#include <core/time/timestamp.hpp>
#include <core/types/units.hpp>
#include <instruments/contract_spec.hpp>
#include <instruments/cross_venue.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair::strategies {

// InstrumentId and kMaxInstruments live in namespace altair (contract_spec.hpp);
// CrossVenuePair and kMaxCrossVenuePairs live in altair::instruments.
using altair::InstrumentId;
using altair::kMaxInstruments;
using altair::instruments::CrossVenuePair;

inline constexpr std::size_t kMaxScheduledPairs = instruments::kMaxCrossVenuePairs;

enum class ScheduleError : std::uint8_t {
    Full,           ///< pair capacity exceeded -- refused, never truncated
    BadPair,        ///< a pair leg is InstrumentId::Invalid
    IdOutOfRange,   ///< a leg id is >= kMaxInstruments (cannot index)
    DuplicateLeg    ///< an instrument already belongs to another pair
};

/// Freshness and skew budget. All UTC-ns durations.
struct ScanBudget {
    Duration max_age{};   ///< a leg older than this at drain time is stale
    Duration max_skew{};  ///< the two legs must have printed within this of each
};

class ScanScheduler {
public:
    struct Stats {
        std::uint64_t updates_admitted = 0;    ///< updates that hit a known leg
        std::uint64_t updates_unsubscribed = 0;///< updates for no registered leg
        std::uint64_t scans_emitted = 0;       ///< pairs handed out to price
        std::uint64_t suppressed_stale = 0;    ///< dirty but a leg was stale
        std::uint64_t suppressed_skew = 0;     ///< dirty but legs too far apart
        std::uint64_t suppressed_incomplete = 0;///< dirty but a leg never printed
    };

    ScanScheduler() noexcept {
        for (std::size_t i = 0; i < kMaxInstruments; ++i) { pair_of_[i] = -1; }
    }

    /// Register a cross-listed pair. Both legs must be valid and in range, and
    /// neither may already belong to another pair. Returns the pair's index.
    [[nodiscard]] std::expected<std::size_t, ScheduleError>
    register_pair(const CrossVenuePair& p) noexcept {
        if (p.nse == InstrumentId::Invalid || p.bse == InstrumentId::Invalid) {
            return std::unexpected(ScheduleError::BadPair);
        }
        const auto nse = static_cast<std::uint32_t>(p.nse);
        const auto bse = static_cast<std::uint32_t>(p.bse);
        if (nse >= kMaxInstruments || bse >= kMaxInstruments) {
            return std::unexpected(ScheduleError::IdOutOfRange);
        }
        if (pair_of_[nse] != -1 || pair_of_[bse] != -1) {
            return std::unexpected(ScheduleError::DuplicateLeg);
        }
        if (count_ >= kMaxScheduledPairs) {
            return std::unexpected(ScheduleError::Full);
        }
        const std::size_t idx = count_++;
        pairs_[idx] = Pair{p.nse, p.bse, Timestamp::epoch(), Timestamp::epoch(),
                           false};
        pair_of_[nse] = static_cast<std::int32_t>(idx);
        pair_of_[bse] = static_cast<std::int32_t>(idx);
        return idx;
    }

    /// Record that `id` printed at `ts`. Marks the owning pair dirty. An update
    /// for an instrument in no pair is counted and ignored (coverage). UNIT: ns.
    void on_update(InstrumentId id, Timestamp ts) noexcept {
        const auto raw = static_cast<std::uint32_t>(id);
        if (id == InstrumentId::Invalid || raw >= kMaxInstruments
            || pair_of_[raw] == -1) {
            ++stats_.updates_unsubscribed;
            return;
        }
        Pair& p = pairs_[static_cast<std::size_t>(pair_of_[raw])];
        if (p.nse == id) { p.nse_ts = ts; } else { p.bse_ts = ts; }
        p.dirty = true;
        ++stats_.updates_admitted;
    }

    /// Hand out up to `max_work` (and up to `cap`) pair indices that are dirty
    /// AND ready at `now`: both legs printed, both within max_age, and within
    /// max_skew of each other. A dirty pair that is not ready is left dirty and
    /// its reason counted, so a later fresher update can still surface it.
    /// Returns the number written to `out`. UNIT: now in ns.
    [[nodiscard]] std::size_t
    drain(Timestamp now, std::size_t max_work,
          std::size_t* out, std::size_t cap, const ScanBudget& budget) noexcept {
        std::size_t n = 0;
        // RULE 11: this is a visible work-budget boundary, not silent data
        // truncation. Only emitted rows are cleared; every pair beyond the
        // caller/output cap remains dirty and is eligible on the next drain.
        const std::size_t limit = max_work < cap ? max_work : cap;
        for (std::size_t i = 0; i < count_ && n < limit; ++i) {
            Pair& p = pairs_[i];
            if (!p.dirty) { continue; }
            const Ready r = ready(p, now, budget);
            if (r == Ready::Yes) {
                out[n++] = i;
                p.dirty = false;
                ++stats_.scans_emitted;
            } else if (r == Ready::Incomplete) {
                ++stats_.suppressed_incomplete;
            } else if (r == Ready::Stale) {
                ++stats_.suppressed_stale;
            } else {
                ++stats_.suppressed_skew;
            }
        }
        return n;
    }

    [[nodiscard]] std::size_t pair_count() const noexcept { return count_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    /// How many registered pairs currently have both legs fresh at `now`. The
    /// honest denominator for "what fraction of the universe is observable".
    [[nodiscard]] std::size_t observable(Timestamp now,
                                         const ScanBudget& budget) const noexcept {
        std::size_t n = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            if (ready(pairs_[i], now, budget) == Ready::Yes) { ++n; }
        }
        return n;
    }

private:
    struct Pair {
        InstrumentId nse{InstrumentId::Invalid};
        InstrumentId bse{InstrumentId::Invalid};
        Timestamp nse_ts{};
        Timestamp bse_ts{};
        bool dirty{false};
    };
    enum class Ready : std::uint8_t { Yes, Incomplete, Stale, Skew };

    [[nodiscard]] static Ready ready(const Pair& p, Timestamp now,
                                     const ScanBudget& b) noexcept {
        if (p.nse_ts <= Timestamp::epoch() || p.bse_ts <= Timestamp::epoch()) {
            return Ready::Incomplete;
        }
        if ((now - p.nse_ts) > b.max_age || (now - p.bse_ts) > b.max_age) {
            return Ready::Stale;
        }
        const Duration skew = (p.nse_ts >= p.bse_ts) ? (p.nse_ts - p.bse_ts)
                                                     : (p.bse_ts - p.nse_ts);
        if (skew > b.max_skew) { return Ready::Skew; }
        return Ready::Yes;
    }

    Pair pairs_[kMaxScheduledPairs]{};
    std::int32_t pair_of_[kMaxInstruments]{};
    std::size_t count_ = 0;
    Stats stats_{};
};

} // namespace altair::strategies
