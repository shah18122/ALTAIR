// feed/source_router.hpp -- verified, epoch-aware headless ingestion pipeline.
#pragma once

#include <feed/fyers_adapter.hpp>
#include <feed/normaliser.hpp>
#include <lockfree/spsc_ring.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace altair {

template <std::size_t TickCapacity = 1024, std::size_t DepthCapacity = 256>
class SourceRouter {
public:
    struct Stats {
        std::uint64_t source_resets{};
        std::uint64_t stale_epoch{};
        std::uint64_t duplicates{};
        std::uint64_t rejected_identity{};
        std::uint64_t normaliser_drops{};
        std::uint64_t backpressure_drops{};
    };

    explicit SourceRouter(Normaliser::Config config) noexcept : normaliser_(config) {}

    [[nodiscard]] bool activate(FeedSource source, std::uint64_t epoch) noexcept {
        if (static_cast<std::size_t>(source) >= kFeedSourceCount || epoch == 0) return false;
        if (active_epoch_ == epoch && normaliser_.active() == source) return true;
        normaliser_.set_active(source);
        normaliser_.reset_session();
        clear_duplicates();
        active_epoch_ = epoch;
        ++depth_generation_; // consumers must discard every older book image
        ++stats_.source_resets;
        return true;
    }

    [[nodiscard]] bool submit(FeedEnvelope<Tick> envelope, Timestamp now) noexcept {
        if (!eligible(envelope.value.source, envelope.epoch)) return false;
        const auto index = static_cast<std::size_t>(envelope.value.id);
        if (index >= kMaxInstruments) { ++stats_.rejected_identity; return false; }
        const auto fingerprint = tick_fingerprint(envelope.value);
        if (tick_seen_[index] && tick_hash_[index] == fingerprint) {
            ++stats_.duplicates;
            return false;
        }
        tick_seen_[index] = true;
        tick_hash_[index] = fingerprint;
        if (normaliser_.submit(envelope.value, now) != Verdictum::Publish) {
            ++stats_.normaliser_drops;
            return false;
        }
        if (!ticks_.try_push(envelope.value)) {
            normaliser_.note_dropped_full();
            ++stats_.backpressure_drops;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool submit(FeedEnvelope<DepthUpdate> envelope, Timestamp now) noexcept {
        if (!eligible(envelope.value.source, envelope.epoch)) return false;
        const auto index = static_cast<std::size_t>(envelope.value.id);
        if (index >= kMaxInstruments) { ++stats_.rejected_identity; return false; }
        const auto fingerprint = depth_fingerprint(envelope.value);
        if (depth_seen_[index] && depth_hash_[index] == fingerprint) {
            ++stats_.duplicates;
            return false;
        }
        depth_seen_[index] = true;
        depth_hash_[index] = fingerprint;
        if (normaliser_.submit(envelope.value, now) != Verdictum::Publish) {
            ++stats_.normaliser_drops;
            return false;
        }
        if (!depths_.try_push(envelope.value)) {
            normaliser_.note_dropped_full();
            ++stats_.backpressure_drops;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool try_pop(Tick& value) noexcept { return ticks_.try_pop(value); }
    [[nodiscard]] bool try_pop(DepthUpdate& value) noexcept { return depths_.try_pop(value); }
    [[nodiscard]] std::uint64_t active_epoch() const noexcept { return active_epoch_; }
    [[nodiscard]] FeedSource active_source() const noexcept { return normaliser_.active(); }
    [[nodiscard]] std::uint64_t depth_generation() const noexcept { return depth_generation_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] const Normaliser::Stats& normaliser_stats() const noexcept {
        return normaliser_.stats();
    }

private:
    [[nodiscard]] bool eligible(FeedSource source, std::uint64_t epoch) noexcept {
        if (active_epoch_ == 0 || epoch != active_epoch_ || source != normaliser_.active()) {
            ++stats_.stale_epoch;
            return false;
        }
        return true;
    }
    static void mix(std::uint64_t& hash, std::uint64_t value) noexcept {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    [[nodiscard]] static std::uint64_t tick_fingerprint(const Tick& value) noexcept {
        std::uint64_t hash = 1469598103934665603ULL;
        mix(hash, static_cast<std::uint32_t>(value.id));
        mix(hash, static_cast<std::uint64_t>(value.exchange_ts.ns_since_epoch()));
        mix(hash, static_cast<std::uint64_t>(value.recv_ts.ns_since_epoch()));
        mix(hash, static_cast<std::uint64_t>(value.last.raw()));
        mix(hash, static_cast<std::uint64_t>(value.last_qty.raw()));
        mix(hash, static_cast<std::uint64_t>(value.volume.raw()));
        mix(hash, static_cast<std::uint64_t>(value.oi));
        return hash;
    }
    [[nodiscard]] static std::uint64_t depth_fingerprint(const DepthUpdate& value) noexcept {
        std::uint64_t hash = 1469598103934665603ULL;
        mix(hash, static_cast<std::uint32_t>(value.id));
        mix(hash, static_cast<std::uint64_t>(value.exchange_ts.ns_since_epoch()));
        mix(hash, static_cast<std::uint64_t>(value.recv_ts.ns_since_epoch()));
        mix(hash, value.bid_levels);
        mix(hash, value.ask_levels);
        for (std::size_t i = 0; i < value.bid_levels && i < kDepthLevels; ++i) {
            mix(hash, static_cast<std::uint64_t>(value.bid[i].px.raw()));
            mix(hash, static_cast<std::uint64_t>(value.bid[i].qty.raw()));
            mix(hash, value.bid[i].orders);
        }
        for (std::size_t i = 0; i < value.ask_levels && i < kDepthLevels; ++i) {
            mix(hash, static_cast<std::uint64_t>(value.ask[i].px.raw()));
            mix(hash, static_cast<std::uint64_t>(value.ask[i].qty.raw()));
            mix(hash, value.ask[i].orders);
        }
        return hash;
    }
    void clear_duplicates() noexcept {
        tick_seen_.fill(false);
        depth_seen_.fill(false);
        tick_hash_.fill(0);
        depth_hash_.fill(0);
    }

    Normaliser normaliser_;
    SpscRing<Tick, TickCapacity> ticks_{};
    SpscRing<DepthUpdate, DepthCapacity> depths_{};
    std::array<std::uint64_t, kMaxInstruments> tick_hash_{};
    std::array<std::uint64_t, kMaxInstruments> depth_hash_{};
    std::array<bool, kMaxInstruments> tick_seen_{};
    std::array<bool, kMaxInstruments> depth_seen_{};
    Stats stats_{};
    std::uint64_t active_epoch_{};
    std::uint64_t depth_generation_{};
};

} // namespace altair
