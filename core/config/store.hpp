#pragma once

// P0-08a — the hot-swappable config store.
//
// One writer publishes snapshots, many readers keep private copies.
//
// A reader NEVER reads through the store on the hot path. It calls refresh()
// on its own copy, which in the steady state is one acquire load and a
// compare, and only pays for a copy when the version actually moved. A
// ConfigSnapshot is several kilobytes, so copying it per tick would be absurd;
// copying it per reload is free.
//
// CX02-C1. NOT ON THE SEQLOCK ANY MORE, AND VERSIONS MUST RISE.
//
// This store was built on P0-06b's seqlock, whose reader copied the ~9 KB
// snapshot while the writer could be assigning it: a data race, undefined
// behaviour whatever the sequence check said afterwards (C02-002, C21-003).
// Its refresh() also spun without bound while a write was in flight (C02-003).
// It now publishes through SnapshotSlots, where the writer never writes a
// slot a reader can be copying, and a contended refresh is REFUSED with
// ConfigRefresh::Contended rather than spun.
//
// And publish() stored whatever version it was given (C02-004). The same
// version with different content, or a lower one, or a snapshot left at
// version 0 by a loader that forgot set_version -- every one was stored where
// no reader would ever notice it, and readers ran on stale or empty config.
// A version that does not rise is now refused and counted.

#include <config/config.hpp>
#include <lockfree/snapshot_slots.hpp>
#include <types/units.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace altair {

/// How many threads may be inside refresh() AT THE SAME INSTANT. A ninth is
/// refused with ConfigRefresh::Contended and retries on its next refresh --
/// it is never blocked and never handed a torn copy.
inline constexpr std::size_t kConfigReaders = 8;

enum class ConfigStoreError : std::uint8_t {
    /// The snapshot's version is not greater than the published one.
    StaleVersion,
    /// The slot store had no free slot. Proven unreachable (snapshot_slots.hpp).
    NoFreeSlot
};

/// What refresh() did. Ordinal 0 is Contended -- "nothing was copied, try
/// again" -- so a zeroed result never reads as "your copy is current".
enum class ConfigRefresh : std::uint8_t {
    Contended = 0,
    /// NOTHING HAS EVER BEEN PUBLISHED. Not "your copy is current": a reader
    /// holding a default-constructed snapshot has an EMPTY config, and the
    /// two used to be the same answer (R-AB-019). C02-004's trigger B -- a
    /// loader that forgot set_version -- is refused at publish(); this is the
    /// same conflation on the reader's side, and rule 9 says it must be
    /// handled rather than mistaken for being up to date.
    NoConfig,
    /// `local` already holds the published version. Nothing copied.
    Unchanged,
    /// `local` was replaced with the published snapshot.
    Updated
};

// ─────────────────────────────────────────────────────────────────────────
// ConfigStore — publish/refresh, allocation-free, never blocking.
// ─────────────────────────────────────────────────────────────────────────
class ConfigStore {
public:
    ConfigStore() noexcept = default;

    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    /// Publish a new snapshot. WRITER THREAD ONLY — exactly one, for the life
    /// of the store. UNIT: none. Never blocks.
    /// Refuses (StaleVersion) unless s.version() is greater than the
    /// published version, so version 0 is always refused.
    [[nodiscard]] std::expected<void, ConfigStoreError>
    publish(const ConfigSnapshot& s) noexcept {
        if (s.version() <= version_.load(std::memory_order_acquire)) {
            refused_.fetch_add(1, std::memory_order_relaxed);
            return std::unexpected(ConfigStoreError::StaleVersion);
        }
        if (!slots_.publish(s)) {
            refused_.fetch_add(1, std::memory_order_relaxed);
            return std::unexpected(ConfigStoreError::NoFreeSlot);
        }
        // Released AFTER the slot is published: a reader that sees the new
        // version can always copy a snapshot at least that new.
        version_.store(s.version(), std::memory_order_release);
        publishes_.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    /// Bring `local` up to date. UNIT: none.
    /// Unchanged when `local` already holds the published version -- the
    /// steady-state path, one acquire load and a compare, no copy. Updated
    /// when it copied. Contended when a copy was needed and could not be made
    /// safely right now; `local` is then untouched.
    /// PRECONDITION: `local` is this thread's own copy, not shared.
    [[nodiscard]] ALTAIR_HOT ConfigRefresh
    refresh(ConfigSnapshot& local) const noexcept {
        const std::uint64_t published = version_.load(std::memory_order_acquire);
        // Nothing has ever been published. Said first, because every version
        // comparison below would otherwise answer "Unchanged" for a reader
        // whose own copy is empty and whose version is also 0 (R-AB-019).
        if (published == 0) {
            return ConfigRefresh::NoConfig;
        }
        // >=, not ==: a reader can copy a snapshot whose slot was published
        // an instant before version_ was released, and must not then treat
        // the older version_ as a change.
        if (local.version() >= published) {
            return ConfigRefresh::Unchanged;
        }
        const auto g = slots_.read(local);
        if (!g) {
            return ConfigRefresh::Contended;
        }
        // Unreachable: published > 0 means a slot publish already succeeded,
        // so read() cannot answer "nothing published". Reported as NoConfig
        // rather than as Unchanged, because that is what it would mean
        // (R-AB-020).
        return *g == 0 ? ConfigRefresh::NoConfig : ConfigRefresh::Updated;
    }

    /// Currently published version. UNIT: none. Zero before the first publish.
    [[nodiscard]] std::uint64_t version() const noexcept {
        return version_.load(std::memory_order_acquire);
    }

    /// Successful publishes since construction. UNIT: count.
    [[nodiscard]] std::uint64_t publishes() const noexcept {
        return publishes_.load(std::memory_order_relaxed);
    }

    /// Refused publishes since construction. UNIT: count.
    [[nodiscard]] std::uint64_t refused() const noexcept {
        return refused_.load(std::memory_order_relaxed);
    }

private:
    SnapshotSlots<ConfigSnapshot, kConfigReaders> slots_{};
    std::atomic<std::uint64_t> version_{0};
    std::atomic<std::uint64_t> publishes_{0};
    std::atomic<std::uint64_t> refused_{0};
};

} // namespace altair
