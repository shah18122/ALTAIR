#pragma once

// P0-08a — the hot-swappable config store.
//
// One writer publishes snapshots, many readers keep private copies. Built on
// P0-06b's seqlock rather than a second mechanism: "one writer publishes a
// value that many readers sample, without ever blocking the writer" is exactly
// what a seqlock is for.
//
// A reader NEVER reads through the store on the hot path. It calls refresh()
// on its own copy, which in the steady state is one acquire load and a
// compare, and only pays for a copy when the version actually moved. A
// ConfigSnapshot is several kilobytes, so copying it per tick would be absurd;
// copying it per reload is free.

#include <config/config.hpp>
#include <lockfree/seqlock.hpp>
#include <types/units.hpp>

#include <atomic>
#include <cstdint>

namespace altair {

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
    /// PRECONDITION: s.version() is greater than the currently published
    /// version, or readers will not notice the change.
    void publish(const ConfigSnapshot& s) noexcept {
        snap_.store(s);
        // Release AFTER the payload: a reader that sees the new version must
        // also see the snapshot it belongs to.
        version_.store(s.version(), std::memory_order_release);
        publishes_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Bring `local` up to date. UNIT: none.
    /// Returns true iff `local` was replaced. When the published version equals
    /// local.version() this does NO copy and returns false — that is the
    /// steady-state path and it is one acquire load.
    /// PRECONDITION: `local` is this thread's own copy, not shared.
    [[nodiscard]] ALTAIR_HOT bool refresh(ConfigSnapshot& local) const noexcept {
        if (version_.load(std::memory_order_acquire) == local.version()) {
            return false;   // steady state: no copy at all
        }
        // Changed. Copy under the seqlock, retrying until the writer is quiet.
        while (!snap_.try_load(local)) {
            // spin
        }
        return true;
    }

    /// Currently published version. UNIT: none. Zero before the first publish.
    [[nodiscard]] std::uint64_t version() const noexcept {
        return version_.load(std::memory_order_acquire);
    }

    /// Successful publishes since construction. UNIT: count.
    [[nodiscard]] std::uint64_t publishes() const noexcept {
        return publishes_.load(std::memory_order_relaxed);
    }

private:
    SeqlockSnapshot<ConfigSnapshot> snap_{};
    std::atomic<std::uint64_t> version_{0};
    std::atomic<std::uint64_t> publishes_{0};
};

} // namespace altair
